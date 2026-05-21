/**
 * LocomotionAnim — shared animation-selection helpers for scripted-
 * position AI actors (NPC Follower, NPC Invader).
 *
 * Phase 6 of the AI Actor Parity Plan (Plans/ai_actor_parity_plan.md).
 *
 * Each actor has its own anim enum (FollowerNpcAnim, InvaderAnim) and
 * its own per-frame anim swap-dedup (EnsureAnimation, InvEnsureAnimation).
 * That divergence is intentional — different enum values, different
 * modelAnimType resolution, different state→anim mapping per actor.
 *
 * What IS shared across both actors:
 *   - Motion-axis detection (per-tick pos delta → vertical vs lateral
 *     vs stationary classification).
 *   - L/R step alternation logic during CLIMBING.
 *   - Stationary-hold semantics (matches Player's
 *     PLAYER_STATE2_STATIONARY_LADDER — vanilla Link freezes mid-rung
 *     when stick is neutral).
 *
 * This module captures that shared logic as a decision API. The helper
 * returns an abstract `ClimbAnimStep` enum; each caller maps that to
 * its own anim enum and feeds it to the actor's EnsureAnimation.
 *
 * Reference implementation that this consolidates: FollowerNPC.cpp:4395-
 * 4435 (the original NPC Follower implementation) and Invader.cpp's
 * post-2026-05-19 ported equivalent.
 */

#pragma once

extern "C" {
#include "z64.h"
}

namespace AnchorAI {

// Per-tick abstract decision for which climb-anim step to fire.
// Caller maps to its actor-specific anim enum (FollowerNpcAnim or
// InvaderAnim).
enum class ClimbAnimStep {
    kHoldCurrent,  // stationary or mid-step — caller keeps current anim
    kFireUpL,      // vertical step, left-foot lead
    kFireUpR,      // vertical step, right-foot lead
    kFireSideL,    // lateral step, left-foot lead
    kFireSideR,    // lateral step, right-foot lead
};

// Inputs to the climb-step decision.
struct ClimbAnimContext {
    Vec3f currentPos;              // actor's world.pos this tick
    Vec3f prevPos;                 // actor's world.pos LAST tick
    bool  climbNextIsRight = false; // alternation toggle (caller-owned)
    bool  currentAnimIsClimb = false; // is the actor currently playing
                                      // ANY of kClimbUpL/R/SideL/R?
    bool  prevStepDone = true;     // is the previous one-shot finished?
                                   // (Caller computes from stopAnimPlaying
                                   //  state or LinkAnimation_Update return.)
};

// Outputs.
struct ClimbAnimResult {
    ClimbAnimStep step;
    bool advanceLR;  // caller should toggle climbNextIsRight if true
};

// Pick the climb-step decision for this tick.
//
// Behaviour (mirrors NPC Follower's logic at FollowerNPC.cpp:4395):
//   - Compute |dy| and |dxz| from currentPos - prevPos.
//   - isMovingVertically = dy > 0.5.  isMovingLaterally = dxz > 0.5.
//   - useSideAnim = isMovingLaterally && dxz > dy (lateral dominates).
//   - If moving AND prevStepDone: fire (Side|Up)(L|R) based on
//     climbNextIsRight; advanceLR = true.
//   - If stationary AND currentAnimIsClimb == false: fire kFireUpL
//     (first-tick fallback so actor visibly attaches to wall);
//     advanceLR = false (not a real step — leave toggle alone).
//   - If stationary AND currentAnimIsClimb == true: kHoldCurrent
//     (matches Player's PLAYER_STATE2_STATIONARY_LADDER — frozen).
//   - Mid-step (moving but prev one-shot still playing): kHoldCurrent.
ClimbAnimResult PickClimbAnimStep(const ClimbAnimContext& ctx);

}  // namespace AnchorAI
