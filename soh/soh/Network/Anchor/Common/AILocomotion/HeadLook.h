/**
 * HeadLook — shared "look at target" head + upper-body yaw decision
 * for scripted-position AI actors (NPC Follower, NPC Invader; future
 * actors that share Link's skel + need head-tracking).
 *
 * Math mirrors NPC Follower's TickHeadLookAtLeader (FollowerNPC.cpp
 * pre-extraction). Pattern shape comes from Player's z_player.c:3735
 * but uses simpler step-toward semantics — no animation blending,
 * no special-case spine handling.
 *
 * Constraints:
 *   - Head yaw apportioned within ±kHeadYawMax (≈70°). Beyond that,
 *     upper-body twist takes over (capped at ±0x4000 = 90°).
 *   - Head pitch capped at ±0x2000 (45°).
 *   - Each component steps toward target at kStepRate per tick
 *     (~5° at 20fps).
 *
 * Caller owns the headRot / upperRot fields; helper writes them
 * directly via references. Caller decides WHEN to call this (e.g.,
 * gated on actor state — Invader has it disabled during CLIMBING,
 * LEDGE_HOIST, CRAWLING since those are body-locked anims where
 * head-tracking looks wrong).
 */

#pragma once

extern "C" {
#include "z64.h"
}

namespace AnchorAI {

struct HeadLookInputs {
    Vec3f actorPos;
    s16   actorYaw;     // shape.rot.y (used as facing-zero reference)
    Vec3f targetPos;

    // Step rate (binary angle units per tick). Default tuned for 20fps.
    s16   stepRate = 0x600;
    // Yaw apportionment cap. ±70° head, rest goes to upper body.
    // Field-tested value 12743 ≈ 0x31C7 — wider caps looked unnatural.
    s16   headYawMax = 12743;
    // Upper-body twist cap once yaw exceeds headYawMax.
    s16   upperYawMax = 0x4000;
    // Head pitch cap (looking up/down at target).
    s16   headPitchMax = 0x2000;
};

// Step the actor's head + upper-body rotation toward the target.
// Writes `*headRot.y`, `*headRot.x`, `*upperRot.y` in place via the
// non-owning pointers. `headRot.z` / `upperRot.x/z` are not touched.
void StepHeadLookToward(const HeadLookInputs& in,
                        Vec3s* headRot,
                        Vec3s* upperRot);

// Snap the head + upper-body rotation to neutral (zero) instantly.
// Used by callers' state gates that hard-disable head-tracking
// (e.g., CLIMBING, LEDGE_HOIST — body-locked anims where any non-
// zero head yaw produces unnatural angles).
void ResetHeadLookToNeutral(Vec3s* headRot, Vec3s* upperRot);

}  // namespace AnchorAI
