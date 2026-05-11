/**
 * GroundFollowing — local steering yaw that prefers continuous floor.
 *
 * Local steering layer of the navigation pipeline (plan §7). Pure
 * function: given a chosen subgoal, returns a steering yaw that
 * avoids cliffs and small obstacles within a ~30u probe radius.
 * Composes with ActorTrail's GetBestReachableSubgoal (global layer)
 * which picks WHICH point to walk toward; this picks WHAT bearing.
 *
 * Default-off: gated by gEnhancements.Nav.Enabled AND
 * gEnhancements.Nav.GroundFollowing. NavTraits.useGroundFollowing
 * also gates per-actor (fliers + Goroiwa-class are off by default).
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §7
 *   - GitHub #206 Phase 1 commit 4
 */

#pragma once

#include <cstdint>

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

// Returns a yaw bearing (s16, OoT angle units) from `navigator` to
// `targetPos`, adjusted to prefer angles that keep the navigator on a
// continuous floor.
//
// Algorithm: probe N angular samples around the direct bearing; for
// each, raycast down at a short forward step. Reject samples where
// the floor is missing or drops more than the configured threshold
// below current floor. Pick the sample closest to direct bearing
// among those that pass.
//
// Falls back to direct bearing (Math_Atan2S) if all samples fail OR
// the navigator's NavTraits opt out OR the master CVar is off.
//
// Pure function — no per-frame state. Cost: 1 floor raycast per
// sample × 7 samples per call × per-actor-per-frame frequency.
// Negligible at typical actor counts.
int16_t GetGroundFollowingBearing(Actor* navigator, const Vec3f* targetPos, PlayState* play);

// True when the navigator currently has ground at its feet within
// kGroundProbeDepth. Useful for "should I navigate or fall" decisions.
// Pure function; no state.
bool HasGroundContact(Actor* navigator, PlayState* play);

// True if a forward step of `probeDistance` units along `yaw` would
// land on walkable floor within `maxDrop` of the navigator's current
// foot Y. False = "stepping forward would walk off a cliff". Pure
// function; one floor raycast per call.
bool HasFloorAhead(Actor* navigator, int16_t yaw, float probeDistance,
                   float maxDrop, PlayState* play);

// True when stepping toward `subgoalPos` is a PLANNED drop — i.e. the
// path planner deliberately routed the navigator through a drop
// anchor. Returns true iff:
//   - `subgoalFlags & NODE_DROP_FROM_ABOVE` is set (BFS tagged the
//     landing waypoint at path-build time, Task 3), AND
//   - the subgoal is significantly below the navigator
//     (subgoalPos.y < navigator->y - kPlannedDropMinDeltaY), AND
//   - the subgoal is within a reasonable drop-anchor footprint
//     in XZ (kPlannedDropMaxXZ).
//
// Consumer pattern: when EdgeAvoidance would otherwise zero the
// stick at a cliff, also check this predicate — if true, the
// "cliff" is actually the planned drop, so allow the step.
bool IsPlannedDropForSubgoal(const Actor* navigator,
                              const Vec3f& subgoalPos,
                              uint16_t subgoalFlags);

// Integrated edge-avoidance check intended for AI navigator input
// drivers. Returns true when the navigator should NOT take a forward
// step toward `targetPos` this frame (because it would walk off a
// cliff AND no drop-intent is on file for `nextSubgoalPos` /
// `nextSubgoalFlags`). Consumers respond by zeroing their stick
// magnitude — the navigator stalls at the edge instead of falling.
//
// `nextSubgoalPos` + `nextSubgoalFlags` may be the same as
// `targetPos`/0 when the consumer has no path planner. In that case
// the predicate reduces to "cliff ahead, no intent → suppress".
//
// Master gate: gEnhancements.Nav.Enabled AND Nav.EdgeAvoidance.
// Per-actor gate: NavTraits.useGroundFollowing.
bool ShouldZeroStickForEdge(Actor* navigator,
                             const Vec3f& targetPos,
                             const Vec3f& nextSubgoalPos,
                             uint16_t     nextSubgoalFlags,
                             PlayState*   play);

// True when the master Nav CVar is on AND Nav.GroundFollowing is on.
bool IsGroundFollowingEnabled();

// True when the master Nav CVar is on AND Nav.EdgeAvoidance is on.
bool IsEdgeAvoidanceEnabled();

// ShipInit registration. Currently a no-op (pure function module);
// kept for parity with sibling modules so future tick hooks can land
// without re-touching the surrounding wiring.
void Register();

} // namespace AnchorNav
