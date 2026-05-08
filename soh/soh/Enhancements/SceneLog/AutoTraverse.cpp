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

// ---------------------------------------------------------------------------
// SkipList — Phase 3. Persistent across game restarts.
//
// File: roommanifests/_autotraverse_skip.json
//
// Schema:
//   {
//     "schemaVersion": 1,
//     "skipEntries": [
//       { "entrance": 20, "reason": "crashed", "scene": 5,
//         "addedAtMs": 1778252770187 }
//     ],
//     "inFlight": {
//       "entrance": 24,
//       "triggeredAtMs": 1778252790000
//     }
//   }
//
// Crash-recovery flow:
//   1. TriggerEntranceLoad writes inFlight = { entrance, ts } BEFORE
//      starting the warp. Survives a hard crash mid-warp.
//   2. On warp commit, ClearInFlight removes the field.
//   3. At boot, LoadSkipList checks if inFlight is non-null. If so,
//      that entrance crashed last session — promotes it to skipEntries
//      with reason="crashed-or-hung" and clears inFlight.
//   4. On commit-reason "timeout", AddSkip stores reason="timeout".
//   5. Cursor advance loops past any cursor in skipEntries.
//
// In-memory mirror: std::set<int> for O(log N) cursor lookups during
// the advance loop. File written via atomic write (temp + rename).
// ---------------------------------------------------------------------------

