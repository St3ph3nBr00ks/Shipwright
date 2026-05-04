#include "GameTimeController.h"
#include "GameTimeControllerBridge.h"
#include "soh/Network/Anchor/Anchor.h"  // ::Anchor::Instance / isEnabled
                                        // (also pre-loads libultraship.h + z64.h)
#include "soh/cvar_prefixes.h"          // CVAR_REMOTE_ANCHOR for live-world toggle

#include <libultraship/bridge/consolevariablebridge.h>  // CVarGetInteger

extern "C" {
#include "macros.h"
extern PlayState* gPlayState;
}

namespace GameTimeController {

// Returns the legacy single-player answer for each context — i.e. what
// the existing OoT call site at the routed location would have answered
// before this gate existed. ShouldAdvanceWorldTime layers Pillar-G
// multiplayer rules on top.
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
    // Pillar G.i: in multiplayer mode, the local pause menu does NOT
    // freeze world time — other players continue acting, so enemies,
    // NPCs, and scripted timers must keep advancing on this client too.
    // The pause-menu UI itself still renders normally; only the
    // "world ticks" gate flips.
    //
    // All other contexts (text-box, item-get, cutscene, ocarina,
    // scene-transition) continue returning the legacy answer until the
    // §4.G.ii rules land.
    const bool multiplayerActive = (::Anchor::Instance != nullptr) &&
                                   ::Anchor::Instance->isEnabled;
    if (multiplayerActive && context == TimeContext::PauseMenu) {
        return true;
    }
    return LegacyAdvanceWorldTimeRule(context);
}

}  // namespace GameTimeController

// ---------------------------------------------------------------------------
// C bridge — lets C translation units (z_play.c, z_actor.c, etc.) call into
// the C++ gate without exposing the namespace or enum class.
//
// Enum values must match the integer ordering of GameTimeController::TimeContext
// AND the ANCHOR_TIME_CTX_* macros in GameTimeControllerBridge.h. Keep all
// three in sync if a new context is added.
// ---------------------------------------------------------------------------
extern "C" bool Anchor_ShouldAdvanceWorldTime(int contextEnum) {
    return GameTimeController::ShouldAdvanceWorldTime(
        static_cast<GameTimeController::TimeContext>(contextEnum));
}

// Inverse predicate for direct call-site replacement of `pauseCtx.state != 0`.
// Reads more naturally at gameplay-freeze sites than negating the advance form.
extern "C" bool Anchor_PauseMenuFreezesWorld(void) {
    return !GameTimeController::ShouldAdvanceWorldTime(
        GameTimeController::TimeContext::PauseMenu);
}

// Tracker #182. Returns true when the live-world pause-menu rendering
// feature should activate this frame. Composition:
//   - Multiplayer is active (Anchor enabled).
//   - Pause UI is open (pauseCtx.state != 0). Equivalently, the world-
//     time gate above has flipped — Anchor_PauseMenuFreezesWorld() is
//     false because we're in MP-during-pause.
//   - The gAnchor.PauseLiveWorld CVar is set (default 0).
//
// All four pause-rendering gates (kaleido pause-Link init, equipment-
// screen rotating-Link draw, z_play.c mode-3 captured-frame backdrop,
// DummyPlayer_Draw early-return) consult this single predicate so the
// CVar acts as an atomic on/off for the feature. Default-off ships
// current safe behaviour while the rendering work soaks for one
// release cycle.
extern "C" bool Anchor_PauseLiveWorldRendering(void) {
    if (gPlayState == nullptr) return false;
    if (gPlayState->pauseCtx.state == 0) return false;  // pause UI not open
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isEnabled) return false;
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("PauseLiveWorld"), 0) != 0;
}

// Pause-menu animation acceleration carry. Returns true ~30% of calls so
// the kaleido path can fire one extra `KaleidoScope_Update` on those
// frames — net pause animation rate ≈ 1.3× world tick (20fps × 1.3 ≈
// 26fps for pause UI animations) without disturbing world-tick pacing.
//
// Only fires when live-world pause rendering is active; otherwise the
// carry resets to 0 so the rate is exact on the first pause-open.
extern "C" bool Anchor_PauseMenuShouldExtraTick(void) {
    static float sCarry = 0.0f;
    if (!Anchor_PauseLiveWorldRendering()) {
        sCarry = 0.0f;
        return false;
    }
    sCarry += 0.3f;
    if (sCarry >= 1.0f) {
        sCarry -= 1.0f;
        return true;
    }
    return false;
}
