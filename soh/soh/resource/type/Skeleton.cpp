#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/OtrArchive.h>
#include <ship/resource/archive/O2rArchive.h>
#include <unordered_set>
#include "Skeleton.h"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "libultraship/libultraship.h"
#include <fast/lus_gbi.h>
#include <fast/f3dex2.h>
#include <fast/resource/type/DisplayList.h>
#include <bitset>
#include <soh_assets.h>
#include <objects/object_link_child/object_link_child.h>
#include <objects/object_link_boy/object_link_boy.h>
#include "macros.h"
#include <filesystem>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "z64player.h"
extern PlayState* gPlayState;
}

extern "C" SaveContext gSaveContext;
extern "C" u16 gEquipMasks[4];
extern "C" u8 gEquipShifts[4];
extern "C" void gfx_texture_cache_clear();

namespace SOH {

// Tracks the filesystem paths of archives added to ArchiveManager by LoadFileFromCoopFolder
// on behalf of the LOCAL player's coop model selection.  When the player switches to a
// different model (or back to default), these archives are removed from ArchiveManager so
// they no longer pollute the alt-asset lookup in UpdateCustomSkeletonFromPath.
// DummyPlayer archive tracking is NOT done here — DummyPlayers use per-client shared_ptrs
// and unique cache keys, so they never conflict with each other or the local player.
static std::unordered_set<std::string> sLocalCoopArchivePaths;

// Tracks the last folder successfully passed to UpdateCustomSkeletonFromFolder so that
// repeated calls with the same folder (e.g. every DummyPlayer spawn firing OnLinkSkeletonInit)
// can skip the expensive ClearLocalCoopArchives + AddArchive cycle.  Reset to "" whenever
// the folder changes or the archives are explicitly cleared via the empty-folder path.
static std::string sLastLoadedCoopFolder;

// Historically this function removed each coop pack archive from ArchiveManager
// before applying a new folder or reverting to default.  That triggered
// ArchiveManager::ResetVirtualFileSystem() either per-call (original) or once
// (via SetArchives batched), and the reset's Unload+Load cycle for every
// surviving archive (including oot.o2r / soh.o2r and every mod) remapped
// their backing mmap regions.  Any cached resource holding a raw pointer into
// the old memory then became invalid — producing the intermittent white-tunic
// regression on Default Link (log 42 analysis: ~600 ms of archive churn on
// revert, then vanilla bake ran against post-remap resources).
//
// LUS's alt-asset priority is "last-added wins", so leaving old pack archives
// mounted is harmless for rendering: the active pack's new archives (or the
// baked skeleton's loadExact=true lookups) take precedence.  The resource
// cache keys are pack-unique ("coopchar/<folder>/...") so there's no
// cross-pack cache collision either.  Memory grows modestly across a session
// as the user samples packs, which is the accepted tradeoff to eliminate the
// ResetVirtualFileSystem() churn.
//
// We keep the tracking set populated so future work can re-enable a safer
// eviction path if one emerges; the set is not currently consulted by any
// cleanup caller.
static void ClearLocalCoopArchives(std::shared_ptr<Ship::ResourceManager> resourceMgr) {
    (void)resourceMgr;
    // Intentionally a no-op.  See comment above for rationale.
}

SkeletonData* Skeleton::GetPointer() {
    return &skeletonData;
}

size_t Skeleton::GetPointerSize() {
    switch (type) {
        case SkeletonType::Normal:
            return sizeof(skeletonData.skeletonHeader);
        case SkeletonType::Flex:
            return sizeof(skeletonData.flexSkeletonHeader);
        case SkeletonType::Curve:
            return sizeof(skeletonData.skelCurveLimbList);
        default:
            return 0;
    }
}

std::vector<SkeletonPatchInfo> SkeletonPatcher::skeletons;

// Searches coopplayercharacters/<folder>/ on the filesystem for an archive
// containing altPath, returning the loaded Ship::File on success or nullptr.
//
// coopplayercharacters/ sits alongside mods/ in the game's data root (e.g.
// Release/coopplayercharacters/) — NOT inside mods/.  This avoids the
// ArchiveManager's filename-dedup collision: mod_menu.cpp keys archives by
// filename (without extension), so two archives with the same filename in
// different character folders — e.g. 3dsLink/3ds_link.otr and
// 3dsMMLink/3ds_link.otr — would collide if loaded through the normal mod path.
// Scanning a separate folder on disk sidesteps that limitation entirely.
//
// The shared_ptr<vector> returned by GetArchives() is stored in a local variable
// before iterating; iterating *GetArchives() directly is UB because the temporary
// shared_ptr is destroyed before the loop body runs, freeing the vector.
// trackForLocal: if true, record each newly-added archive path in sLocalCoopArchivePaths
// so ClearLocalCoopArchives() can remove it when the local player switches models.
// Pass false for DummyPlayer calls — the archive is opened transiently (not added to
// ArchiveManager) so it cannot pollute the global alt-asset lookup.
static std::shared_ptr<Ship::File> LoadFileFromCoopFolder(const std::string& folder,
                                                           const std::string& altPath,
                                                           std::shared_ptr<Ship::ResourceManager> resourceMgr,
                                                           bool trackForLocal = false) {
    const std::string coopRoot = Ship::Context::LocateFileAcrossAppDirs("coopplayercharacters", appShortName);
    const std::filesystem::path charDir =
        std::filesystem::path(coopRoot) / folder;

    SPDLOG_INFO("[CoopModel] LoadFileFromCoopFolder: folder=\"{}\" altPath=\"{}\" trackForLocal={} charDir=\"{}\"",
                folder, altPath, trackForLocal, charDir.generic_string());

    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
        SPDLOG_WARN("[CoopModel]   charDir does not exist or is not a directory: \"{}\" — returning nullptr",
                    charDir.generic_string());
        return nullptr;
    }

    auto archiveManager = resourceMgr->GetArchiveManager();

    // Recursive so packs that nest archives in per-age subfolders (e.g.
    // DarkMalon/DarkMalonChild/DarkMalonChild.otr) are still discovered.
    //
    // Only archives that CONTAIN the probe path get added to ArchiveManager.
    // Sibling archives in the same folder (e.g. DarkMalonChildTextures.otr
    // when we're probing for the skel) are opened transiently at bake time
    // via OpenCoopPackArchives() — that bake path reads their textures
    // directly and caches them under pack-unique "coopchar/<folder>/..."
    // keys, so render-time never needs them in ArchiveManager.  Eagerly
    // registering non-matching siblings here caused the white-tunic
    // regression (log 35) because their presence perturbed the revert-time
    // ResetVirtualFileSystem() cycle.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(charDir)) {
        if (entry.is_directory()) continue;
        const std::string ext = entry.path().extension().generic_string();
        if (ext != ".otr" && ext != ".o2r") continue;

        const std::string archivePath = entry.path().generic_string();

        // Already in the manager?  Then just probe it in place.
        std::shared_ptr<Ship::Archive> archive = nullptr;
        {
            auto loaded = archiveManager->GetArchives();
            for (auto& a : *loaded) {
                if (a->GetPath() == archivePath) {
                    archive = a;
                    break;
                }
            }
        }

        if (archive != nullptr) {
            SPDLOG_INFO("[CoopModel]   archive already loaded: \"{}\"", archivePath);
            auto file = archive->LoadFile(altPath);
            if (file != nullptr) {
                SPDLOG_INFO("[CoopModel]   found \"{}\" in archive \"{}\"", altPath, archivePath);
                return file;
            }
            continue;
        }

        // Not in the manager — open transiently FIRST to peek inside without
        // committing to registration.  Only upgrade to ArchiveManager if the
        // probe hits (for the local-player path) AND the caller wants tracking.
        std::shared_ptr<Ship::Archive> opened;
        if (ext == ".o2r" || ext == ".zip") {
            opened = std::make_shared<Ship::O2rArchive>(archivePath);
        } else {
            opened = std::make_shared<Ship::OtrArchive>(archivePath);
        }
        opened->Load();
        if (!opened->IsLoaded()) continue;

        auto file = opened->LoadFile(altPath);
        if (file == nullptr) continue;  // not this one; don't register

        // Probe hit.  For the local player, promote to ArchiveManager so
        // render-time alt-asset lookups see it.  For DummyPlayer, the
        // transient open is sufficient — the caller holds the File.
        if (trackForLocal) {
            auto added = archiveManager->AddArchive(archivePath);
            if (added != nullptr) {
                SPDLOG_INFO("[CoopModel]   AddArchive (tracked, probe-match): \"{}\"", archivePath);
                sLocalCoopArchivePaths.insert(archivePath);
                // Re-read from the archive in the manager so File's backing archive
                // matches the long-lived handle, not the transient one.
                auto persistent = added->LoadFile(altPath);
                if (persistent != nullptr) {
                    SPDLOG_INFO("[CoopModel]   found \"{}\" in archive \"{}\"", altPath, archivePath);
                    return persistent;
                }
            }
        }
        SPDLOG_INFO("[CoopModel]   found \"{}\" in transient archive \"{}\"", altPath, archivePath);
        return file;
    }
    SPDLOG_INFO("[CoopModel]   \"{}\" not found in any archive under folder \"{}\"", altPath, folder);
    return nullptr;
}

bool SkeletonPatcher::IsLinkSkeletonPath(const std::string& path) {
    return (sOtr + path == std::string(gLinkAdultSkel)) || (sOtr + path == std::string(gLinkChildSkel));
}

bool SkeletonPatcher::IsLocalPlayerSkelAnime(SkelAnime* skelAnime) {
    if (gPlayState == nullptr) {
        return false;
    }

    Player* player = GET_PLAYER(gPlayState);

    if (player == nullptr) {
        return false;
    }

    PauseContext* pauseCtx = &gPlayState->pauseCtx;

    return (skelAnime == &player->skelAnime) || (skelAnime == &player->upperSkelAnime) ||
           (skelAnime == &pauseCtx->playerSkelAnime);
}

void SkeletonPatcher::RegisterSkeleton(std::string& path, SkelAnime* skelAnime) {
    SkeletonPatchInfo info;

    info.skelAnime = skelAnime;
    info.isLocalPlayer = false;

    if (path.starts_with(sOtr)) {
        path = path.substr(sOtr.length());
    }

    // Determine if we're using an alternate skeleton
    if (path.starts_with(Ship::IResource::gAltAssetPrefix)) {
        info.vanillaSkeletonPath = path.substr(Ship::IResource::gAltAssetPrefix.length(),
                                               path.size() - Ship::IResource::gAltAssetPrefix.length());
    } else {
        info.vanillaSkeletonPath = path;
    }

    if (IsLinkSkeletonPath(info.vanillaSkeletonPath)) {
        info.isLocalPlayer = IsLocalPlayerSkelAnime(skelAnime);

        // Skip registering skeletons that do not belong to the local player (e.g. Anchor dummy actors)
        if (!info.isLocalPlayer) {
            return;
        }
    }

    skeletons.push_back(std::move(info));
}

void SkeletonPatcher::UnregisterSkeleton(SkelAnime* skelAnime) {

    // TODO: Should probably just use a dictionary here...
    for (size_t i = 0; i < skeletons.size(); i++) {
        if (skeletons[i].skelAnime == skelAnime) {
            skeletons.erase(skeletons.begin() + i);
            break;
        }
    }
}
void SkeletonPatcher::ClearSkeletons() {
    skeletons.clear();
}

void SkeletonPatcher::UpdateSkeletons() {
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    bool isAlt = resourceMgr->IsAltAssetsEnabled();
    for (auto& skel : skeletons) {
        Skeleton* newSkel =
            (Skeleton*)resourceMgr
                ->LoadResource((isAlt ? Ship::IResource::gAltAssetPrefix : "") + skel.vanillaSkeletonPath, true)
                .get();

        if (newSkel != nullptr) {
            skel.skelAnime->skeleton = newSkel->skeletonData.skeletonHeader.segment;
            uintptr_t skelPtr = (uintptr_t)newSkel->GetPointer();
            memcpy(&skel.skelAnime->skeletonHeader, &skelPtr,
                   sizeof(uintptr_t)); // Dumb thing that needs to be done because cast is not cooperating
        }
    }
}

void SkeletonPatcher::UpdateCustomSkeletons() {
    int localCount = 0;
    for (auto& skel : skeletons) {
        if (skel.isLocalPlayer) localCount++;
    }
    SPDLOG_INFO("[CoopModel] UpdateCustomSkeletons: {} registered, {} local",
                skeletons.size(), localCount);
    for (auto& skel : skeletons) {
        if (!skel.isLocalPlayer) {
            continue;
        }

        UpdateTunicSkeletons(skel);
    }
}

