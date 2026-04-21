#include "VoicePack.h"
#include "AudioCollection.h"

#include <libultraship/libultraship.h>
#include <ship/Context.h>
#include <ship/resource/archive/Archive.h>
#include <ship/resource/archive/OtrArchive.h>
#include <ship/resource/archive/O2rArchive.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "soh/OTRGlobals.h"  // appShortName

namespace {

// Start registering pack-assigned custom voice seqNums well past any vanilla
// voice id and past the BGM_CUSTOM range that audio_load.c already consumes
// for music packs.  Vanilla voice ids fall under 0x68xx; music packs start
// around 110 (startingSeqNum in audio_load.c) and grow.  0xF000 gives us a
// comfortable reservation above anything sequenceMap ever fills.
constexpr uint16_t kVoicePackSeqNumBase = 0xF000;

struct LoadedSample {
    uint16_t    customSeqNum;      // seqNum we assigned for this sample
    std::string label;             // VRP sample name (e.g. "Adult Link - Attack 3")
    std::string sfxKey;            // resolved SoH sfxKey (e.g. "NA_SE_VO_LI_SWORD_N")
    uint16_t    vanillaSeqId;      // resolved vanilla voice id we're overriding
};

struct ActivePack {
    std::string folderName;
    // vanilla voice seqId → custom seqNum we registered for it.
    // Consulted every time GetReplacement is called; hot path, keep it cheap.
    std::unordered_map<uint16_t, uint16_t> remap;
    // Full record of what we registered, used on unload.
    std::vector<LoadedSample> loadedSamples;
};

std::mutex                           gStateMutex;
std::unique_ptr<ActivePack>          gActivePack;                // null = Default Voices
std::unordered_map<std::string, std::string> gDefaultTranslation; // VRP label → SoH sfxKey
bool                                 gDefaultTranslationLoaded = false;
uint16_t                             gNextCustomSeqNum = kVoicePackSeqNumBase;

// ---------------------------------------------------------------------------
// Translation table I/O
// ---------------------------------------------------------------------------

// Parse a JSON object into `out`.  The file format is a flat object of
// {"<VRP label>": "<NA_SE_VO_LI_*>"}.  Silently skips non-string values and
// logs a warning per bad entry.
void ParseTranslationJson(const nlohmann::json& j,
                          std::unordered_map<std::string, std::string>& out) {
    if (!j.is_object()) {
        SPDLOG_WARN("[VoicePack] translation JSON root is not an object; ignoring");
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_string()) {
            SPDLOG_WARN("[VoicePack] translation entry \"{}\" has non-string value; skipping",
                        it.key());
            continue;
        }
        out[it.key()] = it.value().get<std::string>();
    }
}

void LoadDefaultTranslationIfNeeded() {
    if (gDefaultTranslationLoaded) return;
    gDefaultTranslationLoaded = true;  // always flip even on failure so we don't retry

    const std::string voiceRoot =
        Ship::Context::LocateFileAcrossAppDirs("player-voices", appShortName);
    const std::filesystem::path path =
        std::filesystem::path(voiceRoot) / "_default_translation.json";

    if (!std::filesystem::exists(path)) {
        SPDLOG_INFO("[VoicePack] no default translation at \"{}\" "
                    "(packs without a manifest will skip all samples)",
                    path.generic_string());
        return;
    }

    try {
        std::ifstream f(path);
        nlohmann::json j;
        f >> j;
        ParseTranslationJson(j, gDefaultTranslation);
        SPDLOG_INFO("[VoicePack] loaded default translation: {} entries from \"{}\"",
                    gDefaultTranslation.size(), path.generic_string());
    } catch (const std::exception& e) {
        SPDLOG_WARN("[VoicePack] failed to parse default translation \"{}\": {}",
                    path.generic_string(), e.what());
    }
}

