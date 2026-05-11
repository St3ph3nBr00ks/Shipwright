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

// Drop-intent predicate (Task 4). A path subgoal is treated as a
// planned drop when its Y is at least this far below the navigator
// AND the subgoal is within kPlannedDropMaxXZ horizontally — matches
// the detector's geometry (RoomNavData kDropMinDeltaY=30,
// kDropMaxXZSq=80²). XZ widened slightly here to absorb path
// imprecision while the navigator is still approaching the high-pos
// waypoint.
constexpr float kPlannedDropMinDeltaY = 30.0f;
constexpr float kPlannedDropMaxXZ     = 120.0f;
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

bool IsPlannedDropForSubgoal(const Actor* navigator,
                              const Vec3f& subgoalPos,
                              uint16_t subgoalFlags) {
    if (navigator == nullptr) return false;
    if ((subgoalFlags & ::AnchorNavRoom::NODE_DROP_FROM_ABOVE) == 0) return false;
    // Subgoal must be meaningfully below; spuriously-low Y on the
    // landing waypoint would otherwise stack with the flag and pass
    // through trivially.
    if (subgoalPos.y > navigator->world.pos.y - kPlannedDropMinDeltaY) return false;
    if (AnchorDist::DistXZSq(subgoalPos, navigator->world.pos) > kPlannedDropMaxXZSq) return false;
    return true;
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
    // planner has marked the next subgoal as an intentional drop.
    if (IsPlannedDropForSubgoal(navigator, nextSubgoalPos, nextSubgoalFlags)) {
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
