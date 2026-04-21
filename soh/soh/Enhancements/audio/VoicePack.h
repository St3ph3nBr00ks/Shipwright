#ifndef SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H
#define SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H

// Voice-pack loader (B2) — issue #84.
//
// Registers per-sample voice overrides for Link's voice lines at pack selection
// time, using the VRP ("Voice Replacement Pack") authoring convention:
//
//   player-voices/<PackFolder>/*.otr                        pack archive
//   └─ audio/samples/<VRP sample label>_META                 one sample per line
//   └─ translation.json                                       (optional) per-pack
//                                                             override of the
//                                                             VRP-name → SoH
//                                                             sfxKey mapping
//
// A default translation table is also shipped at
//   player-voices/_default_translation.json
// so VRP-standard packs work without authoring.  Pack-specific manifests
// overlay the default per-key; see translation layering in VoicePack.cpp.
//
// Remap strategy: in-memory, session-scoped.  The active pack's mapping is
// consulted by AudioCollection::GetReplacementSequence BEFORE the existing
// user-facing ReplacedSequences CVar layer, so user-configured voice swaps
// are untouched and restore automatically on pack deselect.

#include <cstdint>

#ifdef __cplusplus
#include <string>

namespace SOH {
namespace VoicePack {

// Called from the Voice Pack dropdown when CVAR_REMOTE_ANCHOR("AudioMod")
// changes.  folder="" deselects the current pack (Default Voices).
void OnAudioModChanged(const std::string& folder);

// Returns the custom seqNum to play instead of `seqId`, or 0 if the currently
// selected pack has no override for that seqId.  Returning 0 lets the caller
// fall through to the existing CVar-driven replacement path.
uint16_t GetReplacement(uint16_t seqId);

} // namespace VoicePack
} // namespace SOH
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

// C-callable façade used by AudioCollection::GetReplacementSequence and the
// SohMenuSettings dropdown change handler.
void     VoicePack_OnAudioModChanged(const char* folder);
uint16_t VoicePack_GetReplacement(uint16_t seqId);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_H
