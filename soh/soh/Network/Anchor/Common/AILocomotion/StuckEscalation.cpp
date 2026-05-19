/**
 * StuckEscalation — see StuckEscalation.h for purpose.
 *
 * Logic source: FollowerNPC.cpp + Invader.cpp STUCK cycle handling
 * (both ported from Player AI Follower's G12 — Follower.cpp:1680).
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

void MarkCursorAdvanced(StuckCycleState& state) {
    state.advancedAt = 2;
}

void OnStuckTeleportFired(StuckCycleState& state) {
    state.count       = 0;
    state.resetFrames = 0;
    state.advancedAt  = 0;
}

}  // namespace AnchorAI
