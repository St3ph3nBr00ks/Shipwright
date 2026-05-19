/**
 * ScriptedFollow — shared substrate-path-driven FOLLOW step for the
 * scripted-position AI actors (NPC Follower, AI Invader).
 *
 * Phase 5 of the AI Actor Parity Plan (Plans/ai_actor_parity_plan.md).
 *
 * After Phase 3 ported both NPC Follower and AI Invader through
 * NavOrDirect, their TickFOLLOW handlers became ~85% structurally
 * identical:
 *   1. Resolve trail key (per-actor: friendly NPC uses owner clientId;
 *      hostile Invader uses target clientId).
 *   2. Build FallbackPolicy (per-actor: friendly vs hostile + ranged).
 *   3. Call NavOrDirect::ChooseSubgoal.
 *   4. Detect climb-cell flag on result.subgoalFlags.
 *   5. ... per-actor state-transition + locomotion drive ...
 *
 * This module captures steps 2-4 in a single function so the per-actor
 * wrapper only does steps 1 + 5. Step 1 (trail-key resolution) stays
 * per-actor because the target-to-trail-key mapping is genuinely
 * different (NPC's leader vs Invader's hostile target).
 *
 * Result struct carries enough state that the caller can branch on:
 *   - shouldEngageClimb: transition to CLIMBING state.
 *   - nav.fallbackJustEngaged: trigger per-actor fallback (RANGED for
 *     Invader, return-to-FOLLOW for NPC, etc.).
 *   - nav.subgoal: locomotion target for per-actor speed band.
 *
 * The helper does NOT mutate the actor; the caller decides what to do
 * with the result. Keeps state-machine ownership per actor.
 */

#pragma once

#include "../AILocomotion/NavOrDirect.h"

extern "C" {
#include "z64.h"
}

namespace AnchorAI {

// Output of one scripted-FOLLOW tick step. Wraps NavOrDirectResult
// with derived flags that all scripted-position consumers need.
struct ScriptedFollowResult {
    // Raw nav decision (subgoal pos + flags + fallback state).
    NavOrDirectResult nav;

    // True iff result.usingNavMesh AND result.subgoalFlags includes
    // NODE_CLIMB_ANY. Caller transitions to its CLIMBING state when
    // this fires.
    bool shouldEngageClimb = false;
};

// Run one scripted-FOLLOW step. Calls ChooseSubgoal under the hood
// and derives the climb-cell-transition flag. Caller is expected to:
//   - Set `navState.trailKey` before this call.
//   - Branch on result.shouldEngageClimb to transition CLIMBING.
//   - Branch on result.nav.fallbackJustEngaged + result.nav.fallbackEngaged
//     to apply actor-specific fallback (RANGED_ATTACK / STANDBY / etc).
//   - Drive locomotion toward result.nav.subgoal at actor-specific speed.
//   - Run actor-specific stuck check / IDLE / progress logs after.
//
// `policy` configures the path-empty fallback selection (RangedInPlace /
// ReturnToLeader / RetreatHostile / HoldDefensive). Caller sets the
// applicable flags (isFriendlyActor / isHostileActor / hasRangedReady)
// per its actor class.
ScriptedFollowResult RunScriptedFollowStep(
    const Actor*          navigator,
    const Vec3f&          target,
    NavState&             navState,
    const FallbackPolicy& policy,
    PlayState*            play);

}  // namespace AnchorAI