// Pull "translation.json" out of the archive if present.  Returns an empty
// map if not found or unparseable; caller overlays this on top of the default.
std::unordered_map<std::string, std::string>
ReadPackManifest(const std::shared_ptr<Ship::Archive>& archive) {
    std::unordered_map<std::string, std::string> out;
    if (!archive) return out;

    auto file = archive->LoadFile("translation.json");
    if (!file || !file->Buffer || file->Buffer->empty()) return out;

    try {
        auto j = nlohmann::json::parse(file->Buffer->begin(), file->Buffer->end());
        ParseTranslationJson(j, out);
        SPDLOG_INFO("[VoicePack] pack manifest \"translation.json\" parsed: {} entries",
                    out.size());
    } catch (const std::exception& e) {
        SPDLOG_WARN("[VoicePack] pack manifest \"translation.json\" parse failed: {}", e.what());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sample registration
// ---------------------------------------------------------------------------

// Preload the sample into the ResourceManager cache under a pack-unique key,
// then register a SequenceInfo for the new seqNum in AudioCollection.  The
// renderer will look up the seqNum → cached resource via the existing
// ResourceMgr_LoadAudioSample path when the voice fires.
//
// Returns true on success.  The caller owns the remap update.
bool RegisterSample(const std::string& packFolder,
                    const std::string& archiveEntryPath,   // "audio/samples/Adult Link - Attack 3_META"
                    const std::string& vrpLabel,            // "Adult Link - Attack 3"
                    const std::string& sfxKey,              // "NA_SE_VO_LI_SWORD_N"
                    uint16_t customSeqNum,
                    const std::shared_ptr<Ship::Archive>& archive) {
    auto file = archive->LoadFile(archiveEntryPath);
    if (!file || !file->Buffer) {
        SPDLOG_WARN("[VoicePack]   sample file not readable: \"{}\"", archiveEntryPath);
        return false;
    }

    auto resourceMgr = Ship::Context::GetInstance()->GetResourceManager();
    auto loader      = resourceMgr->GetResourceLoader();

    // Unique cache key so switching packs with the same archive entry name
    // doesn't collide with a previously-loaded pack's sample.
    const std::string cacheKey =
        "player-voices/" + packFolder + "/" + archiveEntryPath;
    auto resource = loader->LoadResource(cacheKey, file);
    if (!resource) {
        SPDLOG_WARN("[VoicePack]   LoadResource failed for \"{}\"", cacheKey);
        return false;
    }
    resourceMgr->SetCachedResource(cacheKey, resource);

    // Register in sequenceMap with a label that identifies both the pack and
    // the target id — makes the Audio Editor's Voices tab readable.
    std::string label = "[" + packFolder + "] " + vrpLabel + " → " + sfxKey;
    AudioCollection::Instance->AddCustomVoiceEntry(customSeqNum, label, cacheKey);
    return true;
}

// ---------------------------------------------------------------------------
// Pack lifecycle
// ---------------------------------------------------------------------------

void UnloadCurrentPack_Locked() {
    if (!gActivePack) return;
    for (const auto& s : gActivePack->loadedSamples) {
        AudioCollection::Instance->RemoveCustomEntry(s.customSeqNum);
    }
    SPDLOG_INFO("[VoicePack] unloaded pack \"{}\" ({} samples)",
                gActivePack->folderName, gActivePack->loadedSamples.size());
    gActivePack.reset();
    // Reset the seqNum allocator now that every custom entry is gone, so
    // repeated pack swaps don't drift toward uint16_t overflow.
    gNextCustomSeqNum = kVoicePackSeqNumBase;
}

std::shared_ptr<Ship::Archive> OpenTransient(const std::string& archivePath,
                                              const std::string& ext) {
    std::shared_ptr<Ship::Archive> archive;
    if (ext == ".o2r" || ext == ".zip") {
        archive = std::make_shared<Ship::O2rArchive>(archivePath);
    } else {
        archive = std::make_shared<Ship::OtrArchive>(archivePath);
    }
    archive->Load();
    return archive->IsLoaded() ? archive : nullptr;
}

// Walk one archive, register every audio/samples/*_META it can translate,
// and accumulate the successful registrations into `pack`.
void LoadPackArchive(const std::string& packFolder,
                     const std::shared_ptr<Ship::Archive>& archive,
                     const std::unordered_map<std::string, std::string>& translation,
                     ActivePack& pack) {
    auto files = archive->ListFiles();
    if (!files) return;

    constexpr const char* kSamplePrefix = "audio/samples/";
    constexpr const char* kMetaSuffix   = "_META";

    int loaded = 0;
    int skippedUnmapped = 0;
    int skippedUnresolvedKey = 0;

    for (auto& [hash, path] : *files) {
        if (path.rfind(kSamplePrefix, 0) != 0) continue;
        if (path.size() <= std::strlen(kMetaSuffix) ||
            path.compare(path.size() - std::strlen(kMetaSuffix), std::strlen(kMetaSuffix),
                         kMetaSuffix) != 0) {
            continue;
        }
        const std::string vrpLabel =
            path.substr(std::strlen(kSamplePrefix),
                        path.size() - std::strlen(kSamplePrefix) - std::strlen(kMetaSuffix));

        auto trIt = translation.find(vrpLabel);
        if (trIt == translation.end()) {
            skippedUnmapped++;
            continue;
        }
        const std::string& sfxKey = trIt->second;

        uint16_t vanillaSeqId = AudioCollection::Instance->FindSeqIdBySfxKey(sfxKey);
        if (vanillaSeqId == 0) {
            SPDLOG_WARN("[VoicePack]   sfxKey \"{}\" from translation doesn't match any "
                        "entry in AudioCollection::sequenceMap — skipping sample \"{}\"",
                        sfxKey, vrpLabel);
            skippedUnresolvedKey++;
            continue;
        }

        const uint16_t customSeqNum = gNextCustomSeqNum++;
        if (!RegisterSample(packFolder, path, vrpLabel, sfxKey, customSeqNum, archive)) {
            continue;
        }

        pack.loadedSamples.push_back({ customSeqNum, vrpLabel, sfxKey, vanillaSeqId });
        pack.remap[vanillaSeqId] = customSeqNum;
        loaded++;
    }

    SPDLOG_INFO("[VoicePack]   archive \"{}\": {} samples loaded, {} unmapped, {} unresolved sfxKey",
                archive->GetPath(), loaded, skippedUnmapped, skippedUnresolvedKey);
}

void LoadPack_Locked(const std::string& folder) {
    LoadDefaultTranslationIfNeeded();

    const std::string voiceRoot =
        Ship::Context::LocateFileAcrossAppDirs("player-voices", appShortName);
    const std::filesystem::path charDir = std::filesystem::path(voiceRoot) / folder;
    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
        SPDLOG_WARN("[VoicePack] folder \"{}\" does not exist under \"{}\"",
                    folder, voiceRoot);
        return;
    }

    auto pack = std::make_unique<ActivePack>();
    pack->folderName = folder;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(charDir)) {
        if (entry.is_directory()) continue;
        const std::string ext = entry.path().extension().generic_string();
        if (ext != ".otr" && ext != ".o2r") continue;

        auto archive = OpenTransient(entry.path().generic_string(), ext);
        if (!archive) {
            SPDLOG_WARN("[VoicePack]   failed to open \"{}\"", entry.path().generic_string());
            continue;
        }

        // Per-pack manifest overlays the default translation.  If the pack
        // redefines an entry, the pack's version wins.
        auto translation = gDefaultTranslation;
        auto packManifest = ReadPackManifest(archive);
        for (auto& [k, v] : packManifest) translation[k] = v;

        LoadPackArchive(folder, archive, translation, *pack);
    }

    SPDLOG_INFO("[VoicePack] loaded pack \"{}\": {} samples, {} vanilla ids overridden",
                folder, pack->loadedSamples.size(), pack->remap.size());

    gActivePack = std::move(pack);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace SOH {
namespace VoicePack {

void OnAudioModChanged(const std::string& folder) {
    std::scoped_lock lock(gStateMutex);
    UnloadCurrentPack_Locked();
    if (folder.empty()) {
        SPDLOG_INFO("[VoicePack] Default Voices selected");
        return;
    }
    LoadPack_Locked(folder);
}

uint16_t GetReplacement(uint16_t seqId) {
    // DISABLED — returning a custom seqNum here crashes the SFX dispatch path
    // (code_800F7260.c:224 uses the returned id to index vanilla SFX bank
    // tables, and 0xF000+ values have no entry there → null deref, observed
    // as Exception 0xc0000005 in test log 55).
    //
    // Voice SFX don't actually flow through sequenceMap's replacement pipeline
    // at play time the way BGM does — sequenceMap is authoritative for BGM
    // seqNums, but for SFX it's just the Audio Editor's display model.  The
    // vanilla SFX bank is a separate table indexed by sfxId.  Correctly
    // interposing pack-supplied samples requires intercepting at the sample-
    // fetch level (likely via a ResourceMgr_LoadAudioSample hook or by
    // registering samples under vanilla sample paths via SetCachedResource),
    // not via GetReplacementSequence.
    //
    // Pack load + sequenceMap registration still happens — the samples are in
    // the resource cache under pack-unique keys and the sequenceMap entries
    // show up in the Audio Editor's Voices tab dropdown, so the diagnostic
    // plumbing is visible.  Actual audio remap will be re-wired at the
    // correct layer in a follow-up.
    (void)seqId;
    return 0;
}

} // namespace VoicePack
} // namespace SOH

extern "C" void VoicePack_OnAudioModChanged(const char* folder) {
    SOH::VoicePack::OnAudioModChanged(folder ? std::string(folder) : std::string());
}

extern "C" uint16_t VoicePack_GetReplacement(uint16_t seqId) {
    return SOH::VoicePack::GetReplacement(seqId);
}
