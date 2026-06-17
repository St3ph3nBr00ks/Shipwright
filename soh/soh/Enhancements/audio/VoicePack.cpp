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

// Voice-pack loader (B2).  See VoicePack.h for the high-level architecture.
//
// PHASE 0 (THIS FILE): single-pack model ported from the WIP scaffolding.
// Loads samples into the ResourceManager cache under per-pack unique keys
// — that's the D7 cache-isolation layer of the D4+D7 hybrid, already
// working.  Phase 1 will:
//   - Replace the single ActivePack with `(clientId → LoadedPack)` map.
//   - Wire peer-pack load/unload from the UPDATE_CLIENT_STATE handler.
//   - Add the per-emitter sample lookup table (D4 layer).
//
// PHASE 4 will add the Audio_GetSfx interception that consults the lookup
// and substitutes the cached SoundFontSample* when an override exists.
//
// Until Phase 4 lands, voice emissions still play the vanilla samples.  The
// pack's samples sit in the resource cache, registered with AudioCollection
// for Audio Editor visibility, but not yet substituted at playback time.

namespace {

struct LoadedSample {
    std::string label;             // VRP sample name (e.g. "Adult Link - Attack 3")
    std::string sfxKey;            // resolved SoH sfxKey (e.g. "NA_SE_VO_LI_SWORD_N")
    std::string cacheKey;          // ResourceManager key (player-voices/<folder>/<path>)
    uint16_t    vanillaSeqId;      // resolved vanilla voice id we're overriding
    uint16_t    displayOnlySeqNum; // pack-assigned seqNum (Audio Editor display only)
};

struct LoadedPack {
    std::string folderName;
    // For the D4 substitution layer (Phase 1+ wiring):
    // vanilla sfxId → resource cache key of the override sample.
    std::unordered_map<uint16_t, std::string> sampleOverridesByVanillaSfxId;
    // Full record of what we registered — used on unload.
    std::vector<LoadedSample> loadedSamples;
};

// Audio-Editor-display-only seqNum allocator.  These seqNums are NOT used
// for playback substitution (that path crashed — see analysis doc §2 root
// cause).  They exist solely so AudioCollection's sequenceMap has an entry
// for each loaded pack sample (informational; aids Audio Editor diagnostics).
constexpr uint16_t kVoicePackDisplaySeqNumBase = 0xF000;

std::mutex                                   gStateMutex;
std::unique_ptr<LoadedPack>                  gActivePack;                 // local pack (null = Default Voices)
std::unordered_map<std::string, std::string> gDefaultTranslation;         // VRP label → SoH sfxKey
bool                                         gDefaultTranslationLoaded = false;
uint16_t                                     gNextDisplaySeqNum = kVoicePackDisplaySeqNumBase;

// ---------------------------------------------------------------------------
// Translation table I/O
// ---------------------------------------------------------------------------

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

// Preloads the sample into the ResourceManager cache under a per-pack-unique
// key (D7 cache-isolation discipline; mirror of BakedPlayerModel's
// `coopchar/<folder>/<altPath>` pattern).  Also registers a SequenceInfo
// for Audio Editor display visibility.
//
// Returns true on success.  Caller updates pack-level bookkeeping.
bool RegisterSample(const std::string& packFolder,
                    const std::string& archiveEntryPath,
                    const std::string& vrpLabel,
                    const std::string& sfxKey,
                    uint16_t displaySeqNum,
                    const std::shared_ptr<Ship::Archive>& archive,
                    std::string& outCacheKey) {
    auto file = archive->LoadFile(archiveEntryPath);
    if (!file || !file->Buffer) {
        SPDLOG_WARN("[VoicePack]   sample file not readable: \"{}\"", archiveEntryPath);
        return false;
    }

    auto resourceMgr = Ship::Context::GetRawInstance()->GetResourceManager();
    auto loader      = resourceMgr->GetResourceLoader();

    // Unique cache key — switching packs that share archive paths cannot
    // collide.  This is the D7 cache-isolation layer of the D4+D7 hybrid.
    outCacheKey = "player-voices/" + packFolder + "/" + archiveEntryPath;
    auto resource = loader->LoadResource(outCacheKey, file);
    if (!resource) {
        SPDLOG_WARN("[VoicePack]   LoadResource failed for \"{}\"", outCacheKey);
        return false;
    }
    resourceMgr->SetCachedResource(outCacheKey, resource);

    // Audio Editor display registration (informational only — the playback
    // substitution path is D4 via per-emitter table consulted at
    // Audio_GetSfx, NOT this seqNum).
    std::string label = "[" + packFolder + "] " + vrpLabel + " -> " + sfxKey;
    AudioCollection::Instance->AddCustomVoiceEntry(displaySeqNum, label, sfxKey);
    return true;
}

// ---------------------------------------------------------------------------
// Pack lifecycle
// ---------------------------------------------------------------------------

void UnloadCurrentPack_Locked() {
    if (!gActivePack) return;
    for (const auto& s : gActivePack->loadedSamples) {
        AudioCollection::Instance->RemoveCustomEntry(s.displayOnlySeqNum);
    }
    SPDLOG_INFO("[VoicePack] unloaded pack \"{}\" ({} samples)",
                gActivePack->folderName, gActivePack->loadedSamples.size());
    gActivePack.reset();
    gNextDisplaySeqNum = kVoicePackDisplaySeqNumBase;
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

void LoadPackArchive(const std::string& packFolder,
                     const std::shared_ptr<Ship::Archive>& archive,
                     const std::unordered_map<std::string, std::string>& translation,
                     LoadedPack& pack) {
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

        const uint16_t displaySeqNum = gNextDisplaySeqNum++;
        std::string cacheKey;
        if (!RegisterSample(packFolder, path, vrpLabel, sfxKey,
                            displaySeqNum, archive, cacheKey)) {
            continue;
        }

        pack.loadedSamples.push_back({ vrpLabel, sfxKey, cacheKey,
                                        vanillaSeqId, displaySeqNum });
        pack.sampleOverridesByVanillaSfxId[vanillaSeqId] = cacheKey;
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

    auto pack = std::make_unique<LoadedPack>();
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

        // Per-pack manifest overlays the default translation.  Pack's keys
        // win on collision.
        auto translation = gDefaultTranslation;
        auto packManifest = ReadPackManifest(archive);
        for (auto& [k, v] : packManifest) translation[k] = v;

        LoadPackArchive(folder, archive, translation, *pack);
    }

    SPDLOG_INFO("[VoicePack] loaded pack \"{}\": {} samples, {} vanilla ids overridden",
                folder, pack->loadedSamples.size(), pack->sampleOverridesByVanillaSfxId.size());

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
    // LEGACY — see header.  The original WIP wired this into
    // AudioCollection::GetReplacementSequence; that direction crashed for
    // 0xF000+ custom seqNums because the SFX dispatch path indexes the
    // fixed-size compile-time gSoundParams[7] table (audio_sound_params.c:238).
    //
    // The D4+D7 substitution path consults a per-emitter sample lookup at
    // Audio_GetSfx time on the game thread, not via this function.  Always
    // returns 0 here; the call site in GetReplacementSequence is NOT ported
    // from the WIP either, so nothing depends on this.  Symbol remains for
    // ABI/link compatibility during the Phase 1–4 wire-in.
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
