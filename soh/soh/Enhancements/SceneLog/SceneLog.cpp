/**
 * SceneLog — per-room troubleshooting / pre-flight diagnostic logging.
 *
 * Goal: emit structured log lines on actor spawn, room transition,
 * cutscene init, flag writes, and item-give events so a single
 * play-through enumerates a dungeon's static + dynamic content.
 *
 * Self-contained — registers GameInteractor hooks from a single file,
 * no edits to pre-Flotilla source. Gated behind CVar
 * gDeveloperTools.SceneLog.Level (default 0 = no output, zero overhead).
 *
 * Two output streams:
 *   - Standard log file: human-readable [Tag] scene=N room=M ... lines
 *     via SPDLOG_INFO (alongside existing log content).
 *   - Manifest sidecar (logs/scenelog_<timestamp>.manifest.jsonl):
 *     one JSON event per line, machine-readable. Consumed by
 *     scripts/manifest_to_scene_data.py (see #202).
 *
 * See:
 *   - Claude/Plans/implementation_plan_logging_and_scene_data.md
 *   - GitHub #201 (logging plan)
 *   - GitHub #202 (scene_data persistent layer)
 */

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ActorDB.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_SCENE_LOG_LEVEL CVAR_DEVELOPER_TOOLS("SceneLog.Level")
#define SCENE_LOG_LEVEL_OFF 0
#define SCENE_LOG_LEVEL_PREFLIGHT 1
#define SCENE_LOG_LEVEL_INVENTORY_STATE 2
#define SCENE_LOG_LEVEL_RUNTIME 3
#define SCENE_LOG_LEVEL_VERBOSE 4

static int SceneLogLevel() {
    return CVarGetInteger(CVAR_SCENE_LOG_LEVEL, SCENE_LOG_LEVEL_OFF);
}

static const char* GetActorSymbolicName(int16_t actorId) {
    ActorDBEntry* dbEntry = ActorDB_Retrieve(actorId);
    if (dbEntry != nullptr && dbEntry->valid && dbEntry->name != nullptr) {
        return dbEntry->name;
    }
    return "<unnamed>";
}

// ---------------------------------------------------------------------------
// Manifest sidecar — appends one JSON event per line to a per-session file
// at logs/scenelog_<timestamp>.manifest.jsonl. Lazy-opens on first emit.
// First event of every file is a BuildInfo header.
// ---------------------------------------------------------------------------

namespace ManifestSidecar {

static std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

static std::ofstream& Stream() {
    static std::ofstream s;
    return s;
}

static bool& Opened() {
    static bool b = false;
    return b;
}

static bool& HeaderEmitted() {
    static bool b = false;
    return b;
}

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static void EnsureOpen() {
    if (Opened()) {
        return;
    }
    Opened() = true;
    try {
        std::filesystem::create_directories("logs");
    } catch (...) {
        // Best-effort; logs/ usually exists.
    }
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[80];
    std::strftime(buf, sizeof(buf), "scenelog_%Y%m%d_%H%M%S.manifest.jsonl", &tm);
    Stream().open(std::string("logs/") + buf, std::ios::out | std::ios::app);
}

static void EmitHeader() {
    if (!Stream().is_open()) {
        return;
    }
    nlohmann::json hdr = {
        { "category", "BuildInfo" },
        { "version", static_cast<const char*>(gBuildVersion) },
        { "versionMajor", gBuildVersionMajor },
        { "versionMinor", gBuildVersionMinor },
        { "versionPatch", gBuildVersionPatch },
        { "branch", static_cast<const char*>(gGitBranch) },
        { "commit", static_cast<const char*>(gGitCommitHash) },
        { "buildDate", reinterpret_cast<const char*>(gBuildDate) },
        { "ts", NowMs() },
    };
    Stream() << hdr.dump() << '\n';
}

static void Emit(nlohmann::json evt) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    std::lock_guard<std::mutex> lock(Mutex());
    EnsureOpen();
    if (!Stream().is_open()) {
        return;
    }
    if (!HeaderEmitted()) {
        HeaderEmitted() = true;
        EmitHeader();
    }
    evt["ts"] = NowMs();
    Stream() << evt.dump() << '\n';
    Stream().flush();
}

}  // namespace ManifestSidecar

// ---------------------------------------------------------------------------
// Hook handlers
// ---------------------------------------------------------------------------

static void OnActorSpawnLog(void* refActor) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    if (refActor == nullptr || gPlayState == nullptr) {
        return;
    }
    Actor* actor = static_cast<Actor*>(refActor);
    const char* actorName = GetActorSymbolicName(actor->id);

    SPDLOG_INFO("[Spawn] scene={} room={} actor=0x{:04X} \"{}\" params=0x{:04X} pos=({:.0f},{:.0f},{:.0f}) cat={} setup={}",
                gPlayState->sceneNum,
                actor->room,
                static_cast<uint16_t>(actor->id),
                actorName,
                static_cast<uint16_t>(actor->params),
                actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                actor->category,
                gPlayState->numSetupActors > 0 ? 1 : 0);

    nlohmann::json evt = {
        { "category", "Spawn" },
        { "scene", gPlayState->sceneNum },
        { "room", actor->room },
        { "actor", static_cast<uint16_t>(actor->id) },
        { "actorName", actorName },
        { "params", static_cast<uint16_t>(actor->params) },
        { "pos", { { "x", actor->world.pos.x }, { "y", actor->world.pos.y }, { "z", actor->world.pos.z } } },
        { "cat", actor->category },
        { "setup", gPlayState->numSetupActors > 0 ? 1 : 0 },
    };
    ManifestSidecar::Emit(std::move(evt));
}

static void OnTransitionEndLog(int16_t sceneNum) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    int16_t roomNum = (gPlayState != nullptr) ? gPlayState->roomCtx.curRoom.num : -1;
    uint16_t entrance = static_cast<uint16_t>(gSaveContext.entranceIndex);

    SPDLOG_INFO("[TransitionEnd] scene={} room={} entrance=0x{:04X}", sceneNum, roomNum, entrance);

    nlohmann::json evt = {
        { "category", "TransitionEnd" },
        { "scene", sceneNum },
        { "room", roomNum },
        { "entrance", entrance },
    };
    ManifestSidecar::Emit(std::move(evt));
}

static void OnSceneInitLog(int16_t sceneNum) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }

    SPDLOG_INFO("[SceneInit] scene={}", sceneNum);

    nlohmann::json evt = {
        { "category", "SceneInit" },
        { "scene", sceneNum },
    };
    ManifestSidecar::Emit(std::move(evt));
}

static void OnSceneSpawnActorsLog() {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    if (gPlayState == nullptr) {
        return;
    }
    int16_t sceneNum = gPlayState->sceneNum;
    int16_t roomNum = gPlayState->roomCtx.curRoom.num;

    SPDLOG_INFO("[SceneSpawnActors] scene={} room={}", sceneNum, roomNum);

    nlohmann::json evt = {
        { "category", "SceneSpawnActors" },
        { "scene", sceneNum },
        { "room", roomNum },
    };
    ManifestSidecar::Emit(std::move(evt));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void RegisterSceneLog() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorSpawn>(OnActorSpawnLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnTransitionEnd>(OnTransitionEndLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneInitLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(OnSceneSpawnActorsLog);
}

static RegisterShipInitFunc registerSceneLog(RegisterSceneLog);
