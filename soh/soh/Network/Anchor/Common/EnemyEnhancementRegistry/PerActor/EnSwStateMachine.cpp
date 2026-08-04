/**
 * EnSwStateMachine — see EnSwStateMachine.h for scope + rationale.
 *
 * M4-M8 implementation covers:
 *   - Dispatch + yield-check (vanilla actionFunc-advance detection)
 *   - TickUninitialized       (6-direction basis raycast, snap-to-wall)
 *   - TickWallIdle            (hold + target-in-range → WallPursue)
 *   - TickWallPursue          (basis validate + tangent-plane motion + rot rebuild)
 *   - TickWallEdgeDrop        (gravity + landing detection → GroundPursue)
 *   - TickGroundPursue        (world-XZ direct-yaw + wall-contact detect)
 *   - TickGroundToWallReattach (basis re-raycast + WallPursue/GroundPursue transition)
 *   - LungeYield              (state entered when vanilla actionFunc != snapshot;
 *                              per-second log + 300-frame safety timeout)
 *   - SnapshotAmbient         (OnInit hook that caches vanilla actionFunc pointer)
 *
 * Diagnostic output (M8): `[EEDiag/SM] TICK ...` per-tick summary at
 * state-entry + once per second per actor; `[EEDiag/SM] EVENT ...` on
 * every state transition with previous framesInState + reason string.
 * Both formats include wallPoly + snapshot + current actionFunc
 * pointers so post-test log analysis can reconstruct full state
 * history. Toggle with `set gEnhancements.EnemyEnhancement.Diag 1`.
 */

#include "EnSwStateMachine.h"

#include "soh/Network/Anchor/Common/PlayerLookup.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GroupMovement.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // Bug 5 fix — nav JumpAnchor consumption

// functions.h + z64bgcheck.h pulled in transitively via EnSwStateMachine.h's
// z_en_sw.h -> global.h -> {functions.h, z64.h -> z64bgcheck.h}. Pitfall 40:
// do NOT wrap functions.h / z64.h directly here — they'd be no-ops due to
// include guards but the include-order rule is what keeps the pattern clean
// for future readers. BgCheck_EntityLineTest1 + COLPOLY_GET_NORMAL are both
// in scope from the transitive chain above.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <libultraship/bridge/consolevariablebridge.h>  // CVarGetInteger
#include <libultraship/libultraship.h>                 // SPDLOG_INFO

// C-side helper — force vanilla actionFunc back to combat ambient
// (func_80B0E5E0). Defined in z_en_sw.c; used by the LungeYield tick
// below to preempt vanilla's post-lunge walk-home / stop transitions.
extern "C" void EnSw_ForceAmbient(EnSw* actor);

