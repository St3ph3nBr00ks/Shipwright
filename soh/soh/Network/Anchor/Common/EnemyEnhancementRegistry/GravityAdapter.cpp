/**
 * GravityAdapter — Phase 1 stub implementation.
 *
 * Real behavior lands in Phase 2. See
 * Plans/vanilla_enemy_enhancements_plan.md §4.7 + §7 Phase 2.
 */

#include "GravityAdapter.h"

namespace AnchorEnemyEnhancement {

bool TickGravity(const EnemyEnhancementDescriptor& descriptor,
                 GravityAdapterState& state,
                 Actor* actor,
                 PlayState* play) {
    (void)descriptor;
    (void)state;
    (void)actor;
    (void)play;
    // Phase 1: no-op. Phase 2 wires this to real gravity math against
    // GravityParams and floor-check via actor->bgCheckFlags.
    return false;
}

}  // namespace AnchorEnemyEnhancement
