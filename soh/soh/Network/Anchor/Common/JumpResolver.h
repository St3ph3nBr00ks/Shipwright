/**
 * JumpResolver — void-avoidance / jump-resolution predicate for
 * navigators about to step off a ledge.
 *
 * When an AI navigator's next subgoal would have it walking off a
 * cliff edge, this predicate determines how to handle the void:
 *
 *   1. If the desiredStepPos itself has walkable floor → SafeTerrain.
 *      The navigator was wrong about there being a ledge ahead;
 *      proceed normally.
 *   2. Else, sample forward in the pursuit direction within
 *      NavTraits.maxJumpDistance. If a walkable landing is found AND
 *      it has a valid graph path (or line-clear) to target →
 *      JumpAcross. Navigator commits to a jump toward that landing.
 *   3. Else, walk the target's breadcrumb trail looking for a waypoint
 *      within jump range that progresses past the gap → TrailContinues.
 *      Target was there → there's likely walkable ground there →
 *      navigator follows the breadcrumb evidence.
 *   4. Else, try the static graph for a route around → PathAround.
 *      Caller re-routes via ComputePathTo Layer 3.
 *   5. None of the above → Retreat to retreatPos (navigator's home).
 *
 * Per user-supplied design intent. See user request 2026-05-08.
 *
 * No consumer wired in this commit — the predicate exists for future
 * AI navigators (AI Follower port, AI Invader, synced enemies that
 * pursue across fragmented terrain) to call before each step.
 */

#pragma once

#include <cstdint>

extern "C" {
#include "z64.h"
}

#include "ActorTrail.h"  // TrailKey

namespace AnchorNav {

// Outcome of ResolveLedgeAhead. The navigator interprets each kind:
//   SafeTerrain     → walk to desiredStepPos as planned.
//   JumpAcross      → trigger a jump animation toward result.landingPos.
//   TrailContinues  → trust target's evidence; jump toward result.landingPos.
//   PathAround      → re-query ComputePathTo; existing path is wrong.
//   Retreat         → walk back toward result.retreatPos; abandon pursuit
//                     for now (re-evaluate on next decision tick).
enum class JumpResolution : uint8_t {
    SafeTerrain    = 0,
    JumpAcross     = 1,
    TrailContinues = 2,
    PathAround     = 3,
    Retreat        = 4,
};

struct JumpResolutionResult {
    JumpResolution kind        = JumpResolution::SafeTerrain;
    Vec3f          landingPos  = { 0, 0, 0 }; // valid for JumpAcross, TrailContinues
    Vec3f          retreatPos  = { 0, 0, 0 }; // valid for Retreat
};

// Evaluates whether `navigator` can safely commit to walking toward
// `desiredStepPos`. See JumpResolution enum for the five outcomes.
//
// Parameters:
//   navigator       — the AI doing the pursuing.
//   targetPos       — what `navigator` is pursuing.
//   desiredStepPos  — the next waypoint the navigator was about to walk
//                     to (typically `path.CurrentSubgoal()` from
//                     ActorTrail::ComputePathTo, or a per-frame steering
//                     output).
//   targetKey       — TrailKey identifying `targetPos`'s trail (for
//                     the breadcrumb fallback). Pass
//                     TrailKeyForPlayer/TrailKeyForActor as appropriate.
//   play            — current PlayState.
//
// Cost: 1 floor raycast (the SafeTerrain check) on the happy path.
// Worst case (Retreat): ~5-7 forward floor raycasts + O(N) trail walk
// + 1 BFS reachability call. Suitable for per-step (not per-frame)
// invocation; throttle by calling only when a step is about to commit.
JumpResolutionResult ResolveLedgeAhead(const Actor* navigator,
                                        const Vec3f& targetPos,
                                        const Vec3f& desiredStepPos,
                                        TrailKey     targetKey,
                                        PlayState* play);

} // namespace AnchorNav