namespace AnchorEnemyEnhancement {

namespace {

// -------------------------------------------------------------------
// Per-actor state map + helpers
// -------------------------------------------------------------------

std::unordered_map<EnSw*, EnSwEnhancedState> sStates;

EnSwEnhancedState& GetOrCreate(EnSw* self) {
    return sStates[self];
}

EnSwEnhancedState* Find(EnSw* self) {
    auto it = sStates.find(self);
    return (it == sStates.end()) ? nullptr : &it->second;
}

// -------------------------------------------------------------------
// Gravity + terminal velocity for airborne spider. Values match the
// prior GravityAdapter Phase 2 body (which is stubbed post-M1) so if
// that helper is ever revived for other wall-crawlers the physics
// stays consistent. Defined here (before SetupJumpToward) so the
// ballistic-aim formula can reference kGravityAccel.
// -------------------------------------------------------------------
constexpr float kGravityAccel   = -1.2f;
constexpr float kMaxFallSpeed   = -20.0f;

// -------------------------------------------------------------------
// JumpLunge tuning + launch-velocity helper — defined here (before
// TickWallPursue + TickGroundPursue) because rule-2 and rule-3
// triggers reference them. TickJumpLunge itself lives further down
// alongside the other TickXxx handlers.
// -------------------------------------------------------------------
constexpr int   kJumpWindupFrames    = 10;     // ~0.5 s telegraph at 20fps
constexpr float kJumpForwardSpeed    = 10.0f;  // horizontal launch magnitude
constexpr int   kJumpMaxAirFrames    = 90;     // ~4.5 s safety cap
constexpr float kJumpTriggerRange    = 300.0f; // rule 2/3 max spider→link
                                                // distance for jump trigger
constexpr float kJumpMinTriggerRange = 60.0f;  // don't jump if already at
                                                // point-blank walk range
constexpr float kJumpMinLaunchVy     = 4.0f;   // always launch with some
                                                // upward kick so the arc
                                                // is visible even for
                                                // downhill jumps
constexpr float kJumpMaxLaunchVy     = 20.0f;  // sanity cap for uphill
                                                // jumps at max trigger
                                                // range
constexpr float kJumpFallbackVy      = 8.0f;   // used when target is
                                                // directly above/below
                                                // (distXZ ~ 0)

// Populate s.jumpVel* with a BALLISTIC AIM at the target. Horizontal
// speed is fixed (kJumpForwardSpeed) and airtime N = distXZ /
// kJumpForwardSpeed frames; we solve for the initial Vy that makes
// the spider's Y equal target Y after N frames of semi-implicit-Euler
// integration used by TickJumpLunge:
//
//   pos_y(N) - pos_y(0) = N*vy_0 + g * N*(N+1)/2
//   → vy_0 = dy/N - g*(N+1)/2
//
// vy_0 is clamped to [kJumpMinLaunchVy, kJumpMaxLaunchVy] so:
//   - downhill jumps (dy < 0) still have a visible arc rather than a
//     straight-line dive; spider will overshoot target Y and continue
//     falling until it hits floor/wall (or safety timeout).
//   - uphill jumps (dy > 0) beyond the spider's launch power fall
//     short of target — trust-physics per design decision.
// jumpAirborne=false so TickJumpLunge's wind-up phase runs first.
inline void SetupJumpToward(EnSwEnhancedState& s, const Vec3f& spiderPos,
                            const Vec3f& targetPos) {
    const float dx     = targetPos.x - spiderPos.x;
    const float dz     = targetPos.z - spiderPos.z;
    const float dy     = targetPos.y - spiderPos.y;
    const float distXZ = std::sqrt(dx * dx + dz * dz);

    if (distXZ < 0.001f) {
        // Target directly overhead / underfoot — no horizontal aim
        // possible, fall back to a fixed upward kick.
        s.jumpVelX = 0.0f;
        s.jumpVelZ = 0.0f;
        s.jumpVelY = kJumpFallbackVy;
    } else {
        const float inv = 1.0f / distXZ;
        s.jumpVelX = dx * inv * kJumpForwardSpeed;
        s.jumpVelZ = dz * inv * kJumpForwardSpeed;

        const float N   = distXZ / kJumpForwardSpeed;
        float       vy0 = dy / N - kGravityAccel * (N + 1.0f) * 0.5f;
        if (vy0 < kJumpMinLaunchVy) vy0 = kJumpMinLaunchVy;
        if (vy0 > kJumpMaxLaunchVy) vy0 = kJumpMaxLaunchVy;
        s.jumpVelY = vy0;
    }
    s.jumpAirborne = false;
}

// -------------------------------------------------------------------
// WalkLunge tuning + helper. WalkLunge replaces the vanilla
// func_80B0E728 walk-lunge for enhanced GroundPursue spiders. Same
// wind-up + dash shape but implemented entirely in our namespace so
// we control shape.rot / world.pos and vanilla actionFunc stays
// pinned at ambient throughout the attack. Reuses s.jumpVelX/Z for
// dash direction (Y unused — walk-lunge is planar).
// -------------------------------------------------------------------
constexpr int   kWalkLungeWindupFrames = 10;    // ~0.5 s telegraph
constexpr int   kWalkLungeDashFrames   = 16;    // ~0.8 s dash phase
constexpr float kWalkLungeDashSpeed    = 8.0f;  // matches vanilla lunge speed

inline void SetupWalkLungeToward(EnSwEnhancedState& s,
                                  const Vec3f& spiderPos,
                                  const Vec3f& targetPos) {
    const float dx = targetPos.x - spiderPos.x;
    const float dz = targetPos.z - spiderPos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ < 0.001f) {
        s.jumpVelX = 0.0f;
        s.jumpVelZ = 0.0f;
    } else {
        const float inv = 1.0f / distXZ;
        s.jumpVelX = dx * inv * kWalkLungeDashSpeed;
        s.jumpVelZ = dz * inv * kWalkLungeDashSpeed;
    }
    s.jumpVelY     = 0.0f;   // no vertical component for walk-lunge
    s.jumpAirborne = false;  // wind-up runs first
}

// -------------------------------------------------------------------
// Detection / visibility / sticky-target helpers.
// Used by TickWallIdle, TickWallPursue, TickGroundPursue for target
// acquisition + attack-gate decisions.
// -------------------------------------------------------------------

// Target-detect radius (squared distance for cheap comparison). Used
// by TickWallIdle / TickWallPursue / TickGroundPursue / TickGround-
// ToWallReattach to gate "target close enough to pursue". Vanilla
// En_Sw uses 130u for its lunge-target-acquisition predicate; we go
// slightly higher (200u) so the spider can see the player from wall
// placement and start crawling toward them, but not so high that
// spiders across the room start migrating (previous 600u caused
// spiders on the ceiling of Deku Tree main room to descend to the
// floor whenever any player entered). Defined here (top of anon
// namespace) so IsTargetVisible + other detection helpers below can
// reference it.
constexpr float kIdleDetectRangeSq = 200.0f * 200.0f;

// Extended detect range when the target is on a climb surface (vine,
// ladder, designated wall). Vine walls in Deku Tree extend 200-800u+
// vertically — the standard 200u gate would lose the target after
// Link climbed just past spider height, causing the spider to idle at
// the vine base (Bug 3). 600u accommodates the tallest vanilla vines
// (~500u in Deku Tree main room) plus headroom.
constexpr float kClimbingTargetDetectRangeSq = 600.0f * 600.0f;

// Vanilla En_Sw picker's 3D distance gate is 130u (func_80B0DEA8 line
// 861); we treat that as the "contact-awareness" range where the
// spider can attack regardless of facing direction. Beyond 130u but
// within kIdleDetectRangeSq, vision-cone gate applies.
constexpr float kVanillaDetectRangeSq = 130.0f * 130.0f;

// Minimum floor for skelAnime.playSpeed when animation is active but
// the spider is moving slowly (Bug 2). Below ~0.15 the leg cycle is
// so slow it reads as "stuck moving"; this ensures barely-moving
// spiders still show a visible slow crawl.
constexpr float kMinAnimPlaySpeed = 0.15f;

// Max vertical reach for a JumpLunge — at kJumpMaxLaunchVy = 20 and
// kGravityAccel = -1.2, theoretical peak is Vy²/(2|g|) = 400/2.4 ≈ 167u.
// We gate at 120u to leave headroom so triggered jumps have a real
// chance of hitting rather than peaking exactly at target Y with no
// energy to spare.
constexpr float kMaxJumpHeightUp = 120.0f;

// Sticky-target grace — spider keeps pursuing / attacking against the
// last-known target for this many frames after target lookup fails
// (rare in practice; mostly scene-transition frames where DummyPlayer
// list is briefly empty). Reference model: NPC Invader's OnTick sticky
// re-eval logic in InvaderDescriptor.cpp.
constexpr int kStickyGraceFrames = 30;

// Idle gaze rotation — ground spider slowly turns yaw between random
// targets when not pursuing / attacking, in a two-phase rest/look
// cycle (see UpdateIdleGaze). Wall spider handled by vanilla
// func_80B0E5E0's shape.rot.z random-gaze mechanism (correct axis
// for wall orientation), so we don't touch wall idle here.
constexpr s16 kIdleGazeStepScale = 8;
constexpr s16 kIdleGazeStepMax   = 0x100; // ~1.4° per frame — slow sweep

// Sticky target lookup — returns cached target if lookup fails this
// tick, up to kStickyGraceFrames of persistence. Reference: NPC
// Invader targetClientId re-eval pattern.
inline Actor* GetStickyTarget(EnSwEnhancedState& s, EnSw* self,
                              PlayState* play) {
    Actor* current = FindNearestPlayerActor(&self->actor, play);
    if (current != nullptr) {
        s.stickyTarget     = current;
        s.stickyLossFrames = 0;
        return current;
    }
    if (s.stickyTarget != nullptr) {
        s.stickyLossFrames++;
        if (s.stickyLossFrames > kStickyGraceFrames ||
            s.stickyTarget->update == nullptr) {
            s.stickyTarget = nullptr;
            return nullptr;
        }
        return s.stickyTarget;
    }
    return nullptr;
}

// 3D squared distance spider→target.
inline float Dist3DSq(const EnSw* spider, const Actor* target) {
    const float dx = target->world.pos.x - spider->actor.world.pos.x;
    const float dy = target->world.pos.y - spider->actor.world.pos.y;
    const float dz = target->world.pos.z - spider->actor.world.pos.z;
    return dx * dx + dy * dy + dz * dz;
}

// Vision cone gate — within 130u 3D always visible. Beyond that, up
// to kIdleDetectRangeSq (200u), require target within a 120° cone
// centered on spider's forward direction (dot > 0.5 = cos(60°)).
//
// Forward direction derivation is state-aware:
//   - Wall states: use s.wallTangentU (the wall-walk direction stored
//     by TickWallPursue's motion block + TryEstablishBasis). shape.rot
//     is a YXZ-Euler extraction from RebuildWorldRotFromWallBasis's
//     compound rotation, so shape.rot.y does NOT map cleanly to XZ
//     facing for wall spiders. Using wallTangentU directly is correct.
//   - Ground/airborne/other: derive from shape.rot.y (correct because
//     our ground orientation (-0x4000, yaw, 0) has shape.rot.y as the
//     true XZ yaw).
//
// Cone width tightened from 180° hemisphere (dot > 0) to 120° cone
// (dot_normalized > 0.5): field test 794 showed the hemisphere let
// Link at ~85° off spider's forward trigger attacks — geometrically
// "in front" but user-perceived as "behind/side."
inline bool IsTargetVisible(const EnSw* spider, const Actor* target,
                            const EnSwEnhancedState& s) {
    const float d2 = Dist3DSq(spider, target);
    if (d2 > kIdleDetectRangeSq)     return false;  // beyond max detect
    if (d2 <= kVanillaDetectRangeSq) return true;   // always visible close

    // Spider forward in XZ (state-aware).
    float fwdX, fwdZ;
    if (s.state == EnSwState::WallIdle || s.state == EnSwState::WallPursue) {
        fwdX = s.wallTangentU.x;
        fwdZ = s.wallTangentU.z;
    } else {
        const float yawRad =
            spider->actor.shape.rot.y * (float)(M_PI / 0x8000);
        fwdX = std::sin(yawRad);
        fwdZ = std::cos(yawRad);
    }
    // Normalize forward XZ (wallTangentU may have Y component; drop it).
    const float fwdMag = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
    if (fwdMag < 0.001f) return true;  // degenerate — pass conservatively
    fwdX /= fwdMag;
    fwdZ /= fwdMag;

    // Delta unit-XZ.
    const float dx     = target->world.pos.x - spider->actor.world.pos.x;
    const float dz     = target->world.pos.z - spider->actor.world.pos.z;
    const float delMag = std::sqrt(dx * dx + dz * dz);
    if (delMag < 0.001f) return true;  // directly overhead — pass
    const float dxN = dx / delMag;
    const float dzN = dz / delMag;

    // 120° cone: cos(60°) = 0.5.
    return (fwdX * dxN + fwdZ * dzN) > 0.5f;
}

// Jump-height gate — only jump if target is at or below spider Y,
// or above by no more than kMaxJumpHeightUp. Physics-based misses on
// horizontal aim are OK (trust-physics per design), but a jump toward
// an unreachable-above target has 0% chance of hitting.
inline bool IsTargetJumpReachable(const EnSw* spider, const Actor* target) {
    return target->world.pos.y - spider->actor.world.pos.y
           <= kMaxJumpHeightUp;
}

// Bug 6 fix (2026-07-31) — direct line-of-flight raycast from spider
// pos to target pos. Returns TRUE if line is clear of walls / ceilings
// / floors / dyna. Applied as an additional gate before triggering
// attack-style JumpLunge. Conservative approximation: ballistic arc
// curves upward, so a CLEAR straight line doesn't guarantee the arc
// clears (arc may exceed line envelope going up), but a BLOCKED line
// GUARANTEES the arc is blocked. This catches the "Link on unreachable
// platform above / behind overhang" case where the physics ceiling
// (kMaxJumpHeightUp=120u) says the jump is possible but geometry blocks
// the arc.
//
// Anchor-driven jumps EXEMPT — nav JumpAnchors are scan-time arc-
// verified (RoomNavData.cpp:4188-4202 arc-samples the parabolic path
// and rejects anchors whose arc hits geometry). If an anchor exists,
// trust it.
inline bool IsLineOfFlightClear(PlayState* play,
                                 const Vec3f& fromPos, const Vec3f& toPos) {
    Vec3f a = fromPos;
    Vec3f b = toPos;
    // Aim at target's mid-body / mid-height so the ray isn't a floor-
    // hugger. Small +Y shift on both endpoints; matches Player LoS
    // convention in TickGroundPursue (Rule 3 losFrom/losTo +20).
    a.y += 15.0f;
    b.y += 15.0f;
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = { 0.0f, 0.0f, 0.0f };
    const bool blocked = BgCheck_EntityLineTest1(
        &play->colCtx, &a, &b, &hitPos, &poly, 1, 1, 1, 0, &bgId);
    return !blocked;
}

// Bug 5 fix (2026-07-31) — query the room's nav JumpAnchors for one
// whose "far endpoint" (the endpoint NOT close to spider) meaningfully
// progresses toward the target. Returns TRUE + writes *outLanding if
// found. False if no useful anchor. Bail cheap if room has no nav data.
//
// Design notes:
//   - JumpAnchors are BIDIRECTIONAL (fromPos↔toPos pair). Spider may
//     be near EITHER endpoint; pick the nearer as "source", the
//     other as "landing".
//   - kAnchorProximity gates "spider is at this anchor" — chosen 60u
//     to allow spider approaching-but-not-yet-on the anchor to still
//     trigger, without hijacking anchors on the far side of the room.
//   - Progress threshold 0.7× ensures the jump meaningfully shortens
//     distance to target (rejects lateral / away-facing anchors).
//   - Y-delta check: landing must be within kMaxJumpHeightUp above
//     current pos (physics ceiling), matches IsTargetJumpReachable.
inline bool FindJumpAnchorTowardTarget(EnSw* self, PlayState* play,
                                        const Vec3f& targetPos,
                                        Vec3f* outLanding) {
    if (play == nullptr || outLanding == nullptr) return false;
    const AnchorNavRoom::RoomNavData* data =
        AnchorNavRoom::GetForRoom(play->sceneNum,
                                    play->roomCtx.curRoom.num);
    if (data == nullptr || data->jumpAnchors.empty()) return false;

    constexpr float kAnchorProximity = 60.0f;
    constexpr float kProgressThreshold = 0.7f;

    const Vec3f& sp = self->actor.world.pos;
    const float curDistToTargetSq =
        (targetPos.x - sp.x) * (targetPos.x - sp.x) +
        (targetPos.z - sp.z) * (targetPos.z - sp.z);
    if (curDistToTargetSq < 100.0f) return false;  // already at target

    bool  found = false;
    float bestProgressSq = curDistToTargetSq * kProgressThreshold *
                            kProgressThreshold;
    Vec3f bestLanding = { 0.0f, 0.0f, 0.0f };

    for (const AnchorNavRoom::JumpAnchor& a : data->jumpAnchors) {
        // Which endpoint is near spider? Pick the closer one as "source"
        // (implicit — we care about the OTHER endpoint as "landing").
        const float dxA = a.fromPos.x - sp.x;
        const float dzA = a.fromPos.z - sp.z;
        const float distFromSq = dxA * dxA + dzA * dzA;
        const float dxB = a.toPos.x - sp.x;
        const float dzB = a.toPos.z - sp.z;
        const float distToSq = dxB * dxB + dzB * dzB;

        Vec3f landing;
        if (distFromSq <= distToSq) {
            if (distFromSq > kAnchorProximity * kAnchorProximity) continue;
            landing = a.toPos;
        } else {
            if (distToSq > kAnchorProximity * kAnchorProximity) continue;
            landing = a.fromPos;
        }

        // Y-delta gate — landing must be within physics ceiling.
        if (landing.y - sp.y > kMaxJumpHeightUp) continue;

        // Progress gate — landing must meaningfully close distance to
        // target vs staying put.
        const float landDx = targetPos.x - landing.x;
        const float landDz = targetPos.z - landing.z;
        const float landDistSq = landDx * landDx + landDz * landDz;
        if (landDistSq < bestProgressSq) {
            bestProgressSq = landDistSq;
            bestLanding = landing;
            found = true;
        }
    }

    if (found) *outLanding = bestLanding;
    return found;
}

// Idle gaze rotation — vanilla-style two-phase cycle for the ground
// spider "looking for prey":
//   Rest phase: sit still (no yaw change, no leg animation) until
//     the scheduled next-look frame.
//   Look phase: pick a random target yaw ±90° from current facing,
//     smooth-step toward it. Legs animate while rotating. When
//     arrived, schedule the next look after a random 10-40-frame
//     rest (matches vanilla func_80B0E5E0 unk_388 =
//     Rand_S16Offset(10, 30) cadence).
inline void UpdateIdleGaze(EnSwEnhancedState& s, EnSw* self,
                            PlayState* play) {
    const int now = (int)play->gameplayFrames;

    // Rest phase — wait until the scheduled next look, then arm
    // the look phase with a fresh target yaw.
    if (!s.idleGazeIsLooking) {
        if (now < s.idleGazeNextChangeFrame) return;  // still resting
        const s16 offset =
            (s16)((Rand_ZeroOne() - 0.5f) * 0x8000);  // ±90°
        s.idleGazeTargetYaw = self->actor.world.rot.y + offset;
        s.idleGazeIsLooking = true;
        // fall through into first look-phase step this tick
    }

    // Look phase — smooth-step yaw toward target; walk-anim active
    // whenever yaw actually changed this tick.
    const s16 preYaw = self->actor.world.rot.y;
    // minStep MUST be non-zero (was 0 pre-fix). vanilla Math_SmoothStepToS
    // (z_lib.c:514): when `diff/scale` rounds to 0, it falls into the
    // else branch and adds ±minStep. With minStep=0 that adds 0 →
    // yaw stalls within `scale-1` units of target and never converges
    // → `yaw == target` check below never fires → isLooking = true
    // forever, spider stuck in look phase with no visible rotation
    // (log 797: rot.y drifted then locked at -19917 for 17+ seconds).
    // minStep=1 guarantees convergence within a few ticks of the
    // small-delta regime.
    Math_SmoothStepToS(&self->actor.world.rot.y, s.idleGazeTargetYaw,
                        kIdleGazeStepScale, kIdleGazeStepMax, 1);
    if (self->actor.world.rot.y != preYaw) {
        s.isWalkAnimActive = true;
        // Bug 2 fix — scale playSpeed by rotation delta. Small rotations
        // (near-target smooth-step converging in single-step increments)
        // get proportionally slow leg animation; large rotations get
        // near-max intensity. kIdleGazeStepMax is the smooth-step cap
        // per frame — used as the "max rotation for full anim" divisor.
        const int diff = std::abs((int)self->actor.world.rot.y - (int)preYaw);
        const float rate = (float)diff / (float)kIdleGazeStepMax;
        s.animMotionRate = std::max(s.animMotionRate,
                                     std::min(rate, 1.0f));
    }

    // Reached target — go back to rest with random duration.
    if (self->actor.world.rot.y == s.idleGazeTargetYaw) {
        s.idleGazeIsLooking       = false;
        s.idleGazeNextChangeFrame =
            now + 10 + (int)(Rand_ZeroOne() * 30.0f);  // 10..40 frames
    }
}

// -------------------------------------------------------------------
// Tuning constants
// -------------------------------------------------------------------

// Basis-raycast probe range from actor.pos in each cardinal direction.
// Combat En_Sw is placed within body-thickness of the wall, so 100u is
// generously wide; 40-60u would likely also work but 100u tolerates
// scene-authoring slop.
constexpr float kBasisProbeRange = 100.0f;

// A poly is treated as a wall (not floor/ceiling) when |normal.y| is
// below this threshold. 0.5 corresponds to ~30° slope tolerance —
// anything shallower is a slope, not a wall.
constexpr float kWallNormalYThreshold = 0.5f;

// (kIdleDetectRangeSq relocated to top of anonymous namespace so
//  IsTargetVisible + related helpers can reference it. See the
//  detection-helpers block near the top of this file.)

// Body-offset distance perpendicular to the surface (both ground and
// wall). Vanilla En_Sw's body naturally sits FLUSH against the wall
// (belly touching wall). Under our enhancement, we lift the body off
// the surface so legs are visible extending from body to surface —
// looks more like an actual spider clinging to / walking on the
// surface, less like a decal.
//
// Applied at three sites:
//   - TryEstablishBasis (initial wall attach)
//   - TickWallEdgeDrop (landing snap)
//   - TickGroundPursue (per-tick ground-follow)
//
// Companion: EnSwDescriptor::OverrideLimbBend applies leg-bend pitch
// so leg tips visually reach the surface across the offset gap.
constexpr float kBodySurfaceOffset = 8.0f;

// 2026-08-04 (user "surface-offset is too little for larger swap-
// spawned En_Sw") — scale the body-off-surface distance with the
// actor's visual scale. Vanilla En_Sw sits at scale 0.02 with 8u
// offset (kBodySurfaceOffset / vanillaScale = 400u-per-unit-scale
// ratio). Swap-spawned En_Sw at scale 0.06 needs 24u offset for
// leg-tips to visually reach the surface. Linear scaling per
// user spec.
inline float BodySurfaceOffsetFor(EnSw* self) {
    if (self == nullptr) return kBodySurfaceOffset;
    constexpr float kVanillaScale = 0.02f;
    return kBodySurfaceOffset * (self->actor.scale.x / kVanillaScale);
}

// Wall-base floor-detection probe. TickWallPursue casts a short ray
// straight down from the actor position to detect when the spider has
// walked to a vertical wall's bottom edge (floor level). Without this,
// ValidateBasis's horizontal probe re-hits the wall poly at floor
// level and the state machine gets stuck in WALL_PURSUE indefinitely
// (analysis 2026-07-30, log 770). Probe extends kWallBaseFloorProbe
// units downward; a floor hit within kGroundContactThreshold of the
// actor's Y counts as "at wall base."
constexpr float kWallBaseFloorProbe     = 20.0f;
constexpr float kGroundContactThreshold = 10.0f;

// -------------------------------------------------------------------
// Diagnostic
// -------------------------------------------------------------------
// Reuses the EnemyEnhancement.Diag CVar (already gates NavConsumer's
// [EEDiag] output). Extended in M8 with per-state fields; for M4 we
// just log state transitions.

constexpr const char* kDiagCVar = "gEnhancements.EnemyEnhancement.Diag";

bool DiagEnabled() {
    return CVarGetInteger(kDiagCVar, 0) != 0;
}

const char* StateName(EnSwState s) {
    switch (s) {
        case EnSwState::Uninitialized:        return "UNINIT";
        case EnSwState::WallIdle:             return "WALL_IDLE";
        case EnSwState::WallPursue:           return "WALL_PURSUE";
        case EnSwState::WallEdgeDrop:         return "WALL_EDGE_DROP";
        case EnSwState::GroundPursue:         return "GROUND_PURSUE";
        case EnSwState::GroundToWallReattach: return "GROUND_TO_WALL";
        case EnSwState::LungeYield:           return "LUNGE_YIELD";
        case EnSwState::JumpLunge:            return "JUMP_LUNGE";
        case EnSwState::WalkLunge:            return "WALK_LUNGE";
        case EnSwState::PermanentlyDisabled:  return "DISABLED";
    }
    return "?";
}

// Whether OUR state machine (not vanilla) owns skelAnime.playSpeed for
// the given state. Post-tick playSpeed write is applied ONLY when
// this returns true. For states like WallIdle where vanilla
// func_80B0E5E0 runs its own rest/look rotation cycle with self-managed
// playSpeed (z_en_sw.c:960-985), our unconditional write would clobber
// vanilla's animation (Bug 1 root cause). Yielding to vanilla for
// those states preserves the vanilla rotation-anim + look-phase
// completion path.
//
// States where WE own animation control:
//   WallPursue     — we drive world.pos writes directly along tangent
//   GroundPursue   — we drive world.pos + orientation each frame
//   WalkLunge      — custom ground dash, we own motion + rotation
//   JumpLunge      — custom ballistic, we own motion + rotation
//   WallEdgeDrop / GroundToWallReattach / LungeYield / Uninitialized
//                  — transitional, vanilla ambient runs; still returns
//                    false so vanilla can drive its own cycle
//
// States where VANILLA owns animation control:
//   WallIdle       — vanilla func_80B0E5E0 runs rest/look cycle
//   PermanentlyDisabled — vanilla drives entire actor
bool IsAnimAuthoritative(EnSwState s) {
    switch (s) {
        case EnSwState::WallPursue:
        case EnSwState::GroundPursue:
        case EnSwState::WalkLunge:
        case EnSwState::JumpLunge:
            return true;
        default:
            return false;
    }
}

// Emit a transition event. Includes the previous framesInState so log
// analysis can measure state dwell times. Includes wallPoly pointer so
// correlated tick-state lines can be joined on that address.
void LogTransition(EnSw* self, const EnSwEnhancedState& s,
                   EnSwState from, EnSwState to, const char* reason) {
    if (!DiagEnabled()) return;
    SPDLOG_INFO("[EEDiag/SM] EVENT actor=0x{:x} {} -> {} reason={} "
                "prevFrames={} pos=({:.0f},{:.0f},{:.0f}) "
                "wallPoly=0x{:x} snapshot=0x{:x} curActionFunc=0x{:x}",
                (uintptr_t)self, StateName(from), StateName(to), reason,
                s.framesInState,
                self->actor.world.pos.x, self->actor.world.pos.y, self->actor.world.pos.z,
                (uintptr_t)s.wallPoly,
                (uintptr_t)s.initialAmbientActionFunc,
                (uintptr_t)self->actionFunc);
}

// Emit a per-tick state summary. Rate-limited to state-entry
// (framesInState == 1) + once per second (every 20 frames at 20fps
// game tick). Comprehensive field set so grep + join over a session's
// log can reconstruct full state history without needing to re-run.
void LogTickState(EnSw* self, const EnSwEnhancedState& s, const char* note) {
    if (!DiagEnabled()) return;
    // Log on first tick after transition + every 20 frames after so
    // state-entry data is always captured (previously first log fired
    // at framesInState=0 in some handlers and =1 in others; consistent
    // "entry + rate-limited steady" is easier to read).
    if (s.framesInState > 1 && (s.framesInState % 20) != 0) return;

    SPDLOG_INFO("[EEDiag/SM] TICK actor=0x{:x} state={} framesInState={} "
                "hasBasis={} wallNormal=({:.2f},{:.2f},{:.2f}) "
                "wallPoly=0x{:x} pos=({:.0f},{:.0f},{:.0f}) "
                "vel=({:.2f},{:.2f},{:.2f}) rot=({},{},{}) "
                "snapshot=0x{:x} curActionFunc=0x{:x} vanillaAmbient={} "
                "note={}",
                (uintptr_t)self, StateName(s.state), s.framesInState,
                s.hasWallBasis ? 1 : 0,
                s.wallNormal.x, s.wallNormal.y, s.wallNormal.z,
                (uintptr_t)s.wallPoly,
                self->actor.world.pos.x, self->actor.world.pos.y, self->actor.world.pos.z,
                self->actor.velocity.x, self->actor.velocity.y, self->actor.velocity.z,
                self->actor.world.rot.x, self->actor.world.rot.y, self->actor.world.rot.z,
                (uintptr_t)s.initialAmbientActionFunc,
                (uintptr_t)self->actionFunc,
                (s.haveAmbientSnapshot &&
                 self->actionFunc == s.initialAmbientActionFunc) ? 1 : 0,
                note);
}

void TransitionTo(EnSw* self, EnSwEnhancedState& s, EnSwState next, const char* reason) {
    if (s.state == next) return;
    LogTransition(self, s, s.state, next, reason);
    s.state = next;
    s.framesInState = 0;
}

// -------------------------------------------------------------------
// Basis math
// -------------------------------------------------------------------

// Cross product: r = a x b.
inline void Cross(const Vec3f& a, const Vec3f& b, Vec3f* r) {
    r->x = a.y * b.z - a.z * b.y;
    r->y = a.z * b.x - a.x * b.z;
    r->z = a.x * b.y - a.y * b.x;
}

inline float Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool Normalize(Vec3f* v) {
    const float m = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
    if (m < 0.001f) return false;
    const float inv = 1.0f / m;
    v->x *= inv;
    v->y *= inv;
    v->z *= inv;
    return true;
}

// Build tangent basis from surface normal. tangentU is chosen as world-
// up projected into the tangent plane (falls back to world-Z reference
// for near-horizontal walls). tangentV = normal x tangentU.
// Returns true on success, false if basis is degenerate.
bool BuildTangentBasis(const Vec3f& normal, Vec3f* tangentU, Vec3f* tangentV) {
    Vec3f up = {0.0f, 1.0f, 0.0f};
    if (std::fabs(Dot(normal, up)) > 0.98f) {
        up = {0.0f, 0.0f, 1.0f};  // near-ceiling/floor edge; use world-Z reference
    }
    const float dotNU = Dot(normal, up);
    tangentU->x = up.x - normal.x * dotNU;
    tangentU->y = up.y - normal.y * dotNU;
    tangentU->z = up.z - normal.z * dotNU;
    if (!Normalize(tangentU)) return false;

    Cross(normal, *tangentU, tangentV);
    if (!Normalize(tangentV)) return false;
    return true;
}

// -------------------------------------------------------------------
// Cardinal-direction basis raycast
// -------------------------------------------------------------------

struct RaycastHit {
    bool           hit = false;
    Vec3f          pos = {0.0f, 0.0f, 0.0f};
    Vec3f          normal = {0.0f, 0.0f, 0.0f};
    CollisionPoly* poly = nullptr;
};

// Cast one ray from actor.pos to actor.pos + direction * range. Returns
// hit info; hit.hit == false when no wall (`|normal.y| < threshold`)
// was found along the ray.
RaycastHit CastForWall(EnSw* self, PlayState* play, const Vec3f& direction, float range) {
    RaycastHit result;
    Vec3f from = self->actor.world.pos;
    Vec3f to = {
        from.x + direction.x * range,
        from.y + direction.y * range,
        from.z + direction.z * range,
    };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = {0.0f, 0.0f, 0.0f};

    // BgCheck_EntityLineTest1 args: colCtx, posA, posB, posResult,
    // outPoly, chkWall, chkFloor, chkCeil, chkOneFace, bgId. Return
    // non-zero on hit.
    if (!BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hitPos, &poly,
                                  /*chkWall*/ 1, /*chkFloor*/ 1, /*chkCeil*/ 1,
                                  /*chkOneFace*/ 0, &bgId)) {
        return result;  // no hit at all
    }
    if (poly == nullptr) return result;

