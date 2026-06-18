#ifndef SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H
#define SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H

// Voice-pack loader (B2) — issues #83 / #84.
//
// Manages per-player Link voice samples in multiplayer.  Each client picks a
// VRP ("Voice Replacement Pack") folder under `player-voices/<PackFolder>/`;
// loader scans the folder's `.otr`/`.o2r` archives, opens transiently, reads
// every `audio/samples/<VRP label>_META` entry, translates the VRP label via
// `<archive>/translation.json` overlayed on
// `player-voices/_default_translation.json` to a SoH sfxKey, and registers
// the sample in the ResourceManager cache under a per-pack unique key
// (`player-voices/<PackFolder>/<archive entry>`).
//
// Architecture: D4+D7 hybrid (see
//   Claude/Analysis/custom_voice_path_reassessment_2026-06-17.md).
//
//   - D7 cache isolation: each pack's samples land under unique resource
//     keys, mirroring BakedPlayerModel's `coopchar/<folder>/<altPath>`
//     discipline (issue #82).
//   - D4 substitution layer: per-emitter (clientId, vanillaSfxId) → Sample*
//     lookup populated by the loader; consulted by the audio thread via
//     Anchor_GetVoiceSampleOverride() at the Audio_GetSfx interception
//     site (Phase α.4c).
//
// Multi-pack lifecycle:
//   - Local player (gLocalPack): load/unload via OnAudioModChanged when
//     CVAR_REMOTE_ANCHOR("AudioMod") changes from the dropdown.
//   - Peer players (gPeerPacks[clientId]): load/unload via
//     OnPeerAudioModChanged from the UPDATE_CLIENT_STATE receive handler
//     when audioModFilename changes.

// C-callable section below uses uint16_t / uint32_t — pull from stdint.h
// (works in both C and C++). <cstdint> would break the C compilation of
// code_800F7260.c which includes this header for the lookup diagnostic.
#include <stdint.h>

#ifdef __cplusplus
#include <string>

namespace SOH {
namespace VoicePack {

// Local-player pack lifecycle. folder="" deselects (Default Voices).
// Called from the Flotilla → Player → Voice Pack dropdown change handler.
void OnAudioModChanged(const std::string& folder);

// Peer pack lifecycle. folder="" unloads peer's pack. Called from the
// UPDATE_CLIENT_STATE receive handler when a remote client's
// audioModFilename changes.
void OnPeerAudioModChanged(uint32_t clientId, const std::string& folder);

// Game-thread lookup (mutex-protected). Returns the pack's Sample* for
// the given (emitterClientId, vanillaSfxId) or nullptr if no override.
//   - emitterClientId == 0 → no Anchor context; falls back to gLocalPack
//     (single-player default-voice swap still works).
//   - emitterClientId == ownClientId → gLocalPack
//   - emitterClientId != ownClientId → gPeerPacks[emitterClientId]
//
// Audio-thread callers must go through Anchor_GetVoiceSampleOverride
// below (the void* C-linkage shim) to avoid pulling SOH::Sample into the
// C decomp side.
struct SampleOverride;  // forward — opaque to the audio thread.
SampleOverride* GetSampleOverride(uint16_t vanillaSfxId, uint32_t emitterClientId);

// LEGACY / STUB — kept for ABI compat during Phase α wire-in. See header.
// Always returns 0; the new substitution path is GetSampleOverride above.
uint16_t GetReplacement(uint16_t seqId);

} // namespace VoicePack
} // namespace SOH
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

// C-callable: voice-pack dropdown change handler.
void VoicePack_OnAudioModChanged(const char* folder);

// C-callable: returns the override SoundFontSample* (as void*) for the
// given vanilla sfxId + emitter, or NULL for vanilla fallback. Game-thread
// safe; audio thread can also call but acquires the substitution-table
// mutex. The audio-thread caller casts the returned void* to
// SoundFontSample* — same memory layout as SOH::Sample (verified via
// AudioSample.h:22 + z64audio.h:140 cross-reference).
//
// Phase α.5 — when multiple sample variants share the same vanilla sfxId
// (vanilla emits these via random pick in fontMap[0]'s soundEffects pool),
// the override picks a variant pseudo-randomly per call. Matches vanilla's
// random-variant cadence (see SOH::AudioSample sample-rate handling).
//
// Phase α.7 — outTuning receives the sample's tuning if specified by the
// resource (SOH::AudioSample::tuning != -1.0f), else -1.0f signaling
// "inherit vanilla tuning". Callers should default to vanilla tuning when
// outTuning is -1.0f or NULL. Honoring the pack's own tuning is critical
// for samples with non-vanilla sample rates — otherwise playback is at
// the wrong pitch.
void* Anchor_GetVoiceSampleOverride(uint16_t vanillaSfxId, uint32_t emitterClientId,
                                     float* outTuning);

// LEGACY — see SOH::VoicePack::GetReplacement above. Always returns 0.
uint16_t VoicePack_GetReplacement(uint16_t seqId);

// Audio-thread dedup reset hook (#83/#84 diagnostic improvement).
// Audio_GetSfxOverride keeps a 32-entry static ring buffer of "seen"
// (channel, sfxId, emitter) hashes to bound diagnostic log volume.
// On pack load / unload, every prior dedup answer is stale: a sfxId
// that previously logged override=none might now hit, and vice
// versa. VoicePack::OnAudioModChanged / OnPeerAudioModChanged call
// this to clear the buffer so post-pack-load emissions log fresh.
//
// Race-safety: only writes to the audio thread's file-static array.
// Game thread invokes; audio thread reads. The race window is bounded
// to one emission per channel; worst case a single stale log line.
// No correctness impact on audio playback (the dedup affects logging
// only, not substitution).
void Anchor_ResetVoicePackPlayDedup(void);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H
