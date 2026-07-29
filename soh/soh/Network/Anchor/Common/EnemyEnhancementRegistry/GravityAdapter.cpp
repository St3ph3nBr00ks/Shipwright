/**
 * GravityAdapter — Phase 1 stub implementation.
 *
 * Real behavior lands in Phase 2. See
 * Plans/vanilla_enemy_enhancements_plan.md §4.7 + §7 Phase 2.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before GravityAdapter.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

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