    Vec3f n = {
        COLPOLY_GET_NORMAL(poly->normal.x),
        COLPOLY_GET_NORMAL(poly->normal.y),
        COLPOLY_GET_NORMAL(poly->normal.z),
    };
    if (std::fabs(n.y) >= kWallNormalYThreshold) {
        return result;  // floor or ceiling, not a wall
    }

    result.hit = true;
    result.pos = hitPos;
    result.normal = n;
    result.poly = poly;
    return result;
}

// Rebuild actor.world.rot from a WALL tangent basis. Matches vanilla
// En_Sw's func_80B0BE20 basis-matrix layout (z_en_sw.c:133-149):
//   Column 0 = tangent-forward (walk direction along wall)
//   Column 1 = wall normal (spider's local +Y — model's SKULL direction,
//              faces away from wall into open room)
//   Column 2 = tangent-perpendicular (fwd x normal — "up along wall")
// Y-X-Z Euler extraction via Matrix_MtxFToYXZRotS.
//
// This convention only produces natural-looking orientation on VERTICAL
// surfaces where the wall normal is horizontal and the spider's skull
// (local +Y) faces horizontally into the room. Do NOT use for floor
// surfaces where the normal is world-up — the skull would point
// straight up. For ground use RebuildWorldRotFromGroundBasis below.
void RebuildWorldRotFromBasis(EnSw* self, const Vec3f& fwd, const Vec3f& normal) {
    Vec3f perp;
    Cross(fwd, normal, &perp);
    if (!Normalize(&perp)) return;  // degenerate — leave rot alone

    MtxF basis;
    basis.xx = fwd.x;    basis.yx = fwd.y;    basis.zx = fwd.z;    basis.wx = 0.0f;
    basis.xy = normal.x; basis.yy = normal.y; basis.zy = normal.z; basis.wy = 0.0f;
    basis.xz = perp.x;   basis.yz = perp.y;   basis.zz = perp.z;   basis.wz = 0.0f;
    basis.xw = 0.0f;     basis.yw = 0.0f;     basis.zw = 0.0f;     basis.ww = 1.0f;

    Matrix_MtxFToYXZRotS(&basis, &self->actor.world.rot, 0);
    // Sync shape.rot directly — no smooth-step (vanilla's smooth-step
    // at z_en_sw.c:244-246 lives inside func_80B0C0CC which only runs
    // during vanilla wall-crawl action funcs; our state machine
    // bypasses those, so shape.rot has no smoothing driver otherwise).
    self->actor.shape.rot = self->actor.world.rot;
}

// Rebuild actor.world.rot from a GROUND (floor / slope) basis.
//
// Second iteration: previous attempt put fwd in col1 (assuming model's
// local +Y = skull) — resulted in "spider walks backwards" (butt toward
// target). Correction: use the standard OoT actor convention where a
// rot=(0,0,0) actor faces +Z direction. That means model's skull is at
// local +Z. So put fwd in col2 (skull along walk direction), normal in
// col1 (up above head), and right in col0.
//
// Basis columns:
//   Column 0 = right-side (normal x fwd)   — model's local +X (right)
//   Column 1 = normal (up from surface)    — model's local +Y (above head)
//   Column 2 = fwd (walk direction)        — model's local +Z (skull front)
//
// For flat floor + facing north (yaw=0): fwd=(0,0,1), normal=(0,1,0),
// right = normal × fwd = (1,0,0). Basis is the identity matrix, which
// decomposes to rot=(0,0,0) — exactly what "spider facing north with
// belly down" should produce.
//
// Handedness: `right = normal × fwd`. Verify: right × normal =
// (normal × fwd) × normal = fwd * (normal·normal) - normal * (fwd·normal)
// = fwd - 0 = fwd = col2 ✓. Right-handed.
//
// If this iteration ALSO produces wrong orientation (e.g., spider
// walks sideways or upside down), the next hypothesis is that vanilla
// En_Sw's model was authored with a 90° offset (Init at z_en_sw.c:291
// adds `+ 0x4000` to yaw when computing tangent-X, hinting at a non-
// standard model orientation). If so, we'd need to apply an additional
// yaw offset here — but let's field-test this first.
// Rebuild actor.world.rot to orient the En_Sw on a WALL surface such
// that its "belly" is against the wall and its skull-face points along
// walk direction. Analogous to the ground orientation but with the
// surface's normal replacing the world-up axis.
//
// Model orientation established from ground fix (iteration 4):
//   Skull  at local -Y  (front of body)
//   Belly  at local -Z  (bottom of body / touches surface)
//   Dorsal at local +Z  (top of body / opposite belly)
//   Right  at local +X
//   (Back  at local +Y, opposite skull)
//
// For wall placement:
//   R * (0,-1,0) = fwd           (skull → walk direction on wall)
//   R * (0,0,-1) = -wall_normal  (belly → into wall)
//   R * (1,0,0)  = wall_normal × fwd   (right side; right-hand rule)
//
// Basis columns (col i = R applied to local +i axis):
//   Col 0 (local +X = right)  = wall_normal × fwd
//   Col 1 (local +Y = back)   = -fwd
//   Col 2 (local +Z = dorsal) = wall_normal
//
// Right-handed check: col0 × col1 = (wall_normal × fwd) × (-fwd)
//   = -(fwd × (wall_normal × fwd))  ... wait, use triple identity:
//   (A × B) × C = B(A·C) - A(B·C)
//   (wall_normal × fwd) × (-fwd) = -[fwd(wall_normal·fwd) - wall_normal(fwd·fwd)]
//   = -[0 - wall_normal] = wall_normal = col2  ✓
void RebuildWorldRotFromWallBasis(EnSw* self, const Vec3f& fwd, const Vec3f& wall_normal) {
    Vec3f right;
    Cross(wall_normal, fwd, &right);
    if (!Normalize(&right)) return;  // degenerate (fwd parallel to normal)

    MtxF basis;
    basis.xx = right.x;         basis.yx = right.y;         basis.zx = right.z;         basis.wx = 0.0f;
    basis.xy = -fwd.x;          basis.yy = -fwd.y;          basis.zy = -fwd.z;          basis.wy = 0.0f;
    basis.xz = wall_normal.x;   basis.yz = wall_normal.y;   basis.zz = wall_normal.z;   basis.wz = 0.0f;
    basis.xw = 0.0f;            basis.yw = 0.0f;            basis.zw = 0.0f;            basis.ww = 1.0f;

    Matrix_MtxFToYXZRotS(&basis, &self->actor.world.rot, 0);
    self->actor.shape.rot = self->actor.world.rot;
}

