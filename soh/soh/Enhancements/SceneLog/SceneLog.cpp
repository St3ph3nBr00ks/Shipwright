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

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
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

static void OnActorSpawnLog(void* refActor) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    if (refActor == nullptr || gPlayState == nullptr) {
        return;
    }
    Actor* actor = static_cast<Actor*>(refActor);
    SPDLOG_INFO("[Spawn] scene={} room={} actor=0x{:04X} \"{}\" params=0x{:04X} pos=({:.0f},{:.0f},{:.0f}) cat={} setup={}",
                gPlayState->sceneNum,
                actor->room,
                static_cast<uint16_t>(actor->id),
                GetActorSymbolicName(actor->id),
                static_cast<uint16_t>(actor->params),
                actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                actor->category,
                gPlayState->numSetupActors > 0 ? 1 : 0);
}

void RegisterSceneLog() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorSpawn>(OnActorSpawnLog);
}

static RegisterShipInitFunc registerSceneLog(RegisterSceneLog);
