/**
 * StepPhase — shared step-phase counter + footstep SFX trigger for
 * scripted-position AI actors that use Link's walk/run skel (NPC
 * Follower, NPC Invader; future actors that share the rig).
 *
 * Pattern mirrors Player's z_player.c:8083-8113 step-phase counter,
 * which advances on every tick based on the active animation's
 * play-speed and fires SFX when the counter crosses a foot-down
 * frame (kStepPhaseFootDownL / kStepPhaseFootDownR). The vanilla
 * "feet hit the ground" rhythm is what triggers footstep audio in
 * OoT — without it the actor walks silently.
 *
 * Caller owns the phase counter (typically a `float` field on the
 * actor or local nav state). Helper advances it, detects foot-down
 * crossings, and fires the SFX directly. No state outside the
 * passed-in float + Actor*.
 *
 * Defaults match Link's walk anim (29-frame cycle, foot-down at
 * frames 10 + 24). Override for actors with different cadences.
 */

#pragma once

extern "C" {
#include "z64.h"
}

namespace AnchorAI {

// Detect whether the phase advanced past `footDown` this tick.
// Handles the wrap-around at cycle so a tick that straddles the
// wrap still fires. Exposed for callers that want the predicate
// without the full SFX side effect.
bool StepPhaseCrossed(float prevPhase, float curPhase, float footDown,
                      float cycle = 29.0f);

// Advance the phase counter and fire footstep SFX on either foot-
// down crossing. `phase` is read AND written. `actor` is needed for
// the projectedPos (SFX listener position) and speedXZ (pitch shift).
// Returns true if SFX fired this tick (for diagnostics).
//
// playSpeed: typically `skelAnime.playSpeed`. The caller may pass
// 0 or a multiplier here to scale phase advance without changing
// the underlying anim playback.
bool TickStepPhase(float& phase, Actor* actor,
                   float playSpeed,
                   float cycle      = 29.0f,
                   float footDownL  = 10.0f,
                   float footDownR  = 24.0f);

}  // namespace AnchorAI
