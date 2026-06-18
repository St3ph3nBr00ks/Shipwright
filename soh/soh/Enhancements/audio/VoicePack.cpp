#include "VoicePack.h"
#include "AudioCollection.h"

#include <libultraship/libultraship.h>
#include <ship/Context.h>
#include <ship/resource/archive/Archive.h>
#include <ship/resource/archive/OtrArchive.h>
#include <ship/resource/archive/O2rArchive.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <cstdlib>  // std::rand for variant pool (Phase α.5)
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "soh/OTRGlobals.h"  // appShortName
#include "soh/resource/type/AudioSample.h"  // SOH::AudioSample + SOH::Sample

// Voice-pack loader (B2) — Phase α.4a/b — multi-pack + lookup.
//
// Phase α.1-α.3 (already shipped) threaded emitterClientId from emission
// site → SoundRequest → SoundBankEntry → audio-thread SequenceChannel.
// Phase α.4 (this file) populates the substitution table that the audio
// thread will consult at Audio_GetSfx interception (Phase α.4c).
//
// Multi-pack: gLocalPack (single, tied to the local CVar) and
// gPeerPacks[clientId] (one per remote player). Local pack loads from the
// dropdown change handler; peer packs load from the UPDATE_CLIENT_STATE
// receive handler.
//
// Sample lifetime: each LoadedSample owns a shared_ptr<SOH::AudioSample>
// returned by the resource loader. The raw SOH::Sample* (used for the
// audio-thread lookup) is stable as long as the shared_ptr is held.
// Unloading a pack drops the shared_ptrs (after also dropping the lookup
// entries) — no dangling pointers.

extern "C" uint32_t Anchor_GetLocalEmitterClientId(void);
extern "C" void     Anchor_ResetVoicePackPlayDedup(void);  // Defined in audio_playback.c

