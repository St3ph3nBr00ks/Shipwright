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

// Per-actor gravity state. Phase 1 empty; Phase 2 populates with
// stun timer, previous-frame airborne flag, land-frame stamp.
struct GravityAdapterState {
    // Phase 2 will populate.
};

// Called from the descriptor's per-tick hook after
// ShouldApplyGravity returned true. Phase 1 no-op returns false.
// Phase 2 returns true when velocity or state was written.
bool TickGravity(const EnemyEnhancementDescriptor& descriptor,
                 GravityAdapterState& state,
                 Actor* actor,
                 PlayState* play);

}  // namespace AnchorEnemyEnhancement
