/**
 * ClimbableSurfaces — pure detection helper for vertical climb paths.
 *
 * Plan §8. Walks ACTORCAT_BG / ACTORCAT_PROP for known climbable scene
 * actor IDs (ladders, vine props), returning the nearest match's foot
 * + estimated top positions. No hooks, no per-frame state — purely
 * a query helper invoked on demand by VerticalTeleport.
 *
 * v1 covers Path A (scene-actor allowlist) only. The vine surface-flag
 * scan (Path B) and the static-geometry fallback (Path C) are reserved
 * for later commits; the helper simply returns no-hit when Path A
 * misses, and the caller falls back to direct attach (existing follower
 * G1/G2 behaviour).
 *
 * Default-off: gated by gEnhancements.Nav.Enabled AND
 * gEnhancements.Nav.ClimbableSurfaces. The CVar exists for parity with
 * sibling modules even though no perf cost is incurred when the helper
 * isn't invoked. Predicates in VerticalTeleport that consume this
 * helper short-circuit when this CVar is off, matching the existing
 * "feature toggle" UX.
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §8
 *   - GitHub #206 Phase 1 commit 5
 *
 * Note: AnchorNavRoom::ClimbAnchor (RoomNavData.h) is the same shape
 * but in a different namespace; per the handoff plan §5 commit 5, we
 * intentionally duplicate the small struct here rather than introduce
 * a shared header. The two structs may diverge later.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

enum class ClimbType : uint8_t {
    None      = 0,
    Vine      = 1,  // scene-baked vine wall (reserved for Path B)
    Ladder    = 2,  // scene actor (Bg_Spot18_Obj, Bg_Ddan_Kd, etc.)
    StaticGeo = 3,  // rough scaling via collision wall (reserved for Path C)
};

// Anchor record for a climbable scene element. `basePos` is the foot
// of the climb; `topPos` is the estimated top (per-actor table value
// added to basePos.y). `actorId` carries the scene actor's id for
// debugging / future Path B hookup; 0 if the anchor came from static
// geometry (not yet implemented).
struct ClimbAnchor {
    Vec3f     basePos = { 0.0f, 0.0f, 0.0f };
    Vec3f     topPos  = { 0.0f, 0.0f, 0.0f };
    int16_t   actorId = 0;     // scene actor id; 0 if static-geometry
    int16_t   approachYaw = 0; // s16 angle units; bearing from basePos to topPos's XZ
    ClimbType type    = ClimbType::None;
};

// Find the nearest climbable scene actor whose basePos is within
// `maxRadius` of `from`. Returns std::nullopt if none found.
//
// `play` may be nullptr → returns nullopt.
std::optional<ClimbAnchor> FindNearestClimbable(const Vec3f& from, float maxRadius, PlayState* play);

// True if the anchor's source actor is still loaded in the active
// scene (Path A). For Path B/C anchors this returns true unconditionally
// in v1 (no surface-flag re-validation yet).
bool IsAnchorStillValid(const ClimbAnchor& anchor, PlayState* play);

// True when the master Nav CVar is on AND Nav.ClimbableSurfaces is on.
bool IsClimbableSurfacesEnabled();

// ShipInit registration. Currently a no-op (pure helper module);
// kept for parity with sibling nav modules.
void RegisterClimbableSurfaces();

} // namespace AnchorNav