void SkeletonPatcher::UpdateTunicSkeletons(SkeletonPatchInfo& skel) {
    std::string skeletonPath;

    // Determine tunic-variant skeleton path for the current age and equipped tunic.
    if (sOtr + skel.vanillaSkeletonPath == std::string(gLinkAdultSkel)) {
        switch (TUNIC_EQUIP_TO_PLAYER(CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC))) {
            case PLAYER_TUNIC_KOKIRI:
                skeletonPath = std::string(gLinkAdultKokiriTunicSkel).substr(sOtr.length());
                break;
            case PLAYER_TUNIC_GORON:
                skeletonPath = std::string(gLinkAdultGoronTunicSkel).substr(sOtr.length());
                break;
            case PLAYER_TUNIC_ZORA:
                skeletonPath = std::string(gLinkAdultZoraTunicSkel).substr(sOtr.length());
                break;
            default:
                return;
        }
    } else if (sOtr + skel.vanillaSkeletonPath == std::string(gLinkChildSkel)) {
        switch (TUNIC_EQUIP_TO_PLAYER(CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC))) {
            case PLAYER_TUNIC_KOKIRI:
                skeletonPath = std::string(gLinkChildKokiriTunicSkel).substr(sOtr.length());
                break;
            case PLAYER_TUNIC_GORON:
                skeletonPath = std::string(gLinkChildGoronTunicSkel).substr(sOtr.length());
                break;
            case PLAYER_TUNIC_ZORA:
                skeletonPath = std::string(gLinkChildZoraTunicSkel).substr(sOtr.length());
                break;
            default:
                return;
        }
    } else {
        return;
    }

    // Check for an explicit Anchor character folder selection.  Read the CVar directly
    // here rather than relying on a hook because CustomSkeletons.cpp's OnLinkSkeletonInit
    // hook fires before Anchor's hooks, so a pre-set static would always be stale on the
    // first call after a scene load.
    const char* overrideFolder = CVarGetString(CVAR_REMOTE_ANCHOR("CharacterModel"), nullptr);
    const std::string folderStr = (overrideFolder != nullptr) ? std::string(overrideFolder) : std::string();
    if (!folderStr.empty()) {
        SPDLOG_INFO("[CoopModel] UpdateTunicSkeletons: folder override=\"{}\" path=\"{}\"",
                    folderStr, skeletonPath);
    } else {
        SPDLOG_INFO("[CoopModel] UpdateTunicSkeletons: no override (revert to default) path=\"{}\"",
                    skeletonPath);
    }
    // Route BOTH cases through UpdateCustomSkeletonFromFolder.  Its folder.empty() branch
    // already performs the archive cleanup (ClearLocalCoopArchives + untracked-archive
    // removal + sLastLoadedCoopFolder reset) and then falls through to the vanilla
    // resolution path — without this, selecting "Default Link" after a coop pack left
    // the previous pack's archives registered in ArchiveManager, so the alt-asset lookup
    // inside UpdateCustomSkeletonFromPath kept returning the previous pack's skeleton
    // (Coop Test 19 log 10: "alt lookup ... -> FOUND" pointing at a 3dsLink skeleton
    // after the user had switched to Default Link).
    UpdateCustomSkeletonFromFolder(skeletonPath, folderStr, skel);
}

// Multi-archive bundle for the bake walker.  Packs frequently split their
// skeleton and texture assets across sibling .otr files (DarkMalon stores the
// skel in DarkMalonChild.otr but the full-res _00_BaseColor_Merged in
// DarkMalonChildTextures.otr).  A single-archive bake probe only finds the
// skel archive's own textures — which for packs that ship a low-res
// placeholder in the skel archive means rendering the low-res version.
// LoadFile iterates in reverse (last-added = highest priority, matching the
// LUS ArchiveManager convention where later archives override earlier ones).
struct CoopPackArchives {
    std::vector<std::shared_ptr<Ship::Archive>> archives;
    std::shared_ptr<Ship::File> LoadFile(const std::string& path) const {
        for (auto it = archives.rbegin(); it != archives.rend(); ++it) {
            auto f = (*it)->LoadFile(path);
            if (f) return f;
        }
        return nullptr;
    }
    bool IsLoaded() const { return !archives.empty(); }
};

// Forward declarations for the bake helpers defined later in this file.
// Issue #82 — UpdateCustomSkeletonFromFolder now invokes these on the local-player
// pack-switch and revert-to-default paths so the rendered visual updates
// immediately rather than on next scene change.
static CoopPackArchives OpenCoopPackArchives(const std::string& folder,
                                              const std::string& probePath);
static bool BuildBakedPlayerModel(
    const std::shared_ptr<Skeleton>& skeleton,
    const std::string& folder,
    const CoopPackArchives& archive,
    std::shared_ptr<Ship::ResourceLoader>& loader,
    BakedPlayerModel& outModel);
static bool BuildVanillaDummyPlayerModel(
    const std::shared_ptr<Skeleton>& skeleton,
    std::shared_ptr<Ship::ResourceManager>& resMgr,
    BakedPlayerModel& outModel);

// Bakes the vanilla skeleton onto the local player's skelAnime under
// "vanilla-dummy/..." cache keys.  Used both by the revert-to-default path and
// by the pack-not-found fallback — a pack that is missing the requested
// skeleton (e.g. adult-only pack while local Link is a child) would otherwise
// hand off to UpdateCustomSkeletonFromPath, which does a GLOBAL alt-asset
// lookup and picks up whatever child skeleton happens to be in an unrelated
// loaded mod.  That produces a Default-Link-ish visual with wrong eye/mouth
// textures (no BakeFaceTextures call on that path).  Vanilla bake gives a
// clean Default Link visual with correct textures instead.
//
// Retires any outgoing bakedModel via kRetireFrames so the renderer can finish
// consuming it before destruction.  Returns true on success.
static bool BakeLocalVanillaFallback(SkeletonPatchInfo& skel,
                                      std::shared_ptr<Ship::ResourceManager>& resourceMgr) {
    // Fix 1 (2026-04-20 test 19 regression): use the base age path, not the
    // caller's tunic-variant path.  Per-tunic paths only exist under "alt/"
    // in packs; loadExact against vanilla OTR on a tunic path returns null.
    auto vanillaRes = resourceMgr->LoadResource(skel.vanillaSkeletonPath, /*loadExact=*/true);
    auto vanillaSkel = std::dynamic_pointer_cast<Skeleton>(vanillaRes);
    if (vanillaSkel == nullptr || vanillaSkel->skeletonData.skeletonHeader.segment == nullptr) {
        return false;
    }
    if (skel.bakedModel != nullptr) {
        skel.retiredBakedModel = std::move(skel.bakedModel);
        skel.retireFrameCounter = 4;  // kRetireFrames
    }
    skel.bakedModel = std::make_unique<BakedPlayerModel>();
    if (!BuildVanillaDummyPlayerModel(vanillaSkel, resourceMgr, *skel.bakedModel)) {
        skel.bakedModel = nullptr;
        return false;
    }
    skel.overrideSkeleton = vanillaSkel;
    skel.skelAnime->skeleton = (void**)skel.bakedModel->segmentPtrs.data();
    uintptr_t skelPtr = (uintptr_t)vanillaSkel->GetPointer();
    memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    return true;
}

// Searches all loaded archives inside coopplayercharacters/<folder>/ for a skeleton
// at altPath.  If found, parses it and patches skel.skelAnime directly, storing the
// shared_ptr in skel.overrideSkeleton so the data stays alive.  Falls back to a
// vanilla bake (via BakeLocalVanillaFallback) if no match is found — keeps the
// local visual clean and prevents #82-style cross-pack bleed-through from the
// global alt-asset lookup.
void SkeletonPatcher::UpdateCustomSkeletonFromFolder(const std::string& skeletonPath, const std::string& folder,
                                                      SkeletonPatchInfo& skel) {
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();

    if (folder.empty()) {
        // Revert to system default (vanilla or user's non-coop alt mod).
        // Leave coop archives mounted — see ClearLocalCoopArchives comment.
        // SetArchives / RemoveArchive both trigger ResetVirtualFileSystem()
        // which unloads+reloads every surviving archive and remaps mmap
        // regions, invalidating any cached resource that holds a raw pointer
        // into archive memory (intermittent white-tunic bug, log 42).
        ClearLocalCoopArchives(resourceMgr);  // now a no-op, retained for call-site clarity
        sLastLoadedCoopFolder = "";
        skel.overrideSkeleton = nullptr;

        // Issue #82 — bake the vanilla skeleton under "vanilla-dummy/..." cache
        // keys so the revert-to-default path also gets a clean render-time
        // resolution. Without this, the previous pack's "coopchar/<folder>/..."
        // entries stay cached and continue to bleed through on limbs whose DLs
        // haven't been touched since the pack switch — identical symptom class
        // to the coop-pack-to-coop-pack case, different surface.
        //
        // Fix 1 (2026-04-20 test 19 regression): LoadResource must use the base
        // age skeleton path (skel.vanillaSkeletonPath) NOT the caller's
        // tunic-variant skeletonPath.  SoH's per-tunic variant paths like
        // gLinkChildKokiriTunicSkel exist only under the "alt/" prefix in packs
        // and don't resolve via loadExact=true against vanilla OTR.  The base
        // path (e.g. gLinkChildSkel) always resolves, and BuildVanillaDummyPlayerModel
        // is pack-agnostic — the bake walk is structurally identical regardless
        // of which age/variant goes in.  Using the tunic path caused vanillaRes
        // to come back null and the whole bake block to be silently skipped.
        //
        // Mirrors the retire-slot lifecycle from the pack-bake path below.
        auto vanillaRes = resourceMgr->LoadResource(skel.vanillaSkeletonPath, /*loadExact=*/true);
        auto vanillaSkel = std::dynamic_pointer_cast<Skeleton>(vanillaRes);
        if (vanillaSkel != nullptr && vanillaSkel->skeletonData.skeletonHeader.segment != nullptr) {
            if (skel.bakedModel != nullptr) {
                skel.retiredBakedModel = std::move(skel.bakedModel);
                skel.retireFrameCounter = 4;  // kRetireFrames
            }
            skel.bakedModel = std::make_unique<BakedPlayerModel>();
            if (BuildVanillaDummyPlayerModel(vanillaSkel, resourceMgr, *skel.bakedModel)) {
                skel.overrideSkeleton = vanillaSkel;  // keep alive for the baked segment's lifetime
                skel.skelAnime->skeleton = (void**)skel.bakedModel->segmentPtrs.data();
                uintptr_t skelPtr = (uintptr_t)vanillaSkel->GetPointer();
                memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
                SPDLOG_INFO("[CoopModel] local player baked vanilla skeleton applied skelAnime={} (limbCount={})",
                            (void*)skel.skelAnime, vanillaSkel->limbCount);
                gfx_texture_cache_clear();
                return;
            }
            // Bake failed — drop the empty slot and fall through to the legacy
            // UpdateCustomSkeletonFromPath resolution.
            skel.bakedModel = nullptr;
            SPDLOG_WARN("[CoopModel] vanilla bake failed for \"{}\", falling through to UpdateCustomSkeletonFromPath",
                        skel.vanillaSkeletonPath);
        } else {
            SPDLOG_WARN("[CoopModel] vanilla LoadResource(\"{}\") returned null — falling through",
                        skel.vanillaSkeletonPath);
        }

        // Fix 2 (2026-04-20 test 19 regression): retire any stale bakedModel
        // BEFORE handing off to UpdateCustomSkeletonFromPath.  The legacy path
        // writes skel.skelAnime->skeleton directly to a vanilla segment pointer
        // (not to bakedModel->segmentPtrs).  If we leave skel.bakedModel alive
        // here, the renderer swings from "baked segment" → "vanilla segment"
        // with no retire barrier, and any Gfx frame mid-flight that referenced
        // the baked pathStrings/bakedDLs can walk into freed memory once the
        // subsequent assignment in the pack-bake path (on the next cosmetic
        // change) destroys it.  Retiring arms the counter so ≥kRetireFrames
        // elapse before destruction.  gfx_texture_cache_clear flushes the
        // F3DZEX2 DL cache — mirror of the successful-bake tail.
        if (skel.bakedModel != nullptr) {
            skel.retiredBakedModel = std::move(skel.bakedModel);
            skel.retireFrameCounter = 4;  // kRetireFrames
        }
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        gfx_texture_cache_clear();
        return;
    }

    // Only clear when switching to a different folder.  If the folder is unchanged
    // (e.g. every DummyPlayer spawn triggers OnLinkSkeletonInit → UpdateCustomSkeletons),
    // the archives are already in the manager — clearing and re-opening them on a
    // VirtualBox shared-folder path costs ~300 ms per call.
    if (folder != sLastLoadedCoopFolder) {
        ClearLocalCoopArchives(resourceMgr);
        sLastLoadedCoopFolder = folder;
    }

    const std::string altPath = Ship::IResource::gAltAssetPrefix + skeletonPath;

    SPDLOG_INFO("[CoopModel] UpdateCustomSkeletonFromFolder: folder=\"{}\" altPath=\"{}\"",
                folder, altPath);

    // Many character packs only ship the base skeleton (gLinkAdultSkel / gLinkChildSkel)
    // rather than the per-tunic variants.  Try the tunic path first; if that fails,
    // retry with the base age skeleton.  Pass trackForLocal=true so the archive is
    // recorded in sLocalCoopArchivePaths for future cleanup.
    std::string resolvedAltPath = altPath;
    auto file = LoadFileFromCoopFolder(folder, altPath, resourceMgr, /*trackForLocal=*/true);
    if (file == nullptr) {
        const std::string baseAltPath = Ship::IResource::gAltAssetPrefix + skel.vanillaSkeletonPath;
        if (baseAltPath != altPath) {
            SPDLOG_INFO("[CoopModel]   tunic path not found, trying base skeleton: {}", baseAltPath);
            file = LoadFileFromCoopFolder(folder, baseAltPath, resourceMgr, /*trackForLocal=*/true);
            if (file != nullptr) {
                resolvedAltPath = baseAltPath;
            }
        }
    }

    if (file == nullptr) {
        // No archive in the selected folder has this skeleton.  Bake vanilla instead
        // of calling UpdateCustomSkeletonFromPath — the legacy path does a global
        // alt-asset lookup that picks up some other loaded mod's child/adult skeleton
        // with no pack-local face-texture bake (issue: 3DS Malon / Linkle / DarkMalon
        // adult-only packs ending up with cross-mod eye/mouth textures).
        SPDLOG_WARN("[CoopModel]   no archive in folder \"{}\" contains \"{}\" or base skeleton — baking vanilla fallback",
                    folder, altPath);
        skel.overrideSkeleton = nullptr;
        if (BakeLocalVanillaFallback(skel, resourceMgr)) {
            gfx_texture_cache_clear();
            return;
        }
        // Vanilla bake unavailable — last-resort legacy resolution.  Retire any stale
        // bakedModel first so UpdateCustomSkeletonFromPath's raw segment assignment
        // can't leave dangling pointers into in-flight Gfx frames.
        if (skel.bakedModel != nullptr) {
            skel.retiredBakedModel = std::move(skel.bakedModel);
            skel.retireFrameCounter = 4;  // kRetireFrames
        }
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        gfx_texture_cache_clear();
        return;
    }

    auto resourceLoader = resourceMgr->GetResourceLoader();
    // Use a folder-specific cache key to avoid cross-pack resource cache collisions:
    // both packs use the same altPath (e.g. alt/objects/object_link_child/gLinkChildSkel),
    // so using altPath directly as the key would return the first pack's skeleton for all
    // subsequent packs.  The "coopchar/<folder>/" prefix gives each folder its own entry.
    const std::string cacheKey = "coopchar/" + folder + "/" + resolvedAltPath;
    auto resource = resourceLoader->LoadResource(cacheKey, file);
    if (resource == nullptr) {
        SPDLOG_WARN("[CoopModel]   LoadResource failed for \"{}\" — baking vanilla fallback",
                    resolvedAltPath);
        skel.overrideSkeleton = nullptr;
        if (BakeLocalVanillaFallback(skel, resourceMgr)) {
            gfx_texture_cache_clear();
            return;
        }
        if (skel.bakedModel != nullptr) {
            skel.retiredBakedModel = std::move(skel.bakedModel);
            skel.retireFrameCounter = 4;
        }
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        gfx_texture_cache_clear();
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        SPDLOG_WARN("[CoopModel]   resource is not a Skeleton for \"{}\" — baking vanilla fallback",
                    resolvedAltPath);
        skel.overrideSkeleton = nullptr;
        if (BakeLocalVanillaFallback(skel, resourceMgr)) {
            gfx_texture_cache_clear();
            return;
        }
        if (skel.bakedModel != nullptr) {
            skel.retiredBakedModel = std::move(skel.bakedModel);
            skel.retireFrameCounter = 4;
        }
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        gfx_texture_cache_clear();
        return;
    }

    // Keep the skeleton alive for as long as this SkeletonPatchInfo exists.
    skel.overrideSkeleton = skeleton;

    // Issue #82 — bake the local player's new skeleton under pack-unique
    // "coopchar/<folder>/..." cache keys. Without baking, the raw OTR paths on
    // each limb's dList[0] collide across packs in the global DL cache (every
    // pack uses the same "alt/objects/object_link_child/gLinkChildSkel*" paths),
    // so the renderer keeps serving the previous pack's limbs until a scene
    // change evicts the cache.
    //
    // This mirrors the DummyPlayer bake path in ApplyCustomSkeletonToDummyPlayer.
    // The tracked archive registration from LoadFileFromCoopFolder above stays
    // intact — OpenCoopPackArchives opens a transient second handle just for
    // this bake walk and discards it.
    //
    // Retire-slot (KB-15 / #110): the outgoing bakedModel cannot be destroyed
    // synchronously. The last-submitted Gfx frame still holds raw c_str()
    // pointers into its pathStrings and data() pointers into its bakedDLs.
    // Move it into retiredBakedModel; OnGameFrameUpdate ticks retireFrameCounter
    // down and destroys it when the renderer has moved past those frames.
    if (skel.bakedModel != nullptr) {
        skel.retiredBakedModel = std::move(skel.bakedModel);
        skel.retireFrameCounter = 4;  // kRetireFrames — matches Anchor.h
    }
    skel.bakedModel = std::make_unique<BakedPlayerModel>();

    auto bakeArchive = OpenCoopPackArchives(folder, resolvedAltPath);
    if (bakeArchive.IsLoaded() && BuildBakedPlayerModel(skeleton, folder, bakeArchive, resourceLoader, *skel.bakedModel)) {
        SPDLOG_INFO("[CoopModel] local player baked skeleton applied skelAnime={} (limbCount={})",
                    (void*)skel.skelAnime, skeleton->limbCount);
        skel.skelAnime->skeleton = (void**)skel.bakedModel->segmentPtrs.data();
        uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
        memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
        // Intentionally NOT writing skelAnime->limbCount / ->dListCount here.
        // Same rationale as ApplyCustomSkeletonToDummyPlayer's success path:
        // overwriting them broke Guard 4 on subsequent pack applications (Coop
        // Test 19 regression).
    } else {
        // Bake failed — fall back to the pre-#82 shared-segment apply. User
        // will see the visual-stale symptom until scene change (original #82
        // behaviour), but rendering is safe.
        skel.bakedModel = nullptr;
        SPDLOG_WARN("[CoopModel] local player bake failed, fell back to shared segment for folder=\"{}\" — "
                    "visual will be stale until scene change (see #82)",
                    folder);
        SPDLOG_INFO("[CoopModel]   skeleton applied to local player skelAnime={} segment: {} -> {}",
                    (void*)skel.skelAnime,
                    (void*)skel.skelAnime->skeleton,
                    (void*)skeleton->skeletonData.skeletonHeader.segment);
        skel.skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
        uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
        memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    }

    // Flush the F3DZEX2 display list cache so the renderer re-uploads the new skeleton's
    // display lists on the next frame.  Without this, the renderer keeps serving cached
    // commands from the OLD skeleton — the visual update only appears after a scene change.
    // This is the same call made in OTRGlobals.cpp when the alt-assets CVar toggles.
    gfx_texture_cache_clear();
}

