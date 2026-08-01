/**
 * EnhancementAudio — volume-boost helpers for enemy enhancement SFX.
 *
 * Extracted 2026-07-31 from Karebaba V7/V8 audio tuning (second
 * consumer is Dekubaba). Vanilla `Audio_PlayActorSound2` has no
 * volume control — falls back to distance attenuation only. When
 * an enhancement wants to boost a specific SFX or all of an actor's
 * SFX during the enhancement, route through `Audio_PlaySoundGeneral`
 * with an explicit volScale pointer.
 *
 * Two helpers:
 *   - `PlayBoostedActorSfx(actor, sfxId, volScalePtr)` — unconditional
 *     scaled playback. Caller supplies volScale (typically 1.5 or
 *     2.0). Pointer must have static lifetime — Audio_PlaySoundGeneral
 *     retains it across the audio-thread queue.
 *   - `PlayCVarGatedActorSfx(actor, sfxId, cvarName, volScalePtr)` —
 *     CVar-gated dispatch. When cvar is 0: vanilla `Audio_PlayActorSound2`.
 *     When cvar is non-zero: boosted playback per volScalePtr. Used at
 *     vanilla `.c` sites where "SFX is louder when enhancement is on,
 *     unchanged otherwise" is the intent.
 *
 * Static volScale variables (kDoubledVolScale = 2.0f, kBoostedVolScale
 * = 1.5f) provided for the common cases so consumers don't need to
 * define their own static pointer per call site.
 */

#pragma once

#include "soh/Network/Anchor/Anchor.h"  // Pitfall 40

#include "soh/Network/Anchor/Common/EnforcedCVars.h"

extern "C" {
#include "z64.h"
#include "functions.h"  // Audio_PlayActorSound2, Audio_PlaySoundGeneral
#include "variables.h"  // gSfxDefaultFreqAndVolScale, gSfxDefaultReverb
}

namespace AnchorEnemyEnhancement {
namespace EnhancementAudio {

// Static-lifetime volScale singletons. Required because
// Audio_PlaySoundGeneral stores the pointer across the audio-thread
// queue — the address must remain valid until the sound plays.
inline f32 kBoostedVolScale = 1.5f;  // +50%
inline f32 kDoubledVolScale = 2.0f;  // +100%

// Boosted playback with caller-supplied volScale pointer. Pointer
// MUST have static lifetime (stack-local won't work — audio thread
// dequeues later). Default volScale is kBoostedVolScale (+50%) —
// matches the Karebaba V7 SFX-boost trajectory. Pass
// `&kDoubledVolScale` (or use PlayDoubledActorSfx) for +100%.
inline void PlayBoostedActorSfx(Actor* actor, u16 sfxId,
                                 f32* volScalePtr = &kBoostedVolScale) {
    if (actor == nullptr || volScalePtr == nullptr) return;
    Audio_PlaySoundGeneral(sfxId, &actor->projectedPos, 4,
                            &gSfxDefaultFreqAndVolScale,
                            volScalePtr,
                            &gSfxDefaultReverb);
}

// Convenience — +100% boosted (uses kDoubledVolScale).
inline void PlayDoubledActorSfx(Actor* actor, u16 sfxId) {
    PlayBoostedActorSfx(actor, sfxId, &kDoubledVolScale);
}

// CVar-gated dispatch. When cvarName's enforced value is 0, forwards
// to vanilla `Audio_PlayActorSound2` (single-player identical). When
// non-zero, plays boosted per volScalePtr.
//
// Typical use: replace `Audio_PlayActorSound2(actor, X)` in vanilla
// `.c` code with a call through a bridge shim that invokes this
// helper. Bridge shim is per-actor (each actor has its own CVar).
inline void PlayCVarGatedActorSfx(Actor* actor, u16 sfxId,
                                     const char* cvarName,
                                     f32* volScalePtr) {
    if (actor == nullptr) return;
    if (AnchorCVarSync::GetEnforcedInt(cvarName, 0) == 0) {
        Audio_PlayActorSound2(actor, sfxId);
        return;
    }
    PlayBoostedActorSfx(actor, sfxId, volScalePtr);
}

}  // namespace EnhancementAudio
}  // namespace AnchorEnemyEnhancement