void RebuildWorldRotFromGroundBasis(EnSw* self, const Vec3f& fwd, const Vec3f& normal) {
    Vec3f right;
    Cross(normal, fwd, &right);
    if (!Normalize(&right)) return;  // degenerate — leave rot alone

    MtxF basis;
    basis.xx = right.x;  basis.yx = right.y;  basis.zx = right.z;  basis.wx = 0.0f;
    basis.xy = normal.x; basis.yy = normal.y; basis.zy = normal.z; basis.wy = 0.0f;
    basis.xz = fwd.x;    basis.yz = fwd.y;    basis.zz = fwd.z;    basis.wz = 0.0f;
    basis.xw = 0.0f;     basis.yw = 0.0f;     basis.zw = 0.0f;     basis.ww = 1.0f;

    Matrix_MtxFToYXZRotS(&basis, &self->actor.world.rot, 0);
    self->actor.shape.rot = self->actor.world.rot;
}

// Per-frame basis validation. Casts a short ray from
// actor.pos + wallNormal*5 toward actor.pos - wallNormal*20 to see if
// the wall we cached is still there. Returns:
//   0 — same poly (basis still valid)
//   1 — different poly with wall-like normal (crossed to adjacent wall)
//   2 — no wall hit (walked off edge → caller should transition to
//       WallEdgeDrop / re-Uninitialized)
enum class BasisValidateResult { SamePoly, DifferentWall, Lost };

BasisValidateResult ValidateBasis(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    if (!s.hasWallBasis) return BasisValidateResult::Lost;

    Vec3f from = {
        self->actor.world.pos.x + s.wallNormal.x * 5.0f,
        self->actor.world.pos.y + s.wallNormal.y * 5.0f,
        self->actor.world.pos.z + s.wallNormal.z * 5.0f,
    };
    Vec3f to = {
        self->actor.world.pos.x - s.wallNormal.x * 20.0f,
        self->actor.world.pos.y - s.wallNormal.y * 20.0f,
        self->actor.world.pos.z - s.wallNormal.z * 20.0f,
    };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = {0.0f, 0.0f, 0.0f};

    if (!BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hitPos, &poly,
                                  1, 1, 1, 0, &bgId)) {
        return BasisValidateResult::Lost;
    }
    if (poly == nullptr) return BasisValidateResult::Lost;
    if (poly == s.wallPoly) return BasisValidateResult::SamePoly;

    // Different poly — check if it's still a wall.
    const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
    if (std::fabs(ny) >= kWallNormalYThreshold) {
        return BasisValidateResult::Lost;  // adjacent surface is floor/ceiling
    }

    // Different wall — update cached basis.
    Vec3f n = {
        COLPOLY_GET_NORMAL(poly->normal.x),
        COLPOLY_GET_NORMAL(poly->normal.y),
        COLPOLY_GET_NORMAL(poly->normal.z),
    };
    Vec3f tanU, tanV;
    if (!BuildTangentBasis(n, &tanU, &tanV)) {
        return BasisValidateResult::Lost;
    }
    s.wallNormal = n;
    s.wallTangentU = tanU;
    s.wallTangentV = tanV;
    s.wallPoly = poly;
    return BasisValidateResult::DifferentWall;
}

// Attempt to establish wall basis via 6-direction cardinal raycast.
// Returns true on success (writes s.wallNormal / tangentU / tangentV /
// wallPoly / hasWallBasis). Returns false if no wall found in any
// direction — caller should transition to PermanentlyDisabled.
bool TryEstablishBasis(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    // Cardinal directions: ±X, ±Y, ±Z. Order matters only for tie-
    // breaking on multi-wall placements; horizontal walls (±X/±Z)
    // checked first because they're the common case for wall-crawling
    // spiders in dungeons.
    const Vec3f dirs[6] = {
        {  1.0f, 0.0f,  0.0f },
        { -1.0f, 0.0f,  0.0f },
        {  0.0f, 0.0f,  1.0f },
        {  0.0f, 0.0f, -1.0f },
        {  0.0f, 1.0f,  0.0f },  // ceiling
        {  0.0f,-1.0f,  0.0f },  // floor-underside (spider hanging?)
    };

    for (int i = 0; i < 6; i++) {
        RaycastHit h = CastForWall(self, play, dirs[i], kBasisProbeRange);
        if (!h.hit) continue;

        // Wall found. Cache basis + snap to hit position with small
        // outward offset to avoid clipping into the wall.
        s.wallNormal = h.normal;
        Vec3f tanU, tanV;
        if (!BuildTangentBasis(h.normal, &tanU, &tanV)) {
            continue;  // degenerate basis; try next direction
        }
        s.wallTangentU = tanU;
        s.wallTangentV = tanV;
        s.wallPoly = h.poly;
        s.hasWallBasis = true;

        // Snap actor to hit point with body offset outward from wall
        // (see kBodySurfaceOffset — body floats away from wall, legs
        // visibly reach across the gap to the wall surface).
        self->actor.world.pos.x = h.pos.x + h.normal.x * BodySurfaceOffsetFor(self);
        self->actor.world.pos.y = h.pos.y + h.normal.y * BodySurfaceOffsetFor(self);
        self->actor.world.pos.z = h.pos.z + h.normal.z * BodySurfaceOffsetFor(self);

        if (DiagEnabled()) {
            SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} basis established via dir[{}] "
                        "hitPos=({:.0f},{:.0f},{:.0f}) normal=({:.2f},{:.2f},{:.2f}) "
                        "tanU=({:.2f},{:.2f},{:.2f}) tanV=({:.2f},{:.2f},{:.2f})",
                        (uintptr_t)self, i,
                        h.pos.x, h.pos.y, h.pos.z,
                        h.normal.x, h.normal.y, h.normal.z,
                        tanU.x, tanU.y, tanU.z,
                        tanV.x, tanV.y, tanV.z);
        }
        return true;
    }
    return false;
}

// Detect-range gate that extends when the target is on a climb
// surface (vine, ladder, designated wall). Vine walls in Deku Tree
// extend 200-800u+ vertically — without the extension the spider
// loses target after Link climbs past ~200u up and idles at the
// vine base (Bug 3). Ladders don't manifest the bug because they're
// typically shorter than 200u in-scene, but the same helper covers
// both uniformly. Cast to Player* is safe — target came from
// FindNearestPlayerActor / GetSyncedPlayerActors which return
// Player-class actors.
float TargetAwareDetectRangeSq(const Actor* target) {
    if (target == nullptr) return kIdleDetectRangeSq;
    const Player* p = reinterpret_cast<const Player*>(target);
    const bool climbing =
        (p->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
    return climbing ? kClimbingTargetDetectRangeSq : kIdleDetectRangeSq;
}

// -------------------------------------------------------------------
// State handlers
// -------------------------------------------------------------------

// TickUninitialized — first-tick basis establishment. Either transitions
// to WallIdle (basis found) or PermanentlyDisabled (no wall in range).
void TickUninitialized(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "attempting basis attach");

    if (TryEstablishBasis(self, play, s)) {
        TransitionTo(self, s, EnSwState::WallIdle, "basis_established");
    } else {
        // No wall within kBasisProbeRange in any cardinal direction.
        // This spider was placed away from any wall (scene authoring
        // edge case) or is somewhere the state machine can't handle.
        // Mark permanently disabled so we don't burn CPU re-raycasting
        // every tick.
        TransitionTo(self, s, EnSwState::PermanentlyDisabled, "no_wall_in_range");
    }
}

// TickWallIdle — on-wall, no target. Hold position (no motion writes).
// Vanilla func_80B0E5E0 handles the random-gaze rotation (shape.rot.z
// smooth-step toward random unk_444) — that's the correct axis for
// wall orientation so we don't touch it here. Watch for a target
// entering 3D detect range → transition to WallPursue.
void TickWallIdle(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "holding");

    // Sticky target — see helper. Grace period covers 1-frame lookup
    // failures (rare, mostly scene-transition frames).
    Actor* target = GetStickyTarget(s, self, play);
    if (target == nullptr) return;  // no valid target; stay idle

    // Bug 3 fix — climbing targets get extended detect range so the
    // spider keeps pursuing a vine-climbing Link who's ascended past
    // the standard 200u threshold.
    if (Dist3DSq(self, target) > TargetAwareDetectRangeSq(target)) return;

    TransitionTo(self, s, EnSwState::WallPursue, "target_in_range");
}

// TickWallPursue — on-wall, target in detect range. Advance world.pos
// along the wall's tangent plane toward the target; rebuild world.rot
// from the resulting tangent basis. Validates basis each frame in case
// we crossed a corner or walked off the edge.
//
// Speed constants hardcoded per plan §4.1 pursuit-speed defaults; real
// tuning candidates go into descriptor NavParams (existing) in a
// follow-up polish pass once field-test surfaces the numbers.
void TickWallPursue(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "pursuing");

    // Wall-base floor detection FIRST — analysis 2026-07-30 (log 770):
    // ValidateBasis's horizontal probe cannot detect when the spider
    // has walked to a vertical wall's bottom edge (still hits the wall
    // poly at floor level, returns SamePoly, no transition fires). We
    // need an independent downward probe to catch this. If the actor
    // is within kGroundContactThreshold of a floor poly AND the target
    // is below the actor's Y, the spider has reached the wall's base
    // and should transition to GroundPursue for floor-plane pursuit.
    //
    // Runs BEFORE ValidateBasis so it takes precedence over the
    // spurious SamePoly result.
    {
        Actor* preTarget = FindNearestPlayerActor(&self->actor, play);
        if (preTarget != nullptr) {
            Vec3f from = self->actor.world.pos;
            from.y += 5.0f;  // start above so we catch when already on floor
            Vec3f to = {
                self->actor.world.pos.x,
                self->actor.world.pos.y + 5.0f - kWallBaseFloorProbe,
                self->actor.world.pos.z,
            };
            CollisionPoly* floorPoly = nullptr;
            s32 bgId = 0;
            Vec3f floorHit = {0.0f, 0.0f, 0.0f};
            if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &floorHit, &floorPoly,
                                         1, 1, 1, 0, &bgId) && floorPoly != nullptr) {
                const float ny = COLPOLY_GET_NORMAL(floorPoly->normal.y);
                if (ny > kWallNormalYThreshold) {
                    const float distToFloor = self->actor.world.pos.y - floorHit.y;
                    const bool atGroundLevel = (distToFloor < kGroundContactThreshold);
                    const bool targetBelow = (preTarget->world.pos.y < self->actor.world.pos.y);
                    if (atGroundLevel && targetBelow) {
                        // Wall's bottom edge reached with target below. Skip
                        // WallEdgeDrop (we're already at floor level; no
                        // gravity fall needed). Snap Y to floor + body offset
                        // and transition to GroundPursue.
                        s.hasWallBasis = false;
                        self->actor.world.pos.y = floorHit.y + BodySurfaceOffsetFor(self);
                        self->actor.velocity.x = 0.0f;
                        self->actor.velocity.y = 0.0f;
                        self->actor.velocity.z = 0.0f;
                        TransitionTo(self, s, EnSwState::GroundPursue, "reached_wall_base");
                        return;
                    }
                }
            }
        }
    }

    // Basis validation — if we walked off the SIDE edge (not the bottom
    // edge, which is caught by the floor probe above), the horizontal
    // ray will miss the wall and return Lost → transition to WallEdgeDrop
    // for gravity fall.
    const BasisValidateResult v = ValidateBasis(self, play, s);
    if (v == BasisValidateResult::Lost) {
        s.hasWallBasis = false;
        // Seed velocity for the drop — most spiders walk off the edge
        // with small tangent velocity, but we set to zero here since
        // WallPursue doesn't accumulate velocity (it uses per-frame
        // position writes). WallEdgeDrop will apply gravity from
        // velocity.y=0 which produces a natural-looking fall.
        self->actor.velocity.x = 0.0f;
        self->actor.velocity.y = 0.0f;
        self->actor.velocity.z = 0.0f;
        TransitionTo(self, s, EnSwState::WallEdgeDrop, "basis_lost");
        return;
    }

    // Target lookup + range checks. Sticky helper covers 1-frame
    // lookup failures.
    Actor* target = GetStickyTarget(s, self, play);
    if (target == nullptr) {
        TransitionTo(self, s, EnSwState::WallIdle, "target_lost");
        return;
    }

    const float dxT = target->world.pos.x - self->actor.world.pos.x;
    const float dzT = target->world.pos.z - self->actor.world.pos.z;
    const float distXZSq = dxT * dxT + dzT * dzT;
    const float dyT = target->world.pos.y - self->actor.world.pos.y;
    const float dist3DSq = distXZSq + dyT * dyT;
    // Bug 3 fix — climbing targets get extended detect range.
    if (dist3DSq > TargetAwareDetectRangeSq(target)) {
        TransitionTo(self, s, EnSwState::WallIdle, "out_of_range");
        return;
    }

    // Within attack range → hold position + rebuild rotation toward
    // target so the spider FACES the player while vanilla lunge state 6-7
    // takes over. Uses 3D distance so vertical separation keeps the
    // spider pursuing (e.g., climbing target directly above ground
    // spider — distXZ small, distY large — should NOT be treated as
    // "in attack range" since the spider needs to attach to a wall
    // and pursue upward).
    constexpr float kAttackRangeSq = 50.0f * 50.0f;
    const bool inAttackRange = (dist3DSq <= kAttackRangeSq);

    // Rule 2 — spider on wall + Link off wall + close enough → jump.
    // Skipped when Link is climbing (any surface — rule 1 walk-lunge).
    // Gated on IsTargetVisible (vision cone beyond 130u) + jump-height
    // (spider can't reach targets above kMaxJumpHeightUp anyway).
    // Range gate: kJumpMinTriggerRange < dist3D <= kJumpTriggerRange
    // keeps close-range walking under the vanilla lunge chain.
    const Player* targetPlayer = reinterpret_cast<const Player*>(target);
    const bool linkClimbing =
        (targetPlayer->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
    const float dist3D = std::sqrt(dist3DSq);
    // Bug 6 fix — additional line-of-flight raycast rejects jumps at
    // targets on unreachable platforms above / behind overhangs.
    // IsTargetJumpReachable checks physics ceiling but NOT geometry;
    // this catches the "spider on wall, Link on platform above" case
    // where Y-delta is < kMaxJumpHeightUp but the arc bonks the
    // platform's underside. Line-of-flight is conservative (rejects
    // some true-positives that arc-clear) — anchor-driven jumps below
    // (Bug 5 fix) are exempt because anchors are scan-time verified.
    if (!linkClimbing &&
        dist3D > kJumpMinTriggerRange &&
        dist3D <= kJumpTriggerRange &&
        IsTargetVisible(self, target, s) &&
        IsTargetJumpReachable(self, target) &&
        IsLineOfFlightClear(play, self->actor.world.pos,
                             target->world.pos)) {
        SetupJumpToward(s, self->actor.world.pos, target->world.pos);
        TransitionTo(self, s, EnSwState::JumpLunge, "wall_jump_at_ground_link");
        return;
    }

    // Project (target - actor) onto tangent plane by dropping normal
    // component. This yields the on-wall direction pointing at the
    // target's projection.
    Vec3f delta = {
        target->world.pos.x - self->actor.world.pos.x,
        target->world.pos.y - self->actor.world.pos.y,
        target->world.pos.z - self->actor.world.pos.z,
    };
    const float dotDN = Dot(delta, s.wallNormal);
    Vec3f deltaTan = {
        delta.x - s.wallNormal.x * dotDN,
        delta.y - s.wallNormal.y * dotDN,
        delta.z - s.wallNormal.z * dotDN,
    };
    const float tanMag = std::sqrt(deltaTan.x * deltaTan.x +
                                    deltaTan.y * deltaTan.y +
                                    deltaTan.z * deltaTan.z);
    if (tanMag < 0.001f) {
        // Target directly along our wall's normal — no in-plane
        // component. Hold rotation, no motion.
        return;
    }

    // Normalize tangent direction. Speed: run when far, walk when
    // moderate, zero when in attack range (vanilla lunge takes it from
    // here).
    constexpr float kPursuitRunSpeed  = 4.0f;
    constexpr float kPursuitWalkSpeed = 2.0f;
    const float speed = inAttackRange   ? 0.0f
                       : (distXZSq > 300.0f * 300.0f) ? kPursuitRunSpeed
                                                      : kPursuitWalkSpeed;
    const float invMag = 1.0f / tanMag;
    Vec3f fwd = {
        deltaTan.x * invMag,
        deltaTan.y * invMag,
        deltaTan.z * invMag,
    };

    // Advance world.pos along tangent (cap at |deltaTan| to prevent
    // overshoot — irrelevant in practice for slow speeds but defensive).
    const float step = (speed < tanMag) ? speed : tanMag;
    if (step > 0.01f) {
        s.isWalkAnimActive = true;  // actually translating this tick
        // Bug 2 fix — scale playSpeed by step magnitude. Divisor is the
        // fastest-possible pursuit speed so run gets full anim, walk
        // gets ~60%, and short steps get proportionally slower legs.
        const float rate = step / kPursuitRunSpeed;
        s.animMotionRate = std::max(s.animMotionRate,
                                     std::min(rate, 1.0f));
    }
    self->actor.world.pos.x += fwd.x * step;
    self->actor.world.pos.y += fwd.y * step;
    self->actor.world.pos.z += fwd.z * step;

    // Wall orientation — analogous to the ground rot fix (iteration 5),
    // with the wall's normal replacing world-up. Model's local axes:
    //   local -Y = skull  → walk direction (fwd)
    //   local -Z = belly  → into wall (-normal)
    //   local +X = right  → wall_normal × fwd
    // See RebuildWorldRotFromWallBasis for the basis derivation.
    RebuildWorldRotFromWallBasis(self, fwd, s.wallNormal);
}

