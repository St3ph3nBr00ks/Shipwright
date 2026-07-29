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
#include "../AILocomotion/NavOrDirect.h"  // AnchorAI::NavState

extern "C" {
#include "z64.h"
}

namespace AnchorEnemyEnhancement {

// Per-actor nav state. One instance per enhanced actor, owned by the
// descriptor's file-static map. Wraps AnchorAI::NavState (the shared
// substrate's per-navigator bookkeeping: path cache, last-refresh
// frame, target-drift tracking, trail key).
struct NavConsumerState {
    AnchorAI::NavState navState;
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
