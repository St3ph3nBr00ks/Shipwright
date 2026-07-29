/**
 * NavConsumer — Phase 1 stub implementation.
 *
 * Real behavior lands in Phase 2. This file exists so descriptors
 * calling TickNavMovement don't produce a link error.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.6 + §7 Phase 2.
 */

#include "NavConsumer.h"

namespace AnchorEnemyEnhancement {

bool TickNavMovement(const EnemyEnhancementDescriptor& descriptor,
                     NavConsumerState& state,
                     Actor* actor,
                     PlayState* play) {
    (void)descriptor;
    (void)state;
    (void)actor;
    (void)play;
    // Phase 1: no-op. No descriptor calls this yet — En_SwDescriptor
    // arrives in Phase 2 with the real implementation delegating to
    // Common/AILocomotion/{NavOrDirect,NavStateTransitions,StuckRecovery}.
    return false;
}

}  // namespace AnchorEnemyEnhancement
