/**
 * AiFollower / Follower — dedicated module for AI Follower state and
 * behaviour. Extracted from HookHandlers.cpp per the SRP refactor
 * tracked at GitHub #173 + #169.
 *
 * Phase 1 (this scaffolding + subsequent move commits) is a behaviour-
 * preserving refactor: the same state machine, same hook handlers, same
 * Anchor:: helper methods — just relocated to dedicated files and a
 * dedicated namespace. HookHandlers.cpp keeps only the cross-cutting
 * hook glue that's not follower-specific.
 *
 * Phase 2 (planned, separate commits) replaces the bespoke
 * pursuit / target-selection / steering code paths with calls to the
 * Anchor nav helpers (ActorTrail::ComputePathTo, TargetSelection,
 * GroundFollowing, JumpResolver). At the end of Phase 2 the follower
 * will route through the same nav substrate that synced enemies and
 * AI Invader use, and the bespoke path code disappears.
 *
 * Naming: namespace `AnchorFollower` — flat single-namespace per
 * project convention (do not nest under `Anchor`; conflicts with
 * `class Anchor` per Pitfall 11 in session_state.md).
 *
 * See:
 *   - Plans/anchor_code_decoupling.md (#173 — module-extraction tracker)
 *   - Plans/ai_follower_enhancements_plan.md (roadmap)
 *   - Plans/nav_system_implementation_plan.md (Phase 2 consumer wiring)
 *   - GitHub #169 (AI Follower demo-ready improvements parent tracker)
 */

#pragma once

#include <cstdint>

extern "C" {
#include "z64.h"
}

namespace AnchorFollower {

// Phase 1 commit 1: scaffolding only. No logic moved yet.
//
// Subsequent Phase 1 commits will populate this module with:
//   - State enum (FollowerAIState) + per-frame state struct
//   - Helper functions (TryEquipRangedWeapon, RestoreItems,
//     SetActive, IsLocalPlayerClimbing, TeleportToLeader, ...)
//   - State machine body (currently inside OnGameFrameUpdate)
//   - Input injection (currently inside ShouldActorUpdate)
//   - Tunable constants (kClimbDismountFrames, kPostTeleportFrames,
//     kFollowerButtons, etc.)
//
// Each move-commit is behaviour-preserving — the new home for code
// that already exists, with no semantic change.

// Module registration. Called once by ShipInit at boot. Sets up any
// hooks the follower module needs separately from Anchor's hook
// registration (currently none — hook bodies still live in
// HookHandlers.cpp; they migrate here in subsequent Phase 1 commits).
void RegisterFollowerModule();

} // namespace AnchorFollower
