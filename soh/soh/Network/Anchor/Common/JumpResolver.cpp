/**
 * JumpResolver — implementation. See header for design intent and the
 * five JumpResolution outcomes.
 *
 * Floor sampling uses BgCheck_AnyRaycastFloor2 to match the convention
 * established in RoomNavData.cpp's scan loop. Forward sample step is
 * 25u, half a typical room-nav cell — granular enough to catch narrow
 * landings without burning excessive raycasts on long jumps.
 */

#include "JumpResolver.h"
#include "ActorTrail.h"
#include "NavTraits.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <limits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------

// Maximum survivable Y delta between navigator's current Y and a candidate
// landing's floor Y. Above this the jump would deal void damage. ~80u
// matches OoT's "from-Goron" tolerance; conservative for most enemies.
static constexpr float kMaxFallDelta       = 80.0f;

// Distance below the navigator past which we treat "no floor" as void.
// Beyond this an enemy-class navigator can't realistically recover even
// if floor exists deep below — survival landing is implausible.
static constexpr float kFloorMissThreshold = 200.0f;

// Forward sample step. Walk forward this far per probe looking for a
// landing. Smaller step = more probes (cost) but tighter granularity.
// 25u = half a kGridResolution cell, fine enough to catch narrow ledges.
static constexpr float kForwardSampleStep  = 25.0f;

// Cast origin lift above navigator. Floor raycast starts here and goes
// down. 100u > standard NPC eye height; safely above any geometry the
// navigator could be standing under.
static constexpr float kCastLift           = 100.0f;

// ---------------------------------------------------------------------------
// Floor sampling helpers.
// ---------------------------------------------------------------------------

// Returns floor Y at the (x, z) of `pos`. Returns NaN when no floor is
// found OR the floor is more than kFloorMissThreshold below `referenceY`
// (treated as void).
static float SampleFloorY(const Vec3f& pos, float referenceY, PlayState* play) {
    if (play == nullptr) return std::numeric_limits<float>::quiet_NaN();
    Vec3f castOrigin = { pos.x, referenceY + kCastLift, pos.z };
    CollisionPoly floorPoly{};
    s32 floorBgId = BGCHECK_SCENE;
    f32 floorY = BgCheck_AnyRaycastFloor2(&play->colCtx, &floorPoly, &floorBgId, &castOrigin);
    if (floorY <= BGCHECK_Y_MIN) return std::numeric_limits<float>::quiet_NaN();
    if ((referenceY - floorY) > kFloorMissThreshold) return std::numeric_limits<float>::quiet_NaN();
    return floorY;
}

