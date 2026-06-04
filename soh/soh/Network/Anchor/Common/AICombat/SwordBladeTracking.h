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

// Build the four AT quad vertices spanning the SWEPT VOLUME between
// the previous frame's blade and the current frame's blade.
//
// Mirrors vanilla Player's `func_80090480` (`z_player_lib.c:1566`)
// which builds Player's sword AT collider from `meleeWeaponInfo[0]`
// (current frame) AND `meleeWeaponInfo[1]` (previous frame). The
// area between two consecutive blade poses IS the swept arc the
// blade visually carved through this frame, and that's what should
// register as the AT hitbox.
//
// Field-test log 358 confirmed the prior single-snapshot quad
// (Anchor_BuildAtQuadFromBlade, removed) was ~5× smaller in area
// than vanilla's swept quad — covering only the blade at one
// instant rather than its swept volume. The user-visible symptom
// was "Invader's hit zone is quite small."
//
// Vertex layout (matches Collider_SetQuadVertices expected order):
//   bottomLeft  = previous frame's base (hand pivot, prev frame).
//   bottomRight = current frame's base.
//   topLeft     = previous frame's tip.
//   topRight    = current frame's tip.
//
// The "left edge" (bottomLeft → topLeft) is the previous frame's
// blade. The "right edge" (bottomRight → topRight) is the current
// frame's blade. The area between is the volume the blade swept
// through this frame.
//
// Degenerate case: if prev ≈ cur (stationary blade, or first frame
// after spawn before prev has been populated), the quad collapses
// toward a line and no hits register. That's correct behaviour —
// a stationary sword doesn't carve through anything.
void Anchor_BuildSweepAtQuadFromBlade(
    const Vec3f* prevTip, const Vec3f* prevBase,
    const Vec3f* curTip,  const Vec3f* curBase,
    Vec3f* outBottomLeft, Vec3f* outBottomRight,
    Vec3f* outTopLeft, Vec3f* outTopRight);

// Tier 1 refactor (2026-06-04) — convenience wrapper combining the
// quad construction above with Collider_SetQuadVertices, extracted
// from byte-identical PositionAttackQuad wrappers in NPC Follower
// (FollowerNPC.cpp:2280-2294) and NPC Invader (Invader.cpp:1947-1961).
// Per-actor wrappers now reduce to a one-line forward to this helper
// loaded with the actor's prev/cur blade fields and its atCollider.
// See Plans/npc_helpers_tier1_extract_2026-06-03.md item 2.
// ColliderQuad without `struct` keyword — z64collision_check.h
// (included via z64.h above) defines it as an anonymous-struct
// typedef, so there is no `struct ColliderQuad` tag. Adding the
// `struct` keyword would forward-declare a separate (incomplete)
// struct tag that doesn't match the typedef, breaking every TU
// that later sees `ColliderQuad atCollider;` field declarations.
void Anchor_PositionAttackQuadFromBlade(
    const Vec3f* prevTip, const Vec3f* prevBase,
    const Vec3f* curTip,  const Vec3f* curBase,
    ColliderQuad* atCollider);

#ifdef __cplusplus
}
#endif
