#include <ship/resource/ResourceManager.h>
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

namespace SOH {
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

// Searches mods/coopplayercharacters/<folder>/ on the filesystem for an archive
// containing altPath, returning the loaded Ship::File on success or nullptr.
//
// Archives not yet in the ArchiveManager are added on demand.  This is necessary
// because mod_menu.cpp keys archives by filename (without extension), so two
// archives with the same filename in different character folders — e.g.
// 3dsLink/3ds_link.otr and 3dsMMLink/3ds_link.otr — collide: only the first is
// loaded into the manager.  Scanning the folder on disk sidesteps that limitation.
//
// The shared_ptr<vector> returned by GetArchives() is stored in a local variable
// before iterating; iterating *GetArchives() directly is UB because the temporary
// shared_ptr is destroyed before the loop body runs, freeing the vector.
static std::shared_ptr<Ship::File> LoadFileFromCoopFolder(const std::string& folder,
                                                           const std::string& altPath,
                                                           std::shared_ptr<Ship::ResourceManager> resourceMgr) {
    const std::string modsRoot = Ship::Context::LocateFileAcrossAppDirs("mods", appShortName);
    const std::filesystem::path charDir =
        std::filesystem::path(modsRoot) / "coopplayercharacters" / folder;

    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
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

        // Not yet in the manager (e.g. filename collision in mod_menu) — load it now.
        if (archive == nullptr) {
            archive = archiveManager->AddArchive(archivePath);
        }
        if (archive == nullptr) continue;

        auto file = archive->LoadFile(altPath);
        if (file != nullptr) {
            return file;
        }
    }
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
        UpdateCustomSkeletonFromFolder(skeletonPath, std::string(overrideFolder), skel);
        return;
    }

    UpdateCustomSkeletonFromPath(skeletonPath, skel);
}

// Searches all loaded archives inside mods/coopplayercharacters/<folder>/ for a skeleton
// at altPath.  If found, parses it and patches skel.skelAnime directly, storing the
// shared_ptr in skel.overrideSkeleton so the data stays alive.  Falls back to
// UpdateCustomSkeletonFromPath (ArchiveManager priority resolution) if no match is found.
void SkeletonPatcher::UpdateCustomSkeletonFromFolder(const std::string& skeletonPath, const std::string& folder,
                                                      SkeletonPatchInfo& skel) {
    if (folder.empty()) {
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }

    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    const std::string altPath = Ship::IResource::gAltAssetPrefix + skeletonPath;

    auto file = LoadFileFromCoopFolder(folder, altPath, resourceMgr);

    if (file == nullptr) {
        // No archive in the selected folder has this skeleton — fall back to normal resolution.
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }

    auto resourceLoader = resourceMgr->GetResourceLoader();
    auto resource = resourceLoader->LoadResource(altPath, file);
    if (resource == nullptr) {
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        skel.overrideSkeleton = nullptr;
        UpdateCustomSkeletonFromPath(skeletonPath, skel);
        return;
    }

    // Keep the skeleton alive for as long as this SkeletonPatchInfo exists.
    skel.overrideSkeleton = skeleton;

    skel.skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
    memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
}

void SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(SkelAnime* skelAnime, bool isAdult, uint8_t tunic,
                                                        const std::string& characterFolder,
                                                        std::shared_ptr<Skeleton>& outSkeleton) {
    if (characterFolder.empty()) {
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

    auto file = LoadFileFromCoopFolder(characterFolder, altPath, resourceMgr);
    if (file == nullptr) {
        SPDLOG_WARN("[CoopModel]   no archive in folder \"{}\" contains \"{}\" — falling back to vanilla",
                    characterFolder, altPath);
        return;
    }

    // Parse the File into a typed Skeleton resource.
    auto resourceLoader = resourceMgr->GetResourceLoader();
    auto resource = resourceLoader->LoadResource(altPath, file);
    if (resource == nullptr) {
        return;
    }
    auto skeleton = std::dynamic_pointer_cast<Skeleton>(resource);
    if (skeleton == nullptr) {
        return;
    }

    // Store the shared_ptr so the skeleton data stays alive while skelAnime uses it.
    // The caller (AnchorClient) must hold this and release it on destroy or model change.
    outSkeleton = skeleton;

    SPDLOG_INFO("[CoopModel]   skeleton applied to skelAnime={}", (void*)skelAnime);
    skelAnime->skeleton = skeleton->skeletonData.skeletonHeader.segment;
    uintptr_t skelPtr = (uintptr_t)skeleton->GetPointer();
    memcpy(&skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
}

void SkeletonPatcher::UpdateCustomSkeletonFromPath(const std::string& skeletonPath, SkeletonPatchInfo& skel) {
    Skeleton* newSkel = nullptr;
    Skeleton* altSkel = nullptr;
    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    bool isAlt = resourceMgr->IsAltAssetsEnabled();

    // If alt assets are on, look for alt tagged skeletons
    if (isAlt) {
        altSkel = (Skeleton*)Ship::Context::GetInstance()
                      ->GetResourceManager()
                      ->LoadResource(Ship::IResource::gAltAssetPrefix + skeletonPath, true)
                      .get();

        // Override non-alt skeleton if necessary
        if (altSkel != nullptr) {
            newSkel = altSkel;
        }
    }

    // Load new skeleton based on the custom model if it exists
    if (altSkel == nullptr) {
        newSkel = (Skeleton*)Ship::Context::GetInstance()->GetResourceManager()->LoadResource(skeletonPath, true).get();
    }

    // Change back to the original skeleton if no skeleton's were found
    if (newSkel == nullptr && skeletonPath != skel.vanillaSkeletonPath) {
        UpdateCustomSkeletonFromPath(skel.vanillaSkeletonPath, skel);
        return;
    }

    if (newSkel != nullptr) {
        skel.skelAnime->skeleton = newSkel->skeletonData.skeletonHeader.segment;
        uintptr_t skelPtr = (uintptr_t)newSkel->GetPointer();
        memcpy(&skel.skelAnime->skeletonHeader, &skelPtr, sizeof(uintptr_t));
    }
}
} // namespace SOH
