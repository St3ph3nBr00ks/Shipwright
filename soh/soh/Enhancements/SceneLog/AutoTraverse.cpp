/**
 * AutoTraverse — CVar-gated automatic entrance-warp driver for SceneLog.
 *
 * Phase 1: skeleton + single-entrance warp test (DONE; see commit
 *          417127742).
 * Phase 2: sequential traversal with pacing across ALL entrances
 *          in gEntranceTable[] (this commit).
 *   - Cursor advances by 4 per step instead of 1, because entrance_table.h
 *     groups entrances in 4-tuples (CHILD_DAY / CHILD_NIGHT / ADULT_DAY
 *     / ADULT_NIGHT scene layers). Only the first entry in each group
 *     is referenced in code; the entrance system applies the layer
 *     offset automatically based on linkAge + dayTime.
 *   - Caps at ENTR_MAX (0x614 = 1556), so a default MaxEntrance of
 *     0xFFFF terminates cleanly when the table is exhausted.
 *   - Persists session state to roommanifests/_autotraverse_state.json
 *     on every advance: cursor, entrances visited, scenes covered,
 *     timestamps. External tools can read this for progress monitoring.
 *
 * Phases 3-6 (hang detection / crash recovery, multi-pass alt-header
 * coverage, state-injection helpers, summary report) are deferred.
 * See Plans/implementation_plan_auto_traversal.md.
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
 * Hang failure mode (Phase 3 will fix): if a triggered warp doesn't
 * commit (sceneNum + roomNum unchanged), inFlight stays true forever.
 * The cursor doesn't advance. User must manually bump Cursor + restart.
 * Acceptable for Phase 2 — most entrances commit cleanly; pathological
 * cases are rare and visible in the log.
 *
 * See:
 *   - Claude/Plans/agent_brief_scenelog_completion.md §5
 *   - Claude/Plans/implementation_plan_auto_traversal.md §4 (Phase 2)
 *   - GitHub #201, #202
 */

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

// Phase 2 session counters. Reset whenever Mode toggles 0 → 1.
static int sEntrancesVisited = 0;
static std::set<int>& ScenesCovered() {
    static std::set<int> s;
    return s;
}
static int64_t sSessionStartMs = 0;
static bool sSessionInitialized = false;

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Persist session state to roommanifests/_autotraverse_state.json on
// every cursor advance. External tools can poll this for progress.
// Best-effort I/O: ~kilobyte JSON, written ~once every 2 seconds during
// active traversal; cost is negligible.
static void WriteStateFile(int cursor, int maxEntrance, const char* status) {
    std::error_code ec;
    std::filesystem::create_directories("roommanifests", ec);
    if (ec) {
        // Same dir as the per-room manifests; if create_directories
        // failed, SceneLog.cpp would also have logged. Skip silently.
        return;
    }

    nlohmann::json j = {
        { "schemaVersion",      1 },
        { "status",             status },
        { "cursor",             cursor },
        { "maxEntrance",        maxEntrance },
        { "entrancesVisited",   sEntrancesVisited },
        { "scenesCovered",      ScenesCovered().size() },
        { "scenesCoveredList",  std::vector<int>(ScenesCovered().begin(), ScenesCovered().end()) },
        { "sessionStartMs",     sSessionStartMs },
        { "lastUpdatedMs",      NowMs() },
    };

    auto target = std::filesystem::path("roommanifests/_autotraverse_state.json");
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f.is_open()) return;
        f << j.dump(2) << '\n';
        if (!f.good()) return;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
    }
}

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
        // Reset one-shot Complete flag and session counters if mode
        // toggles away from Running. Next start (0 → 1 transition)
        // gets a fresh session.
        if (mode != AT_MODE_COMPLETE) {
            sCompleteEmitted = false;
        }
        if (mode == AT_MODE_OFF) {
            sSessionInitialized = false;
            sEntrancesVisited = 0;
            ScenesCovered().clear();
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

    // First-frame-of-session setup. Records the start time and clears
    // counters; subsequent frames keep accumulating. Triggers the warp
    // to the initial cursor immediately — without this, the advance-
    // then-warp loop below skips the user-provided starting entrance.
    if (!sSessionInitialized) {
        sSessionInitialized = true;
        sSessionStartMs = NowMs();
        sEntrancesVisited = 0;
        ScenesCovered().clear();
        int initialCursor = CVarGetInteger(CVAR_AT_CURSOR, 0);
        int maxEntrance = CVarGetInteger(CVAR_AT_MAX_ENTRANCE, 0xFFFF);
        SPDLOG_INFO("[AutoTraverse] Session start: cursor={} max={} step=4 (entrance-group stride)",
                    initialCursor, maxEntrance);
        WriteStateFile(initialCursor, maxEntrance, "starting");
        // Trigger the first warp (cursor as-is). Subsequent ticks advance
        // by step 4 and warp to the next primary entrance.
        TriggerEntranceLoad(initialCursor);
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
            sEntrancesVisited++;
            ScenesCovered().insert(gPlayState->sceneNum);
            SPDLOG_INFO("[AutoTraverse] Warp committed: scene={} room={}; visited={}/group, scenes_covered={}; holding {} frames",
                        gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num,
                        sEntrancesVisited, ScenesCovered().size(), sHoldFrameCounter);
        }
        return;
    }

    if (sHoldFrameCounter > 0) {
        sHoldFrameCounter--;
        return;
    }

    // Hold expired; advance cursor and trigger the next warp.
    //
    // entrance_table.h groups entrances in 4-tuples (scene layers):
    //   CHILD_DAY, CHILD_NIGHT, ADULT_DAY, ADULT_NIGHT.
    // Only the first entry in each group is referenced in code; the
    // entrance system applies the layer offset automatically based on
    // linkAge + dayTime. So step by 4 to land on primary entrances
    // only — covers ~390 unique entrances out of the 1556 total slots.
    constexpr int kEntranceGroupStride = 4;
    constexpr int kEntranceTableMax = ENTR_MAX;  // 0x614 = 1556

    int cursor = CVarGetInteger(CVAR_AT_CURSOR, 0);
    int maxEntrance = CVarGetInteger(CVAR_AT_MAX_ENTRANCE, 0xFFFF);

    // Effective ceiling is min(MaxEntrance CVar, last valid index).
    int ceiling = std::min(maxEntrance, kEntranceTableMax - 1);

    cursor += kEntranceGroupStride;
    if (cursor > ceiling) {
        SPDLOG_INFO("[AutoTraverse] Session complete: cursor={} ceiling={} (cap from {} = ENTR_MAX-1, MaxEntrance={}); "
                    "entrances={}, scenes_covered={}, elapsed_ms={}",
                    cursor, ceiling, kEntranceTableMax - 1, maxEntrance,
                    sEntrancesVisited, ScenesCovered().size(),
                    NowMs() - sSessionStartMs);
        CVarSetInteger(CVAR_AT_MODE, AT_MODE_COMPLETE);
        sCompleteEmitted = true;
        WriteStateFile(cursor, maxEntrance, "complete");
        return;
    }

    CVarSetInteger(CVAR_AT_CURSOR, cursor);
    WriteStateFile(cursor, maxEntrance, "running");
    TriggerEntranceLoad(cursor);
}

void RegisterAutoTraverse() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrameTick);
}

}  // namespace AutoTraverse

static RegisterShipInitFunc registerAutoTraverse(AutoTraverse::RegisterAutoTraverse);
