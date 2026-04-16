#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/OtrArchive.h>
#include <ship/resource/archive/O2rArchive.h>
#include <unordered_set>
#include "Skeleton.h"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "libultraship/libultraship.h"
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

// Removes any archives in sLocalCoopArchivePaths from ArchiveManager, then clears
// the tracking set.  Call before applying a new local player coop folder (or reverting
// to default) to prevent stale archives from satisfying alt-asset lookups in
// UpdateCustomSkeletonFromPath.
//
// We do NOT evict the "coopchar/..." resource cache entries because:
// (a) UnloadResources(mask) lists keys from ArchiveManager — our synthetic cache keys
//     aren't archive paths and would not be found.
// (b) Keeping the cache avoids re-parsing the skeleton from disk when the player
//     switches back to a previously-loaded folder later in the session.  The cached
//     Skeleton objects own their data and remain valid after the archive is removed.
static void ClearLocalCoopArchives(std::shared_ptr<Ship::ResourceManager> resourceMgr) {
    if (sLocalCoopArchivePaths.empty()) return;
    auto archiveManager = resourceMgr->GetArchiveManager();
    for (const auto& archivePath : sLocalCoopArchivePaths) {
        archiveManager->RemoveArchive(archivePath);
    }
    sLocalCoopArchivePaths.clear();
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

    for (const auto& entry : std::filesystem::directory_iterator(charDir)) {
        if (entry.is_directory()) continue;
        const std::string ext = entry.path().extension().generic_string();
        if (ext != ".otr" && ext != ".o2r") continue;

        const std::string archivePath = entry.path().generic_string();

        // Prefer the already-loaded archive for this path to avoid duplicate entries.
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

        // Not yet in the manager — behaviour depends on who is calling.
        if (archive == nullptr) {
            if (trackForLocal) {
                // Local player: add to ArchiveManager so textures are reachable via
                // the global alt-asset lookup during Player_Draw.
                archive = archiveManager->AddArchive(archivePath);
                if (archive != nullptr) {
                    SPDLOG_INFO("[CoopModel]   AddArchive (tracked): \"{}\"", archivePath);
                    sLocalCoopArchivePaths.insert(archivePath);
                }
            } else {
                // DummyPlayer: open transiently — Load(), read the file, then discard.
                // The archive is never added to ArchiveManager, so it cannot pollute
                // the global alt-asset lookup or persist across DummyPlayer spawns.
                const std::string ext = entry.path().extension().generic_string();
                std::shared_ptr<Ship::Archive> transient;
                if (ext == ".o2r" || ext == ".zip") {
                    transient = std::make_shared<Ship::O2rArchive>(archivePath);
                } else {
                    transient = std::make_shared<Ship::OtrArchive>(archivePath);
                }
                transient->Load();
                if (transient->IsLoaded()) {
                    SPDLOG_INFO("[CoopModel]   transient open: \"{}\"", archivePath);
                    auto file = transient->LoadFile(altPath);
                    if (file != nullptr) {
                        SPDLOG_INFO("[CoopModel]   found \"{}\" in transient archive \"{}\"", altPath, archivePath);
                        return file;
                    }
                }
                continue;  // transient archive discarded here; skip the shared archive->LoadFile below
            }
        } else {
            SPDLOG_INFO("[CoopModel]   archive already loaded: \"{}\"", archivePath);
        }
        if (archive == nullptr) continue;

        auto file = archive->LoadFile(altPath);
        if (file != nullptr) {
            SPDLOG_INFO("[CoopModel]   found \"{}\" in archive \"{}\"", altPath, archivePath);
            return file;
        }
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

    skeletons.push_back(info);
}

void SkeletonPatcher::UnregisterSkeleton(SkelAnime* skelAnime) {

    // TODO: Should probably just use a dictionary here...
    for (size_t i = 0; i < skeletons.size(); i++) {
        auto skel = skeletons[i];

        if (skel.skelAnime == skelAnime) {
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
    for (auto skel : skeletons) {
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
    if (overrideFolder != nullptr && overrideFolder[0] != '\0') {
        SPDLOG_INFO("[CoopModel] UpdateTunicSkeletons: folder override=\"{}\" path=\"{}\"",
                    overrideFolder, skeletonPath);
        UpdateCustomSkeletonFromFolder(skeletonPath, std::string(overrideFolder), skel);
        return;
    }

    SPDLOG_INFO("[CoopModel] UpdateTunicSkeletons: no override, system default path=\"{}\"",
                skeletonPath);
    UpdateCustomSkeletonFromPath(skeletonPath, skel);
}

// Searches all loaded archives inside coopplayercharacters/<folder>/ for a skeleton
// at altPath.  If found, parses it and patches skel.skelAnime directly, storing the
// shared_ptr in skel.overrideSkeleton so the data stays alive.  Falls back to
// UpdateCustomSkeletonFromPath (ArchiveManager priority resolution) if no match is found.
void SkeletonPatcher::UpdateCustomSkeletonFromFolder(const std::string& skeletonPath, const std::string& folder,
                                                      SkeletonPatchInfo& skel) {
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();

    if (folder.empty()) {
        // Revert to system default (vanilla or user's non-coop alt mod).
        // Clear tracked local archives first.
        ClearLocalCoopArchives(resourceMgr);
        // ALSO remove any coopplayercharacters archives added UNTRACKED by DummyPlayer
        // calls (trackForLocal=false path in LoadFileFromCoopFolder).  These are not in
        // sLocalCoopArchivePaths but if left in ArchiveManager they cause
        // UpdateCustomSkeletonFromPath to return a coop skeleton instead of vanilla.
        {
            const std::string coopRoot = Ship::Context::LocateFileAcrossAppDirs("coopplayercharacters", appShortName);
            auto archiveManager = resourceMgr->GetArchiveManager();
            auto loaded = archiveManager->GetArchives();
            std::vector<std::string> toRemove;
            for (auto& a : *loaded) {
                const std::string& ap = a->GetPath();
                if (ap.find(coopRoot) != std::string::npos ||
                    ap.find("coopplayercharacters") != std::string::npos) {
                    toRemove.push_back(ap);
                }
            }
            for (const auto& ap : toRemove) {
                SPDLOG_INFO("[CoopModel] ClearAllCoopArchives (revert to default): removing \"{}\"", ap);
                archiveManager->RemoveArchive(ap);
            }
        }
        sLastLoadedCoopFolder = "";
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
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
        // No archive in the selected folder has this skeleton — fall back to normal resolution.
        SPDLOG_WARN("[CoopModel]   no archive in folder \"{}\" contains \"{}\" or base skeleton — falling back to system default",
                    folder, altPath);
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
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
        SPDLOG_WARN("[CoopModel]   LoadResource failed for \"{}\" — falling back to system default",
                    resolvedAltPath);
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        SPDLOG_WARN("[CoopModel]   resource is not a Skeleton for \"{}\" — falling back to system default",
                    resolvedAltPath);
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }

    // Keep the skeleton alive for as long as this SkeletonPatchInfo exists.
    skel.overrideSkeleton = skeleton;

    SPDLOG_INFO("[CoopModel]   skeleton applied to local player skelAnime={} segment: {} -> {}",
                (void*)skel.skelAnime,
                (void*)skel.skelAnime->skeleton,
                (void*)skeleton->skeletonData.skeletonHeader.segment);
    skel.skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
    memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));

    // Flush the F3DZEX2 display list cache so the renderer re-uploads the new skeleton's
    // display lists on the next frame.  Without this, the renderer keeps serving cached
    // commands from the OLD skeleton — the visual update only appears after a scene change.
    // This is the same call made in OTRGlobals.cpp when the alt-assets CVar toggles.
    gfx_texture_cache_clear();
}

void SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(SkelAnime* skelAnime, bool isAdult, uint8_t tunic,
                                                        const std::string& characterFolder,
                                                        std::shared_ptr<Skeleton>& outSkeleton) {
    if (characterFolder.empty()) {
        return;
    }

    // Guard 0: child-age DummyPlayers always use vanilla child skeleton.
    //
    // OoT's SkelAnime_InitLink (called from play->playerInit) already ran before this
    // function and applied the ArchiveManager's alt-asset lookup for gLinkChildSkel to
    // the DummyPlayer's skelAnime.  If multiple coop archives are loaded, this may resolve
    // to the LOCAL player's coop child skeleton instead of the remote player's.  We must
    // reset skelAnime to the real vanilla child skeleton regardless.
    //
    // We do NOT attempt to apply the pack's own child skeleton here.  Both tested packs
    // (3dsLink, Malon-Heroine) contain a child skeleton at alt/objects/object_link_child/
    // gLinkChildSkel that passes Guards 1-5 numerically but crashes Player_Draw on the
    // first render frame.  The crash cause is invalid limb display-list data that cannot
    // be detected by the validation guards.
    //
    // loadExact=true bypasses the alt-asset auto-lookup in ResourceManager::LoadResourceProcess,
    // so no coop archive can bleed through into this vanilla load.
    if (!isAdult) {
        auto rMgr = Ship::Context::GetInstance()->GetResourceManager();
        const std::string vanillaChildPath = std::string(gLinkChildSkel).substr(sOtr.length());
        auto vanillaRes = rMgr->LoadResource(vanillaChildPath, true);
        auto vanillaSkel = std::dynamic_pointer_cast<Skeleton>(vanillaRes);
        if (vanillaSkel != nullptr && vanillaSkel->skeletonData.skeletonHeader.segment != nullptr) {
            SPDLOG_INFO("[CoopModel]   child DummyPlayer: reset to vanilla child skeleton (skelAnime={})", (void*)skelAnime);
            skelAnime->skeleton = vanillaSkel->skeletonData.skeletonHeader.segment;
            uintptr_t skelPtr = (uintptr_t)vanillaSkel->GetPointer();
            memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
        } else {
            SPDLOG_WARN("[CoopModel]   child DummyPlayer: vanilla child skeleton unavailable, leaving skelAnime unchanged");
        }
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
    auto file = LoadFileFromCoopFolder(characterFolder, altPath, resourceMgr);
    if (file == nullptr) {
        const std::string baseSkeletonPath = isAdult
            ? std::string(gLinkAdultSkel).substr(sOtr.length())
            : std::string(gLinkChildSkel).substr(sOtr.length());
        const std::string baseAltPath = Ship::IResource::gAltAssetPrefix + baseSkeletonPath;
        if (baseAltPath != altPath) {
            SPDLOG_INFO("[CoopModel]   tunic path not found, trying base skeleton: {}", baseAltPath);
            file = LoadFileFromCoopFolder(characterFolder, baseAltPath, resourceMgr);
            if (file != nullptr) {
                resolvedAltPath = baseAltPath;
            }
        }
    }
    if (file == nullptr) {
        SPDLOG_WARN("[CoopModel]   no archive in folder \"{}\" contains \"{}\" or base skeleton — falling back to vanilla",
                    characterFolder, altPath);
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
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        return;
    }

    // Validate the loaded skeleton before applying it to the DummyPlayer.
    //
    // Character packs that only ship an adult model sometimes include a stale or
    // wrong-age entry at the child skeleton path (e.g. adult skeleton stored at
    // alt/objects/object_link_child/gLinkChildSkel).  Applying a skeleton with the
    // wrong limb count to a SkelAnime initialized for a different age causes an
    // out-of-bounds access in Player_Draw → crash.
    //
    // Guard 1: null segment means the archive entry is incomplete / not properly
    //          relocated.  Writing nullptr to skelAnime->skeleton would crash on render.
    if (skeleton->skeletonData.skeletonHeader.segment == nullptr) {
        SPDLOG_WARN("[CoopModel]   skeleton segment is null in folder \"{}\" — falling back to vanilla",
                    characterFolder);
        return;
    }
    // Guard 2: reject only if the loaded skeleton has MORE limbs than the SkelAnime was
    //          allocated for — that would overflow the joint table buffer during animation.
    //          A skeleton with FEWER limbs (e.g. a 21-limb character pack applied to a
    //          22-limb adult SkelAnime) is safe: OoT's draw code reads limb count from the
    //          raw skeleton header (the loaded count), not from skelAnime->limbCount, so
    //          the extra joint-table entries are written by animation but never rendered.
    if (skelAnime->limbCount != 0 && skeleton->limbCount > (int)skelAnime->limbCount) {
        SPDLOG_WARN("[CoopModel]   limb count overflow in folder \"{}\": loaded={} expected={} (too many limbs) — falling back to vanilla",
                    characterFolder, skeleton->limbCount, (int)skelAnime->limbCount);
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
        SPDLOG_WARN("[CoopModel]   skeleton type={} is not Flex in folder \"{}\" — falling back to vanilla",
                    (int)skeleton->type, characterFolder);
        return;
    }
    // Guard 4: The dListCount cached in skelAnime (set by SkelAnime_InitFlex at
    //          playerInit time) must match the skeleton's dListCount.  A mismatch
    //          means Player_Draw will iterate the wrong number of secondary display
    //          lists per limb, leading to out-of-bounds access.
    if (skelAnime->dListCount != 0 && skeleton->dListCount != (int)skelAnime->dListCount) {
        SPDLOG_WARN("[CoopModel]   dListCount mismatch in folder \"{}\": loaded={} expected={} — falling back to vanilla",
                    characterFolder, skeleton->dListCount, (int)skelAnime->dListCount);
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
                SPDLOG_WARN("[CoopModel]   null limb pointer at index {} in folder \"{}\" — falling back to vanilla",
                            i, characterFolder);
                return;
            }
        }
    }

    // Store the shared_ptr so the skeleton data stays alive while skelAnime uses it.
    // The caller (AnchorClient) must hold this and release it on destroy or model change.
    outSkeleton = skeleton;

    SPDLOG_INFO("[CoopModel]   skeleton applied to skelAnime={}", (void*)skelAnime);
    skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
    memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));

    // Flush the F3DZEX2 display list cache so the renderer rebuilds from the new
    // skeleton on the next frame.  Without this, stale cached display list commands
    // from a previous skeleton (e.g. vanilla Link or a prior DummyPlayer at the same
    // memory address) are served instead, producing the old model geometry + wrong textures.
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
