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

// functions.h + z64bgcheck.h pulled in transitively via EnSwStateMachine.h's
// z_en_sw.h -> global.h -> {functions.h, z64.h -> z64bgcheck.h}. Pitfall 40:
// do NOT wrap functions.h / z64.h directly here — they'd be no-ops due to
// include guards but the include-order rule is what keeps the pattern clean
// for future readers. BgCheck_EntityLineTest1 + COLPOLY_GET_NORMAL are both
// in scope from the transitive chain above.

#include <cmath>
#include <cstdint>
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

// Target-detect radius (squared distance for cheap comparison). Used
// by TickWallIdle / TickWallPursue / TickGroundPursue / TickGround-
// ToWallReattach to gate "target close enough to pursue". Vanilla
// En_Sw uses 130u for its lunge-target-acquisition predicate; we go
// slightly higher (200u) so the spider can see the player from wall
// placement and start crawling toward them, but not so high that
// spiders across the room start migrating (previous 600u caused
// spiders on the ceiling of Deku Tree main room to descend to the
// floor whenever any player entered).
constexpr float kIdleDetectRangeSq = 200.0f * 200.0f;

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
        case EnSwState::PermanentlyDisabled:  return "DISABLED";
    }
    return "?";
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
        self->actor.world.pos.x = h.pos.x + h.normal.x * kBodySurfaceOffset;
        self->actor.world.pos.y = h.pos.y + h.normal.y * kBodySurfaceOffset;
        self->actor.world.pos.z = h.pos.z + h.normal.z * kBodySurfaceOffset;

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
// Watch for a target entering detect range → transition to WallPursue.
void TickWallIdle(EnSw* self, PlayState* play, EnSwEnhancedState& s) {
    LogTickState(self, s, "holding");

    // Target detection — mirror of what NavConsumer used to do. Uses
    // FindNearestPlayerActor (which honours dead-player + cutscene
    // filters landed 2026-06-17).
    Actor* target = FindNearestPlayerActor(&self->actor, play);
    if (target == nullptr) return;  // no valid target; stay idle

    const float dx = target->world.pos.x - self->actor.world.pos.x;
    const float dz = target->world.pos.z - self->actor.world.pos.z;
    if ((dx * dx + dz * dz) > kIdleDetectRangeSq) return;

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
                        self->actor.world.pos.y = floorHit.y + kBodySurfaceOffset;
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

    // Target lookup + range checks.
    Actor* target = FindNearestPlayerActor(&self->actor, play);
    if (target == nullptr) {
        TransitionTo(self, s, EnSwState::WallIdle, "target_lost");
        return;
    }

    const float dxT = target->world.pos.x - self->actor.world.pos.x;
    const float dzT = target->world.pos.z - self->actor.world.pos.z;
    const float distXZSq = dxT * dxT + dzT * dzT;
    if (distXZSq > kIdleDetectRangeSq) {
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
    const float dyT = target->world.pos.y - self->actor.world.pos.y;
    const float dist3DSq = distXZSq + dyT * dyT;
    const bool inAttackRange = (dist3DSq <= kAttackRangeSq);

    // Rule 2 — spider on wall + Link off wall + close enough → jump.
    // Skipped when Link is climbing (any surface), since that's rule 1
    // (walk-lunge only). Range gate: kJumpMinTriggerRange < dist3D <=
    // kJumpTriggerRange, so close-range walking still resolves via the
    // vanilla lunge chain instead of a jump.
    const Player* targetPlayer = reinterpret_cast<const Player*>(target);
    const bool linkClimbing =
        (targetPlayer->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
    const float dist3D = std::sqrt(dist3DSq);
    if (!linkClimbing &&
        dist3D > kJumpMinTriggerRange &&
        dist3D <= kJumpTriggerRange) {
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

// Gravity + terminal velocity for airborne spider. Values match the
// prior GravityAdapter Phase 2 body (which is stubbed post-M1) so if
// that helper is ever revived for other wall-crawlers the physics
// stays consistent.
constexpr float kGravityAccel   = -1.2f;
constexpr float kMaxFallSpeed   = -20.0f;

// Ground pursuit tuning. Slower than wall pursuit because spider legs
// on floor don't have the same purchase as tangent-plane wall walk.
constexpr float kGroundWalkSpeed = 2.0f;
constexpr float kGroundRunSpeed  = 3.5f;

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
            self->actor.world.pos.y = hitPos.y + kBodySurfaceOffset;
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
                self->actor.world.pos.y = floorHit.y + kBodySurfaceOffset;
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

    Actor* target = FindNearestPlayerActor(&self->actor, play);
    if (target == nullptr) {
        TransitionTo(self, s, EnSwState::Uninitialized, "target_lost_on_ground");
        return;
    }

    const float dx = target->world.pos.x - self->actor.world.pos.x;
    const float dz = target->world.pos.z - self->actor.world.pos.z;
    const float distXZSq = dx * dx + dz * dz;
    if (distXZSq > kIdleDetectRangeSq) {
        TransitionTo(self, s, EnSwState::Uninitialized, "out_of_range_on_ground");
        return;
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

    // Attack-range hold uses 3D distance so a climbing target directly
    // above the spider (small distXZ, large distY) doesn't trigger
    // premature hold. Reported symptom: spider waited at wall base
    // when player was climbing above. With 3D check, spider stays in
    // pursuit mode → hits wall via forward probe → GroundToWallReattach
    // → transitions to WallPursue to climb after the player.
    constexpr float kAttackRangeSq = 50.0f * 50.0f;
    const float dyG = target->world.pos.y - self->actor.world.pos.y;
    const float dist3DSq = distXZSq + dyG * dyG;
    if (dist3DSq <= kAttackRangeSq) {
        // Rule 3 — spider ANY position + Link off wall + close enough
        // + no path → JumpLunge. "No path" is proxied by LoS check:
        // if a raycast from spider to Link is blocked by geometry,
        // walk-lunge cannot reach Link (direct-yaw motion doesn't
        // route around obstacles) and jumping is the only option.
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
            if (losBlocked) {
                SetupJumpToward(s, self->actor.world.pos, target->world.pos);
                TransitionTo(self, s, EnSwState::JumpLunge, "ground_jump_no_path");
                return;
            }
        }
        // Force vanilla's wind-up timer to 0 so `func_80B0E5E0`'s
        // next-frame check `(DECR(unk_442) == 0 && picker_ok)` fires
        // immediately. The picker itself succeeds because
        // ShouldRelaxLungeGates returns true when NavConsume is on,
        // skipping the climbing-required gate for our floor target.
        // Vanilla then transitions actionFunc to `func_80B0E728`
        // (purple wind-up flash + lunge motion), and our snapshot
        // detector routes us into LungeYield to let vanilla drive.
        self->unk_442 = 0;
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
    static constexpr float   kBodyBobAmplitude   = 1.5f;   // world units +Y
    static constexpr int16_t kBodyPitchAmplitude = 0x0400;  // ~4.5°
    static constexpr int16_t kBodyRollAmplitude  = 0x05C0;  // ~8°
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
    self->actor.world.pos.x += fwdTangent.x * speed;
    self->actor.world.pos.y += fwdTangent.y * speed;
    self->actor.world.pos.z += fwdTangent.z * speed;

    // Wall-hit detection — cast forward from actor.pos in the direction
    // we just moved. If we hit a wall within kGroundForwardProbe, try
    // to reattach next tick. Uses fwdTangent so the probe follows slope.
    Vec3f from = self->actor.world.pos;
    Vec3f to = {
        from.x + fwdTangent.x * kGroundForwardProbe,
        from.y + fwdTangent.y * kGroundForwardProbe,
        from.z + fwdTangent.z * kGroundForwardProbe,
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
constexpr int   kJumpWindupFrames    = 10;     // ~0.5 s telegraph at 20fps
constexpr float kJumpInitialVy       = 12.0f;  // upward launch velocity
constexpr float kJumpForwardSpeed    = 10.0f;  // horizontal launch magnitude
constexpr int   kJumpMaxAirFrames    = 90;     // ~4.5 s safety cap
constexpr float kJumpTriggerRange    = 300.0f; // rule 2/3 max spider→link
                                                // distance for jump trigger
constexpr float kJumpMinTriggerRange = 60.0f;  // don't jump if already at
                                                // point-blank walk range

// Populate s.jumpVel* with horizontal aim toward target + fixed upward
// launch. Called from rule 2 (TickWallPursue) and rule 3 (TickGroundPursue)
// trigger sites before TransitionTo(JumpLunge). jumpAirborne=false so
// TickJumpLunge's wind-up phase runs first.
void SetupJumpToward(EnSwEnhancedState& s, const Vec3f& spiderPos,
                     const Vec3f& targetPos) {
    const float dx = targetPos.x - spiderPos.x;
    const float dz = targetPos.z - spiderPos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ < 0.001f) {
        s.jumpVelX = 0.0f;
        s.jumpVelZ = 0.0f;
    } else {
        const float inv = 1.0f / distXZ;
        s.jumpVelX = dx * inv * kJumpForwardSpeed;
        s.jumpVelZ = dz * inv * kJumpForwardSpeed;
    }
    s.jumpVelY     = kJumpInitialVy;
    s.jumpAirborne = false;
}

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
    if (s.framesInState < kJumpWindupFrames) {
        LogTickState(self, s, "jump_windup");
        return;
    }

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

    // Airborne — integrate position with gravity on Y component.
    self->actor.velocity.y += kGravityAccel;
    if (self->actor.velocity.y < kMaxFallSpeed) {
        self->actor.velocity.y = kMaxFallSpeed;
    }
    self->actor.world.pos.x += self->actor.velocity.x;
    self->actor.world.pos.y += self->actor.velocity.y;
    self->actor.world.pos.z += self->actor.velocity.z;

    LogTickState(self, s, "jump_airborne");

    // Landing detection — raycast from just-above-actor down through
    // this-tick's velocity.y sweep to catch floor/wall contact.
    Vec3f from = self->actor.world.pos;
    from.y += 5.0f;
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
            // Floor landing.
            self->actor.world.pos.y = hitPos.y + kBodySurfaceOffset;
            self->actor.velocity.x = self->actor.velocity.y =
                self->actor.velocity.z = 0.0f;
            s.jumpAirborne = false;
            TransitionTo(self, s, EnSwState::GroundPursue, "jump_landed_floor");
            return;
        }
        if (std::fabs(ny) < kWallNormalYThreshold) {
            // Wall contact — zero velocity, transition to Uninitialized
            // so the basis raycast re-establishes on the new wall.
            self->actor.velocity.x = self->actor.velocity.y =
                self->actor.velocity.z = 0.0f;
            s.jumpAirborne = false;
            s.hasWallBasis = false;
            TransitionTo(self, s, EnSwState::Uninitialized, "jump_hit_wall");
            return;
        }
        // Ceiling — ignore, keep falling.
    }

    // Safety timeout — void fall.
    if (s.framesInState > kJumpMaxAirFrames + kJumpWindupFrames) {
        s.jumpAirborne = false;
        TransitionTo(self, s, EnSwState::PermanentlyDisabled, "jump_timeout");
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
                self->actionFunc != s.lungeEntryActionFunc) {
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
        case EnSwState::PermanentlyDisabled:
            // No-op forever. Vanilla actionFunc runs unchanged (which
            // for combat En_Sw is: ambient wait + lunge cycle via
            // vanilla's own state machine).
            break;
    }
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

}  // namespace AnchorEnemyEnhancement