namespace {

// Phase α.5/α.7 — sample + per-sample tuning bundled. Multiple LoadedSamples
// can share the same vanilla sfxId (variant pool); the variant picker at
// lookup time chooses one per emission.
struct PackedSample {
    SOH::Sample* samplePtr;  // stable pointer (resource->sample)
    float        tuning;     // SOH::AudioSample::tuning, or -1.0f for "inherit vanilla"
};

struct LoadedSample {
    std::string                      vrpLabel;             // VRP sample name
    std::string                      sfxKey;               // resolved SoH sfxKey (e.g. NA_SE_VO_LI_SWORD_N)
    std::string                      cacheKey;             // ResourceManager key (D7 isolation)
    uint16_t                         vanillaSeqId;         // vanilla voice id being overridden
    uint16_t                         displayOnlySeqNum;    // Audio Editor display registration id
    std::shared_ptr<SOH::AudioSample> resource;            // lifetime owner — keeps Sample* alive
    PackedSample                     packed;               // {samplePtr, tuning} for the lookup table
};

struct LoadedPack {
    std::string                                       folderName;
    std::vector<LoadedSample>                         loadedSamples;
    // Phase α.5 variant-pool lookup: vanilla sfxId → vector of variants.
    // Vanilla picks a random variant per emission from fontMap[0]'s
    // soundEffects pool when multiple localIds share an sfxKey; we mirror
    // the random-pick behaviour at GetSampleOverride.
    std::unordered_map<uint16_t, std::vector<PackedSample>> samplesByVanillaSfxId;
};

constexpr uint16_t kVoicePackDisplaySeqNumBase = 0xF000;

std::recursive_mutex                                       gStateMutex;
std::unique_ptr<LoadedPack>                                gLocalPack;
std::unordered_map<uint32_t, std::unique_ptr<LoadedPack>>  gPeerPacks;
std::unordered_map<std::string, std::string>               gDefaultTranslation;
bool                                                       gDefaultTranslationLoaded = false;
uint16_t                                                   gNextDisplaySeqNum        = kVoicePackDisplaySeqNumBase;

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
    gDefaultTranslationLoaded = true;

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

bool RegisterSample(const std::string& packFolder,
                    const std::string& archiveEntryPath,
                    const std::string& vrpLabel,
                    const std::string& sfxKey,
                    uint16_t displaySeqNum,
                    const std::shared_ptr<Ship::Archive>& archive,
                    LoadedSample& outSample) {
    auto file = archive->LoadFile(archiveEntryPath);
    if (!file || !file->Buffer) {
        SPDLOG_WARN("[VoicePack]   sample file not readable: \"{}\"", archiveEntryPath);
        return false;
    }

    auto resourceMgr = Ship::Context::GetRawInstance()->GetResourceManager();
    auto loader      = resourceMgr->GetResourceLoader();

    // D7 cache-isolation key: each pack's sample under a unique path so
    // simultaneously-loaded peer packs can't collide.
    const std::string cacheKey =
        "player-voices/" + packFolder + "/" + archiveEntryPath;
    auto rawResource = loader->LoadResource(cacheKey, file);
    if (!rawResource) {
        SPDLOG_WARN("[VoicePack]   LoadResource failed for \"{}\"", cacheKey);
        return false;
    }
    resourceMgr->SetCachedResource(cacheKey, rawResource);

    // The audio sample factory (AudioSampleFactory.cpp) creates AudioSample
    // resources for VRP-format files. static_pointer_cast pattern mirrors
    // AudioSoundFontFactory.cpp:310-315.
    auto audioSample = std::static_pointer_cast<SOH::AudioSample>(rawResource);
    if (!audioSample) {
        SPDLOG_WARN("[VoicePack]   resource for \"{}\" is not AudioSample-typed", cacheKey);
        return false;
    }
    SOH::Sample* samplePtr =
        static_cast<SOH::Sample*>(audioSample->GetRawPointer());
    if (samplePtr == nullptr) {
        SPDLOG_WARN("[VoicePack]   AudioSample for \"{}\" has null GetRawPointer", cacheKey);
        return false;
    }

    // Audio Editor display registration (informational; canBeUsedAsReplacement
    // is false so users don't see them in the substitution dropdown — the
    // playback path is the per-emitter table, not GetReplacementSequence).
    std::string label = "[" + packFolder + "] " + vrpLabel + " -> " + sfxKey;
    AudioCollection::Instance->AddCustomVoiceEntry(displaySeqNum, label, sfxKey);

    outSample.vrpLabel          = vrpLabel;
    outSample.sfxKey            = sfxKey;
    outSample.cacheKey          = cacheKey;
    outSample.displayOnlySeqNum = displaySeqNum;
    outSample.resource          = audioSample;  // shared_ptr keeps lifetime
    outSample.packed.samplePtr  = samplePtr;
    // Phase α.7 — honor the resource's tuning when it was specified by the
    // pack (sample-rate-aware authoring). When SOH::AudioSample default-
    // constructs it leaves tuning = -1.0f, signaling "inherit vanilla". The
    // audio thread reads this and only overrides the SoundFontSound's
    // tuning when the value != -1.0f. Mirrors the AudioSoundFontFactory
    // pattern at lines 310-315 / 322-327 / 336-341.
    outSample.packed.tuning     = audioSample->tuning;

    // Diagnostic D1 (per
    // Claude/Analysis/voice_substitution_intermittent_bug_2026-06-17.md
    // §"Diagnostic instrumentation plan"). Dumps full SOH::Sample field
    // contents per loaded sample. Fires once at pack-load time — bounded
    // log volume (~80 lines per Femme Link pack). Pinpoints the codec /
    // medium / loop / book / sampleAddr values that the audio synth path
    // demands at audio_synthesis.c:759-790. SPDLOG game-thread context is
    // safe (no audio-thread access).
    SPDLOG_INFO("[VoicePackFmt] \"{}\" -> {}: codec={} medium={} size={} fileSize={} "
                "sampleAddr={} loop={} book={} tuning={:.4f} isRelocated={}",
                vrpLabel, sfxKey,
                (unsigned)samplePtr->codec, (unsigned)samplePtr->medium,
                samplePtr->size, samplePtr->fileSize,
                (void*)samplePtr->sampleAddr,
                (void*)samplePtr->loop, (void*)samplePtr->book,
                audioSample->tuning, (unsigned)samplePtr->isRelocated);
    return true;
}

// ---------------------------------------------------------------------------
// Pack lifecycle
// ---------------------------------------------------------------------------

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
        LoadedSample loaded_sample;
        if (!RegisterSample(packFolder, path, vrpLabel, sfxKey,
                            displaySeqNum, archive, loaded_sample)) {
            continue;
        }
        loaded_sample.vanillaSeqId = vanillaSeqId;