// ---------------------------------------------------------------------------
// Pre-baked DL helpers
// ---------------------------------------------------------------------------

// Opens every .otr/.o2r archive under coopplayercharacters/<folder>/ transiently
// (NOT via ArchiveManager), with the probe-matching archive guaranteed to be LAST
// in the returned vector.  Placement matters: CoopPackArchives::LoadFile iterates
// in reverse, so the probe archive takes lookup priority, but when a path is
// missing from the probe archive the bake walker still finds it in a sibling
// (e.g. DarkMalonChildTextures.otr's full-res _00_BaseColor_Merged wins over
// DarkMalonChild.otr's low-res placeholder).
//
// Recursive so packs that nest archives in per-age subfolders (e.g.
// DarkMalon/DarkMalonChild/DarkMalonChild.otr) are still discovered.
static CoopPackArchives OpenCoopPackArchives(const std::string& folder,
                                              const std::string& probePath) {
    CoopPackArchives out;
    const std::string coopRoot = Ship::Context::LocateFileAcrossAppDirs("coopplayercharacters", appShortName);
    const std::filesystem::path charDir = std::filesystem::path(coopRoot) / folder;

    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
        return out;
    }

    std::shared_ptr<Ship::Archive> probeArchive;
    std::string probeArchivePath;
    std::vector<std::shared_ptr<Ship::Archive>> siblings;

    // Reuse archives already held by ArchiveManager whenever possible.  Opening
    // a transient second OtrArchive against the same MPQ file creates a parallel
    // StormLib HMPQ handle; when the transient is destroyed (the bake returns)
    // the destructor closes that handle and can invalidate the persistent copy's
    // internal file tables — which is what caused the DummyPlayer sync failure in
    // log 40 (archive->LoadFile returned nullptr on a previously-working archive)
    // and the white-tunic regression on Default Link.  The shared_ptr from
    // ArchiveManager lives as long as the manager holds it, so we just ride along.
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    auto archiveManager = resourceMgr->GetArchiveManager();
    auto getExistingArchive = [&](const std::string& archivePath) -> std::shared_ptr<Ship::Archive> {
        auto loaded = archiveManager->GetArchives();
        if (!loaded) return nullptr;
        for (auto& a : *loaded) {
            if (a->GetPath() == archivePath) return a;
        }
        return nullptr;
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator(charDir)) {
        if (entry.is_directory()) continue;
        const std::string ext = entry.path().extension().generic_string();
        if (ext != ".otr" && ext != ".o2r") continue;

        const std::string archivePath = entry.path().generic_string();

        std::shared_ptr<Ship::Archive> archive = getExistingArchive(archivePath);
        bool fromManager = (archive != nullptr);
        if (!archive) {
            // Not in ArchiveManager — safe to transient-open.  This happens on
            // the DummyPlayer path (trackForLocal=false in LoadFileFromCoopFolder
            // doesn't register the probe), and for sibling archives the local
            // LoadFileFromCoopFolder path doesn't eagerly register.
            if (ext == ".o2r") {
                archive = std::make_shared<Ship::O2rArchive>(archivePath);
            } else {
                archive = std::make_shared<Ship::OtrArchive>(archivePath);
            }
            archive->Load();
            if (!archive->IsLoaded()) continue;
        }
        if (!probeArchive && archive->LoadFile(probePath) != nullptr) {
            probeArchive = archive;
            probeArchivePath = archivePath;
            SPDLOG_INFO("[CoopModel][Bake]   Probe archive ({}): \"{}\"",
                        fromManager ? "reused from manager" : "transient", archivePath);
        } else {
            siblings.push_back(archive);
            SPDLOG_INFO("[CoopModel][Bake]   Sibling pack archive ({}): \"{}\"",
                        fromManager ? "reused from manager" : "transient", archivePath);
        }
    }

    if (!probeArchive) {
        SPDLOG_ERROR("[CoopModel][Bake] No archive in folder \"{}\" contains probe \"{}\"",
                     folder, probePath);
        return out;
    }

    // Probe archive FIRST, siblings AFTER so reverse-iteration (used by
    // CoopPackArchives::LoadFile) tries siblings first.  This matches the
    // "supplement overrides base" convention for packs that ship a low-res
    // placeholder in the skel archive (e.g. DarkMalonChild.otr's 2 KB
    // _00_BaseColor_Merged) alongside a full-res sibling archive
    // (DarkMalonChildTextures.otr's 40 KB version) — we want the full-res
    // to win whenever both contain the same path.  Paths that only exist in
    // the probe archive still resolve via the fallthrough.
    out.archives.push_back(probeArchive);
    for (auto& s : siblings) {
        out.archives.push_back(s);
    }
    SPDLOG_INFO("[CoopModel][Bake] Probe archive: \"{}\" (contains probe \"{}\"); {} sibling(s)",
                probeArchivePath, probePath, (int)siblings.size());
    return out;
}

// Per-bake counters that tell us, at a glance, how self-contained a pack is
// and whether the bake is relying heavily on vanilla inheritance.  Logged
// once per BuildBakedPlayerModel call (across all limbs and recursive sub-DLs).
//
// "pack":      the referenced file was found in the pack archive and preloaded
//              under a "coopchar/<folder>/..." key (isolated, pack-local).
// "inherited": the file was not in the pack; we kept the original OTR path
//              (or ArchiveManager::HashToCString fallback) so render-time
//              resolution falls through to vanilla / other archives.
// "sentinel":  path could not be resolved anywhere at bake time — a sentinel
//              string was emitted.  Render-time null checks handle the miss.
struct BakeStats {
    int vtxPack = 0;      int vtxInherited = 0;
    int subdlPack = 0;    int subdlInherited = 0;
    int texPack = 0;      int texInherited = 0;    int texSentinel = 0;
};

