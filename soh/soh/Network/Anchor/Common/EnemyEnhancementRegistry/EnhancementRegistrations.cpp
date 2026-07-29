/**
 * EnhancementRegistrations — one-stop registration of all enemy
 * enhancement descriptors.
 *
 * Central registration file per plan §4.2 — "no auto-registration
 * macros, no static-init-order ambiguity, grep-able and re-orderable
 * for debugging." Each new per-actor descriptor adds one line here.
 *
 * Runs via ShipInit before game main-loop starts. `RegisterShipInitFunc`
 * is the same lifecycle hook used by TeamMarker / NPC Companion / etc.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.2 + §7 Phase 2 step 3.
 */

#include "EnhancementRegistry.h"
#include "PerActor/EnSwDescriptor.h"

#include "soh/ShipInit.hpp"

namespace {

void RegisterEnemyEnhancementDescriptors() {
    auto& reg = AnchorEnemyEnhancement::EnhancementRegistry::Instance();

    // ----- Per-actor descriptors -----
    // Phase 2 pilot — Skullwalltula.
    reg.Register(std::make_unique<AnchorEnemyEnhancement::EnSwDescriptor>());

    // Future per-actor descriptors land here:
    //   reg.Register(std::make_unique<BariDescriptor>());       // Phase 4
    //   reg.Register(std::make_unique<FloormasterDescriptor>()); // Phase 5+
    //   ... etc.
}

}  // namespace

static RegisterShipInitFunc initFuncEnemyEnhancementDescriptors(
    RegisterEnemyEnhancementDescriptors, {});
