/**
 * AutoTraverse — CVar-gated automatic entrance-warp driver for SceneLog.
 *
 * Phase 1: skeleton + single-entrance warp test.
 *   - Set gDeveloperTools.SceneLog.AutoTraverse.Cursor = N.
 *   - Set gDeveloperTools.SceneLog.AutoTraverse.Mode = 1.
 *   - Game warps to entrance N, holds HoldFrames frames, then advances
 *     cursor to N+1 and warps again. Stops when cursor reaches
 *     MaxEntrance.
 *   - Existing SceneLog hooks (Phase A/B) capture per-room manifest
 *     data on each landing.
 *   - Existing RoomNavData hooks (room-entry trigger) capture nav
 *     graphs as a side-effect — verified in this phase by checking for
 *     roomnavdata_<n>_<m>.bin files appearing after one warp.
 *
 * Phases 2-6 (sequential traversal across all entrances, hang detection,
 * crash recovery, multi-pass alt-header coverage, state-injection
 * helpers, summary report) are deferred. See
 * Plans/implementation_plan_auto_traversal.md.
 *
 * Direct entrance-warp (`gPlayState->nextEntranceIndex` +
 * `transitionTrigger`) bypasses En_Holl loading-plane routing — the path
 * implicated in the room-loop bug per BugFindings_RoomLoopOnDoorExit.md.
 * This is intentional and required.
 *
 * Refusal-to-start guards:
 *   - gPlayState == nullptr (game not yet running).
 *   - gameMode != GAMEMODE_NORMAL (title screen, file select, credits).
 * The 2026-05-07 first manifest run captured the title-screen attract
 * demo (Hyrule Field Epona ride) because the CVar was enabled before
 * save load. This guard prevents that.
 *
 * See:
 *   - Claude/Plans/agent_brief_scenelog_completion.md §5
 *   - Claude/Plans/implementation_plan_auto_traversal.md
 *   - GitHub #201, #202
 */

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_AT_MODE          CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Mode")
#define CVAR_AT_CURSOR        CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Cursor")
#define CVAR_AT_HOLD_FRAMES   CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.HoldFrames")
#define CVAR_AT_MAX_ENTRANCE  CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.MaxEntrance")

// Mode CVar values.
#define AT_MODE_OFF      0
#define AT_MODE_RUNNING  1
#define AT_MODE_PAUSED   2
#define AT_MODE_COMPLETE 3

#define AT_DEFAULT_HOLD_FRAMES 120  // 2 seconds at 60fps; enough for
                                    // static-actor spawn batch to complete
                                    // and RoomNavData scan (~216ms) to
                                    // settle.

namespace AutoTraverse {

// In-flight = a warp has been triggered but the scene/room hasn't
// committed yet. We watch for play->sceneNum to change before starting
// the hold timer.
static bool sInFlight = false;
static int sLastObservedSceneNum = -1;
static int sLastObservedRoomNum = -1;
static int sHoldFrameCounter = 0;

// One-shot: emit a [Complete] log line once when we transition to
// AT_MODE_COMPLETE. Without this, the log spams the same line every
// frame the CVar stays at 3.
static bool sCompleteEmitted = false;

static void TriggerEntranceLoad(int entranceIndex) {
    if (gPlayState == nullptr) return;
    SPDLOG_INFO("[AutoTraverse] Triggering entrance 0x{:04X}", entranceIndex);
    gPlayState->nextEntranceIndex = entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    sInFlight = true;
    sLastObservedSceneNum = gPlayState->sceneNum;
    sLastObservedRoomNum = gPlayState->roomCtx.curRoom.num;
    sHoldFrameCounter = 0;
}

// Returns true when it's safe to drive auto-traversal. Refuses to start
// from title screen / file select / credits, and from process-startup
// before gPlayState exists.
static bool SafeToTraverse() {
    if (gPlayState == nullptr) return false;
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) return false;
    return true;
}

static void OnFrameTick() {
    int mode = CVarGetInteger(CVAR_AT_MODE, AT_MODE_OFF);

    if (mode != AT_MODE_RUNNING) {
        // Reset one-shot Complete flag if mode toggles away from Complete.
        if (mode != AT_MODE_COMPLETE) {
            sCompleteEmitted = false;
        }
        return;
    }

    if (!SafeToTraverse()) {
        // Don't auto-shut-off; user may have just enabled this from the
        // title screen menu. Wait until the player's actually in a
        // loaded save. Log once per ~5 seconds (300 frames).
        static int sUnsafeWarnCounter = 0;
        if ((sUnsafeWarnCounter++ % 300) == 0) {
            SPDLOG_INFO("[AutoTraverse] Waiting for loaded save (Mode=1, but gPlayState/gameMode not ready)");
        }
        return;
    }

    if (sInFlight) {
        // Wait for the engine to commit the scene/room change before
        // starting the hold timer. Either the scene number or the room
        // number changed signals the warp landed.
        bool sceneChanged = (gPlayState->sceneNum != sLastObservedSceneNum);
        bool roomChanged = (gPlayState->roomCtx.curRoom.num != sLastObservedRoomNum);
        if (sceneChanged || roomChanged) {
            sInFlight = false;
            sHoldFrameCounter = CVarGetInteger(CVAR_AT_HOLD_FRAMES, AT_DEFAULT_HOLD_FRAMES);
            SPDLOG_INFO("[AutoTraverse] Warp committed: scene={} room={}; holding {} frames",
                        gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num, sHoldFrameCounter);
        }
        return;
    }

    if (sHoldFrameCounter > 0) {
        sHoldFrameCounter--;
        return;
    }

    // Hold expired; advance cursor and trigger the next warp.
    int cursor = CVarGetInteger(CVAR_AT_CURSOR, 0);
    int maxEntrance = CVarGetInteger(CVAR_AT_MAX_ENTRANCE, 0xFFFF);

    cursor++;
    if (cursor > maxEntrance) {
        SPDLOG_INFO("[AutoTraverse] Reached MaxEntrance ({}); marking Mode=Complete", maxEntrance);
        CVarSetInteger(CVAR_AT_MODE, AT_MODE_COMPLETE);
        sCompleteEmitted = true;
        return;
    }

    CVarSetInteger(CVAR_AT_CURSOR, cursor);
    TriggerEntranceLoad(cursor);
}

void RegisterAutoTraverse() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrameTick);
}

}  // namespace AutoTraverse

static RegisterShipInitFunc registerAutoTraverse(AutoTraverse::RegisterAutoTraverse);