// -------------------------------------------------------------------
// M6 airborne + ground states
// -------------------------------------------------------------------

// (kGravityAccel + kMaxFallSpeed relocated to top of anonymous
//  namespace so SetupJumpToward's ballistic-aim formula can reference
//  kGravityAccel. See the constants block near the top of this file.)

// Ground pursuit tuning. Slower than wall pursuit because spider legs
// on floor don't have the same purchase as tangent-plane wall walk.
constexpr float kGroundWalkSpeed = 3.0f;    // ×1.5 from prior 2.0 —
                                             // ground felt too slow in
                                             // field test vs wall pursuit
constexpr float kGroundRunSpeed  = 5.25f;   // ×1.5 from prior 3.5

// Group-movement separation config for ground En_Sw pursuit. Default
// predicate (IsAnyOtherActor) covers Zelda's "any enemy avoids any
// other enemy" flocking semantic. See Plans/group_movement_helper_plan.md.
//   neighborRadius = 45u — spider collider radius ~15u × 3, i.e., start
//                          repelling when body-widths apart
//   weight         = 40u  — tuned so at neighborRadius edge the force is
//                          small (0.02 = weight/radius²) but at
//                          minDistance overlap it dominates pursuit
//                          (40/64 = 0.625 vs pursuit ≈ 1.0 unit forward)
//   minDistance    = 8u   — safe below spider collider radius (~15u); clamps
//                          divide-by-zero, spider bodies barely brush
constexpr AnchorGroupMovement::SeparationConfig kEnSwSepGround = {
    /* neighborRadius   */ 45.0f,
    /* weight           */ 40.0f,
    /* minDistance      */ 8.0f,
    /* projectToSurface */ false,
    /* surfaceNormal    */ {0.0f, 1.0f, 0.0f},
};

// Forward-probe distance for GroundPursue wall-hit detection.
constexpr float kGroundForwardProbe = 30.0f;

// Safety timeout for WallEdgeDrop — if we've been airborne this long
// without landing, we've fallen into scene geometry we can't recover
// from (void hole, etc.). Give up gracefully to PermanentlyDisabled.
// ~3s at 20fps game tick.
constexpr int kEdgeDropTimeoutFrames = 60;

// TickWallEdgeDrop — spider left the wall (WallPursue basis-lost).
// Apply gravity, integrate to world.pos.y, watch for floor contact
// via short downward raycast. On land → snap Y to hit, zero velocity,
// transition to GroundPursue. Timeout → PermanentlyDisabled (void
// fall).
void TickWallEdgeDrop(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "falling");

    // Apply gravity + terminal-velocity clamp.
    self->actor.velocity.y += kGravityAccel;
    if (self->actor.velocity.y < kMaxFallSpeed) {
        self->actor.velocity.y = kMaxFallSpeed;
    }

    // Integrate Y position. XZ position stays where WallPursue left it
    // (spider walks off the edge and drops straight down; no horizontal
    // drift). Simpler + gives predictable landing spot.
    self->actor.world.pos.y += self->actor.velocity.y;

    // Landing detection — raycast from actor.pos slightly upward to
    // actor.pos + (0, velocity.y - kMargin, 0). If we hit a floor
    // (normal.y > 0.5) the spider has landed.
    Vec3f from = self->actor.world.pos;
    from.y += 5.0f;  // start slightly above so the raycast catches even
                     // when we're already inside a thin floor poly
    Vec3f to = {
        self->actor.world.pos.x,
        self->actor.world.pos.y + self->actor.velocity.y - 5.0f,
        self->actor.world.pos.z,
    };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = {0.0f, 0.0f, 0.0f};

    if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hitPos, &poly,
                                 1, 1, 1, 0, &bgId) && poly != nullptr) {
        const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
        if (ny > kWallNormalYThreshold) {
            // Landed on a floor. Snap position with body offset above
            // the surface (see kBodySurfaceOffset). Zero velocity.
            self->actor.world.pos.y = hitPos.y + BodySurfaceOffsetFor(self);
            self->actor.velocity.y = 0.0f;

            // Reset rotation to upright (world-Y aligned). Vanilla En_Sw
            // ground orientation isn't meaningful — spider hasn't been
            // authored for ground walk — but keeping world.rot at zero
            // pitch/roll matches typical actor convention.
            self->actor.world.rot.x = 0;
            self->actor.world.rot.z = 0;
            self->actor.shape.rot = self->actor.world.rot;

            TransitionTo(self, s, EnSwState::GroundPursue, "landed_on_floor");
            return;
        }
        // Hit ceiling or wall — spider bumped into geometry mid-fall.
        // Ignore + keep falling; landing detection continues next tick.
    }

    // Safety timeout — spider fell into void or stuck geometry.
    if (s.framesInState > kEdgeDropTimeoutFrames) {
        TransitionTo(self, s, EnSwState::PermanentlyDisabled, "edge_drop_timeout");
    }
}

// Downward-probe distance for TickGroundPursue's ground-follow. 200u
// covers most reasonable single-tick vertical drops between platforms;
// beyond that we treat it as "walked off the edge" and hand back to
// WallEdgeDrop for gravity fall.
constexpr float kGroundFollowProbe = 200.0f;