// Recursively bakes a single display list from the archive.
// Returns pointer to the baked Gfx array (owned by model.bakedDLs), or nullptr.
static Gfx* BakeDL(
    const std::string& dlPath,
    const std::string& folder,
    const CoopPackArchives& archive,
    const std::unordered_map<uint64_t, std::string>& hashMap,
    std::shared_ptr<Ship::ResourceLoader>& loader,
    BakeStats& stats,
    BakedPlayerModel& model,
    std::unordered_map<std::string, Gfx*>& dlCache)
{
    // Guard against infinite recursion / repeated baking
    auto cacheIt = dlCache.find(dlPath);
    if (cacheIt != dlCache.end()) {
        return cacheIt->second;
    }
    // Mark in-progress with nullptr to break cycles
    dlCache[dlPath] = nullptr;

    // Load and parse the DL from the archive.
    // Character pack archives store resources with "alt/" prefix; try both bare and prefixed paths.
    auto file = archive.LoadFile(dlPath);
    std::string resolvedDlPath = dlPath;
    if (!file) {
        const std::string altDlPath = Ship::IResource::gAltAssetPrefix + dlPath;
        file = archive.LoadFile(altDlPath);
        if (file) {
            resolvedDlPath = altDlPath;
        }
    }
    if (!file) {
        SPDLOG_WARN("[CoopModel][Bake] BakeDL: file not found in archive: \"{}\"", dlPath);
        return nullptr;
    }
    const std::string cacheKey = "coopchar/" + folder + "/" + resolvedDlPath;
    auto resource = loader->LoadResource(cacheKey, file);
    // Inject into ResourceManager cache so render-time GetResourceRawPointer(key)
    // finds it.  Without this, loader->LoadResource only transforms the raw bytes
    // into a resource object without populating the cache, and render-time lookup
    // falls through to archive search → NotFound → null (Coop Test 15 symptom).
    Ship::Context::GetInstance()->GetResourceManager()->SetCachedResource(cacheKey, resource);
    auto dl = std::dynamic_pointer_cast<Fast::DisplayList>(resource);
    if (!dl) {
        SPDLOG_WARN("[CoopModel][Bake] BakeDL: not a DisplayList: \"{}\"", dlPath);
        return nullptr;
    }

    // Deep-copy the instructions; we will patch them in place
    std::vector<Gfx> baked = dl->Instructions;

    for (size_t i = 0; i < baked.size(); i++) {
        const uint8_t op = (uint8_t)((baked[i].words.w0 >> 24) & 0xFF);

        // For all FILEPATH/HASH branches below: a referenced file missing from the
        // pack archive is expected (packs ship only assets they change and inherit
        // the rest from vanilla).  Rewriting w1 to a "coopchar/..." key without
        // having actually loaded the resource produces a cache miss at render time;
        // for vertex lookups that miss crashes GfxSpVertex on a null pointer.
        // On fallthrough we leave the original OTR opcode intact so the interpreter
        // resolves through the global archive cache — and we always push the string
        // onto model.pathStrings so the w1 char* is owned by the BakedPlayerModel
        // (the source DisplayList resource may be evicted from the cache).

        // ── FILEPATH texture ──────────────────────────────────────────────
        // Packs typically ship overrides under "alt/<path>" (the gAltAssetPrefix).
        // Try bare first, then alt-prefixed — same pattern as the outer DL load.
        if (op == (uint8_t)Fast::OTR_G_SETTIMG_OTR_FILEPATH) {
            const char* origPath = (const char*)baked[i].words.w1;
            auto texFile = archive.LoadFile(origPath);
            std::string resolvedPath = origPath;
            if (!texFile) {
                const std::string altPath = Ship::IResource::gAltAssetPrefix + origPath;
                texFile = archive.LoadFile(altPath);
                if (texFile) resolvedPath = altPath;
            }
            if (texFile) {
                const std::string uniqueKey = "coopchar/" + folder + "/" + resolvedPath;
                auto texRes = loader->LoadResource(uniqueKey, texFile);
                Ship::Context::GetInstance()->GetResourceManager()->SetCachedResource(uniqueKey, texRes);
                model.pathStrings.push_back(uniqueKey);
                stats.texPack++;
            } else {
                model.pathStrings.push_back(origPath);  // keep OTR op, stabilize string
                stats.texInherited++;
            }
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
        }
        // ── HASH texture ──────────────────────────────────────────────────
        //
        // ALWAYS convert HASH → FILEPATH unconditionally.  Upstream
        // `gfx_set_timg_otr_hash_handler_custom` (libultraship
        // interpreter.cpp:3438) mis-advances the instruction pointer on both
        // resource-miss paths (extra `(*cmd0)++` at L3448 when fileName is null,
        // and L3497 when texture is null), skipping one real instruction and
        // causing "Unhandled OP code" crashes when the skipped word's bytes are
        // read as an opcode.  The FILEPATH handler is miss-safe — it logs the
        // failure and returns without advancing.
        //
        // Baked stride stays 2 Gfx: FILEPATH (1-Gfx) + zeroed Gfx (opcode 0 =
        // G_NOOP) preserves the HASH instruction's original width so indices
        // around this instruction remain correct for subsequent ops.
        else if (op == (uint8_t)Fast::OTR_G_SETTIMG_OTR_HASH) {
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;

            std::string pathStr;
            auto hashIt = hashMap.find(hash);
            if (hashIt != hashMap.end()) {
                pathStr = hashIt->second;
                // Best-effort: pre-load into the resource cache under a
                // pack-unique key to avoid cross-pack cache collisions.
                auto texFile = archive.LoadFile(pathStr);
                if (texFile) {
                    const std::string uniqueKey = "coopchar/" + folder + "/" + pathStr;
                    auto texRes = loader->LoadResource(uniqueKey, texFile);
                    Ship::Context::GetInstance()->GetResourceManager()->SetCachedResource(uniqueKey, texRes);
                    pathStr = uniqueKey;
                    stats.texPack++;
                } else {
                    stats.texInherited++;
                }
            } else {
                // Hash not in this pack's file list — ask the global
                // ArchiveManager for the path so the FILEPATH handler can try
                // vanilla / other-pack resolution at render time.
                auto archiveMgr =
                    Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
                const char* globalPath = archiveMgr->HashToCString(hash);
                if (globalPath != nullptr) {
                    pathStr = globalPath;
                    stats.texInherited++;
                } else {
                    pathStr = "__coopbake_unresolved_hash__";
                    stats.texSentinel++;
                }
            }

            model.pathStrings.push_back(pathStr);
            baked[i].words.w0 = (baked[i].words.w0 & 0x00FFFFFF)
                              | ((uintptr_t)(uint8_t)Fast::OTR_G_SETTIMG_OTR_FILEPATH << 24);
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
            baked[i+1].words.w0 = 0;  // G_NOOP padding — preserves 2-Gfx stride
            baked[i+1].words.w1 = 0;
            i++;  // skip hash word
        }
        // ── FILEPATH vertex ───────────────────────────────────────────────
        //
        // Two-tier resolution at bake time:
        //   1. Pack has it     → preload under "coopchar/<folder>/<path>" (isolated)
        //   2. Pack miss       → keep origPath; resolves at render via the global
        //                        ArchiveManager alt-asset fallback (vanilla inheritance)
        //
        // Note: we do NOT neuter on a probe miss here.  The pack's DLs legitimately
        // reference vanilla-stored vertex arrays (e.g. "objects/object_link_boy/..."
        // — the pack overrides the DL but inherits vtx data from vanilla).  An
        // eager bake-time probe (ArchiveManager::HasFile) is too strict: vanilla
        // resolution uses alt-asset/loader logic that HasFile does not replicate,
        // so a false-negative would wrongly drop healthy vertex batches, leaving
        // the DummyPlayer rendering against stale GPU vertex cache (Coop Test 13
        // scramble symptom).
        //
        // If a path is genuinely unresolvable at render time,
        // gfx_vtx_otr_filepath_handler_custom's null check (interpreter.cpp)
        // logs and skips the op safely — no crash, single missing batch.
        else if (op == (uint8_t)Fast::OTR_G_VTX_OTR_FILEPATH) {
            // Packs typically ship overrides under "alt/<path>" (the gAltAssetPrefix).
            // Try bare first, then alt-prefixed — same pattern as the outer DL load.
            // Without this, the bake resolves every vtx via render-time alt-asset
            // fallback, which on a multi-pack setup returns the LOCAL player's
            // vertex data for a REMOTE DummyPlayer — Coop Test 14 "exploded" symptom.
            const char* origPath = (const char*)baked[i].words.w1;
            auto vtxFile = archive.LoadFile(origPath);
            std::string resolvedPath = origPath;
            if (!vtxFile) {
                const std::string altPath = Ship::IResource::gAltAssetPrefix + origPath;
                vtxFile = archive.LoadFile(altPath);
                if (vtxFile) resolvedPath = altPath;
            }
            if (vtxFile) {
                const std::string uniqueKey = "coopchar/" + folder + "/" + resolvedPath;
                auto vtxRes = loader->LoadResource(uniqueKey, vtxFile);
                Ship::Context::GetInstance()->GetResourceManager()->SetCachedResource(uniqueKey, vtxRes);
                model.pathStrings.push_back(uniqueKey);
                stats.vtxPack++;
            } else {
                model.pathStrings.push_back(origPath);
                stats.vtxInherited++;
            }
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
            i++;  // skip vtx-params word [i+1]
        }
        // ── HASH vertex ───────────────────────────────────────────────────
        else if (op == (uint8_t)Fast::OTR_G_VTX_OTR_HASH) {
            const uint32_t vtxW0    = (uint32_t)baked[i].words.w0;
            const uint32_t vtxCnt   = (vtxW0 >> 12) & 0xFF;
            const uint32_t vtxIdxOff = ((vtxW0 >> 1) & 0x7F) - vtxCnt;
            const uint32_t byteOffset = (uint32_t)baked[i].words.w1;
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;
            auto hashIt = hashMap.find(hash);
            if (hashIt != hashMap.end()) {
                auto vtxFile = archive.LoadFile(hashIt->second);
                if (vtxFile) {
                    const std::string uniqueKey = "coopchar/" + folder + "/" + hashIt->second;
                    auto vtxRes = loader->LoadResource(uniqueKey, vtxFile);
                    Ship::Context::GetInstance()->GetResourceManager()->SetCachedResource(uniqueKey, vtxRes);
                    model.pathStrings.push_back(uniqueKey);
                    // Convert HASH → FILEPATH (vtxDataOff is byteOffset / sizeof F3DVtx)
                    baked[i].words.w0 = (uintptr_t)(uint8_t)Fast::OTR_G_VTX_OTR_FILEPATH << 24;
                    baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
                    baked[i+1].words.w0 = (uintptr_t)vtxCnt;
                    baked[i+1].words.w1 = (uintptr_t)((vtxIdxOff << 16) | (byteOffset / 16));
                    stats.vtxPack++;
                } else {
                    stats.vtxInherited++;  // left HASH op intact
                }
            } else {
                stats.vtxInherited++;  // left HASH op intact
            }
            i++;  // skip hash word
        }
        // ── FILEPATH sub-DL ───────────────────────────────────────────────
        else if (op == (uint8_t)Fast::OTR_G_DL_OTR_FILEPATH) {
            const char* origPath = (const char*)baked[i].words.w1;
            Gfx* subBaked = BakeDL(origPath, folder, archive, hashMap, loader, stats, model, dlCache);
            if (subBaked) {
                const uintptr_t pushBit = (baked[i].words.w0 >> 16) & 1;
                baked[i].words.w0 = ((uintptr_t)(uint8_t)0xDE << 24) | (pushBit << 16);
                baked[i].words.w1 = (uintptr_t)subBaked;
                stats.subdlPack++;
            } else {
                model.pathStrings.push_back(origPath);  // keep OTR op, stabilize string
                baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
                stats.subdlInherited++;
            }
        }
        // ── HASH sub-DL ───────────────────────────────────────────────────
        else if (op == (uint8_t)Fast::OTR_G_DL_OTR_HASH) {
            const uintptr_t pushBit = (baked[i].words.w0 >> 16) & 1;
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;
            auto hashIt = hashMap.find(hash);
            if (hashIt != hashMap.end()) {
                Gfx* subBaked = BakeDL(hashIt->second, folder, archive, hashMap, loader, stats, model, dlCache);
                if (subBaked) {
                    baked[i].words.w0 = ((uintptr_t)(uint8_t)0xDE << 24) | (pushBit << 16);
                    baked[i].words.w1 = (uintptr_t)subBaked;
                    baked[i+1].words.w0 = 0;
                    baked[i+1].words.w1 = 0;
                    stats.subdlPack++;
                } else {
                    stats.subdlInherited++;
                }
            } else {
                stats.subdlInherited++;
            }
            i++;  // skip hash word
        }
        // ── 2-word commands with no path reference: skip second word ──────
        else if (op == (uint8_t)Fast::OTR_G_MARKER       ||
                 op == (uint8_t)Fast::OTR_G_BRANCH_Z_OTR ||
                 op == (uint8_t)Fast::OTR_G_MTX_OTR      ||
                 op == (uint8_t)Fast::OTR_G_MOVEMEM_HASH) {
            i++;
        }
        // All other opcodes (standard GBI, RDP, MTX_OTR_FILEPATH, etc.) pass through
    }

    model.bakedDLs.push_back(std::move(baked));
    Gfx* ptr = model.bakedDLs.back().data();
    dlCache[dlPath] = ptr;
    return ptr;
}

