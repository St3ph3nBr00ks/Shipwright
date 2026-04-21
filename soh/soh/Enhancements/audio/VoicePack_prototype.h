#ifndef SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_PROTOTYPE_H
#define SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_PROTOTYPE_H

// Voice-pack backend prototype — issue #84, plan "B2 Voice Pack Backend — Feasibility Report".
//
// READ-ONLY probe.  Does NOT register custom fonts, does NOT touch the audio
// heap, does NOT remap sfxIds.  The only job is to confirm that a voice pack
// dropped on disk can be discovered and enumerated by the running game, so
// that the real B2 implementation can decide between the three scan strategies
// (2a pre-register at startup / 2b transient filesystem scan / 2c lazy on
// CVar change) and the path convention (custom/fonts/ vs audio/fonts/ vs
// something else) with data instead of guesses.
//
// Call VoicePack_Prototype_OnAudioModChanged from the Voice Pack dropdown's
// change handler.  It will log everything audio-shaped it finds inside the
// selected pack folder's archives.

#ifdef __cplusplus
extern "C" {
#endif

// Invoked when the user changes CVAR_REMOTE_ANCHOR("AudioMod").
// `folder` may be "" (Default Voices selected) or a coopplayercharacters/
// subfolder name.  No-op on "".
void VoicePack_Prototype_OnAudioModChanged(const char* folder);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ENHANCEMENTS_AUDIO_VOICE_PACK_PROTOTYPE_H