        // Phase α.5 — append-to-vector instead of overwrite, so multiple
        // VRP samples that translate to the same vanilla sfxId all become
        // selectable variants (vanilla picks a random variant per emission;
        // we mirror that in GetSampleOverride). Without this, only the
        // last-loaded variant gets the override and gameplay falls back to
        // vanilla for all the others — manifests as intermittent
        // "sometimes custom, sometimes default" substitution.
        pack.samplesByVanillaSfxId[vanillaSeqId].push_back(loaded_sample.packed);
        pack.loadedSamples.push_back(std::move(loaded_sample));
        loaded++;
    }

    SPDLOG_INFO("[VoicePack]   archive \"{}\": {} samples loaded, {} unmapped, {} unresolved sfxKey",
                archive->GetPath(), loaded, skippedUnmapped, skippedUnresolvedKey);
}

// Loads a fresh LoadedPack from disk. Caller is responsible for installing
// the resulting unique_ptr into the right slot (gLocalPack or gPeerPacks).
// Returns nullptr if the folder doesn't exist or contains no usable archives.
std::unique_ptr<LoadedPack> LoadPackFromFolder(const std::string& folder) {
    LoadDefaultTranslationIfNeeded();

    const std::string voiceRoot =
        Ship::Context::LocateFileAcrossAppDirs("player-voices", appShortName);
    const std::filesystem::path charDir = std::filesystem::path(voiceRoot) / folder;
    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
        SPDLOG_WARN("[VoicePack] folder \"{}\" does not exist under \"{}\"",
                    folder, voiceRoot);
        return nullptr;
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

        auto translation = gDefaultTranslation;
        auto packManifest = ReadPackManifest(archive);
        for (auto& [k, v] : packManifest) translation[k] = v;

        LoadPackArchive(folder, archive, translation, *pack);
    }

    return pack;
}

