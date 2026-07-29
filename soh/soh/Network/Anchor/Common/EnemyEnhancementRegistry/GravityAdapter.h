/**
 * GravityAdapter — shared substrate that applies gravity to an
 * enhanced enemy that's off its anchored surface.
 *
 * Phase 1 STUB. Real implementation lands in Phase 2 alongside the
 * En_Sw pilot.
 *
 * Design: descriptor's ShouldApplyGravity returns true when the
 * enemy is off its anchor (e.g. En_Sw knocked off a wall by a
 * player attack). TickGravity applies gravity from GravityParams
 * to actor->velocity.y, floor-check via bgCheckFlags, calls
 * descriptor->OnLandedFromFall when the fall resolves. Stun-on-land
 * writes a stun timer that the descriptor's OnLandedFromFall reads.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.7.
 */

#pragma once

#include "EnemyEnhancementDescriptor.h"

extern "C" {
#include "z64.h"
}

namespace AnchorEnemyEnhancement {

// Per-actor gravity state. Owned per-actor by the descriptor's state
// map; NOT thread-safe (game thread only).
struct GravityAdapterState {
    // Rising-edge detector for landing. Set true when TickGravity
    // integrated Y this frame (i.e. actor is airborne); cleared on
    // grounded. Landing transition = airborne last frame + grounded
    // this frame — that's when OnLandedFromFall fires and the stun
    // timer arms.
    bool wasAirborneLastFrame = false;

    // Post-landing stun timer. Non-zero while the actor should hold
    // still after a fall. Caller checks this via
    // descriptor->OnLandedFromFall side-effect and its own state
    // machine — GravityAdapter only writes velocity, not action state.
    uint16_t stunFramesRemaining = 0;
};

// Called from the descriptor's per-tick hook after
// ShouldApplyGravity returned true. Returns true when velocity or
// state was written (caller may treat as "gravity tick applied").
//
// Takes non-const descriptor so it can invoke non-const virtual hooks
// (OnLandedFromFall). Descriptor is logically stateless — the
// non-const-ness is just to satisfy the virtual-dispatch contract.
bool TickGravity(EnemyEnhancementDescriptor& descriptor,
                 GravityAdapterState& state,
                 Actor* actor,
                 PlayState* play);

}  // namespace AnchorEnemyEnhancement
