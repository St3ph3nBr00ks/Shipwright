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

// True when the master Nav CVar is on AND Nav.GroundFollowing is on.
bool IsGroundFollowingEnabled();

// ShipInit registration. Currently a no-op (pure function module);
// kept for parity with sibling modules so future tick hooks can land
// without re-touching the surrounding wiring.
void Register();

} // namespace AnchorNav
