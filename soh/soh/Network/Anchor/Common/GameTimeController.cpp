#include "GameTimeController.h"
#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace GameTimeController {

// Phase 1 stub: returns the legacy answer for every context, preserving
// existing behaviour. Pillar G.i implementation
// (Claude/Plans/pillar_g_time_control.md) flips PauseMenu to return true
// in multiplayer mode.
//
// "Legacy answer" here means: what the existing call site at the routed
// location would have answered before this gate existed.
static bool LegacyAdvanceWorldTimeRule(TimeContext ctx) {
    if (gPlayState == nullptr) return true;
    switch (ctx) {
        case TimeContext::PauseMenu:
            // Time advances when pause menu is OFF (state == 0; the codebase
            // uses the literal — there is no PAUSE_STATE_OFF macro).
            return gPlayState->pauseCtx.state == 0;
        case TimeContext::TextBox:
            return gPlayState->msgCtx.msgMode == MSGMODE_NONE;
        case TimeContext::ItemGet: {
            Player* player = GET_PLAYER(gPlayState);
            return player == nullptr || !(player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM);
        }
        case TimeContext::Cutscene:
            return gPlayState->csCtx.state == CS_STATE_IDLE;
        case TimeContext::Ocarina:
            return gPlayState->msgCtx.ocarinaMode == OCARINA_MODE_00;
        case TimeContext::SceneTransition:
            return gPlayState->transitionMode == TRANS_MODE_OFF;
    }
    return true;
}

bool ShouldAdvanceWorldTime(TimeContext context) {
    // Phase 1: pure passthrough to legacy. Future implementation
    // (Pillar G.i) checks Anchor::Instance->isEnabled and flips
    // PauseMenu to always-true in multiplayer.
    return LegacyAdvanceWorldTimeRule(context);
}

}  // namespace GameTimeController
