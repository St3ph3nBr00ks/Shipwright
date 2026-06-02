#pragma once

// Shared sword blade-position tracking for AI actors using Link's
// skeleton (EnInvader, EnFollower, and any future actor that holds a
// sword via Player_DrawImpl).
//
// Why this exists — issue #238 / Plans/invader_combat_repair_sequenced_plan.md
// Step 2+4. Both NPC Invader and NPC Follower previously positioned
// their sword AT colliders as a fixed-distance plane 60u in front of
// the actor's body. The OoT collision system treats that as planar —
// only targets at ~60u distance register. When the actor closes to
// point-blank (typical post-ENGAGE→ATTACK), the plane overshoots and
// every swing misses.
//
// This helper mirrors how vanilla Player does its sword collision:
// each draw frame, the L_HAND limb's transform is on the matrix stack
// inside SkelAnime's post-limb callback. Multiplying a fixed local-
// space sword tip/base offset by the current matrix yields world-
// space positions that track the blade through the swing animation.
// The AT quad is built per-frame from those positions; collision is
// tested against the actual blade location at the time of registration.
//
// USAGE:
//   1. Actor (EnInvader / EnFollower) declares `Vec3f swordTip;
//      Vec3f swordBase;` fields.
//   2. Actor's draw function passes a custom post-limb callback to
//      Player_DrawImpl. The callback must check
//      `limbIndex == PLAYER_LIMB_L_HAND` (Link is left-handed; the
//      sword is in the left hand) and call
//      `Anchor_ComputeBladeWorldFromMatrix(&this->swordTip, &this->swordBase)`.
//   3. Actor's per-frame AT-positioning function (called inside the
//      ATTACK swing's active-frame window) calls
//      `Anchor_BuildAtQuadFromBlade` to get the four quad vertices,
//      then passes them to Collider_SetQuadVertices.
//
// MUST be called from inside the post-limb callback while the L_HAND
// limb's matrix is on the stack — calling outside this window yields
// meaningless positions (likely identity matrix, putting the tip near
// world origin).
//
// SWORD LENGTH ASSUMPTION:
// Vanilla Player uses per-weapon lengths from sMeleeWeaponLengths[]
// (`z_player_lib.c:1719`) — Kokiri Sword 4000, Master Sword 3000,
// Biggoron 5500, etc., in limb-local units. For AI actors, AT
// collider geometry only needs to approximate; matching the exact
// visual sword type per frame isn't worth a per-frame inventory
// branch. Kokiri Sword's 4000-unit length is used as the default,
// scaled by the actor's world scale (typical Player scale 0.01 →
// 40-world-unit reach, similar to the prior fixed-plane reach).

#include <libultraship/libultraship.h>  // pre-load C++ bridge headers

#ifdef __cplusplus
extern "C" {
#endif

#include "z64.h"  // Vec3f, MtxF

// Transform the hardcoded sword tip + base local offsets through the
// CURRENT matrix stack to produce world-space positions. Must be
// called from inside a SkelAnime post-limb callback when the L_HAND
// limb is being drawn (matrix stack reflects the L_HAND transform).
//
// `outTip` receives the blade tip world position.
// `outBase` receives the hand-pivot world position (base of the
// blade closest to the hand).
//
// Caller is responsible for limb-index gating — this function does
// not check `limbIndex`.
void Anchor_ComputeBladeWorldFromMatrix(Vec3f* outTip, Vec3f* outBase);

// Build the four AT quad vertices spanning the blade from base to
// tip with `halfWidth` perpendicular extent on either side.
//
// The perpendicular direction is computed as cross(bladeDir,
// world_up). For a vertical sword swing this produces a horizontal
// quad strip whose XZ footprint passes through target cylinders'
// XZ footprint as the blade swings. For a horizontal swing the quad
// is vertical and similarly intersects.
//
// Degenerate case: if `tip == base` or the blade is exactly vertical
// (cross product zero), the function falls back to a world-X-axis
// perpendicular so the quad is still non-degenerate.
//
// Output Vec3f pointers are pass-by-pointer for C-compat. Vertex
// order matches Collider_SetQuadVertices's expected layout
// (bottomLeft, bottomRight, topLeft, topRight) — the names "bottom"/
// "top" map to base/tip respectively along the blade.
void Anchor_BuildAtQuadFromBlade(
    const Vec3f* tip, const Vec3f* base, float halfWidth,
    Vec3f* outBottomLeft, Vec3f* outBottomRight,
    Vec3f* outTopLeft, Vec3f* outTopRight);

#ifdef __cplusplus
}
#endif
