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
//   - D4 substitution layer: per-emitter sample lookup table consulted at
//     `Audio_GetSfx` time on the game thread.  Wired in Phases 1–4 of the
//     implementation plan.
//
// Multi-pack lifecycle (Phase 1):
//   - Local player: load when CVAR_REMOTE_ANCHOR("AudioMod") changes.
//   - Peer players: load when their `audioModFilename` arrives via
//     `UPDATE_CLIENT_STATE`.
//   - Pack swap: previous pack's resources retire via a delayed-destroy
//     slot (mirror of BakedPlayerModel retire-slot, issue #110 / KB-15).

#include <cstdint>

#ifdef __cplusplus
#include <string>

namespace SOH {
namespace VoicePack {

// Called from the voice-pack dropdown when CVAR_REMOTE_ANCHOR("AudioMod")
// changes.  folder="" deselects the current local pack (Default Voices).
//
// Phase 0 (this commit): single-pack model inherited from the WIP scaffolding.
// Phase 1 will introduce the (clientId → pack) map so peer packs and the
// local pack coexist.
void OnAudioModChanged(const std::string& folder);

// LEGACY / STUB — do not consume.  The original WIP wired this into
// AudioCollection::GetReplacementSequence, which crashed for custom seqNums
// past the 7-bank limit (see analysis doc §2).  Always returns 0 now.  Kept
// as a named symbol only to preserve link-compatibility with code that may
// still reference it during the D4+D7 wire-in.  Will be removed at end of
// Phase 4 once the new substitution layer fully replaces it.
uint16_t GetReplacement(uint16_t seqId);

} // namespace VoicePack
} // namespace SOH
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

// C-callable façade.  Used by the SohMenu dropdown change handler.
void     VoicePack_OnAudioModChanged(const char* folder);
// LEGACY — see SOH::VoicePack::GetReplacement above.  Always returns 0.
uint16_t VoicePack_GetReplacement(uint16_t seqId);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H
