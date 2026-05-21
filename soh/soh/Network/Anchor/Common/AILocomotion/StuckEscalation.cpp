/**
 * StuckEscalation — see StuckEscalation.h for purpose.
 *
 * Logic source: FollowerNPC.cpp + Invader.cpp STUCK cycle handling
 * (both ported from AI Player Follower's G12 — Follower.cpp:1680).
 */

#include "StuckEscalation.h"

namespace AnchorAI {

void TickStuckCycleWindow(StuckCycleState& state) {
    if (state.resetFrames > 0) {
        state.resetFrames--;
        if (state.resetFrames == 0) {
            state.count      = 0;
            state.advancedAt = 0;
        }
    }
}

void NoteStuckEntered(StuckCycleState& state, int windowTicks) {
    state.count++;
    state.resetFrames = (windowTicks > 0) ? (uint32_t)windowTicks : 0;
}

StuckCycleAction GetStuckAction(const StuckCycleState& state,
                                int escalationThreshold) {
    if ((int)state.count >= escalationThreshold) {
        return StuckCycleAction::Teleport;
    }
    if (state.count == 2 && state.advancedAt != 2) {
        return StuckCycleAction::CursorAdvance;
    }
    return StuckCycleAction::Nudge;
}

StuckCycleAction GetStuckAction(const StuckCycleState& state,
                                int escalationThreshold,
                                bool verticalDominant) {
    if (!verticalDominant) {
        return GetStuckAction(state, escalationThreshold);
    }
    // Vertical-dominant: skip the Nudge tier — horizontal motion can't
    // close a Y gap. Cycle 1 promotes to CursorAdvance (lets the path
    // walk to the climb/drop subgoal); cycle 2+ promotes to Teleport.
    if (state.count >= 2) {
        return StuckCycleAction::Teleport;
    }
    if (state.advancedAt != 2) {
        return StuckCycleAction::CursorAdvance;
    }
    // Cursor already advanced this cycle — fall through to Teleport
    // rather than a doomed Nudge.
    return StuckCycleAction::Teleport;
}

void MarkCursorAdvanced(StuckCycleState& state) {
    state.advancedAt = 2;
}

void OnStuckTeleportFired(StuckCycleState& state) {
    state.count       = 0;
    state.resetFrames = 0;
    state.advancedAt  = 0;
}

}  // namespace AnchorAI
