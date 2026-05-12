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

// Per-frame frame-input bundle for the follower state machine.
//
// HookHandlers.cpp's OnGameFrameUpdate body computes a number of
// per-frame derived values (leader pointer, leader position, distance,
// transition state, etc.) before the follower's state machine consumes
// them. Currently those values live as named locals in the giant
// OnGameFrameUpdate lambda; the state machine accesses them via lambda
// capture. This is what makes the state machine code hard to extract
// — the captures couple it to the enclosing scope.
//
// FollowerFrameContext is the explicit data dependency. The HookHandlers
// callback builds one of these per frame, then calls
// AnchorFollower::TickFollower(ctx). Subsequent Phase 1 commits move
// state-machine logic from the lambda into TickFollower (and from there
// into per-state helpers), reading from `ctx` instead of from lambda
// captures. The architectural value: at the end of Phase 1, the data
// flow into the follower is documented in one struct definition rather
// than spread across hundreds of lines of capture references.
//
// All fields are caller-owned references / value snapshots — TickFollower
// reads from `ctx` but doesn't outlive it. The struct is never stored or
// retained.
struct FollowerFrameContext {
    PlayState* play             = nullptr;       // gPlayState
    Player*    player           = nullptr;       // local player (the follower)
    Actor*     leaderActor      = nullptr;       // leader's DummyPlayer or nullptr
    Vec3f      leaderPos        = { 0, 0, 0 };   // leaderActor->world.pos snapshot
    f32        distToLeaderSq   = 0.0f;          // 3D squared dist; cached
    f32        distToLeaderXZ   = 0.0f;          // XZ-only dist; for proximity gates
    bool       roomsDiffer      = false;         // leader and follower in different rooms
    int8_t     leaderRoom       = -1;            // leader's room number per UPDATE_CLIENT_STATE
    bool       leaderIsClimbing = false;         // leader's UPDATE_CLIENT_STATE.isClimbing
    bool       leaderIsCrawling = false;         // leader's UPDATE_CLIENT_STATE.isCrawling
};

// Per-frame entry point. Called from HookHandlers.cpp's OnGameFrameUpdate
// after the context is built. Currently a stub — Phase 1 commit 4 will
// move the OnGameFrameUpdate follower body into this function.
//
// Returns nothing; mutates the Anchor:: follower-state members and
// optionally `ctx.player->actor.world.pos` for STUCK / teleport paths.
void TickFollower(FollowerFrameContext& ctx);

// Module registration. Called once by ShipInit at boot. Currently a
// no-op log line — verifies the new namespace + ShipInit boot path
// work end-to-end. Anchor's two follower hooks (state-machine driver +
// input injection) are (re-)registered per-connect from
// Anchor::RegisterFollowerHooks (declared in Anchor.h, defined in
// Follower.cpp) since they need Anchor:: state access and re-registration
// on each enable/disable cycle.
void RegisterFollowerModule();

// Phase 2 master gate. True when both gEnhancements.Nav.Enabled AND
// gEnhancements.Nav.AiFollowerConsumer are non-zero. Per-state handlers
// (HandleStateFollow, HandleStateEngage, HandleStateStuck,
// HandleStateClimbing) consult this to pick between:
//   - false (default): legacy bespoke pursuit / steering code (the
//     verbatim Phase 1 extraction).
//   - true: nav substrate (ActorTrail::ComputePathTo,
//     TargetSelection::ChooseTarget, GroundFollowing,
//     JumpResolver::ResolveLedgeAhead, VerticalTeleport Shape A wrapper).
//
// Default off; ships and stays off permanently per Flotilla policy.
// See feedback_vanilla_altering_default_off.md.
bool IsAiFollowerNavSubstrateEnabled();

} // namespace AnchorFollower
