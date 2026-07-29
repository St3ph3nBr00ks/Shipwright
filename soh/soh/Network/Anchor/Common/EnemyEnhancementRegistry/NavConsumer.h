/**
 * NavConsumer — shared substrate that drives a Flotilla-enhanced enemy
 * across the RoomNavData nav graph toward the nearest player.
 *
 * Phase 1 STUB. Real implementation lands in Phase 2 alongside the
 * En_Sw pilot. This file exists so descriptors can reference the API
 * without waiting for behavior.
 *
 * Design: descriptors call TickNavMovement from their OnNavTick
 * override; NavConsumer resolves the nearest target, refreshes / picks
 * a nav path via Common/AILocomotion/NavOrDirect, and applies velocity
 * to the actor toward the next subgoal. Uses NavStateTransitions +
 * StuckRecovery helpers already used by NPC Follower / NPC Invader.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.6.
 * Registry.md pattern: this is the shared substrate; per-actor logic
 * (climb mask, speeds, ranges) comes from the descriptor's NavParams.
 */

#pragma once

#include "EnemyEnhancementDescriptor.h"

extern "C" {
#include "z64.h"
}

namespace AnchorEnemyEnhancement {

// Per-actor nav state. Descriptors hold one instance per instance of
// their vanilla actor (via ObjectExtension). Fields opaque to callers;
// the struct is passed by pointer through the API.
//
// Phase 1: struct exists but has no fields yet. Phase 2 populates
// with NavPath cache, last-refresh frame, target snapshot, etc.
struct NavConsumerState {
    // Phase 2 will populate. Empty in Phase 1 so the header compiles
    // without pulling in Common/AILocomotion/NavOrDirect.h yet.
};

// Called by the descriptor's OnNavTick. Returns true when velocity
// was actually written to the actor. Descriptor supplies NavParams
// (climb mask, speeds, ranges) via its virtual dispatch — helper
// stays generic and shared across per-actor consumers. Takes
// non-const descriptor for parity with GravityAdapter and future
// non-const virtual invocations.
bool TickNavMovement(EnemyEnhancementDescriptor& descriptor,
                     NavConsumerState& state,
                     Actor* actor,
                     PlayState* play);

}  // namespace AnchorEnemyEnhancement