// TickGroundPursue — spider is on the floor pursuing player via world-XZ
// direct-yaw motion. Each tick raycasts down to find the floor and
// snaps world.pos.y to the hit — spider stays on the ground surface
// as it moves between elevations, even without GravityAware (log 762
// user feedback: "enemy types should be on the ground when navigating
// between ground based nav points").
//
// Transitions:
//   - target lost → Uninitialized (attempts wall re-attach; may go
//     PermanentlyDisabled if no wall in range)
//   - forward raycast hits wall → GroundToWallReattach
//   - downward raycast finds NO floor within probe → WallEdgeDrop
//     (spider walked off a platform edge → fall via gravity)
//   - attack range → held position (vanilla lunge cycle will fire via
//     actionFunc snapshot mismatch → LungeYield)
void TickGroundPursue(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "ground_pursue");

    // Pin vanilla's ambient wind-up timer so `func_80B0E5E0` never
    // transitions to `func_80B0E728` on its own. WalkLunge and JumpLunge
    // are the ONLY paths into an attack for enhanced ground spiders —
    // vanilla's actionFunc chain is inappropriate for our floor
    // orientation (wall-oriented rotation writes, straight-line dash
    // that clashes with our motion). LungeYield remains as a defensive
    // fallback for edge cases (damage → func_80B0D878 death, etc).
    self->unk_442 = 100;

    // Ground-follow probe FIRST — snap Y to floor before any motion
    // writes. Raycast from a small height above actor.pos down by
    // kGroundFollowProbe. Anything with normal.y > threshold counts as
    // a floor; snap Y to hit position. Cache the floor normal for
    // slope-aware orientation below. No floor found → walked off
    // edge → transition to WallEdgeDrop for gravity fall.
    Vec3f floorNormal = {0.0f, 1.0f, 0.0f};  // default flat floor if raycast misses (early-return path)
    {
        Vec3f from = self->actor.world.pos;
        from.y += 5.0f;
        Vec3f to = {
            self->actor.world.pos.x,
            self->actor.world.pos.y - kGroundFollowProbe,
            self->actor.world.pos.z,
        };
        CollisionPoly* floorPoly = nullptr;
        s32 floorBgId = 0;
        Vec3f floorHit = {0.0f, 0.0f, 0.0f};
        bool onFloor = false;
        if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &floorHit, &floorPoly,
                                     1, 1, 1, 0, &floorBgId) && floorPoly != nullptr) {
            const float ny = COLPOLY_GET_NORMAL(floorPoly->normal.y);
            if (ny > kWallNormalYThreshold) {
                // Body offset above the floor (see kBodySurfaceOffset).
                self->actor.world.pos.y = floorHit.y + BodySurfaceOffsetFor(self);
                floorNormal.x = COLPOLY_GET_NORMAL(floorPoly->normal.x);
                floorNormal.y = ny;
                floorNormal.z = COLPOLY_GET_NORMAL(floorPoly->normal.z);
                onFloor = true;
            }
        }
        if (!onFloor) {
            self->actor.velocity.x = 0.0f;
            self->actor.velocity.y = 0.0f;
            self->actor.velocity.z = 0.0f;
            TransitionTo(self, s, EnSwState::WallEdgeDrop, "walked_off_ground");
            return;
        }
    }

    // Sticky target — see helper. Grace period covers 1-frame lookup
    // failures (mostly during scene transitions).
    Actor* target = GetStickyTarget(s, self, play);
    if (target == nullptr) {
        // No valid target this frame — idle in place with ground
        // orientation + slow random gaze rotation ("looking for prey"
        // per user spec). Transitioning to Uninitialized here would
        // route to WallIdle via TryEstablishBasis and reintroduce
        // vanilla ambient rotation on the wrong (wall) axis.
        UpdateIdleGaze(s, self, play);
        self->actor.world.rot.x = (s16)-0x4000;
        self->actor.world.rot.z = 0;
        self->actor.shape.rot   = self->actor.world.rot;
        return;
    }

    const float dx = target->world.pos.x - self->actor.world.pos.x;
    const float dz = target->world.pos.z - self->actor.world.pos.z;
    const float dy = target->world.pos.y - self->actor.world.pos.y;
    const float distXZSq = dx * dx + dz * dz;
    const float dist3DSq = distXZSq + dy * dy;
    // Bug 3 fix — climbing targets get extended detect range so the
    // spider keeps pursuing a Link who has climbed high on a vine.
    if (dist3DSq > TargetAwareDetectRangeSq(target)) {
        // Target out of 3D detect range — idle in place with slow
        // gaze rotation. Same rationale as target-null path.
        // Previously used XZ distance which let the spider "engage"
        // Link when Link was climbing 500u up a nearby wall (small
        // XZ, huge Y) — silly. 3D range gates engagement to what the
        // spider can realistically reach.
        UpdateIdleGaze(s, self, play);
        self->actor.world.rot.x = (s16)-0x4000;
        self->actor.world.rot.z = 0;
        self->actor.shape.rot   = self->actor.world.rot;
        return;
    }

    // Bug 3 fix — opportunistic wall re-attach. When the target is
    // significantly above and horizontally close (spider is under
    // Link's climbing position), try to establish a wall basis
    // immediately rather than waiting for a forward-raycast wall-hit
    // to trigger GroundToWallReattach naturally. The forward-raycast
    // path may miss vine walls if the pursuit direction is undefined
    // (Link directly overhead → atan2(0,0) → arbitrary +Z motion)
    // OR if the spider's already past the vine-column XZ.
    // Preconditions:
    //   - Target is climbing (has a climbable surface nearby)
    //   - Target is > 60u above spider (worth climbing to reach)
    //   - Spider is within 80u XZ of target (close enough for the
    //     100u basis-probe raycast to plausibly find the target's wall)
    {
        const Player* p = reinterpret_cast<const Player*>(target);
        const bool targetClimbing =
            (p->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
        if (targetClimbing && dy > 60.0f && distXZSq < 80.0f * 80.0f) {
            if (TryEstablishBasis(self, play, s)) {
                if (DiagEnabled()) {
                    SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} GROUND->WALL "
                                "opportunistic attach (climbing target above) "
                                "dy={:.1f} distXZ={:.1f}",
                                (uintptr_t)self, dy, std::sqrt(distXZSq));
                }
                TransitionTo(self, s, EnSwState::WallPursue,
                             "opportunistic_climb_attach");
                return;
            }
        }
    }

    // Bug 5 fix — nav jump-anchor gap traversal. Before running normal
    // pursuit motion, check if a nearby JumpAnchor bridges toward the
    // target (e.g., across a gap the spider would otherwise fall into
    // via TickWallEdgeDrop). Anchor arc is scan-time verified so no
    // runtime line-of-flight check needed. Skip if spider is already
    // in melee range — vanilla lunge cycle should handle that.
    if (dist3DSq > 100.0f * 100.0f) {
        Vec3f anchorLanding = { 0.0f, 0.0f, 0.0f };
        if (FindJumpAnchorTowardTarget(self, play, target->world.pos,
                                         &anchorLanding)) {
            if (DiagEnabled()) {
                SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} GROUND->JUMP "
                            "anchor-driven gap traversal "
                            "landing=({:.0f},{:.0f},{:.0f})",
                            (uintptr_t)self,
                            anchorLanding.x, anchorLanding.y,
                            anchorLanding.z);
            }
            SetupJumpToward(s, self->actor.world.pos, anchorLanding);
            TransitionTo(self, s, EnSwState::JumpLunge,
                         "traverse_jump_via_anchor");
            return;
        }
    }

    // Yaw toward target in world XZ, project onto floor tangent plane
    // for slope-following motion.
    const s16 yaw = (s16)(std::atan2(dx, dz) * (0x8000 / M_PI));
    Vec3f fwd = {
        std::sin(yaw * (M_PI / 0x8000)),
        0.0f,
        std::cos(yaw * (M_PI / 0x8000)),
    };
    const float fwdDotN = Dot(fwd, floorNormal);
    Vec3f fwdTangent = {
        fwd.x - floorNormal.x * fwdDotN,
        fwd.y - floorNormal.y * fwdDotN,
        fwd.z - floorNormal.z * fwdDotN,
    };
    if (!Normalize(&fwdTangent)) {
        fwdTangent = fwd;  // floor was near-vertical; use unprojected fwd
    }

    // Ground orientation — fourth iteration.
    //
    // History:
    //   1. Wall basis on floor       → skull clipped into floor
    //   2. Ground basis (col1=fwd)   → spider walked backwards
    //   3. rot=(0, yaw-0x4000, 0)    → spider upside-down (regression)
    //   4. rot=(0x8000, yaw, 0)      → spider upright, skull-UP
    //   5. THIS: rot=(-0x4000, yaw, 0) → 90° pitch, skull-forward
    //
    // Math: En_Sw model has skull at local -Y (per field observation
    // of iterations 3-4). YXZ Euler rotation of skull vector (0,-1,0)
    // through (rx, ry, 0) gives skull_world =
    //   (-sin(rx)·sin(ry), -cos(rx), -sin(rx)·cos(ry))
    // For target-north (yaw=0), want skull_world=(0,0,1):
    //   need cos(rx)=0 → rx=±90°; -sin(rx)=1 → rx=-90° = -0x4000
    // For target-east (yaw=+0x4000), want skull_world=(1,0,0):
    //   with rx=-0x4000, sin(rx)=-1 → sin(ry)=1 → ry=+0x4000 = yaw ✓
    // General: rx = -0x4000, ry = yaw (no offset), rz = 0.
    self->actor.world.rot.x = (s16)-0x4000;  // -90° pitch → dorsal-up + skull-forward
    self->actor.world.rot.y = yaw;
    self->actor.world.rot.z = 0;
    self->actor.shape.rot = self->actor.world.rot;

    // Attack-range check uses 3D distance so vertical separation
    // matters. dist3DSq already computed at target-range gate above.
    constexpr float kAttackRangeSq = 50.0f * 50.0f;
    if (dist3DSq <= kAttackRangeSq) {
        // Rule 3 — spider on ground + Link off wall + close enough +
        // (LoS blocked OR jumpable-above) → JumpLunge. "No path" is
        // proxied by LoS check; walk-lunge can't route around walls,
        // jumping is the only option. Also gated on IsTargetVisible
        // (vision cone beyond 130u) + IsTargetJumpReachable
        // (kMaxJumpHeightUp physics ceiling).
        const Player* targetPlayer = reinterpret_cast<const Player*>(target);
        const bool linkClimbing =
            (targetPlayer->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
        if (!linkClimbing) {
            Vec3f losFrom = self->actor.world.pos;
            losFrom.y += 20.0f;  // above body so ray doesn't clip the
                                  // spider's own floor poly
            Vec3f losTo   = target->world.pos;
            losTo.y      += 20.0f;  // aim at Link's mid-body, not feet
            CollisionPoly* losPoly = nullptr;
            s32 losBgId = 0;
            Vec3f losHit = {0.0f, 0.0f, 0.0f};
            const bool losBlocked =
                BgCheck_EntityLineTest1(&play->colCtx, &losFrom, &losTo,
                                         &losHit, &losPoly, 1, 0, 0, 1,
                                         &losBgId);
            // Bug 6 fix — line-of-flight gate rejects jumps at targets
            // on unreachable platforms above. IsTargetJumpReachable only
            // checks physics ceiling (Y-delta) — a target on a platform
            // 100u above but behind an overhang passes that check even
            // though the arc bonks the platform's edge. Direct raycast
            // from spider to target is conservative (arc may clear when
            // line is blocked, but arc is DEFINITELY blocked when line
            // is blocked) — matches user's own suggested fix for this
            // symptom class.
            if (losBlocked &&
                IsTargetVisible(self, target, s) &&
                IsTargetJumpReachable(self, target) &&
                IsLineOfFlightClear(play, self->actor.world.pos,
                                     target->world.pos)) {
                SetupJumpToward(s, self->actor.world.pos, target->world.pos);
                TransitionTo(self, s, EnSwState::JumpLunge, "ground_jump_no_path");
                return;
            }
            // LoS clear (or jump gated) → WalkLunge (custom ground
            // wind-up + straight-line dash). Vision-cone gate applies
            // — at 50u attack range dist3D is within 130u vanilla-
            // exempt zone, so IsTargetVisible always passes here in
            // practice; kept for consistency + defense against future
            // range widening.
            //
            // Pre-lunge path-blocker probe: LoS above (spider Y+20 to
            // target Y+20) catches walls tall enough to block a
            // waist-level ray, but flies OVER short obstacles (steps,
            // curbs, pots at body level). Additional horizontal ray at
            // spider body height along the dash direction over the
            // full dash reach catches those low blockers. If hit,
            // refuse the WalkLunge trigger this tick — spider walks
            // closer next frame and re-decides. Wall-normal check
            // (|ny| < threshold) so slopes / floors aren't treated as
            // blockers.
            if (IsTargetVisible(self, target, s)) {
                constexpr float kDashReach =
                    kWalkLungeDashSpeed * (float)kWalkLungeDashFrames;
                const float dxL = target->world.pos.x - self->actor.world.pos.x;
                const float dzL = target->world.pos.z - self->actor.world.pos.z;
                const float lenL = std::sqrt(dxL * dxL + dzL * dzL);
                bool pathBlocked = false;
                if (lenL > 0.001f) {
                    const float invL = 1.0f / lenL;
                    Vec3f bpFrom = self->actor.world.pos;
                    bpFrom.y += BodySurfaceOffsetFor(self);  // body center-ish
                    Vec3f bpTo = {
                        bpFrom.x + dxL * invL * kDashReach,
                        bpFrom.y,
                        bpFrom.z + dzL * invL * kDashReach,
                    };
                    CollisionPoly* bpPoly = nullptr;
                    s32 bpBgId = 0;
                    Vec3f bpHit = {0.0f, 0.0f, 0.0f};
                    if (BgCheck_EntityLineTest1(&play->colCtx, &bpFrom,
                                                 &bpTo, &bpHit, &bpPoly,
                                                 1, 0, 0, 1, &bpBgId) &&
                        bpPoly != nullptr) {
                        const float ny =
                            COLPOLY_GET_NORMAL(bpPoly->normal.y);
                        if (std::fabs(ny) < kWallNormalYThreshold) {
                            pathBlocked = true;
                        }
                    }
                }
                if (!pathBlocked) {
                    SetupWalkLungeToward(s, self->actor.world.pos,
                                          target->world.pos);
                    TransitionTo(self, s, EnSwState::WalkLunge,
                                 "ground_walk_lunge");
                }
                // pathBlocked: fall through (return below). Next tick
                // spider approaches closer; may re-fire or route via
                // JumpLunge if LoS is also blocked.
            }
        }
        return;
    }

    // Body oscillation — vertical bob + pitch + roll during walking.
    // Amplitudes drawn from Weihmann 2013 (PLOS One 10.1371/journal.
    // pone.0065788) forward-walking Cupiennius salei kinematics:
    // vertical bob ≈ 1.7mm, pitch ≈ 4.6°, roll ≈ 6.6°. All oscillate
    // at ~stride frequency (one full cycle per gait stride). Real-
    // spider fluctuations are "relatively low" (paper's conclusion),
    // so amplitudes stay modest. Frequency matches the gait's
    // kStepPeriod (8 frames) used by the leg-bend animation.
    //
    // Pitch is offset 90° from bob so the nose leads the bob
    // (nose-up at rising-body midpoint, nose-down at plant); roll
    // shares bob's phase (body tilts one way at swing peak, other
    // way at plant). This 3-axis phase relationship reads as a
    // rolling stride rather than a jitter.
    static constexpr float   kBodyBobAmplitude   = 0.75f;   // ×0.5 from prior
                                                             // 1.5 — walking
                                                             // oscillation
                                                             // was too much
    static constexpr int16_t kBodyPitchAmplitude = 0x0200;   // ~2.25°, ×0.5
                                                             // from prior 0x0400
    static constexpr int16_t kBodyRollAmplitude  = 0x02E0;   // ~4°, ×0.5 from
                                                             // prior 0x05C0
    static constexpr float   kBodyOscPeriod     = 8.0f;    // frames per cycle
                                                            // (matches leg
                                                            // kStepPeriod).
    const float bodyPhase =
        (float)play->gameplayFrames * (2.0f * (float)M_PI / kBodyOscPeriod);
    const float bobT   = std::sin(bodyPhase);
    const float pitchT = std::sin(bodyPhase + (float)M_PI * 0.5f);  // +90°
    const float rollT  = std::sin(bodyPhase);
    // Anchored surface = floor; away = +Y in world.
    self->actor.world.pos.y += bobT * kBodyBobAmplitude;
    // Overlay pitch + roll on base ground orientation.
    self->actor.world.rot.x =
        (s16)((-0x4000) + (int16_t)(pitchT * (float)kBodyPitchAmplitude));
    self->actor.world.rot.z = (s16)((int16_t)(rollT * (float)kBodyRollAmplitude));
    self->actor.shape.rot   = self->actor.world.rot;

    // Advance world.pos along the slope-projected tangent direction.
    // On flat floor fwdTangent ≈ (sin(yaw), 0, cos(yaw)); on a slope
    // it includes small Y component that keeps the spider hugging the
    // surface. Next tick's ground-follow probe (top of TickGroundPursue)
    // snaps Y precisely, so pos.y drift from this write is corrected
    // immediately.
    const float dist = std::sqrt(distXZSq);
    const float speed = (dist > 300.0f) ? kGroundRunSpeed : kGroundWalkSpeed;
    s.isWalkAnimActive = true;  // actually translating this tick
    // Bug 2 fix — scale playSpeed by pursuit speed. Run gets full anim,
    // walk gets ~57% (3.0/5.25), guaranteeing barely-moving spiders
    // don't animate at max intensity while imperceptibly translating.
    // Post-hook clamps to kMinAnimPlaySpeed floor.
    {
        const float rate = speed / kGroundRunSpeed;
        s.animMotionRate = std::max(s.animMotionRate,
                                     std::min(rate, 1.0f));
    }

    // Group-movement separation — combine pursuit-forward with a
    // repulsion vector from nearby enemies so multiple pursuers don't
    // stack on top of each other at Link. Default predicate + nullptr
    // categories = walks all synced enemy categories, filters to "any
    // other actor" — matches Plans §8 Q3 "no enemy-vs-enemy hostility
    // in Zelda; every enemy avoids overlapping every other enemy."
    // Combined vector is then re-projected onto the floor tangent so
    // slope-following stays correct.
    Vec3f sep = {0.0f, 0.0f, 0.0f};
    AnchorGroupMovement::ComputeSeparation(
        &self->actor, play,
        /* categories     */ nullptr,
        /* categoryCount  */ 0,
        AnchorGroupMovement::IsAnyOtherActor,
        kEnSwSepGround, &sep);
    Vec3f steer = {
        fwdTangent.x + sep.x,
        fwdTangent.y + sep.y,
        fwdTangent.z + sep.z,
    };
    if (!Normalize(&steer)) {
        // Degenerate (separation exactly cancels pursuit) — fall back
        // to pursuit direction.
        steer = fwdTangent;
    }
    // Re-project the combined steer onto the floor tangent plane so
    // slope-following stays correct (separation might have introduced
    // a small vertical component from stacked-height neighbors).
    const float steerDotN = Dot(steer, floorNormal);
    steer.x -= floorNormal.x * steerDotN;
    steer.y -= floorNormal.y * steerDotN;
    steer.z -= floorNormal.z * steerDotN;
    if (!Normalize(&steer)) steer = fwdTangent;

    self->actor.world.pos.x += steer.x * speed;
    self->actor.world.pos.y += steer.y * speed;
    self->actor.world.pos.z += steer.z * speed;

    // Wall-hit detection — cast forward from actor.pos in the direction
    // we just moved. If we hit a wall within kGroundForwardProbe, try
    // to reattach next tick. Probe uses `steer` (pursuit+separation
    // combined) so it catches walls we're actually moving into after
    // group-movement deflection, not the un-deflected pursuit heading.
    Vec3f from = self->actor.world.pos;
    Vec3f to = {
        from.x + steer.x * kGroundForwardProbe,
        from.y + steer.y * kGroundForwardProbe,
        from.z + steer.z * kGroundForwardProbe,
    };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = {0.0f, 0.0f, 0.0f};
    if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hitPos, &poly,
                                 1, 1, 1, 0, &bgId) && poly != nullptr) {
        const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
        if (std::fabs(ny) < kWallNormalYThreshold) {
            TransitionTo(self, s, EnSwState::GroundToWallReattach, "wall_ahead");
        }
    }
}

