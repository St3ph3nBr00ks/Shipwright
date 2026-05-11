/**
 * JumpResolver — implementation. See header for design intent and the
 * five JumpResolution outcomes.
 *
 * Floor sampling uses BgCheck_AnyRaycastFloor2 to match the convention
 * established in RoomNavData.cpp's scan loop. Forward sample step is
 * 25u (half a typical room-nav cell). The forward fan covers ±45° from
 * the pursuit vector at 22.5° increments — broader than a single ray
 * to catch off-axis landings without being so wide the AI looks
 * indecisive. Air-clearance line tests ensure the chosen landing is
 * actually reachable horizontally (no wall in the jump arc).
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
// landing's floor Y. ~80u matches OoT's "from-Goron" tolerance;
// conservative for most enemies.
static constexpr float kMaxFallDelta       = 80.0f;

// Distance below the navigator past which we treat "no floor" as void.
// Beyond this, an enemy-class navigator can't realistically recover.
static constexpr float kFloorMissThreshold = 200.0f;

// Forward sample step. Walk forward this far per probe looking for a
// landing. Smaller step = more probes (cost) but tighter granularity.
// 25u = half a kGridResolution cell.
static constexpr float kForwardSampleStep  = 25.0f;

// Direction-tolerance fan angles (radians). Sample at the central
// pursuit vector first; if no landing, fan out. Order matters — closer
// alignment to pursuit direction wins ties.
static constexpr float kFanAngleStepRad    = 0.3927f;  // 22.5°
static constexpr int   kFanAngleSteps      = 2;        // ±22.5°, ±45°

// Cast origin lift above navigator. Floor raycast starts here and goes
// down. 100u > standard NPC eye height; safely above any geometry the
// navigator could be standing under.
static constexpr float kCastLift           = 100.0f;

// Navigator-trail retreat: minimum age of trail waypoint we'll choose.
// Picks something the navigator was at >= 1 second ago (gives it
// somewhere to walk back to, not its current location).
static constexpr uint32_t kRetreatLookbackMs = 1000;

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

// Walkable iff there's a floor within survivable Y delta of referenceY,
// and (when not eligibleForSwimming) the floor isn't submerged. Submerged
// floors are valid landings ONLY for swim-eligible navigators (AI
// Follower, NPC Invader).
static bool IsWalkable(const Vec3f& pos, float referenceY,
                       bool eligibleForSwimming, PlayState* play) {
    float fy = SampleFloorY(pos, referenceY, play);
    if (std::isnan(fy)) return false;
    float dy = referenceY - fy;
    if (std::fabs(dy) > kMaxFallDelta) return false;

    // Water gate: non-swimmers reject submerged floors. The water box
    // surface Y is checked vs floor Y; floor >= 5u below water surface
    // is "submerged" (the 5u tolerance prevents shallow puddles from
    // false-rejecting walkable terrain).
    if (!eligibleForSwimming) {
        f32       waterSurfaceY = 0.0f;
        WaterBox* waterBox      = nullptr;
        if (WaterBox_GetSurface1(play, &play->colCtx, pos.x, pos.z,
                                  &waterSurfaceY, &waterBox)) {
            if (waterSurfaceY > fy + 5.0f) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

JumpResolutionResult ResolveLedgeAhead(const Actor* navigator,
                                        const Vec3f& targetPos,
                                        const Vec3f& desiredStepPos,
                                        TrailKey     targetKey,
                                        TrailKey     navigatorKey,
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
    const bool  swimOK      = traits.eligibleForSwimming;

    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(play->sceneNum,
                                     (int8_t)play->roomCtx.curRoom.num);

    // -----------------------------------------------------------------
    // (1) SafeTerrain — desiredStepPos has walkable floor → no ledge.
    // -----------------------------------------------------------------
    if (IsWalkable(desiredStepPos, navPos.y, swimOK, play)) {
        result.kind = JumpResolution::SafeTerrain;
        return result;
    }

    // -----------------------------------------------------------------
    // (2) JumpAcross — sample a fan of forward directions (±45° from
    //     pursuit vector at 22.5° steps) at increasing distances.
    //     Each sample: floor walkability + horizontal air-clearance
    //     line-test + path-to-target check. First valid wins.
    // -----------------------------------------------------------------
    float dx = targetPos.x - navPos.x;
    float dz = targetPos.z - navPos.z;
    float distXZ = std::sqrt(dx * dx + dz * dz);

    if (distXZ > 0.001f && maxJumpDist > 0.0f) {
        float baseAngle = std::atan2(dz, dx);

        // Fan ordering: 0, +22.5, -22.5, +45, -45 — closer-aligned tested
        // first. The first match within the loop wins, so pursuit-aligned
        // landings beat off-axis ones at equal distance.
        const int   kFanCount = 2 * kFanAngleSteps + 1;
        float fanOffsets[kFanCount] = { 0.0f };
        {
            int idx = 1;
            for (int s = 1; s <= kFanAngleSteps; ++s) {
                fanOffsets[idx++] = +s * kFanAngleStepRad;
                fanOffsets[idx++] = -s * kFanAngleStepRad;
            }
        }

        for (float d = kForwardSampleStep; d <= maxJumpDist; d += kForwardSampleStep) {
            for (int f = 0; f < kFanCount; ++f) {
                float a = baseAngle + fanOffsets[f];
                float dirX = std::cos(a);
                float dirZ = std::sin(a);
                Vec3f sample = { navPos.x + dirX * d, navPos.y, navPos.z + dirZ * d };

                if (!IsWalkable(sample, navPos.y, swimOK, play)) continue;

                float landingY = SampleFloorY(sample, navPos.y, play);
                if (std::isnan(landingY)) continue;
                Vec3f landing = { sample.x, landingY, sample.z };

                // Horizontal air-clearance check: line-test from
                // navigator → landing (body-height). A jump arc can pass
                // over short obstacles, but a tall wall between the two
                // would block the jump entirely. Use MovementClear at
                // navPos.y so the test is effectively "is there a wall
                // at body height in the gap?"
                if (!MovementClearAtPosition(navPos, landing, play)) continue;

                // Path-to-target check.
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
                // Floor at this angle/distance has no path to target —
                // try the next fan offset, then the next distance.
            }
        }
    }

    // -----------------------------------------------------------------
    // (3) TrailContinues — target's breadcrumbs offer evidence the gap
    //     was crossed. Walks newest→oldest; first reachable + walkable
    //     waypoint wins.
    // -----------------------------------------------------------------
    Vec3f trailLanding = {};
    if (ActorTrail::GetInstance().FindTrailWaypointBeyondGap(
            targetKey, navPos, maxJumpDist, targetPos, trailLanding)) {
        if (IsWalkable(trailLanding, navPos.y, swimOK, play) &&
            MovementClearAtPosition(navPos, trailLanding, play)) {
            result.kind = JumpResolution::TrailContinues;
            result.landingPos = trailLanding;
            return result;
        }
    }

    // -----------------------------------------------------------------
    // (4) PathAround — graph BFS produces a path that avoids the gap.
    //     Returns the actual path (not just "exists"), so the caller
    //     can adopt it directly into its NavPath without re-querying.
    // -----------------------------------------------------------------
    if (navData != nullptr) {
        int fromIdx = ::AnchorNavRoom::FindNearestNode(navData, navPos);
        if (fromIdx >= 0) {
            std::vector<Vec3f> graphPath;
            bool ok = ::AnchorNavRoom::FindBestReachableSubgoalPath(
                navData, fromIdx, targetPos,
                traits.eligibleForSwimming,
                traits.avoidHazardNodes,
                graphPath,
                /*outFlags=*/nullptr,
                /*climbSurfaceMask=*/0,
                traits.useDropAnchors,
                (float)traits.maxDropDistance);
            if (ok && !graphPath.empty()) {
                result.kind = JumpResolution::PathAround;
                result.pathAround = std::move(graphPath);
                // Append target if line-clear from final node.
                if (MovementClearAtPosition(result.pathAround.back(), targetPos, play)) {
                    result.pathAround.push_back(targetPos);
                }
                return result;
            }
        }
    }

    // -----------------------------------------------------------------
    // (5) Retreat — pick the navigator's own oldest reachable trail
    //     waypoint (somewhere it was a moment ago). Falls back to
    //     navigator->home.pos when no trail exists.
    // -----------------------------------------------------------------
    result.kind = JumpResolution::Retreat;
    result.retreatPos = navigator->home.pos;
    if (navigatorKey != 0) {
        TrailWaypoint wp;
        if (ActorTrail::GetInstance().GetWaypointBefore(
                navigatorKey, kRetreatLookbackMs, wp)) {
            // Validate it's still in the same scene and walkable.
            if (wp.sceneNum == play->sceneNum &&
                IsWalkable(wp.pos, navPos.y, swimOK, play)) {
                result.retreatPos = wp.pos;
            }
        }
    }
    return result;
}

} // namespace AnchorNav