namespace SkipList {

static std::set<int>& InMemory() {
    static std::set<int> s;
    return s;
}

// -1 == no in-flight warp; non-negative == cursor of warp in progress.
static int& InFlightCursor() {
    static int c = -1;
    return c;
}

static std::filesystem::path FilePath() {
    return "roommanifests/_autotraverse_skip.json";
}

// Write current in-memory state to disk via atomic temp-and-rename.
static void Save() {
    std::error_code ec;
    std::filesystem::create_directories("roommanifests", ec);
    if (ec) return;

    nlohmann::json entries = nlohmann::json::array();
    for (int e : InMemory()) {
        // Existing entries preserve their original reason/scene/addedAt
        // by being read from disk and merged at boot. Re-emitting the
        // in-memory set here intentionally collapses to a minimal
        // representation; richer per-entry metadata is preserved across
        // sessions only if explicitly maintained on every Add() call.
        entries.push_back({ { "entrance", e } });
    }

    nlohmann::json j;
    j["schemaVersion"] = 1;
    j["skipEntries"] = entries;
    if (InFlightCursor() >= 0) {
        j["inFlight"] = {
            { "entrance",      InFlightCursor() },
            { "triggeredAtMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count() },
        };
    } else {
        j["inFlight"] = nullptr;
    }

    auto target = FilePath();
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

// Read file, populate in-memory set, and PROMOTE any in-flight entry
// to a skipEntry (because if inFlight survived, last session crashed
// during that warp).
static void LoadAndPromoteCrashed() {
    InMemory().clear();
    InFlightCursor() = -1;

    std::ifstream f(FilePath(), std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        return;  // first-ever boot; no skip list yet.
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[AutoTraverse] SkipList: parse failed for {}: {} (treating as empty)",
                    FilePath().string(), e.what());
        return;
    }

    if (j.contains("skipEntries") && j["skipEntries"].is_array()) {
        for (const auto& entry : j["skipEntries"]) {
            if (entry.contains("entrance") && entry["entrance"].is_number_integer()) {
                InMemory().insert(entry["entrance"].get<int>());
            }
        }
    }

    bool promoted = false;
    if (j.contains("inFlight") && j["inFlight"].is_object()) {
        if (j["inFlight"].contains("entrance") &&
            j["inFlight"]["entrance"].is_number_integer()) {
            int crashedEntrance = j["inFlight"]["entrance"].get<int>();
            InMemory().insert(crashedEntrance);
            SPDLOG_WARN("[AutoTraverse] SkipList: prior session crashed mid-warp on "
                        "entrance 0x{:04X}; promoting to skip list",
                        crashedEntrance);
            promoted = true;
        }
    }
    InFlightCursor() = -1;

    if (promoted) {
        Save();  // persist the promotion immediately
    }

    if (!InMemory().empty()) {
        SPDLOG_INFO("[AutoTraverse] SkipList loaded: {} entries (cursor advance will skip these)",
                    InMemory().size());
    }
}

static bool IsSkipped(int cursor) {
    return InMemory().count(cursor) > 0;
}

// Add an entrance to the skip list and persist. Reason logged but not
// preserved per-entry (Save() collapses to {entrance:N} entries).
// Future enhancement: full reason/scene/timestamp preservation.
static void Add(int entrance, const char* reason) {
    if (IsSkipped(entrance)) return;
    InMemory().insert(entrance);
    SPDLOG_WARN("[AutoTraverse] SkipList: adding entrance 0x{:04X} (reason: {})",
                entrance, reason);
    Save();
}

static void MarkInFlight(int entrance) {
    InFlightCursor() = entrance;
    Save();
}

static void ClearInFlight() {
    InFlightCursor() = -1;
    Save();
}

}  // namespace SkipList

// In-flight = a warp has been triggered but hasn't committed yet. We
// watch for either scene/room number to change OR for transitionTrigger
// to return to OFF after a non-zero observation, OR a frame timeout —
// whichever fires first. The timeout handles the edge case where the
// warp's destination is the SAME (scene, room) as the source (e.g.,
// user is in scene 0 room 0 and warps to entrance 0 which lands there).
static bool sInFlight = false;
static int sLastObservedSceneNum = -1;
static int sLastObservedRoomNum = -1;
static int sInFlightFrames = 0;
static bool sObservedNonZeroTrigger = false;
static int sHoldFrameCounter = 0;

// Timeout after which a stuck in-flight warp is considered complete.
// 360 frames at 60fps = 6 seconds; long enough for any vanilla scene
// load (boss arenas with heavy texture banks land in <3s) but short
// enough that pathological hangs don't stall traversal indefinitely.
#define AT_INFLIGHT_TIMEOUT_FRAMES 360

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
    // Phase 3: write inFlight to skip-list file BEFORE actually triggering
    // the warp. If the engine crashes mid-warp, this entry survives in
    // _autotraverse_skip.json and the next boot promotes it to a
    // permanent skip entry — preventing infinite re-crash loops.
    SkipList::MarkInFlight(entranceIndex);
    gPlayState->nextEntranceIndex = entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    sInFlight = true;
    sLastObservedSceneNum = gPlayState->sceneNum;
    sLastObservedRoomNum = gPlayState->roomCtx.curRoom.num;
    sInFlightFrames = 0;
    sObservedNonZeroTrigger = false;
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
        constexpr int kEntranceGroupStride = 4;
        constexpr int kEntranceTableMax = ENTR_MAX;
        int ceiling = std::min(maxEntrance, kEntranceTableMax - 1);
        // Phase 3: skip past any initial cursor in the persistent skip
        // list (prior crashes/hangs); ensures resumption after a crash
        // doesn't immediately re-trigger the same fatal entrance.
        while (initialCursor <= ceiling && SkipList::IsSkipped(initialCursor)) {
            SPDLOG_INFO("[AutoTraverse] Initial cursor 0x{:04X} in skip list; advancing", initialCursor);
            initialCursor += kEntranceGroupStride;
        }
        if (initialCursor > ceiling) {
            SPDLOG_INFO("[AutoTraverse] Session complete at start: all entrances skipped or beyond ceiling");
            CVarSetInteger(CVAR_AT_MODE, AT_MODE_COMPLETE);
            sCompleteEmitted = true;
            WriteStateFile(initialCursor, maxEntrance, "complete");
            return;
        }
        CVarSetInteger(CVAR_AT_CURSOR, initialCursor);
        SPDLOG_INFO("[AutoTraverse] Session start: cursor={} max={} step=4 (entrance-group stride)",
                    initialCursor, maxEntrance);
        WriteStateFile(initialCursor, maxEntrance, "starting");
        // Trigger the first warp (cursor as-is). Subsequent ticks advance
        // by step 4 and warp to the next primary entrance.
        TriggerEntranceLoad(initialCursor);
        return;
    }

    if (sInFlight) {
        // Detect warp commit. Three signals, any one triggers commit:
        //   1. scene or room number changed (fast path; most warps)
        //   2. transitionTrigger returned to OFF after going non-zero
        //      (handles same-(scene, room) warps where (1) misses)
        //   3. timeout (failsafe; handles the engine fizzling the warp
        //      or unusual cases (1) and (2) both miss)
        sInFlightFrames++;

        bool sceneChanged = (gPlayState->sceneNum != sLastObservedSceneNum);
        bool roomChanged = (gPlayState->roomCtx.curRoom.num != sLastObservedRoomNum);

        // Track the trigger state machine. After TriggerEntranceLoad,
        // transitionTrigger should be non-zero (TRANS_TRIGGER_START or
        // ..._END during the in-flight period); when it returns to OFF
        // we know the engine finished the transition.
        if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
            sObservedNonZeroTrigger = true;
        }
        bool triggerReturnedToOff = sObservedNonZeroTrigger &&
                                    (gPlayState->transitionTrigger == TRANS_TRIGGER_OFF);

        bool timedOut = sInFlightFrames >= AT_INFLIGHT_TIMEOUT_FRAMES;

        if (sceneChanged || roomChanged || triggerReturnedToOff || timedOut) {
            sInFlight = false;
            sHoldFrameCounter = CVarGetInteger(CVAR_AT_HOLD_FRAMES, AT_DEFAULT_HOLD_FRAMES);
            sEntrancesVisited++;
            ScenesCovered().insert(gPlayState->sceneNum);
            const char* commitReason = sceneChanged          ? "scene-changed"
                                       : roomChanged         ? "room-changed"
                                       : triggerReturnedToOff ? "trigger-off"
                                                              : "timeout";
            // Phase 3: timeouts indicate the engine fizzled the warp —
            // record so future sessions skip this entrance. Otherwise the
            // warp committed successfully; clear in-flight only.
            int inFlightEntrance = SkipList::InFlightCursor();
            if (timedOut && inFlightEntrance >= 0) {
                SkipList::Add(inFlightEntrance, "timeout");
            }
            SkipList::ClearInFlight();
            SPDLOG_INFO("[AutoTraverse] Warp committed ({}): scene={} room={}; visited={}, scenes_covered={}; holding {} frames",
                        commitReason,
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

    // Advance by stride, then skip past any entries the persistent skip
    // list has marked as crashed-or-hung from prior sessions.
    cursor += kEntranceGroupStride;
    while (cursor <= ceiling && SkipList::IsSkipped(cursor)) {
        SPDLOG_INFO("[AutoTraverse] Skipping entrance 0x{:04X} (in skip list)", cursor);
        cursor += kEntranceGroupStride;
    }

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
    // Force Mode and OneClickStart back to OFF at boot. Both are session-
    // state CVars that should not persist across game restarts: prior
    // sessions writing Mode=1 (running) or OneClickStart=1 to
    // shipofharkinian.json would otherwise auto-resume traversal on the
    // next launch with stale Cursor/MaxEntrance/HoldFrames values from
    // the JSON file (often zero or partially set), producing pathological
    // behavior like "session completes after 1 entrance because
    // MaxEntrance=0" without the user knowing AutoTraverse is active.
    //
    // User must explicitly opt in each session via the UI toggle (which
    // sets all values consistently) or via console (with explicit
    // MaxEntrance / Cursor values).
    CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Mode"), AT_MODE_OFF);
    CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.OneClickStart"), 0);

    // Phase 3: load persistent skip list and promote any in-flight entry
    // from the prior session to a permanent skip (means last session
    // crashed mid-warp on that entrance — see SkipList::LoadAndPromoteCrashed).
    SkipList::LoadAndPromoteCrashed();

    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrameTick);
}

}  // namespace AutoTraverse

static RegisterShipInitFunc registerAutoTraverse(AutoTraverse::RegisterAutoTraverse);