// Unload helper — drops Audio Editor display registrations + releases
// shared_ptr<AudioSample> references. The resource cache still holds them
// (via SetCachedResource); they'll be evicted on natural cache pressure.
// IMPORTANT: clear the sampleByVanillaSfxId lookup BEFORE the shared_ptrs
// drop so the audio thread can't observe a dangling Sample* through a
// concurrent lookup.
void UnloadPack_Locked(LoadedPack& pack) {
    pack.samplesByVanillaSfxId.clear();
    for (const auto& s : pack.loadedSamples) {
        AudioCollection::Instance->RemoveCustomEntry(s.displayOnlySeqNum);
    }
    pack.loadedSamples.clear();  // drops shared_ptrs
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace SOH {
namespace VoicePack {

// Phase α.5/α.7 — opaque lookup result carrying sample + tuning. Thread-
// local storage keeps the returned pointer valid for the calling thread
// up to the next GetSampleOverride call on that thread (audio thread
// reads it once per Audio_GetSfxOverride invocation; no cross-call
// retention needed).
struct SampleOverride {
    SOH::Sample* sample;
    float        tuning;
};
static thread_local SampleOverride sLookupResult;

void OnAudioModChanged(const std::string& folder) {
    std::scoped_lock lock(gStateMutex);
    if (gLocalPack) {
        SPDLOG_INFO("[VoicePack] unloading local pack \"{}\" ({} samples)",
                    gLocalPack->folderName, gLocalPack->loadedSamples.size());
        UnloadPack_Locked(*gLocalPack);
        gLocalPack.reset();
    }
    // Fix 2 (Claude/Analysis/voice_substitution_root_cause_2026-06-17.md)
    // — clear the audio-thread [VoicePackPlay] dedup buffer so post-load
    // emissions log fresh, not "I've seen this hash pre-load" suppressed.
    Anchor_ResetVoicePackPlayDedup();
    if (folder.empty()) {
        SPDLOG_INFO("[VoicePack] Default Voices selected (local)");
        return;
    }
    auto pack = LoadPackFromFolder(folder);
    if (!pack) return;
    SPDLOG_INFO("[VoicePack] loaded LOCAL pack \"{}\": {} samples, {} vanilla ids overridden",
                folder, pack->loadedSamples.size(), pack->samplesByVanillaSfxId.size());
    gLocalPack = std::move(pack);
}

void OnPeerAudioModChanged(uint32_t clientId, const std::string& folder) {
    std::scoped_lock lock(gStateMutex);
    auto it = gPeerPacks.find(clientId);
    if (it != gPeerPacks.end() && it->second) {
        SPDLOG_INFO("[VoicePack] unloading peer pack clientId={} \"{}\" ({} samples)",
                    clientId, it->second->folderName, it->second->loadedSamples.size());
        UnloadPack_Locked(*it->second);
        gPeerPacks.erase(it);
    }
    Anchor_ResetVoicePackPlayDedup();  // Fix 2 — see OnAudioModChanged
    if (folder.empty()) {
        SPDLOG_INFO("[VoicePack] Default Voices selected (peer clientId={})", clientId);
        return;
    }
    auto pack = LoadPackFromFolder(folder);
    if (!pack) return;
    SPDLOG_INFO("[VoicePack] loaded PEER pack clientId={} \"{}\": {} samples, {} vanilla ids overridden",
                clientId, folder, pack->loadedSamples.size(), pack->samplesByVanillaSfxId.size());
    gPeerPacks[clientId] = std::move(pack);
}

SampleOverride* GetSampleOverride(uint16_t vanillaSfxId, uint32_t emitterClientId) {
    std::scoped_lock lock(gStateMutex);

    // Phase α.5 — variant picker: when the pack has multiple samples for
    // one vanilla sfxId, pick a pseudo-random variant. Vanilla picks
    // randomly from fontMap[0]'s soundEffects pool when an sfxKey has
    // multiple localId variants; this mirrors that behavior so combat
    // voice doesn't sound monotone. rand() is acceptable here — we hold
    // gStateMutex and aren't in a tight loop. Future α.7 cleanup may
    // switch to per-emitter RNG state for reproducibility.
    auto pickVariant = [vanillaSfxId](const LoadedPack* pack) -> const PackedSample* {
        if (!pack) return nullptr;
        auto it = pack->samplesByVanillaSfxId.find(vanillaSfxId);
        if (it == pack->samplesByVanillaSfxId.end() || it->second.empty()) {
            return nullptr;
        }
        const auto& variants = it->second;
        size_t idx = (variants.size() == 1) ? 0
                     : static_cast<size_t>(std::rand()) % variants.size();
        return &variants[idx];
    };

    const uint32_t ownId = Anchor_GetLocalEmitterClientId();
    const PackedSample* hit = nullptr;
    if (emitterClientId == 0 || emitterClientId == ownId) {
        hit = pickVariant(gLocalPack.get());
    } else {
        auto it = gPeerPacks.find(emitterClientId);
        hit = pickVariant(it != gPeerPacks.end() ? it->second.get() : nullptr);
    }

    if (hit == nullptr) return nullptr;
    sLookupResult.sample = hit->samplePtr;
    sLookupResult.tuning = hit->tuning;
    return &sLookupResult;
}

uint16_t GetReplacement(uint16_t seqId) {
    (void)seqId;
    return 0;  // legacy stub
}

} // namespace VoicePack
} // namespace SOH

extern "C" void VoicePack_OnAudioModChanged(const char* folder) {
    SOH::VoicePack::OnAudioModChanged(folder ? std::string(folder) : std::string());
}

extern "C" void* Anchor_GetVoiceSampleOverride(uint16_t vanillaSfxId, uint32_t emitterClientId,
                                                float* outTuning) {
    auto* override = SOH::VoicePack::GetSampleOverride(vanillaSfxId, emitterClientId);
    if (override == nullptr) {
        if (outTuning) *outTuning = -1.0f;
        return nullptr;
    }
    if (outTuning) *outTuning = override->tuning;
    return override->sample;
}

extern "C" uint16_t VoicePack_GetReplacement(uint16_t seqId) {
    return SOH::VoicePack::GetReplacement(seqId);
}
