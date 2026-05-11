/**
 * GroundFollowing — implementation.
 *
 * Per plan §7. Pure-function module: probe a small fan of headings
 * around the direct bearing, reject samples without floor or with
 * cliff drop > threshold, pick the survivor with the smallest yaw
 * deviation from direct.
 *
 * No per-frame state. ShipInit::Register is a no-op for now (kept for
 * parity with sibling nav modules); future tick hooks can land
 * without re-touching the wiring.
 */

#include "GroundFollowing.h"

#include "DistanceMath.h"
#include "NavCVars.h"
#include "NavTraits.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // NODE_DROP_FROM_ABOVE
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <cstdlib>
#include <limits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Tunables (file-scope constants per plan §7).
// ---------------------------------------------------------------------------

namespace {

constexpr int   kSampleCount     = 7;        // angular samples around direct bearing
constexpr int   kSampleSpread    = 0x4000;   // ±90° in s16 angle units
constexpr float kProbeDistance   = 30.0f;    // forward step from current pos
constexpr float kMaxDropPerStep  = 30.0f;    // unit drop before "cliff" rejection
constexpr float kProbeAboveFeet  = 50.0f;    // raise probe origin Y above feet
                                             // before raycasting downward
constexpr float kGroundProbeDepth = 60.0f;   // look this far below current foot
                                             // for HasGroundContact()

// Edge-avoidance probe (Task 4). Forward step we test for floor when
// deciding whether the navigator is about to walk off. kEdgeProbeDistance
// is large enough to land on the next adjacent floor tile (~30u grid) yet
// short enough that the lookahead doesn't slow the navigator's reaction.
// kEdgeMaxDrop is the same threshold the existing fan-bearing search uses
// — a small step-up / step-down range is fine, a real cliff is not.
constexpr float kEdgeProbeDistance = 36.0f;
constexpr float kEdgeMaxDrop       = 30.0f;

// Planned off-edge intent predicate (Task 4 + JumpAnchor reuse).
//
// NODE_DROP_FROM_ABOVE is set by the BFS path reconstruction on the
// LANDING waypoint of any planned off-edge step — both DropAnchor
// (downward, line-clear, passive fall) and JumpAnchor (any direction
// within bounds, arc-cleared, active jump). Both share the "this
// off-edge motion is intentional, don't suppress" intent semantic.
//
// The Y-delta range matches the UNION of both detectors' Y ranges:
//   - DropAnchor: takeoff is up to kDropMaxDeltaY (200u) ABOVE landing
//     → from the navigator's POV at takeoff, subgoal Y can be down to
//     -200u.
//   - JumpAnchor: subgoal Y in [-kJumpDownExcludeFloor=30u (downward
//     mostly drops; small overlap permitted by detector), +kJumpUpMax
//     (80u, upward broad-jump apex)] from takeoff.
//
// Union: subgoal Y in (-200, +80) from the navigator's POV. The XZ
// cap (180u) is slightly wider than JumpAnchor's 160u max to absorb
// path-imprecision when the navigator is still approaching the
// takeoff waypoint.
//
// Bug history: original predicate required subgoal STRICTLY below by
// ≥30u (drop-only semantics). After JumpAnchor reused the flag for
// landings of upward/flat jumps, those landings failed the Y check
// → edge avoidance suppressed the takeoff step → follower froze at
// the cliff edge (field-test log 49, 2026-05-12).
constexpr float kPlannedOffEdgeMinDownDeltaY = -200.0f;  // subgoal Y ≥ navY - 200u
constexpr float kPlannedOffEdgeMaxUpDeltaY   = 80.0f;    // subgoal Y ≤ navY + 80u
constexpr float kPlannedDropMaxXZ     = 180.0f;
constexpr float kPlannedDropMaxXZSq   = kPlannedDropMaxXZ * kPlannedDropMaxXZ;

inline int16_t DirectYaw(const Actor* navigator, const Vec3f& target) {
    return Math_Atan2S(target.z - navigator->world.pos.z,
                       target.x - navigator->world.pos.x);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool IsGroundFollowingEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kGroundFollowing);
}

int16_t GetGroundFollowingBearing(Actor* navigator, const Vec3f* targetPos, PlayState* play) {
    if (navigator == nullptr || targetPos == nullptr) return 0;

    int16_t direct = DirectYaw(navigator, *targetPos);

    // Master / per-feature off: fall through to direct bearing (no probe cost).
    if (!IsGroundFollowingEnabled() || play == nullptr) {
        return direct;
    }

    const NavTraits& traits = GetTraitsForActor(navigator->id);
    if (!traits.useGroundFollowing) {
        return direct;
    }

    const float currentFloor = navigator->world.pos.y;

    int16_t bestYaw     = direct;
    int32_t bestPenalty = std::numeric_limits<int32_t>::max();
    bool    found       = false;

    for (int i = 0; i < kSampleCount; i++) {
        // Symmetric fan around direct: i=0 is leftmost, i=kSampleCount-1 is
        // rightmost; centre sample is i = kSampleCount/2.
        int     centre = kSampleCount / 2;
        int16_t offset = (int16_t)(((i - centre) * kSampleSpread) / centre);
        int16_t yaw    = (int16_t)(direct + offset);

        Vec3f probe = {
            navigator->world.pos.x + Math_SinS(yaw) * kProbeDistance,
            navigator->world.pos.y + kProbeAboveFeet,
            navigator->world.pos.z + Math_CosS(yaw) * kProbeDistance,
        };

        CollisionPoly floorPoly{};  // filled by-reference; result unused beyond Y
        f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &floorPoly, &probe);
        if (floorY <= BGCHECK_Y_MIN) {
            continue;  // no floor at all (off the world)
        }
        float drop = currentFloor - floorY;
        if (drop > kMaxDropPerStep) {
            continue;  // cliff
        }

        int32_t absOffset = (int32_t)std::abs((int32_t)offset);
        int32_t dropPart  = (int32_t)(drop > 0.0f ? drop * 10.0f : 0.0f);
        int32_t penalty   = absOffset + dropPart;

        if (penalty < bestPenalty) {
            bestPenalty = penalty;
            bestYaw     = yaw;
            found       = true;
        }
    }

    return found ? bestYaw : direct;
}