// Pre-loads any face-texture overrides that this pack ships into the resource
// cache under pack-unique "coopchar/<folder>/..." keys and records the keys on
// outModel.eyeTexKeys / mouthTexKeys.
//
// Background: Player_DrawImpl binds the active eye/mouth texture at runtime via
// gSPSegment(0x08/0x09, sEyeTextures[age][idx]) (z_player_lib.c:1050/1062).  The
// char* stored in each slot is a bare OTR path string (e.g. gLinkAdultEyesOpenTex).
// Those bindings are not inside any DL that BakeDL walks — they're generated
// per-frame into POLY_OPA_DISP — so the face G_SETTIMG resolves through the global
// ArchiveManager alt-asset lookup, which on a multi-pack setup wins with the
// LOCAL player's pack (Coop Test 16 residual).
//
// Fix: at bake time, probe each of the 12 known face paths (per age) against the
// pack archive with the same bare→"alt/<path>" fallback pattern used elsewhere.
// On hit, preload the resource under "coopchar/<folder>/<resolvedPath>" and store
// that key.  DummyPlayer_Draw swaps the populated slots into sEyeTextures/
// sMouthTextures around its Player_Draw call, then restores.
//
// On miss we leave the key empty — the swap falls through to the saved original,
// so packs that only override the base geometry continue to work (with the same
// acceptable cross-pack bleed as the non-face miss case).
static void BakeFaceTextures(
    const std::string& folder,
    const CoopPackArchives& archive,
    std::shared_ptr<Ship::ResourceLoader>& loader,
    BakedPlayerModel& model)
{
    // [age][slot] → OTR-relative path (no "__OTR__" prefix; BakeDL strips it too).
    // Empty slots (none here) would be skipped.
    static const char* kEyePaths[2][8] = {
        { // adult (age 0)
            "objects/object_link_boy/gLinkAdultEyesOpenTex",
            "objects/object_link_boy/gLinkAdultEyesHalfTex",
            "objects/object_link_boy/gLinkAdultEyesClosedfTex",
            "objects/object_link_boy/gLinkAdultEyesRollLeftTex",
            "objects/object_link_boy/gLinkAdultEyesRollRightTex",
            "objects/object_link_boy/gLinkAdultEyesShockTex",
            "objects/object_link_boy/gLinkAdultEyesUnk1Tex",
            "objects/object_link_boy/gLinkAdultEyesUnk2Tex",
        },
        { // child (age 1)
            "objects/object_link_child/gLinkChildEyesOpenTex",
            "objects/object_link_child/gLinkChildEyesHalfTex",
            "objects/object_link_child/gLinkChildEyesClosedfTex",
            "objects/object_link_child/gLinkChildEyesRollLeftTex",
            "objects/object_link_child/gLinkChildEyesRollRightTex",
            "objects/object_link_child/gLinkChildEyesShockTex",
            "objects/object_link_child/gLinkChildEyesUnk1Tex",
            "objects/object_link_child/gLinkChildEyesUnk2Tex",
        },
    };
    static const char* kMouthPaths[2][4] = {
        { // adult
            "objects/object_link_boy/gLinkAdultMouth1Tex",
            "objects/object_link_boy/gLinkAdultMouth2Tex",
            "objects/object_link_boy/gLinkAdultMouth3Tex",
            "objects/object_link_boy/gLinkAdultMouth4Tex",
        },
        { // child
            "objects/object_link_child/gLinkChildMouth1Tex",
            "objects/object_link_child/gLinkChildMouth2Tex",
            "objects/object_link_child/gLinkChildMouth3Tex",
            "objects/object_link_child/gLinkChildMouth4Tex",
        },
    };

    auto resMgr = Ship::Context::GetInstance()->GetResourceManager();
    int ageHits[2] = { 0, 0 };   // adult / child total hits (eye + mouth)
    int eyeHits[2] = { 0, 0 };
    int mouthHits[2] = { 0, 0 };

    auto tryBake = [&](const char* origPath, std::string& outKey, int age, int* ageCounter, int* slotCounter) {
        auto file = archive.LoadFile(origPath);
        std::string resolvedPath = origPath;
        if (!file) {
            const std::string altPath = Ship::IResource::gAltAssetPrefix + origPath;
            file = archive.LoadFile(altPath);
            if (file) resolvedPath = altPath;
        }
        if (!file) {
            return; // pack doesn't ship this variant — leave outKey empty
        }
        const std::string uniqueKey = "coopchar/" + folder + "/" + resolvedPath;
        auto res = loader->LoadResource(uniqueKey, file);
        if (res) {
            resMgr->SetCachedResource(uniqueKey, res);
            // Prefix with "__OTR__" so the segmented G_SETTIMG path in the
            // renderer (gfx_set_timg_handler_rdp → gfx_check_image_signature →
            // ResourceManager::OtrSignatureCheck) recognises the swapped
            // sEyeTextures/sMouthTextures pointer as an OTR resource path
            // rather than raw pixel data.  LoadResourceProcess strips the
            // 7-byte prefix before the cache lookup, so the resource remains
            // cached under the bare "coopchar/..." key used everywhere else.
            outKey = "__OTR__" + uniqueKey;
            ageHits[age]++;
            (*slotCounter)++;
        }
    };

    for (int age = 0; age < 2; age++) {
        for (int i = 0; i < 8; i++) tryBake(kEyePaths[age][i],   model.eyeTexKeys[age][i],   age, ageHits, &eyeHits[age]);
        for (int i = 0; i < 4; i++) tryBake(kMouthPaths[age][i], model.mouthTexKeys[age][i], age, ageHits, &mouthHits[age]);
    }

    SPDLOG_INFO("[CoopModel][Bake] folder=\"{}\" face textures: {}/24 pack overrides "
                "(adult: eye={}/8 mouth={}/4; child: eye={}/8 mouth={}/4)",
                folder, ageHits[0] + ageHits[1],
                eyeHits[0], mouthHits[0], eyeHits[1], mouthHits[1]);
}

