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

#include "NavTraits.h"
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

#define CVAR_NAV_ENABLED          CVAR_ENHANCEMENT("Nav.Enabled")
#define CVAR_NAV_GROUND_FOLLOWING CVAR_ENHANCEMENT("Nav.GroundFollowing")

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

inline int16_t DirectYaw(const Actor* navigator, const Vec3f& target) {
    return Math_Atan2S(target.z - navigator->world.pos.z,
                       target.x - navigator->world.pos.x);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool IsGroundFollowingEnabled() {
    return CVarGetInteger(CVAR_NAV_ENABLED, 0) != 0
        && CVarGetInteger(CVAR_NAV_GROUND_FOLLOWING, 0) != 0;
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

        CollisionPoly* floorPoly = nullptr;
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
    CollisionPoly* floorPoly = nullptr;
    f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &floorPoly, &probe);
    if (floorY <= BGCHECK_Y_MIN) return false;
    float drop = navigator->world.pos.y - floorY;
    return drop < kGroundProbeDepth;
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
