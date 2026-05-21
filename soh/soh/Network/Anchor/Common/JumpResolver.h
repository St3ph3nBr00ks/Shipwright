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
 * AI navigators (AI Player Follower port, NPC Invader, synced enemies that
 * pursue across fragmented terrain) to call before each step.
 */

#pragma once

#include <cstdint>
#include <vector>

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
    // PathAround: caller can adopt this directly into its own NavPath.
    // Excludes the navigator's current node (already there); ordered
    // first-to-last; ends near targetPos. Empty for non-PathAround
    // outcomes.
    std::vector<Vec3f> pathAround;
};

// Evaluates whether `navigator` can safely commit to walking toward
// `desiredStepPos`. See JumpResolution enum for the five outcomes.
//
// Recommended activation point: invoke when the navigator is about to
// advance its NavPath cursor onto a non-walkable cell — i.e., when the
// next subgoal in the path would have it stepping off a ledge. NOT every
// frame. Typical pattern:
//
//     if (path.Empty() || nextSubgoalIsNonWalkable) {
//         auto r = ResolveLedgeAhead(...);
//         dispatch on r.kind;
//     }
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
//   navigatorKey    — TrailKey identifying `navigator`'s OWN trail (for
//                     the Retreat fallback — retreats to the navigator's
//                     oldest valid trail waypoint when available). Pass 0
//                     to fall back to navigator->home.pos directly.
//   play            — current PlayState.
//
// Cost: 1 floor raycast (the SafeTerrain check) on the happy path.
// Worst case (Retreat): forward fan of (5 distances × 3 angles)
// raycasts + horizontal line-clear tests + O(N) trail walk + 1 BFS
// path search. Suitable for per-step (not per-frame) invocation.
JumpResolutionResult ResolveLedgeAhead(const Actor* navigator,
                                        const Vec3f& targetPos,
                                        const Vec3f& desiredStepPos,
                                        TrailKey     targetKey,
                                        TrailKey     navigatorKey,
                                        PlayState* play);

} // namespace AnchorNav