// Builds a BakedPlayerModel for the given skeleton loaded from the pack archive.
// Returns true on success.
static bool BuildBakedPlayerModel(
    const std::shared_ptr<Skeleton>& skeleton,
    const std::string& folder,
    const CoopPackArchives& archive,
    std::shared_ptr<Ship::ResourceLoader>& loader,
    BakedPlayerModel& outModel)
{
    const int limbCount = skeleton->limbCount;
    outModel.limbCopies.resize(limbCount);
    outModel.segmentPtrs.resize(limbCount);

    // Build hash→path map across ALL archives in the pack bundle.  Iterate
    // front-to-back so later archives (siblings) overwrite probe entries —
    // matches the LoadFile reverse-iteration priority.
    std::unordered_map<uint64_t, std::string> hashMap;
    int altPrefixed = 0;
    int bareObjects = 0;
    int other = 0;
    std::vector<std::string> samples;
    samples.reserve(6);
    for (auto& a : archive.archives) {
        auto files = a->ListFiles();
        if (!files) continue;
        for (auto& [hash, path] : *files) {
            hashMap[hash] = path;
            if (path.rfind("alt/", 0) == 0) altPrefixed++;
            else if (path.rfind("objects/", 0) == 0) bareObjects++;
            else other++;
            if (samples.size() < 6) samples.push_back(path);
        }
    }
    // Pack manifest diagnostic: tells us at a glance whether the pack ships
    // overrides under "alt/" (expected), bare "objects/" (unusual), or another
    // scheme entirely.  If vtxPack stays at 0 in the bake stats, the sample
    // paths reveal what prefix the pack actually uses.
    {
        std::string sampleStr;
        for (size_t k = 0; k < samples.size(); k++) {
            sampleStr += samples[k];
            if (k + 1 < samples.size()) sampleStr += ", ";
        }
        SPDLOG_INFO("[CoopModel][Bake] folder=\"{}\" pack manifest: {} files "
                    "(alt/={}, objects/={}, other={}); samples: {}",
                    folder, hashMap.size(), altPrefixed, bareObjects, other, sampleStr);
    }

    // Per-DL cache to avoid baking the same DL twice (also breaks cycles)
    std::unordered_map<std::string, Gfx*> dlCache;

    // Per-bake resource-source counters (see BakeStats declaration).
    BakeStats stats;

    // Count limbs whose resource actually loaded from the archive.  If this stays
    // at zero, the archive is the wrong one (e.g. 3ds_link.otr opened when the
    // child skeleton's limbs live in 3ds_young_link.otr) and we should refuse
    // to apply a zombie skeleton.
    int limbLoadedCount = 0;

    for (int i = 0; i < limbCount; i++) {
        // segmentPtrs[i] = pointer to this limb's LodLimb — set before early-continues
        // so the skeleton segment array is populated even if this limb has no DL
        outModel.segmentPtrs[i] = &outModel.limbCopies[i];

        if (i >= (int)skeleton->limbTable.size()) {
            SPDLOG_WARN("[CoopModel][Bake] limbTable shorter than limbCount at index {}", i);
            continue;
        }

        const std::string& limbPath = skeleton->limbTable[i];

        // Load the limb resource from the archive.
        // Character pack archives store resources with "alt/" prefix; try both bare and prefixed paths.
        auto limbFile = archive.LoadFile(limbPath);
        std::string resolvedLimbPath = limbPath;
        if (!limbFile) {
            const std::string altLimbPath = Ship::IResource::gAltAssetPrefix + limbPath;
            limbFile = archive.LoadFile(altLimbPath);
            if (limbFile) {
                resolvedLimbPath = altLimbPath;
            }
        }
        if (!limbFile) {
            SPDLOG_WARN("[CoopModel][Bake] limb not found in archive: \"{}\" or alt/ variant", limbPath);
            // Copy hierarchy from vanilla limb (populated by SkeletonFactory via global ResourceManager)
            // to prevent zero-initialized child/sibling fields from causing infinite recursion in
            // SkelAnime_DrawFlexLimb (limb 0 with child=0 loops forever → stack overflow).
            {
                void** limbPtrs = (void**)skeleton->skeletonData.skeletonHeader.segment;
                if (limbPtrs && limbPtrs[i]) {
                    LodLimb* vanillaLimb = (LodLimb*)limbPtrs[i];
                    outModel.limbCopies[i].jointPos = vanillaLimb->jointPos;
                    outModel.limbCopies[i].child    = vanillaLimb->child;
                    outModel.limbCopies[i].sibling  = vanillaLimb->sibling;
                }
            }
            continue;
        }
        const std::string limbKey = "coopchar/" + folder + "/" + resolvedLimbPath;
        auto limbResource = loader->LoadResource(limbKey, limbFile);
        auto skeletonLimb = std::dynamic_pointer_cast<SkeletonLimb>(limbResource);
        if (!skeletonLimb) {
            SPDLOG_WARN("[CoopModel][Bake] limb resource not a SkeletonLimb: \"{}\"", limbPath);
            continue;
        }
        limbLoadedCount++;

        // Hierarchy fields (jointPos / child / sibling) come from the Skeleton's
        // pre-built segment[] array, NOT from the per-limb SkeletonLimb resource.
        //
        // The SkeletonLimb factory and the Skeleton factory encode these fields
        // differently — per-limb resources can use different sentinels or
        // relative-vs-absolute indexing than the Skeleton's post-processed
        // segment[].  Reading from SkeletonLimb produced cyclic hierarchies on
        // some packs, causing SkelAnime_DrawFlexLimb to recurse forever and
        // stack-overflow (Coop Test 11 retest logs 106/107).
        //
        // The non-baked fallback path in ApplyCustomSkeletonToDummyPlayer reads
        // from segment[] directly and is known to work.  This bake path now does
        // the same.
        {
            void** limbPtrs = (void**)skeleton->skeletonData.skeletonHeader.segment;
            if (limbPtrs && limbPtrs[i]) {
                LodLimb* srcLimb = (LodLimb*)limbPtrs[i];
                outModel.limbCopies[i].jointPos = srcLimb->jointPos;
                outModel.limbCopies[i].child    = srcLimb->child;
                outModel.limbCopies[i].sibling  = srcLimb->sibling;
            } else {
                SPDLOG_WARN("[CoopModel][Bake] skeleton segment[{}] is null; zero-initialising hierarchy",
                            i);
                outModel.limbCopies[i].child   = 0xFF; // LIMB_DONE
                outModel.limbCopies[i].sibling = 0xFF;
            }
        }
        outModel.limbCopies[i].dLists[0] = nullptr;
        outModel.limbCopies[i].dLists[1] = nullptr;

        // Bake near dList (dLists[0])
        // dListPtr has "__OTR__" prefix added by SkeletonLimbFactory; strip it.
        if (!skeletonLimb->dListPtr.empty()) {
            const std::string sOtr = "__OTR__";
            const std::string dlPath = skeletonLimb->dListPtr.starts_with(sOtr)
                ? skeletonLimb->dListPtr.substr(sOtr.size())
                : skeletonLimb->dListPtr;
            Gfx* baked = BakeDL(dlPath, folder, archive, hashMap, loader, stats, outModel, dlCache);
            if (baked) {
                outModel.limbCopies[i].dLists[0] = baked;
            }
        }
        // dLists[1] (LOD far) — bake if present
        if (!skeletonLimb->dList2Ptr.empty()) {
            const std::string sOtr = "__OTR__";
            const std::string dlPath = skeletonLimb->dList2Ptr.starts_with(sOtr)
                ? skeletonLimb->dList2Ptr.substr(sOtr.size())
                : skeletonLimb->dList2Ptr;
            Gfx* baked = BakeDL(dlPath, folder, archive, hashMap, loader, stats, outModel, dlCache);
            if (baked) {
                outModel.limbCopies[i].dLists[1] = baked;
            }
        }
    }

    if (limbLoadedCount == 0) {
        SPDLOG_ERROR("[CoopModel][Bake] BuildBakedPlayerModel: 0/{} limbs loaded from archive — "
                     "refusing to apply zombie skeleton (wrong archive for this skeleton age?)",
                     limbCount);
        return false;
    }

    // Diagnostic: count limbs without a baked near-DL.  If many, the DummyPlayer
    // will render as a partial skeleton — useful signal when diagnosing missing
    // geometry vs. a hierarchy bug.
    {
        int limbsWithNoDL = 0;
        for (int i = 0; i < limbCount; i++) {
            if (outModel.limbCopies[i].dLists[0] == nullptr) limbsWithNoDL++;
        }
        if (limbsWithNoDL > 0) {
            SPDLOG_WARN("[CoopModel][Bake] {}/{} limbs have no baked dList[0] — may render partially",
                        limbsWithNoDL, limbCount);
        }
    }

    // Diagnostic: dump the built limb tree.  One INFO line per bake in a
    // machine-greppable compact format: `[i:c=XX,s=XX]` for each limb, with `-`
    // appended when that limb has no baked dList[0].  When a crash log shows a
    // successful bake but misbehaving render, this lets us reconstruct the full
    // hierarchy from the log alone without attaching a debugger.
    {
        std::string treeStr;
        treeStr.reserve(limbCount * 16);
        for (int i = 0; i < limbCount; i++) {
            const LodLimb& l = outModel.limbCopies[i];
            char buf[24];
            const char* flag = (l.dLists[0] == nullptr) ? "-" : "";
            int n = std::snprintf(buf, sizeof(buf), "[%d:c=%02X,s=%02X%s]",
                                  i,
                                  (unsigned)(uint8_t)l.child,
                                  (unsigned)(uint8_t)l.sibling,
                                  flag);
            if (n > 0 && n < (int)sizeof(buf)) {
                treeStr.append(buf, n);
                if (i < limbCount - 1) treeStr.push_back(' ');
            }
        }
        SPDLOG_INFO("[CoopModel][Bake] limb tree (c=child, s=sibling hex; '-' = no dList[0]): {}",
                    treeStr);
    }

    // Bake-time cycle detector.  Walk the limb hierarchy from limbCopies[0] via
    // child/sibling, tracking visited indices.  If we hit a back-edge or an
    // out-of-range index, refuse the bake — SkelAnime_DrawFlexLimb recurses on
    // these indices and would stack-overflow at render time (Fix 4 belt-and-
    // braces).  0xFF (LIMB_DONE) terminates a branch cleanly.
    //
    // On error, log the exact visit path that reached the bad index so the
    // failure is diagnosable from the log alone.
    {
        std::bitset<256> visited;
        std::vector<uint8_t> path;
        path.reserve(limbCount);
        auto formatPath = [&](uint8_t finalIdx, const char* marker) -> std::string {
            std::string s;
            s.reserve(path.size() * 5 + 16);
            for (auto p : path) {
                s += std::to_string((int)p);
                s += "->";
            }
            s += std::to_string((int)finalIdx);
            s += " (";
            s += marker;
            s += ")";
            return s;
        };
        auto walk = [&](auto& self, uint8_t idx) -> bool {
            if (idx == 0xFF) return true; // LIMB_DONE — branch complete
            if (idx >= (uint8_t)limbCount) {
                SPDLOG_ERROR("[CoopModel][Bake] Limb index {} out of range (limbCount={}); "
                             "refusing to apply",
                             (int)idx, limbCount);
                SPDLOG_ERROR("[CoopModel][Bake]   path: {}", formatPath(idx, "OOR"));
                return false;
            }
            if (visited.test(idx)) {
                SPDLOG_ERROR("[CoopModel][Bake] Limb hierarchy cycle at index {}; refusing to apply",
                             (int)idx);
                SPDLOG_ERROR("[CoopModel][Bake]   path: {}", formatPath(idx, "cycle"));
                return false;
            }
            visited.set(idx);
            path.push_back(idx);
            const LodLimb& l = outModel.limbCopies[idx];
            if (!self(self, l.child))   return false;
            if (!self(self, l.sibling)) return false;
            path.pop_back();
            return true;
        };
        if (!walk(walk, 0)) {
            return false;
        }
        // Unreachable-limb sanity: if some limbs weren't visited by the walk,
        // they're disconnected from the root.  That's a separate class of
        // malformed skeleton — log it, but don't fail the bake (unreachable
        // limbs just never draw, which is survivable).
        int unreachable = 0;
        for (int i = 0; i < limbCount; i++) {
            if (!visited.test(i)) unreachable++;
        }
        if (unreachable > 0) {
            SPDLOG_WARN("[CoopModel][Bake] {}/{} limbs are unreachable from root — will not render",
                        unreachable, limbCount);
        }
    }

    // Pre-load any pack-local face textures (sEyeTextures / sMouthTextures
    // bindings are outside the walked DLs; see BakeFaceTextures for why).
    BakeFaceTextures(folder, archive, loader, outModel);

    outModel.isValid = true;
    SPDLOG_INFO("[CoopModel][Bake] BuildBakedPlayerModel: {}/{} limbs loaded, {} DLs, {} path strings",
                limbLoadedCount, limbCount, outModel.bakedDLs.size(), outModel.pathStrings.size());
    // Per-bake resource-source summary.  "pack" = file loaded from this pack's
    // archive and isolated under a coopchar/... key; "inherited" = kept the
    // original OTR path so render-time resolution falls through to vanilla /
    // other archives; "sentinel" (textures only) = bake-time hash miss with no
    // global path — render-time FILEPATH handler logs and skips safely.
    // Mostly-pack ≈ self-contained pack; mostly-inherited ≈ pack overrides
    // DLs but inherits vtx/tex from vanilla (normal for minimal packs).
    SPDLOG_INFO("[CoopModel][Bake] folder=\"{}\" resource sources: "
                "vtx={} pack/{} inherited, subdl={} pack/{} inherited, "
                "tex={} pack/{} inherited/{} sentinel",
                folder,
                stats.vtxPack, stats.vtxInherited,
                stats.subdlPack, stats.subdlInherited,
                stats.texPack, stats.texInherited, stats.texSentinel);
    // Self-check: sample up to 3 injected "coopchar/..." keys and verify they
    // resolve via the public ResourceManager API.  If SetCachedResource worked,
    // these should all be non-null.  If a key comes back null here, the fix
    // didn't stick (wrong cache identifier fields, immediate eviction, type
    // mismatch) — render-time null errors would follow and we already know
    // why without waiting.
    {
        auto resMgr = Ship::Context::GetInstance()->GetResourceManager();
        int probed = 0, resolved = 0;
        std::string firstMiss;
        for (const auto& pathStr : outModel.pathStrings) {
            if (pathStr.rfind("coopchar/", 0) != 0) continue;
            if (probed >= 3) break;
            probed++;
            void* raw = resMgr->GetResourceRawPointer(pathStr.c_str());
            if (raw != nullptr) {
                resolved++;
            } else if (firstMiss.empty()) {
                firstMiss = pathStr;
            }
        }
        if (probed > 0) {
            if (resolved == probed) {
                SPDLOG_INFO("[CoopModel][Bake] folder=\"{}\" cache self-check: {}/{} sample keys resolved",
                            folder, resolved, probed);
            } else {
                SPDLOG_ERROR("[CoopModel][Bake] folder=\"{}\" cache self-check FAILED: {}/{} resolved; "
                             "first miss=\"{}\"",
                             folder, resolved, probed, firstMiss);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Vanilla-variant bake path — issue #163
//
// When a remote player selects "Default Link" (empty customModelFilename),
// the viewer's DummyPlayer renders the vanilla Link skeleton.  The previous
// fix (Coop Test 21) loaded the vanilla skeleton via LoadResource(loadExact=true)
// and pointed skelAnime->skeleton at its shared segment — which prevented the
// retire-slot dangling crash but did NOT protect render-time resource resolution
// from alt-asset collisions.  Symptom: the vanilla face TLUT, bound inside the
// vanilla face DL via an OTR path, was resolved at render time through
// LoadResourceProcess which applies alt-asset resolution.  If the viewer's own
// coop pack is registered in ArchiveManager (typical: the viewer picked a coop
// model), its alt TLUT at the same path wins — CI8 face pixels sampled against
// the wrong palette produced the blue-line scramble reported in #163.
//
// The fix below mirrors BuildBakedPlayerModel + BakeDL + BakeFaceTextures but
// resolves every reference through LoadResource(path, loadExact=true), and
// re-caches each under a pack-unique "vanilla-dummy/<path>" key.  Baked DLs
// reference those keys via the same __OTR__-prefix trick BakeFaceTextures uses,
// so render-time OtrSignatureCheck / LoadResourceProcess bypass alt-asset
// resolution (no archive has "alt/vanilla-dummy/..." entries) and hit the
// cache directly.
//
// Duplication with the pack bake is intentional for now.  Consolidation into
// a shared ResourceResolver lambda is tracked as a follow-up once a second
// real caller exists.

static void BakeVanillaFaceTexturesForDummy(
    std::shared_ptr<Ship::ResourceManager>& resMgr,
    BakedPlayerModel& model)
{
    // Same 24 paths as BakeFaceTextures but loaded via loadExact=true so we
    // get the genuine vanilla face textures regardless of which coop packs
    // are registered globally.
    static const char* kEyePaths[2][8] = {
        { "objects/object_link_boy/gLinkAdultEyesOpenTex",
          "objects/object_link_boy/gLinkAdultEyesHalfTex",
          "objects/object_link_boy/gLinkAdultEyesClosedfTex",
          "objects/object_link_boy/gLinkAdultEyesRollLeftTex",
          "objects/object_link_boy/gLinkAdultEyesRollRightTex",
          "objects/object_link_boy/gLinkAdultEyesShockTex",
          "objects/object_link_boy/gLinkAdultEyesUnk1Tex",
          "objects/object_link_boy/gLinkAdultEyesUnk2Tex" },
        { "objects/object_link_child/gLinkChildEyesOpenTex",
          "objects/object_link_child/gLinkChildEyesHalfTex",
          "objects/object_link_child/gLinkChildEyesClosedfTex",
          "objects/object_link_child/gLinkChildEyesRollLeftTex",
          "objects/object_link_child/gLinkChildEyesRollRightTex",
          "objects/object_link_child/gLinkChildEyesShockTex",
          "objects/object_link_child/gLinkChildEyesUnk1Tex",
          "objects/object_link_child/gLinkChildEyesUnk2Tex" },
    };
    static const char* kMouthPaths[2][4] = {
        { "objects/object_link_boy/gLinkAdultMouth1Tex",
          "objects/object_link_boy/gLinkAdultMouth2Tex",
          "objects/object_link_boy/gLinkAdultMouth3Tex",
          "objects/object_link_boy/gLinkAdultMouth4Tex" },
        { "objects/object_link_child/gLinkChildMouth1Tex",
          "objects/object_link_child/gLinkChildMouth2Tex",
          "objects/object_link_child/gLinkChildMouth3Tex",
          "objects/object_link_child/gLinkChildMouth4Tex" },
    };

    int hits = 0;
    auto tryBake = [&](const char* origPath, std::string& outKey) {
        auto res = resMgr->LoadResource(origPath, /*loadExact=*/true);
        if (!res) return;
        const std::string uniqueKey = "vanilla-dummy/" + std::string(origPath);
        resMgr->SetCachedResource(uniqueKey, res);
        outKey = "__OTR__" + uniqueKey;
        hits++;
    };
    for (int age = 0; age < 2; age++) {
        for (int i = 0; i < 8; i++) tryBake(kEyePaths[age][i],   model.eyeTexKeys[age][i]);
        for (int i = 0; i < 4; i++) tryBake(kMouthPaths[age][i], model.mouthTexKeys[age][i]);
    }
    SPDLOG_INFO("[CoopModel][Bake][Vanilla] face textures: {}/24 vanilla overrides preloaded", hits);
}

static Gfx* BakeVanillaDummyDL(
    const std::string& dlPath,
    std::shared_ptr<Ship::ResourceManager>& resMgr,
    BakedPlayerModel& model,
    std::unordered_map<std::string, Gfx*>& dlCache)
{
    // Guard against re-baking and cycles
    auto cacheIt = dlCache.find(dlPath);
    if (cacheIt != dlCache.end()) return cacheIt->second;
    dlCache[dlPath] = nullptr;

    auto resource = resMgr->LoadResource(dlPath, /*loadExact=*/true);
    if (!resource) {
        SPDLOG_WARN("[CoopModel][Bake][Vanilla] DL not found via loadExact: \"{}\"", dlPath);
        return nullptr;
    }
    const std::string cacheKey = "vanilla-dummy/" + dlPath;
    resMgr->SetCachedResource(cacheKey, resource);
    auto dl = std::dynamic_pointer_cast<Fast::DisplayList>(resource);
    if (!dl) return nullptr;

    std::vector<Gfx> baked = dl->Instructions;
    auto archiveMgr = resMgr->GetArchiveManager();

    // Reuse BakeDL's rewrite logic, but source every preload through
    // LoadResource(loadExact=true) instead of a pack archive's LoadFile.
    for (size_t i = 0; i < baked.size(); i++) {
        const uint8_t op = (uint8_t)((baked[i].words.w0 >> 24) & 0xFF);

        if (op == (uint8_t)Fast::OTR_G_SETTIMG_OTR_FILEPATH) {
            const char* origPath = (const char*)baked[i].words.w1;
            auto texRes = resMgr->LoadResource(origPath, /*loadExact=*/true);
            if (texRes) {
                const std::string uniqueKey = "vanilla-dummy/" + std::string(origPath);
                resMgr->SetCachedResource(uniqueKey, texRes);
                model.pathStrings.push_back(uniqueKey);
            } else {
                model.pathStrings.push_back(origPath);  // preserve the original so the c_str() outlives `dl`
            }
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
        }
        else if (op == (uint8_t)Fast::OTR_G_SETTIMG_OTR_HASH) {
            // Same HASH→FILEPATH conversion as BakeDL, targeting the vanilla-dummy/
            // cache namespace instead of coopchar/<folder>/.
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;
            std::string pathStr;
            const char* globalPath = archiveMgr->HashToCString(hash);
            if (globalPath != nullptr) {
                auto texRes = resMgr->LoadResource(globalPath, /*loadExact=*/true);
                if (texRes) {
                    const std::string uniqueKey = "vanilla-dummy/" + std::string(globalPath);
                    resMgr->SetCachedResource(uniqueKey, texRes);
                    pathStr = uniqueKey;
                } else {
                    pathStr = globalPath;
                }
            } else {
                pathStr = "__coopbake_unresolved_hash__";
            }
            model.pathStrings.push_back(pathStr);
            baked[i].words.w0 = (baked[i].words.w0 & 0x00FFFFFF)
                              | ((uintptr_t)(uint8_t)Fast::OTR_G_SETTIMG_OTR_FILEPATH << 24);
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
            baked[i+1].words.w0 = 0;
            baked[i+1].words.w1 = 0;
            i++;
        }
        else if (op == (uint8_t)Fast::OTR_G_VTX_OTR_FILEPATH) {
            const char* origPath = (const char*)baked[i].words.w1;
            auto vtxRes = resMgr->LoadResource(origPath, /*loadExact=*/true);
            if (vtxRes) {
                const std::string uniqueKey = "vanilla-dummy/" + std::string(origPath);
                resMgr->SetCachedResource(uniqueKey, vtxRes);
                model.pathStrings.push_back(uniqueKey);
            } else {
                model.pathStrings.push_back(origPath);
            }
            baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
            i++;  // skip vtx-params word [i+1]
        }
        else if (op == (uint8_t)Fast::OTR_G_VTX_OTR_HASH) {
            const uint32_t vtxW0 = (uint32_t)baked[i].words.w0;
            const uint32_t vtxCnt = (vtxW0 >> 12) & 0xFF;
            const uint32_t vtxIdxOff = ((vtxW0 >> 1) & 0x7F) - vtxCnt;
            const uint32_t byteOffset = (uint32_t)baked[i].words.w1;
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;
            const char* globalPath = archiveMgr->HashToCString(hash);
            if (globalPath != nullptr) {
                auto vtxRes = resMgr->LoadResource(globalPath, /*loadExact=*/true);
                if (vtxRes) {
                    const std::string uniqueKey = "vanilla-dummy/" + std::string(globalPath);
                    resMgr->SetCachedResource(uniqueKey, vtxRes);
                    model.pathStrings.push_back(uniqueKey);
                    baked[i].words.w0 = (uintptr_t)(uint8_t)Fast::OTR_G_VTX_OTR_FILEPATH << 24;
                    baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
                    baked[i+1].words.w0 = (uintptr_t)vtxCnt;
                    baked[i+1].words.w1 = (uintptr_t)((vtxIdxOff << 16) | (byteOffset / 16));
                }
            }
            i++;
        }
        else if (op == (uint8_t)Fast::OTR_G_DL_OTR_FILEPATH) {
            const char* origPath = (const char*)baked[i].words.w1;
            Gfx* subBaked = BakeVanillaDummyDL(origPath, resMgr, model, dlCache);
            if (subBaked) {
                const uintptr_t pushBit = (baked[i].words.w0 >> 16) & 1;
                baked[i].words.w0 = ((uintptr_t)(uint8_t)0xDE << 24) | (pushBit << 16);
                baked[i].words.w1 = (uintptr_t)subBaked;
            } else {
                model.pathStrings.push_back(origPath);
                baked[i].words.w1 = (uintptr_t)model.pathStrings.back().c_str();
            }
        }
        else if (op == (uint8_t)Fast::OTR_G_DL_OTR_HASH) {
            const uintptr_t pushBit = (baked[i].words.w0 >> 16) & 1;
            const uint64_t hash = ((uint64_t)(uint32_t)baked[i+1].words.w0 << 32)
                                | (uint64_t)(uint32_t)baked[i+1].words.w1;
            const char* globalPath = archiveMgr->HashToCString(hash);
            if (globalPath != nullptr) {
                Gfx* subBaked = BakeVanillaDummyDL(globalPath, resMgr, model, dlCache);
                if (subBaked) {
                    baked[i].words.w0 = ((uintptr_t)(uint8_t)0xDE << 24) | (pushBit << 16);
                    baked[i].words.w1 = (uintptr_t)subBaked;
                    baked[i+1].words.w0 = 0;
                    baked[i+1].words.w1 = 0;
                }
            }
            i++;
        }
        else if (op == (uint8_t)Fast::OTR_G_MARKER       ||
                 op == (uint8_t)Fast::OTR_G_BRANCH_Z_OTR ||
                 op == (uint8_t)Fast::OTR_G_MTX_OTR      ||
                 op == (uint8_t)Fast::OTR_G_MOVEMEM_HASH) {
            i++;
        }
    }

    model.bakedDLs.push_back(std::move(baked));
    Gfx* ptr = model.bakedDLs.back().data();
    dlCache[dlPath] = ptr;
    return ptr;
}

static bool BuildVanillaDummyPlayerModel(
    const std::shared_ptr<Skeleton>& skeleton,
    std::shared_ptr<Ship::ResourceManager>& resMgr,
    BakedPlayerModel& outModel)
{
    const int limbCount = skeleton->limbCount;
    outModel.limbCopies.resize(limbCount);
    outModel.segmentPtrs.resize(limbCount);

    std::unordered_map<std::string, Gfx*> dlCache;
    int limbLoadedCount = 0;

    for (int i = 0; i < limbCount; i++) {
        outModel.segmentPtrs[i] = &outModel.limbCopies[i];

        if (i >= (int)skeleton->limbTable.size()) continue;
        const std::string& limbPath = skeleton->limbTable[i];

        // Hierarchy always copied from the skeleton's own segment[] (same invariant
        // as BuildBakedPlayerModel — per-limb SkeletonLimb resources can encode
        // child/sibling indices inconsistently with the Skeleton factory output,
        // which causes SkelAnime_DrawFlexLimb stack overflows).
        auto copyHierarchyFromSegment = [&]() {
            void** limbPtrs = (void**)skeleton->skeletonData.skeletonHeader.segment;
            if (limbPtrs && limbPtrs[i]) {
                LodLimb* srcLimb = (LodLimb*)limbPtrs[i];
                outModel.limbCopies[i].jointPos = srcLimb->jointPos;
                outModel.limbCopies[i].child    = srcLimb->child;
                outModel.limbCopies[i].sibling  = srcLimb->sibling;
            } else {
                outModel.limbCopies[i].child   = 0xFF;
                outModel.limbCopies[i].sibling = 0xFF;
            }
        };

        auto limbRes = resMgr->LoadResource(limbPath, /*loadExact=*/true);
        if (!limbRes) {
            copyHierarchyFromSegment();
            continue;
        }
        auto skeletonLimb = std::dynamic_pointer_cast<SkeletonLimb>(limbRes);
        if (!skeletonLimb) {
            copyHierarchyFromSegment();
            continue;
        }
        limbLoadedCount++;
        copyHierarchyFromSegment();
        outModel.limbCopies[i].dLists[0] = nullptr;
        outModel.limbCopies[i].dLists[1] = nullptr;

        if (!skeletonLimb->dListPtr.empty()) {
            const std::string pfx = "__OTR__";
            const std::string dlPath = skeletonLimb->dListPtr.starts_with(pfx)
                ? skeletonLimb->dListPtr.substr(pfx.size())
                : skeletonLimb->dListPtr;
            Gfx* baked = BakeVanillaDummyDL(dlPath, resMgr, outModel, dlCache);
            if (baked) outModel.limbCopies[i].dLists[0] = baked;
        }
        if (!skeletonLimb->dList2Ptr.empty()) {
            const std::string pfx = "__OTR__";
            const std::string dlPath = skeletonLimb->dList2Ptr.starts_with(pfx)
                ? skeletonLimb->dList2Ptr.substr(pfx.size())
                : skeletonLimb->dList2Ptr;
            Gfx* baked = BakeVanillaDummyDL(dlPath, resMgr, outModel, dlCache);
            if (baked) outModel.limbCopies[i].dLists[1] = baked;
        }
    }

    if (limbLoadedCount == 0) {
        SPDLOG_ERROR("[CoopModel][Bake][Vanilla] BuildVanillaDummyPlayerModel: 0/{} limbs loaded — refusing",
                     limbCount);
        return false;
    }

    BakeVanillaFaceTexturesForDummy(resMgr, outModel);

    outModel.isValid = true;
    SPDLOG_INFO("[CoopModel][Bake][Vanilla] BuildVanillaDummyPlayerModel: {}/{} limbs loaded, {} DLs, {} path strings",
                limbLoadedCount, limbCount, outModel.bakedDLs.size(), outModel.pathStrings.size());
    return true;
}

// ---------------------------------------------------------------------------

// DummyPlayer equivalent of BakeLocalVanillaFallback.  Called from both the
// revert-to-default branch and every pack-incompatible / pack-missing failure
// inside ApplyCustomSkeletonToDummyPlayer.  Without this fallback, the caller's
// freshly-allocated empty bakedModel leaves skelAnime->skeleton pointing at the
// just-retired previous bakedModel's segmentPtrs — dangling after kRetireFrames
// → Unhandled-OP-code flood → crash (issue: remote P2 crashes when local P1
// switches to an adult-only pack while P2 is a child).
//
// Returns true if skelAnime was updated to a valid state; false only when even
// the vanilla skeleton couldn't be loaded (catastrophic).
static bool BakeDummyPlayerVanillaFallback(SkelAnime* skelAnime, bool isAdult,
                                            std::shared_ptr<Skeleton>& outSkeleton,
                                            BakedPlayerModel& outBakedModel) {
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    // "__OTR__" prefix length (7). Matches SkeletonPatcher::sOtr which is private.
    constexpr size_t kOtrPrefixLen = 7;
    const std::string vanillaPath = isAdult
        ? std::string(gLinkAdultSkel).substr(kOtrPrefixLen)
        : std::string(gLinkChildSkel).substr(kOtrPrefixLen);
    auto vanillaRes = resourceMgr->LoadResource(vanillaPath, /*loadExact=*/true);
    auto vanillaSkel = std::dynamic_pointer_cast<Skeleton>(vanillaRes);
    if (vanillaSkel == nullptr || vanillaSkel->skeletonData.skeletonHeader.segment == nullptr) {
        SPDLOG_WARN("[CoopModel] BakeDummyPlayerVanillaFallback: vanilla \"{}\" not loadable — "
                    "leaving skelAnime unchanged", vanillaPath);
        return false;
    }
    outBakedModel = BakedPlayerModel{};
    if (BuildVanillaDummyPlayerModel(vanillaSkel, resourceMgr, outBakedModel)) {
        skelAnime->skeleton = (void**)outBakedModel.segmentPtrs.data();
        uintptr_t skelPtr = (uintptr_t)vanillaSkel->GetPointer();
        memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
        outSkeleton = vanillaSkel;
        gfx_texture_cache_clear();
        return true;
    }
    // Bake failed — shared-segment fallback.  Face textures may scramble on a
    // viewer with a coop pack registered (#163), but the pointer is valid.
    skelAnime->skeleton = vanillaSkel->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)vanillaSkel->GetPointer();
    memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    outSkeleton = vanillaSkel;
    gfx_texture_cache_clear();
    return true;
}

void SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(SkelAnime* skelAnime, bool isAdult, uint8_t tunic,
                                                        const std::string& characterFolder,
                                                        std::shared_ptr<Skeleton>& outSkeleton,
                                                        BakedPlayerModel& outBakedModel) {
    if (characterFolder.empty()) {
        // Revert-to-default path: the remote player switched their CharacterModel back
        // to "Default Link" (UPDATE_CLIENT_STATE carries customModelFilename="").  We
        // MUST restore skelAnime->skeleton to a valid vanilla segment here — simply
        // returning would leave it pointing at the retired bakedModel's segmentPtrs,
        // which becomes dangling after kRetireFrames and crashes the renderer with
        // Unhandled-OP-code floods (Coop Test 19 log 10, issue #110 regression).
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        SPDLOG_INFO("[CoopModel] ApplyCustomSkeletonToDummyPlayer: revert to baked vanilla (isAdult={})", isAdult);
        return;
    }

    // Select the tunic-variant skeleton path (strip __OTR__ prefix used in asset macros)
    std::string skeletonPath;
    if (isAdult) {
        switch (tunic) {
            case PLAYER_TUNIC_GORON: skeletonPath = std::string(gLinkAdultGoronTunicSkel).substr(sOtr.length()); break;
            case PLAYER_TUNIC_ZORA:  skeletonPath = std::string(gLinkAdultZoraTunicSkel).substr(sOtr.length());  break;
            default:                 skeletonPath = std::string(gLinkAdultKokiriTunicSkel).substr(sOtr.length()); break;
        }
    } else {
        switch (tunic) {
            case PLAYER_TUNIC_GORON: skeletonPath = std::string(gLinkChildGoronTunicSkel).substr(sOtr.length()); break;
            case PLAYER_TUNIC_ZORA:  skeletonPath = std::string(gLinkChildZoraTunicSkel).substr(sOtr.length());  break;
            default:                 skeletonPath = std::string(gLinkChildKokiriTunicSkel).substr(sOtr.length()); break;
        }
    }

    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    const std::string altPath = Ship::IResource::gAltAssetPrefix + skeletonPath;
    SPDLOG_INFO("[CoopModel] ApplyCustomSkeletonToDummyPlayer: folder=\"{}\" altPath=\"{}\" isAdult={} tunic={}",
                characterFolder, altPath, isAdult, tunic);

    // Many character packs only ship the base skeleton (gLinkAdultSkel / gLinkChildSkel)
    // rather than the per-tunic variants.  Try the tunic path first; if that fails,
    // retry with the base age skeleton.
    std::string resolvedAltPath = altPath;
    auto file = LoadFileFromCoopFolder(characterFolder, altPath, resourceMgr, false);
    if (file == nullptr) {
        const std::string baseSkeletonPath = isAdult
            ? std::string(gLinkAdultSkel).substr(sOtr.length())
            : std::string(gLinkChildSkel).substr(sOtr.length());
        const std::string baseAltPath = Ship::IResource::gAltAssetPrefix + baseSkeletonPath;
        if (baseAltPath != altPath) {
            SPDLOG_INFO("[CoopModel]   tunic path not found, trying base skeleton: {}", baseAltPath);
            file = LoadFileFromCoopFolder(characterFolder, baseAltPath, resourceMgr, false);
            if (file != nullptr) {
                resolvedAltPath = baseAltPath;
            }
        }
    }
    if (file == nullptr) {
        // Silent early return here would leave skelAnime->skeleton pointing at the
        // freshly-retired previous bakedModel's segmentPtrs — dangling after
        // kRetireFrames → crash.  Bake genuine vanilla so the remote renders a
        // clean Default Link instead.
        SPDLOG_WARN("[CoopModel]   no archive in folder \"{}\" contains \"{}\" or base skeleton — baking vanilla fallback",
                    characterFolder, altPath);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }

    // Parse the File into a typed Skeleton resource.
    // Use a folder-specific cache key to avoid cross-pack resource cache collisions:
    // different character packs share the same altPath (e.g. alt/objects/object_link_child/gLinkChildSkel).
    // Using altPath directly as the key causes the resource manager to return the first pack's
    // cached skeleton for all subsequent packs.  The "coopchar/<folder>/" prefix gives each
    // folder its own isolated cache entry, so switching between packs always gets fresh data.
    auto resourceLoader = resourceMgr->GetResourceLoader();
    const std::string cacheKey = "coopchar/" + characterFolder + "/" + resolvedAltPath;
    auto resource = resourceLoader->LoadResource(cacheKey, file);
    if (resource == nullptr) {
        SPDLOG_WARN("[CoopModel]   LoadResource failed for \"{}\" — baking vanilla fallback", resolvedAltPath);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        SPDLOG_WARN("[CoopModel]   resource is not a Skeleton for \"{}\" — baking vanilla fallback", resolvedAltPath);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }

    // Validate the loaded skeleton before applying it to the DummyPlayer.
    //
    // Character packs that only ship an adult model sometimes include a stale or
    // wrong-age entry at the child skeleton path (e.g. adult skeleton stored at
    // alt/objects/object_link_child/gLinkChildSkel).  Applying a skeleton with the
    // wrong limb count to a SkelAnime initialized for a different age causes an
    // out-of-bounds access in Player_Draw → crash.  Every Guard failure falls back
    // to the baked vanilla skeleton so the remote still renders a valid Link.
    //
    // Guard 1: null segment means the archive entry is incomplete / not properly
    //          relocated.  Writing nullptr to skelAnime->skeleton would crash on render.
    if (skeleton->skeletonData.skeletonHeader.segment == nullptr) {
        SPDLOG_WARN("[CoopModel]   skeleton segment is null in folder \"{}\" — baking vanilla fallback",
                    characterFolder);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }
    // Guard 2: reject only if the loaded skeleton has MORE limbs than the SkelAnime was
    //          allocated for — that would overflow the joint table buffer during animation.
    //          A skeleton with FEWER limbs (e.g. a 21-limb character pack applied to a
    //          22-limb adult SkelAnime) is safe: OoT's draw code reads limb count from the
    //          raw skeleton header (the loaded count), not from skelAnime->limbCount, so
    //          the extra joint-table entries are written by animation but never rendered.
    if (skelAnime->limbCount != 0 && skeleton->limbCount > (int)skelAnime->limbCount) {
        SPDLOG_WARN("[CoopModel]   limb count overflow in folder \"{}\": loaded={} expected={} (too many limbs) — baking vanilla fallback",
                    characterFolder, skeleton->limbCount, (int)skelAnime->limbCount);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }
    // Log the relevant counts for both the skeleton being loaded and the SkelAnime target.
    // This is critical for diagnosing mismatches that survive Guards 1–2 but still crash.
    SPDLOG_INFO("[CoopModel]   skeleton type={} limbCount={} dListCount={} | skelAnime limbCount={} dListCount={}",
                (int)skeleton->type, skeleton->limbCount, skeleton->dListCount,
                (int)skelAnime->limbCount, (int)skelAnime->dListCount);

    // Guard 3: Player_Draw uses SkelAnime_DrawFlexLimb which expects a FlexSkeleton.
    //          If a Normal-type skeleton is stored at the child/adult path in a pack
    //          (e.g. the pack author didn't provide a proper child variant), applying
    //          it to a Flex SkelAnime causes Player_Draw to access display lists using
    //          the wrong stride — crash.
    if (skeleton->type != SkeletonType::Flex) {
        SPDLOG_WARN("[CoopModel]   skeleton type={} is not Flex in folder \"{}\" — baking vanilla fallback",
                    (int)skeleton->type, characterFolder);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }
    // Guard 4: The dListCount cached in skelAnime (set by SkelAnime_InitFlex at
    //          playerInit time) must match the skeleton's dListCount.  A mismatch
    //          means Player_Draw will iterate the wrong number of secondary display
    //          lists per limb, leading to out-of-bounds access.
    if (skelAnime->dListCount != 0 && skeleton->dListCount != (int)skelAnime->dListCount) {
        SPDLOG_WARN("[CoopModel]   dListCount mismatch in folder \"{}\": loaded={} expected={} — baking vanilla fallback",
                    characterFolder, skeleton->dListCount, (int)skelAnime->dListCount);
        BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
        return;
    }
    // Guard 5: validate that every limb pointer in the skeleton segment is non-null.
    //          Character packs that store an adult skeleton at the child skeleton path
    //          (or vice versa) sometimes produce a skeleton with valid limbCount but
    //          null limb pointers — Player_Draw dereferences each limb pointer and
    //          crashes immediately if any are null.
    {
        void** limbPtrs = (void**)skeleton->skeletonData.skeletonHeader.segment;
        for (int i = 0; i < skeleton->limbCount; i++) {
            if (limbPtrs[i] == nullptr) {
                SPDLOG_WARN("[CoopModel]   null limb pointer at index {} in folder \"{}\" — baking vanilla fallback",
                            i, characterFolder);
                BakeDummyPlayerVanillaFallback(skelAnime, isAdult, outSkeleton, outBakedModel);
                return;
            }
        }
    }

    // Store the skeleton shared_ptr so the skeleton data stays alive while skelAnime uses it.
    outSkeleton = skeleton;

    // Try to build per-DummyPlayer baked display lists so the remote player's textures
    // are resolved from their own pack archive rather than the global DL cache.
    // This prevents "both players same model" bugs caused by shared "__OTR__" path lookups.
    outBakedModel = BakedPlayerModel{};  // reset any previous baked data

    // Probe with resolvedAltPath (the skeleton path that actually loaded via
    // LoadFileFromCoopFolder) so we pick the archive matching this skeleton's age.
    auto archive = OpenCoopPackArchives(characterFolder, resolvedAltPath);
    if (archive.IsLoaded()) {
        auto loader = resourceMgr->GetResourceLoader();
        if (BuildBakedPlayerModel(skeleton, characterFolder, archive, loader, outBakedModel)) {
            // Point skelAnime at our per-DummyPlayer limb copies instead of the shared
            // global skeletonHeaderSegments vector.
            skelAnime->skeleton = (void**)outBakedModel.segmentPtrs.data();
            uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
            memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
            skelAnime->limbCount  = (u8)skeleton->limbCount;
            skelAnime->dListCount = (s8)skeleton->dListCount;
            SPDLOG_INFO("[CoopModel]   baked skeleton applied to skelAnime={} (limbCount={})",
                        (void*)skelAnime, skeleton->limbCount);
            gfx_texture_cache_clear();
            return;
        }
        SPDLOG_ERROR("[CoopModel]   BuildBakedPlayerModel failed; falling back to shared segment "
                     "(may exhibit #85 same-model bleed-through)");
    } else {
        SPDLOG_ERROR("[CoopModel]   OpenCoopPackArchives failed for \"{}\" probe=\"{}\"; "
                     "falling back to shared segment (may exhibit #85 same-model bleed-through)",
                     characterFolder, resolvedAltPath);
    }

    // Fallback: use the globally-cached skeleton segment (may show wrong textures if
    // packs share resource paths, but at least geometry is correct).
    SPDLOG_INFO("[CoopModel]   skeleton applied to skelAnime={} (fallback, shared segment)", (void*)skelAnime);
    skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
    memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    skelAnime->limbCount  = (u8)skeleton->limbCount;
    skelAnime->dListCount = (s8)skeleton->dListCount;

    gfx_texture_cache_clear();
}

void SkeletonPatcher::UpdateCustomSkeletonFromPath(const std::string& skeletonPath, SkeletonPatchInfo& skel) {
    Skeleton* newSkel = nullptr;
    Skeleton* altSkel = nullptr;
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    bool isAlt = resourceMgr->IsAltAssetsEnabled();

    SPDLOG_WARN("[CoopModel] UpdateCustomSkeletonFromPath FALLBACK: skeletonPath=\"{}\" isAlt={} skelAnime={}",
                skeletonPath, isAlt, (void*)skel.skelAnime);

    // If alt assets are on, look for alt tagged skeletons
    if (isAlt) {
        const std::string altLookupPath = Ship::IResource::gAltAssetPrefix + skeletonPath;
        altSkel = (Skeleton*)Ship::Context::GetInstance()
                      ->GetResourceManager()
                      ->LoadResource(altLookupPath, true)
                      .get();

        SPDLOG_WARN("[CoopModel]   alt lookup \"{}\" -> {} (ptr={})",
                    altLookupPath, altSkel != nullptr ? "FOUND" : "not found", (void*)altSkel);

        // Override non-alt skeleton if necessary
        if (altSkel != nullptr) {
            newSkel = altSkel;
        }
    }

    // Load new skeleton based on the custom model if it exists
    if (altSkel == nullptr) {
        newSkel = (Skeleton*)Ship::Context::GetInstance()->GetResourceManager()->LoadResource(skeletonPath, true).get();
        SPDLOG_WARN("[CoopModel]   vanilla lookup \"{}\" -> {} (ptr={})",
                    skeletonPath, newSkel != nullptr ? "FOUND" : "not found", (void*)newSkel);
    }

    // Change back to the original skeleton if no skeleton's were found
    if (newSkel == nullptr && skeletonPath != skel.vanillaSkeletonPath) {
        SPDLOG_WARN("[CoopModel]   no skeleton found, retrying with vanilla path \"{}\"", skel.vanillaSkeletonPath);
        UpdateCustomSkeletonFromPath(skel.vanillaSkeletonPath, skel);
        return;
    }

    if (newSkel != nullptr) {
        SPDLOG_WARN("[CoopModel]   applying skeleton ptr={} to local player skelAnime={}", (void*)newSkel, (void*)skel.skelAnime);
        skel.skelAnime->skeleton = newSkel->skeletonData.skeletonHeader.segment;
        uintptr_t skelPtr = (uintptr_t)newSkel->GetPointer();
        memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    }
}
} // namespace SOH

// ---------------------------------------------------------------------------
// Issue #82 — face-texture swap for the LOCAL player's Player_Draw.
//
// Player_DrawImpl (z_player_lib.c:1050/1062) binds face textures via
// gSPSegment(0x08/0x09, sEyeTextures[age][idx]) where each slot holds a raw
// vanilla OTR path string.  Alt-asset resolution on that path caches the
// texture resource under the raw "alt/objects/.../gLinkAdultEyesOpenTex" key
// — keyed on the path, not on which pack supplied it.  When the local player
// switches packs, the cache entry sticks and the renderer keeps drawing the
// previous pack's face texture.
//
// BakeFaceTextures preloads each pack's face textures under pack-unique
// "coopchar/<folder>/..." keys with "__OTR__"-prefixed strings stored in
// bakedModel.eyeTexKeys / mouthTexKeys.  Anchor_LocalPlayerFaceSwapBegin /
// End save/swap/restore the shared sEyeTextures / sMouthTextures arrays
// around the local player's Player_Draw so the bound paths resolve to the
// pack-local cache entries for the duration of that one call.  Same mechanism
// as DummyPlayer_Draw's swap, different caller.
//
// Reentrancy: Player_Draw is not reentrant for the local player, so a single
// save slot is sufficient.  DummyPlayer_Draw owns its own swap scope; the
// two do not interleave because actors are drawn serially.
extern "C" {
    extern void* sEyeTextures[2][8];
    extern void* sMouthTextures[2][4];

    static void* sLocalSavedEye[8];
    static void* sLocalSavedMouth[4];
    static int   sLocalSavedFaceAge = -1;
    static bool  sLocalFaceSwapActive = false;

    void Anchor_LocalPlayerFaceSwapBegin(Actor* thisx, PlayState* play) {
        if (sLocalFaceSwapActive) return;  // defensive: nested call would corrupt the save slot
        if (thisx == nullptr || play == nullptr) return;
        Player* localPlayer = GET_PLAYER(play);
        if (localPlayer == nullptr || thisx != &localPlayer->actor) return;

        SOH::BakedPlayerModel* bm = nullptr;
        for (auto& skel : SOH::SkeletonPatcher::skeletons) {
            if (!skel.isLocalPlayer) continue;
            if (skel.skelAnime != &localPlayer->skelAnime) continue;
            if (skel.bakedModel && skel.bakedModel->isValid) {
                bm = skel.bakedModel.get();
            }
            break;
        }
        if (bm == nullptr) return;

        const int faceAge = (gSaveContext.linkAge == LINK_AGE_CHILD) ? 1 : 0;
        for (int i = 0; i < 8; i++) sLocalSavedEye[i]   = sEyeTextures[faceAge][i];
        for (int i = 0; i < 4; i++) sLocalSavedMouth[i] = sMouthTextures[faceAge][i];
        for (int i = 0; i < 8; i++) {
            if (!bm->eyeTexKeys[faceAge][i].empty()) {
                sEyeTextures[faceAge][i] = (void*)bm->eyeTexKeys[faceAge][i].c_str();
            }
        }
        for (int i = 0; i < 4; i++) {
            if (!bm->mouthTexKeys[faceAge][i].empty()) {
                sMouthTextures[faceAge][i] = (void*)bm->mouthTexKeys[faceAge][i].c_str();
            }
        }
        sLocalSavedFaceAge = faceAge;
        sLocalFaceSwapActive = true;
    }

    void Anchor_LocalPlayerFaceSwapEnd(void) {
        if (!sLocalFaceSwapActive) return;
        const int faceAge = sLocalSavedFaceAge;
        if (faceAge >= 0 && faceAge < 2) {
            for (int i = 0; i < 8; i++) sEyeTextures[faceAge][i]   = sLocalSavedEye[i];
            for (int i = 0; i < 4; i++) sMouthTextures[faceAge][i] = sLocalSavedMouth[i];
        }
        sLocalFaceSwapActive = false;
        sLocalSavedFaceAge = -1;
    }
}