// TickGroundToWallReattach — try to re-attach to the wall we just
// bumped into during GroundPursue. Uses the same 6-direction raycast
// as TickUninitialized. On success, target-in-range check decides
// WallPursue vs WallIdle transition.
void TickGroundToWallReattach(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "reattach_attempt");

    if (TryEstablishBasis(self, play, s)) {
        // Wall found. Check if target is still in range to decide
        // between WallPursue (pursue immediately) or WallIdle (settle).
        Actor* target = FindNearestPlayerActor(&self->actor, play);
        if (target != nullptr) {
            const float dx = target->world.pos.x - self->actor.world.pos.x;
            const float dz = target->world.pos.z - self->actor.world.pos.z;
            if ((dx * dx + dz * dz) <= kIdleDetectRangeSq) {
                TransitionTo(self, s, EnSwState::WallPursue, "reattach_success_with_target");
                return;
            }
        }
        TransitionTo(self, s, EnSwState::WallIdle, "reattach_success_no_target");
        return;
    }

    // Reattach failed — fall back to GroundPursue. Reason for failure
    // is usually that the wall we hit is out of raycast range (the
    // 30u forward probe from GroundPursue detected a wall further
    // than kBasisProbeRange=100u, or the wall is a slope steeper
    // than 30° threshold). Continue ground pursuit.
    TransitionTo(self, s, EnSwState::GroundPursue, "reattach_failed_fallback");
}

// TickJumpLunge — Tektite-style ballistic attack. Two phases:
//   1. Wind-up (framesInState < kJumpWindupFrames): no motion.
//      Purple color visible via Anchor_Enhance_EnSw_IsJumpAttacking
//      bridge → EnSw_Draw applies fog color. Telegraph phase.
//   2. Airborne (framesInState >= kJumpWindupFrames + jumpAirborne=true):
//      apply per-tick velocity + gravity accumulation. Land on floor
//      (→ GroundPursue) or wall (→ Uninitialized for basis re-establish).
//
// Trust-physics landing model — the spider is a predator without regard
// for whether the target landing is reachable; if it lands somewhere
// isolated, subsequent GroundPursue / GroundToWallReattach / WallEdgeDrop
// handle recovery. Safety timeout kJumpMaxAirFrames aborts to
// PermanentlyDisabled if flight never lands (void fall).
void TickJumpLunge(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    // Suppress vanilla's ambient-actionFunc lunge trigger for the
    // duration of the jump. func_80B0E5E0 checks
    // `(DECR(unk_442) == 0 && picker_ok)` each frame; pinning
    // unk_442 above 0 keeps that check false so vanilla never
    // transitions to func_80B0E728 mid-jump (which would flip our
    // snapshot detector into LungeYield and preempt our custom
    // airborne motion).
    self->unk_442 = 100;

    // Wind-up phase — hold in place, telegraph via purple fog color.
    // IMPORTANT (Bug 4 fix 2026-07-31): do NOT rotate the spider
    // toward ground orientation during wind-up. Previously the
    // Math_SmoothStepToS rotation block ran unconditionally at the
    // top of this tick — the spider began tilting away from its
    // pre-jump orientation (wall for rule-2 wall jumps) BEFORE the
    // launch actually fired, producing a 1-2s "detached and hovering"
    // visual glitch during the wind-up telegraph. Match WalkLunge's
    // pattern: wind-up returns early WITHOUT rotation changes.
    if (s.framesInState < kJumpWindupFrames) {
        LogTickState(self, s, "jump_windup");
        return;
    }

    // Smoothly rotate toward the target ground orientation over ~20
    // frames (~1 s at 20 fps). Runs ONLY during airborne phase (per
    // Bug 4 fix). Starts from whatever rotation the launch state
    // left us in (wall orientation for rule-2 wall jumps, ground
    // orientation for rule-3 ground jumps). Yaw aims at the jump
    // direction (same as dash direction, since horizontal velocity
    // was set that way in SetupJumpToward). Math_SmoothStepToS with
    // scale=4 covers ~75% of the remaining angle per frame — a 90°
    // pitch delta closes to <1° within ~15 frames.
    const s16 flightYaw =
        (s16)(std::atan2(s.jumpVelX, s.jumpVelZ) * (0x8000 / M_PI));
    Math_SmoothStepToS(&self->actor.world.rot.x, (s16)-0x4000, 4, 0x1000, 0x40);
    Math_SmoothStepToS(&self->actor.world.rot.y, flightYaw,    4, 0x1000, 0x40);
    Math_SmoothStepToS(&self->actor.world.rot.z, (s16)0,       4, 0x1000, 0x40);
    self->actor.shape.rot = self->actor.world.rot;

    // Rising edge from wind-up to airborne — capture initial velocity
    // that was pre-computed at TransitionTo(JumpLunge) call site.
    if (!s.jumpAirborne) {
        s.jumpAirborne = true;
        self->actor.velocity.x = s.jumpVelX;
        self->actor.velocity.y = s.jumpVelY;
        self->actor.velocity.z = s.jumpVelZ;
        if (DiagEnabled()) {
            SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} JUMP_LUNGE launch "
                        "vel=({:.2f},{:.2f},{:.2f}) pos=({:.1f},{:.1f},{:.1f})",
                        (uintptr_t)self, s.jumpVelX, s.jumpVelY, s.jumpVelZ,
                        self->actor.world.pos.x, self->actor.world.pos.y,
                        self->actor.world.pos.z);
        }
    }

    // Capture position BEFORE integrating — used for swept collision
    // check below. See Analysis/en_sw_jumplunge_wall_clip_2026-07-31.md
    // for full rationale (Option A: continuous collision detection).
    s.jumpPrevPos = self->actor.world.pos;

    // Airborne — integrate position with gravity on Y component.
    self->actor.velocity.y += kGravityAccel;
    if (self->actor.velocity.y < kMaxFallSpeed) {
        self->actor.velocity.y = kMaxFallSpeed;
    }
    self->actor.world.pos.x += self->actor.velocity.x;
    self->actor.world.pos.y += self->actor.velocity.y;
    self->actor.world.pos.z += self->actor.velocity.z;

    LogTickState(self, s, "jump_airborne");

    // Swept collision — cast a ray from prevPos (start of this frame)
    // to current pos (after integration). Catches ANY geometry the arc
    // crossed this frame — floor, wall, ceiling — during BOTH upward
    // and downward phases. Replaces the previous vertical-only raycast
    // which:
    //   - missed all upward-phase collisions (gated on velocity.y < 0)
    //   - missed wall clips in horizontal motion (probed strictly DOWN)
    //   - allowed thin-wall tunneling at high speeds
    // Cost: one BgCheck_EntityLineTest1 per airborne frame, trivial.
    {
        CollisionPoly* poly = nullptr;
        s32 bgId = 0;
        Vec3f hitPos = {0.0f, 0.0f, 0.0f};
        if (BgCheck_EntityLineTest1(&play->colCtx, &s.jumpPrevPos,
                                     &self->actor.world.pos,
                                     &hitPos, &poly, 1, 1, 1, 0, &bgId) &&
            poly != nullptr) {
            const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
            if (ny > kWallNormalYThreshold) {
                // Floor landing — snap XZ to hit + body offset above.
                self->actor.world.pos.x = hitPos.x;
                self->actor.world.pos.y = hitPos.y + BodySurfaceOffsetFor(self);
                self->actor.world.pos.z = hitPos.z;
                self->actor.velocity.x = self->actor.velocity.y =
                    self->actor.velocity.z = 0.0f;
                s.jumpAirborne = false;
                TransitionTo(self, s, EnSwState::GroundPursue,
                             "jump_landed_floor");
                return;
            }
            if (std::fabs(ny) < kWallNormalYThreshold) {
                // Wall contact — snap to hit, back off along the wall
                // normal by kBodySurfaceOffset so next-tick basis
                // raycast doesn't start inside the wall poly.
                const float nx = COLPOLY_GET_NORMAL(poly->normal.x);
                const float nz = COLPOLY_GET_NORMAL(poly->normal.z);
                self->actor.world.pos.x = hitPos.x + nx * BodySurfaceOffsetFor(self);
                self->actor.world.pos.y = hitPos.y + ny * BodySurfaceOffsetFor(self);
                self->actor.world.pos.z = hitPos.z + nz * BodySurfaceOffsetFor(self);
                self->actor.velocity.x = self->actor.velocity.y =
                    self->actor.velocity.z = 0.0f;
                s.jumpAirborne = false;
                s.hasWallBasis = false;
                TransitionTo(self, s, EnSwState::Uninitialized,
                             "jump_hit_wall");
                return;
            }
            // Ceiling (ny < -threshold) — spider bonked head. Ignore
            // and continue arc; gravity brings it back down. Not
            // treated as a landing since the spider can't "hang" from
            // a ceiling mid-jump.
        }
    }

    // Safety timeout — void fall.
    if (s.framesInState > kJumpMaxAirFrames + kJumpWindupFrames) {
        s.jumpAirborne = false;
        TransitionTo(self, s, EnSwState::PermanentlyDisabled, "jump_timeout");
    }
}

// TickWalkLunge — custom ground walk-lunge (replaces vanilla
// func_80B0E728 for enhanced GroundPursue spiders). Two phases:
//   1. Wind-up (kWalkLungeWindupFrames): stationary, purple flash via
//      Anchor_Enhance_EnSw_IsJumpAttacking bridge.
//   2. Dash (kWalkLungeDashFrames): apply s.jumpVelX/Z to world.pos
//      each frame, no vertical motion (Y stays at entry level). Wall
//      contact during dash → GroundToWallReattach. Duration expiry →
//      GroundPursue for re-evaluation.
//
// Pins unk_442 = 100 each frame so vanilla's ambient func_80B0E5E0
// never triggers its own lunge chain (which would fight our motion
// via shape.rot.z writes designed for wall spiders).
void TickWalkLunge(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    self->unk_442 = 100;

    // Lock ground orientation + face dash direction every frame.
    // Without this, vanilla func_80B0E430 line 981's `world.rot =
    // shape.rot` copy carries shape.rot.z drift (from vanilla's
    // random-gaze smooth-step toward unk_444) — spider visibly
    // rotates side-to-side during the dash. Our writes here overwrite
    // vanilla's per the post-hook pattern.
    const s16 dashYaw =
        (s16)(std::atan2(s.jumpVelX, s.jumpVelZ) * (0x8000 / M_PI));
    self->actor.world.rot.x = (s16)-0x4000;
    self->actor.world.rot.y = dashYaw;
    self->actor.world.rot.z = 0;
    self->actor.shape.rot   = self->actor.world.rot;

    // Wind-up phase — hold in place, telegraph via purple fog color.
    if (s.framesInState < kWalkLungeWindupFrames) {
        LogTickState(self, s, "walk_lunge_windup");
        return;
    }

    // Dash phase — apply horizontal velocity, integrate world.pos.
    const int dashFramesElapsed = s.framesInState - kWalkLungeWindupFrames;
    if (dashFramesElapsed >= kWalkLungeDashFrames) {
        // Dash complete — return to GroundPursue for re-evaluation.
        // If Link is still in attack range next tick, another WalkLunge
        // (or JumpLunge if LoS blocked) will chain naturally.
        TransitionTo(self, s, EnSwState::GroundPursue, "walk_lunge_complete");
        return;
    }

    s.isWalkAnimActive = true;  // actually translating during dash
    s.animMotionRate   = 1.0f;   // Bug 2 fix — dash is always full speed
    self->actor.world.pos.x += s.jumpVelX;
    self->actor.world.pos.z += s.jumpVelZ;

    // Ground-follow — snap Y to floor + kBodySurfaceOffset each frame
    // so the spider hugs terrain instead of dashing horizontally through
    // dips and hills. Same probe shape as TickGroundPursue's top-block
    // ground-follow. Only accepts polys with normal.y > kWallNormalYThreshold
    // (walkable floors, not walls / ceilings encountered mid-dash).
    {
        Vec3f gfFrom = self->actor.world.pos;
        gfFrom.y += 5.0f;
        Vec3f gfTo = {
            self->actor.world.pos.x,
            self->actor.world.pos.y - kGroundFollowProbe,
            self->actor.world.pos.z,
        };
        CollisionPoly* gfPoly = nullptr;
        s32 gfBgId = 0;
        Vec3f gfHit = {0.0f, 0.0f, 0.0f};
        if (BgCheck_EntityLineTest1(&play->colCtx, &gfFrom, &gfTo, &gfHit,
                                     &gfPoly, 1, 1, 1, 0, &gfBgId) &&
            gfPoly != nullptr) {
            const float ny = COLPOLY_GET_NORMAL(gfPoly->normal.y);
            if (ny > kWallNormalYThreshold) {
                self->actor.world.pos.y = gfHit.y + BodySurfaceOffsetFor(self);
            }
        }
    }
    LogTickState(self, s, "walk_lunge_dash");

    // Wall-hit detection — cast forward from actor.pos in the dash
    // direction. If we bump into a wall mid-dash, transition to
    // reattach (same shape as TickGroundPursue's wall-ahead check).
    Vec3f from = self->actor.world.pos;
    Vec3f to = {
        from.x + s.jumpVelX * kGroundForwardProbe / kWalkLungeDashSpeed,
        from.y,
        from.z + s.jumpVelZ * kGroundForwardProbe / kWalkLungeDashSpeed,
    };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    Vec3f hitPos = {0.0f, 0.0f, 0.0f};
    if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hitPos, &poly,
                                 1, 1, 1, 0, &bgId) && poly != nullptr) {
        const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
        if (std::fabs(ny) < kWallNormalYThreshold) {
            TransitionTo(self, s, EnSwState::GroundToWallReattach,
                         "walk_lunge_wall_ahead");
        }
    }
}