bool HasGroundContact(Actor* navigator, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return false;
    Vec3f probe = {
        navigator->world.pos.x,
        navigator->world.pos.y + kProbeAboveFeet,
        navigator->world.pos.z,
    };
    CollisionPoly floorPoly{};  // filled by-reference; result unused beyond Y
    f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &floorPoly, &probe);
    if (floorY <= BGCHECK_Y_MIN) return false;
    float drop = navigator->world.pos.y - floorY;
    return drop < kGroundProbeDepth;
}

bool IsEdgeAvoidanceEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kEdgeAvoidance);
}

bool HasFloorAhead(Actor* navigator, int16_t yaw, float probeDistance,
                   float maxDrop, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return false;
    Vec3f probe = {
        navigator->world.pos.x + Math_SinS(yaw) * probeDistance,
        navigator->world.pos.y + kProbeAboveFeet,
        navigator->world.pos.z + Math_CosS(yaw) * probeDistance,
    };
    CollisionPoly floorPoly{};
    f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &floorPoly, &probe);
    if (floorY <= BGCHECK_Y_MIN) return false;          // off the world
    float drop = navigator->world.pos.y - floorY;
    return drop <= maxDrop;
}

bool IsPlannedOffEdgeStep(const Actor* navigator,
                           const Vec3f& subgoalPos,
                           uint16_t subgoalFlags) {
    if (navigator == nullptr) return false;

    // Climb-surface subgoal: the navigator is approaching a ladder/
    // vine to grab it. The climb anchor's base often sits at the
    // navmesh edge — the floor immediately around the ladder isn't
    // covered by the scan because the wall blocks floodfill — so the
    // approach step crosses a non-walkable cell. Edge avoidance must
    // permit this; the OoT climb-collider grab handles the actual
    // transition once the navigator reaches the basePos.
    //
    // No geometric envelope check for climb intent: the subgoal IS
    // the climb-surface cell position, which by construction may be
    // at any Y relative to the navigator (bottom row of a tall vine
    // wall might be 30u above floor; mid-grid cells higher).
    if (subgoalFlags & ::AnchorNavRoom::NODE_CLIMB_ANY) {
        return true;
    }

    // Drop/jump-anchor landing: NODE_DROP_FROM_ABOVE is set by the
    // BFS path reconstruction for drop-anchor and jump-anchor edges
    // (Task 3 + JumpAnchor reuse). Y range covers the union of both
    // detectors' ranges. XZ check guards against spurious flag set.
    if (subgoalFlags & ::AnchorNavRoom::NODE_DROP_FROM_ABOVE) {
        const float dy = subgoalPos.y - navigator->world.pos.y;
        if (dy < kPlannedOffEdgeMinDownDeltaY) return false;
        if (dy > kPlannedOffEdgeMaxUpDeltaY)   return false;
        if (AnchorDist::DistXZSq(subgoalPos, navigator->world.pos) > kPlannedDropMaxXZSq) return false;
        return true;
    }

    return false;
}

bool ShouldZeroStickForEdge(Actor* navigator,
                             const Vec3f& targetPos,
                             const Vec3f& nextSubgoalPos,
                             uint16_t     nextSubgoalFlags,
                             PlayState*   play) {
    if (navigator == nullptr || play == nullptr) return false;
    if (!IsEdgeAvoidanceEnabled()) return false;

    const NavTraits& traits = GetTraitsForActor(navigator->id);
    // useGroundFollowing is the natural per-actor gate — anything
    // opted out of ground-fan steering is opted out of edge-avoidance
    // too (fliers, waypoint-driven Goroiwa-class, bosses).
    if (!traits.useGroundFollowing) return false;

    // Already airborne — the step has already happened, no decision
    // to make at the input layer. Let physics settle.
    if (!HasGroundContact(navigator, play)) return false;

    // Forward step has a survivable floor — no edge to avoid.
    const int16_t yaw = DirectYaw(navigator, targetPos);
    if (HasFloorAhead(navigator, yaw, kEdgeProbeDistance, kEdgeMaxDrop, play)) {
        return false;
    }

    // No floor ahead — would be an edge step. Allow it iff the path
    // planner has marked the next subgoal as an intentional off-edge
    // step (drop, jump, or climb-surface approach).
    if (IsPlannedOffEdgeStep(navigator, nextSubgoalPos, nextSubgoalFlags)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle. No-op for now — pure-function module.
// ---------------------------------------------------------------------------

void Register() {
    // No hooks needed. Stub kept so future tick / scene hooks can land
    // without re-touching the wiring.
}

} // namespace AnchorNav

static RegisterShipInitFunc registerGroundFollowing(AnchorNav::Register);