// Walkable iff there's a floor within survivable Y delta of referenceY.
static bool IsWalkable(const Vec3f& pos, float referenceY, PlayState* play) {
    float fy = SampleFloorY(pos, referenceY, play);
    if (std::isnan(fy)) return false;
    float dy = referenceY - fy;
    return std::fabs(dy) <= kMaxFallDelta;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

JumpResolutionResult ResolveLedgeAhead(const Actor* navigator,
                                        const Vec3f& targetPos,
                                        const Vec3f& desiredStepPos,
                                        TrailKey     targetKey,
                                        PlayState* play) {
    JumpResolutionResult result;

    if (navigator == nullptr || play == nullptr) {
        result.kind = JumpResolution::Retreat;
        if (navigator != nullptr) result.retreatPos = navigator->home.pos;
        return result;
    }

    const Vec3f& navPos = navigator->world.pos;
    const NavTraits& traits = GetTraitsForActor(navigator->id);
    const float maxJumpDist = (float)traits.maxJumpDistance;

    // Per-room nav data (may be nullptr if RoomNavData is off / not yet
    // scanned). Used by JumpAcross's path-validity check and PathAround.
    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(play->sceneNum,
                                     (int8_t)play->roomCtx.curRoom.num);

    // -----------------------------------------------------------------
    // (1) SafeTerrain — desiredStepPos has walkable floor → no ledge.
    // -----------------------------------------------------------------
    if (IsWalkable(desiredStepPos, navPos.y, play)) {
        result.kind = JumpResolution::SafeTerrain;
        return result;
    }

    // -----------------------------------------------------------------
    // (2) JumpAcross — sample forward in pursuit direction; find a
    //     walkable landing within maxJumpDist with a graph path to
    //     target.
    // -----------------------------------------------------------------
    float dx = targetPos.x - navPos.x;
    float dz = targetPos.z - navPos.z;
    float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ > 0.001f) {
        float dirX = dx / distXZ;
        float dirZ = dz / distXZ;

        for (float d = kForwardSampleStep; d <= maxJumpDist; d += kForwardSampleStep) {
            Vec3f sample = { navPos.x + dirX * d, navPos.y, navPos.z + dirZ * d };
            if (!IsWalkable(sample, navPos.y, play)) continue;

            // Snap to actual floor Y for the candidate landing.
            float landingY = SampleFloorY(sample, navPos.y, play);
            if (std::isnan(landingY)) continue;
            Vec3f landing = { sample.x, landingY, sample.z };

            // Path-to-target check. Two cases:
            //   - With graph: IsReachable from landing to target.
            //   - Without graph: line-clear from landing to target
            //     (not strictly proof of pathability — walls beyond
            //     could block — but better than nothing).
            bool hasPath = false;
            if (navData != nullptr) {
                hasPath = ::AnchorNavRoom::IsReachable(navData, landing, targetPos);
            } else {
                hasPath = MovementClearAtPosition(landing, targetPos, play);
            }
            if (hasPath) {
                result.kind = JumpResolution::JumpAcross;
                result.landingPos = landing;
                return result;
            }
            // Floor exists at this distance but no path to target from
            // there. Don't keep probing further along the same line —
            // a closer landing was rejected, more distant ones will be
            // even less reachable. Break and try the next layer.
            break;
        }
    }

    // -----------------------------------------------------------------
    // (3) TrailContinues — target's breadcrumbs offer evidence the gap
    //     was crossed. If a waypoint within jump range progresses
    //     toward target AND has walkable floor, trust the breadcrumb.
    // -----------------------------------------------------------------
    Vec3f trailLanding = {};
    if (ActorTrail::GetInstance().FindTrailWaypointBeyondGap(
            targetKey, navPos, maxJumpDist, targetPos, trailLanding)) {
        if (IsWalkable(trailLanding, navPos.y, play)) {
            result.kind = JumpResolution::TrailContinues;
            result.landingPos = trailLanding;
            return result;
        }
    }

    // -----------------------------------------------------------------
    // (4) PathAround — graph BFS finds a route avoiding the gap.
    //     We use FindBestReachableSubgoalNode (cheaper than the
    //     path-returning variant) just to prove a route EXISTS.
    //     The caller's response is to invalidate its current path
    //     and re-query ComputePathTo, which will pick the path
    //     around via Layer 3.
    // -----------------------------------------------------------------
    if (navData != nullptr) {
        int fromIdx = ::AnchorNavRoom::FindNearestNode(navData, navPos);
        if (fromIdx >= 0) {
            int bestIdx = ::AnchorNavRoom::FindBestReachableSubgoalNode(
                navData, fromIdx, targetPos,
                traits.eligibleForSwimming,
                traits.avoidHazardNodes);
            if (bestIdx >= 0) {
                result.kind = JumpResolution::PathAround;
                return result;
            }
        }
    }

    // -----------------------------------------------------------------
    // (5) Retreat — no resolution. v1: navigator's home.pos. Future
    //     tunable: oldest valid trail waypoint of navigator's own
    //     trail (closer / safer than home for far-roaming AI).
    // -----------------------------------------------------------------
    result.kind = JumpResolution::Retreat;
    result.retreatPos = navigator->home.pos;
    return result;
}

} // namespace AnchorNav