// Placeholder handlers for states landed in M7. Each emits a once-per-
// entry diagnostic so we know if execution ever reaches them before
// their real bodies land.
void TickPlaceholder(EnSw* self, PlayState* play, EnSwEnhancedState& s,
                     const char* stateName, const char* nextMilestone) {
    (void)play;
    if (s.framesInState == 0 && DiagEnabled()) {
        SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} state={} — placeholder handler "
                    "(real body lands in {}). Yielding to no-op.",
                    (uintptr_t)self, stateName, nextMilestone);
    }
}

}  // namespace

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

void EnSw_EnhancedStateMachine_Tick(EnSw* self, PlayState* play) {
    if (self == nullptr || play == nullptr) return;

    EnSwEnhancedState& s = GetOrCreate(self);

    // 2026-08-04 (Pillar 5 Phase 3) — spawned-from-St-swap overrides.
    // Runs BEFORE the yield-check so the overrides apply every tick
    // regardless of what vanilla En_Sw's Update did. Vanilla writes
    // scale=0.02 in multiple places (Init line 280 + Math_ApproachF
    // targets at 553/571/596); this post-write forces 0.06 (matches
    // En_St BIG variant visual). Floor-snap prevents the wall-cling
    // dangling behavior — En_Sw's Init runs func_80B0DFFC looking for
    // a wall behind the actor; if none found (ceiling-Skulltula drop
    // has no wall backing), the actor hangs in midair. Force
    // world.pos.y to floorHeight so gravity + ground-walking mode
    // (via NavConsume descriptor) take over.
    if (s.spawnedFromStSwap) {
        Actor_SetScale(&self->actor, 0.06f);
        // Floor snap: only when we have a valid floor reading (≠ sentinel).
        // Snap DOWN if actor is above floor by any margin — keeps the
        // spider grounded even if vanilla's per-tick physics tried to
        // lift it.
        if (self->actor.floorHeight > BGCHECK_Y_MIN &&
            self->actor.world.pos.y > self->actor.floorHeight) {
            self->actor.world.pos.y = self->actor.floorHeight;
            self->actor.velocity.y = 0.0f;
        }
        // 2026-08-04 (user "still spawned in wall-climbing state") —
        // force into GroundPursue on first tick. Vanilla En_Sw Init
        // + TickUninitialized both try to find a wall to cling to;
        // for a swap-spawned actor there IS no wall (we're on the
        // floor). Skip the raycast wall search and go straight to
        // ground-walking. TickGroundPursue handles all subsequent
        // motion + wall-attach transitions naturally.
        if (s.state == EnSwState::Uninitialized ||
            s.state == EnSwState::WallIdle ||
            s.state == EnSwState::WallPursue) {
            TransitionTo(self, s, EnSwState::GroundPursue, "en_st_swap_force_ground");
        }
    }

    // Walk-anim gate default: false each tick. Actual motion sites
    // opt-in by setting s.isWalkAnimActive = true. Placed BEFORE yield-
    // check so LungeYield / any other early-return path also resets
    // the flag (default: not walking).
    s.isWalkAnimActive = false;
    // Bug 2 fix — animMotionRate mirrors the isWalkAnimActive reset
    // pattern. State code writes the rate alongside setting the flag.
    s.animMotionRate = 0.0f;

    // Yield-check: if we have an ambient-actionFunc snapshot AND vanilla
    // has advanced actionFunc away from it, vanilla is in a non-ambient
    // state (lunge cycle, return-to-position). Our state machine yields
    // this tick so vanilla motion runs uncontested.
    //
    // Design note (see plan §11.R1): the pilot plan originally proposed
    // explicit vanilla-state-address detection (e.g. compare
    // `self->actionFunc == func_80B0DA00`). Those symbols are `static`
    // in z_en_sw.c and not exportable without vanilla edits. The
    // general "any non-ambient state" detection here is a strict
    // superset — catches lunge cycle AND any other future non-ambient
    // vanilla state transparently. Same net effect for the lunge case;
    // more resilient to vanilla changes.
    if (s.haveAmbientSnapshot &&
        s.initialAmbientActionFunc != nullptr &&
        self->actionFunc != s.initialAmbientActionFunc) {
        if (s.state != EnSwState::LungeYield) {
            // Snapshot the vanilla actionFunc we entered LungeYield
            // WITH. Typically this is func_80B0E728 (lunge wind-up +
            // motion). We use it below to detect when vanilla has
            // moved on from the lunge to a follow-up state (walk-home
            // = func_80B0E9BC, or post-lunge stop = func_80B0E90C) —
            // both of which we want to preempt.
            s.lungeEntryActionFunc = self->actionFunc;
            TransitionTo(self, s, EnSwState::LungeYield, "vanilla_actionfunc_advanced");
        } else {
            // Steady LungeYield tick — increment framesInState so the
            // safety timeout below can trigger if we get stuck.
            s.framesInState++;

            // Log per-second so diagnostic output shows lunge duration
            // cleanly (vs the once-at-entry log in TransitionTo).
            if (s.framesInState > 0 && (s.framesInState % 20) == 0 && DiagEnabled()) {
                SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} LUNGE_YIELD framesInState={} "
                            "vanillaActionFunc=0x{:x} snapshot=0x{:x}",
                            (uintptr_t)self, s.framesInState,
                            (uintptr_t)self->actionFunc,
                            (uintptr_t)s.initialAmbientActionFunc);
            }

            // Post-lunge preemption: if vanilla has moved off the
            // initial lunge actionFunc into a follow-up state (walk-
            // home / post-lunge-stop) but hasn't returned to ambient
            // yet, force it back to ambient so the state machine can
            // re-take control on the next tick. Without this, vanilla
            // walks the spider in a straight line to actor.home.pos
            // (the original wall spawn point), bypassing our nav
            // substrate and reverting to the wall-oriented rotation.
            if (s.lungeEntryActionFunc != nullptr &&
                self->actionFunc != s.lungeEntryActionFunc &&
                self->actor.colChkInfo.health > 0) {
                // Health check — if the spider is dead, vanilla has
                // set actionFunc to func_80B0D878 (death sequence).
                // Forcing back to ambient here would interrupt the
                // death animation + stick-drop spawn. Let vanilla
                // drive death sequences unimpeded.
                if (DiagEnabled()) {
                    SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} LUNGE_YIELD preempt "
                                "post-lunge state: vanillaActionFunc=0x{:x} "
                                "lungeEntry=0x{:x} → ForceAmbient",
                                (uintptr_t)self,
                                (uintptr_t)self->actionFunc,
                                (uintptr_t)s.lungeEntryActionFunc);
                }
                EnSw_ForceAmbient(self);
                s.hasWallBasis = false;
                TransitionTo(self, s, EnSwState::Uninitialized,
                             "lunge_yield_post_lunge_preempt");
                return;
            }

            // Safety timeout — vanilla lunge cycle typically completes in
            // 60-120 frames. If we've been yielded >300 frames (~15s at
            // 20fps), something's wrong: vanilla actionFunc got stuck OR
            // the actor entered a state we don't understand. Force back
            // to Uninitialized so the state machine can re-establish
            // control. Worst case: repeated re-entry into LungeYield if
            // vanilla is genuinely doing something long-running.
            constexpr int kLungeYieldTimeoutFrames = 300;
            if (s.framesInState > kLungeYieldTimeoutFrames) {
                EnSw_ForceAmbient(self);
                s.hasWallBasis = false;
                TransitionTo(self, s, EnSwState::Uninitialized,
                             "lunge_yield_timeout");
            }
        }
        return;
    }

    // Vanilla returned to ambient — clear LungeYield.
    if (s.state == EnSwState::LungeYield) {
        // Re-establish basis after lunge (position + rotation may have
        // moved during vanilla lunge cycle).
        s.hasWallBasis = false;
        TransitionTo(self, s, EnSwState::Uninitialized, "vanilla_returned_to_ambient");
    }

    s.framesInState++;

    // (s.isWalkAnimActive reset at top of Tick, before yield-check.)

    switch (s.state) {
        case EnSwState::Uninitialized:
            TickUninitialized(self, play, s);
            break;
        case EnSwState::WallIdle:
            TickWallIdle(self, play, s);
            break;
        case EnSwState::WallPursue:
            TickWallPursue(self, play, s);
            break;
        case EnSwState::WallEdgeDrop:
            TickWallEdgeDrop(self, play, s);
            break;
        case EnSwState::GroundPursue:
            TickGroundPursue(self, play, s);
            break;
        case EnSwState::GroundToWallReattach:
            TickGroundToWallReattach(self, play, s);
            break;
        case EnSwState::LungeYield:
            // Unreachable here — yield-check above returns early when
            // LungeYield is the correct state. Left in the switch for
            // exhaustiveness against the enum.
            break;
        case EnSwState::JumpLunge:
            TickJumpLunge(self, play, s);
            break;
        case EnSwState::WalkLunge:
            TickWalkLunge(self, play, s);
            break;
        case EnSwState::PermanentlyDisabled:
            // No-op forever. Vanilla actionFunc runs unchanged (which
            // for combat En_Sw is: ambient wait + lunge cycle via
            // vanilla's own state machine).
            break;
    }

    // SkelAnime playSpeed override — freeze when spider isn't walking.
    // Vanilla's built-in playSpeed rhythm (func_80B0E430 in z_en_sw.c
    // — freezes anim when unk_388 > 0, runs at 6× when converged +
    // reset) BREAKS for our ground spider because our hold branches
    // clobber shape.rot.z = 0 each frame, preventing vanilla's
    // Math_SmoothStepToS(&shape.rot.z, unk_444, ...) from ever
    // reaching unk_444 (the trigger for func_80B0E430 to return 1 →
    // func_80B0E5E0 to reset unk_388). Result: unk_388 stays 0, anim
    // never freezes, body/head/mouth animate perpetually even when
    // spider is idle. Log 797 evidence: user report "ground spider
    // walk animation always playing while sitting still" —
    // wall spider unaffected because our WallIdle handler doesn't
    // clobber shape.rot.z, so vanilla's rhythm still works there.
    // Fix: take direct control. isWalkAnimActive false → freeze,
    // true → run at 1.0. Simpler than un-clobbering shape.rot.z (which
    // would require substantially reworking the ground orientation
    // model) and universally correct (works for wall/ground/any
    // future consumer that adopts the isWalkAnimActive flag).
    //
    // Bug 1 fix (2026-07-31) — this write is now STATE-CONDITIONAL.
    // Applied ONLY for states where our machine owns motion (per
    // IsAnimAuthoritative). For WallIdle and other vanilla-owned
    // states, we DON'T touch playSpeed and vanilla's rest/look
    // rhythm runs unimpeded (rotation animation plays during look
    // phase). Previously the unconditional write clobbered vanilla's
    // playSpeed during rotation → user report "no leg animation
    // during wall rotation + rotates more, stops less".
    //
    // Bug 2 fix (2026-07-31) — the write also SCALES by animMotionRate
    // (0..1) instead of binary 0/1. Small motions get proportionally
    // slower leg animation; run/dash gets full speed. Floor at
    // kMinAnimPlaySpeed so barely-moving spiders still show a visible
    // slow cycle rather than freezing mid-motion.
    if (IsAnimAuthoritative(s.state)) {
        if (s.isWalkAnimActive) {
            const float rate = std::max(s.animMotionRate,
                                          kMinAnimPlaySpeed);
            self->skelAnime.playSpeed = std::min(rate, 1.0f);
        } else {
            self->skelAnime.playSpeed = 0.0f;
        }
    }
    // else: leave playSpeed alone — vanilla func_80B0E5E0 /
    // func_80B0E430 manage it for wall-idle rotation animation.
}

void EnSw_EnhancedStateMachine_SnapshotAmbient(EnSw* self) {
    if (self == nullptr) return;
    EnSwEnhancedState& s = GetOrCreate(self);
    if (s.haveAmbientSnapshot) return;  // idempotent
    s.initialAmbientActionFunc = self->actionFunc;
    s.haveAmbientSnapshot = true;
    if (DiagEnabled()) {
        SPDLOG_INFO("[EEDiag/SM] actor=0x{:x} ambient snapshot: actionFunc=0x{:x}",
                    (uintptr_t)self, (uintptr_t)self->actionFunc);
    }
}

void EnSw_EnhancedStateMachine_Forget(EnSw* self) {
    if (self == nullptr) return;
    sStates.erase(self);
}

EnSwState EnSw_EnhancedStateMachine_QueryState(EnSw* self) {
    auto* s = Find(self);
    return (s == nullptr) ? EnSwState::Uninitialized : s->state;
}

bool EnSw_EnhancedStateMachine_IsWalkAnimActive(EnSw* self) {
    auto* s = Find(self);
    return (s != nullptr) && s->isWalkAnimActive;
}

// 2026-08-04 (Pillar 5 Phase 3) — mark as spawned via En_St→En_Sw
// swap. GetOrCreate ensures the state block exists before the flag
// is set (the En_Sw's own Tick call may not have run yet at this point).
void EnSw_EnhancedStateMachine_MarkFromStSwap(EnSw* self) {
    if (self == nullptr) return;
    auto& s = GetOrCreate(self);
    s.spawnedFromStSwap = true;
    SPDLOG_INFO("[EnStSwap] Marked En_Sw as spawned-from-St-swap actor={}",
                (void*)&self->actor);
}

}  // namespace AnchorEnemyEnhancement
