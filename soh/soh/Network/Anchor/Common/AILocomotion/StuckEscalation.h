/**
 * StuckEscalation — shared STUCK recovery escalation tiers for
 * scripted-position AI actors (NPC Follower, NPC Invader; future
 * follower / hostile actors with substrate paths).
 *
 * Ports AI Player Follower's G12 cycle tracking (Follower.cpp:1680).
 * Without escalation, a STUCK→nudge→STUCK loop can persist indefinitely
 * if the simple nudge keeps failing against the same geometry.
 *
 * Escalation tiers:
 *   Cycle 1: legacy nudge (caller fires).
 *   Cycle 2: edge-triggered navPath cursor advance — skip the
 *            unreachable subgoal. Caller fires the advance and the
 *            nudge. Latch (state.advancedAt) prevents the advance
 *            from re-firing every tick while still in STUCK.
 *   Cycle 3+: caller-fired teleport — to next path subgoal if
 *            available, otherwise to a fallback destination (leader
 *            for friendly follower, target for hostile).
 *
 * Cycle counter decays via state.resetFrames — armed at every new
 * STUCK entry, decremented each tick at the dispatcher entry. When
 * the window expires, the counter resets so the next unrelated stuck
 * episode starts fresh from cycle 1.
 *
 * Helper is pure decision logic. Caller owns the StuckCycleState
 * struct, the nudge math, the cursor advance call (NavPath::Advance),
 * the teleport target selection, and the world.pos write.
 */

#pragma once

#include <cstdint>

namespace AnchorAI {

struct StuckCycleState {
    uint32_t count       = 0;
    uint32_t resetFrames = 0;
    uint32_t advancedAt  = 0;  // latch for cycle-2 cursor advance
};

enum class StuckCycleAction {
    Nudge,          // cycle 1 — caller does the nudge + path reset
    CursorAdvance,  // cycle 2 — caller advances path cursor + does the nudge
    Teleport,       // cycle 3+ — caller picks teleport destination + writes pos
};

// Per-tick decay. Call once at the top of the actor's tick dispatcher.
// When resetFrames hits zero, count + advancedAt are cleared.
void TickStuckCycleWindow(StuckCycleState& state);

// Called when the FOLLOW handler observes no-progress and transitions
// to STUCK. Increments cycle count and arms the reset window.
// `windowTicks` is the framerate-aware tick count for the decay
// window (caller computes via Anchor::Instance->MsToGameTicks).
void NoteStuckEntered(StuckCycleState& state, int windowTicks);

// Read the current escalation tier. Returns the action the caller
// should take in TickSTUCK. Cycle 2 latching is the caller's
// responsibility — call MarkCursorAdvanced after firing the advance.
StuckCycleAction GetStuckAction(const StuckCycleState& state,
                                int escalationThreshold = 3);

// Vertical-dominant overload (Phase 4). When the separation to the
// target is vertical-dominant (|dy| >> distXZ — caller checks via
// AnchorAI::IsVerticalDominantSeparation in NavStateTransitions.h),
// horizontal nudge cycles are still applied by the caller's TickSTUCK
// body — but this overload upgrades the RETURNED action so the
// caller also fires a cursor advance in cycle 1 (instead of nudge
// alone) and escalates to a hard Teleport in cycle 2+.
//
//   verticalDominant=false → identical to the non-overloaded form.
//   verticalDominant=true  → cycle 1 returns CursorAdvance (the
//                            caller advances the path AND still
//                            does its legacy nudge — the legacy
//                            nudge code is unconditional after the
//                            action dispatch, so this is a net
//                            "advance + nudge" upgrade vs default
//                            cycle 1's "nudge only").
//                            cycle 2+ returns Teleport (caller
//                            short-circuits before the nudge).
//
// Caller still owns the world.pos write + path Reset/Advance — the
// helper just decides which tier the caller should execute.
StuckCycleAction GetStuckAction(const StuckCycleState& state,
                                int escalationThreshold,
                                bool verticalDominant);

// Caller fired the cycle-2 cursor advance. Latches state.advancedAt
// so the next GetStuckAction call won't re-fire the advance.
void MarkCursorAdvanced(StuckCycleState& state);

// Caller fired the cycle-3 teleport. Clears all cycle state so the
// next true-stuck episode starts fresh from cycle 1.
void OnStuckTeleportFired(StuckCycleState& state);

}  // namespace AnchorAI
