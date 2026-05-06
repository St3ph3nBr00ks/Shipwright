/**
 * RoomNavData — pre-scanned navigation graph per room.
 *
 * Commit 1: skeleton + master CVar wiring. Defines the public API and
 * structures, registers the GameInteractor hook, and gates everything
 * behind gEnhancements.RoomNavData.Enabled. No scan / load / save logic
 * yet; those land in commits 2-8.
 *
 * See:
 *   - Claude/Plans/room_nav_data_plan.md
 *   - GitHub #207 (this tracker)
 */

#include "RoomNavData.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cstdint>
#include <cstdio>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------------
// CVar definitions — see plan §3 for full layout.
// ---------------------------------------------------------------------------

#define CVAR_ROOM_NAV_ENABLED       CVAR_ENHANCEMENT("RoomNavData.Enabled")
#define CVAR_ROOM_NAV_AUTO_SCAN     CVAR_ENHANCEMENT("RoomNavData.AutoScan")
#define CVAR_ROOM_NAV_DEBUG_DRAW    CVAR_ENHANCEMENT("RoomNavData.DebugDraw")

namespace Anchor::Nav::Room {

// ---------------------------------------------------------------------------
// Master gate. Every per-feature query short-circuits to vanilla behaviour
// when this returns false. Zero overhead when disabled.
// ---------------------------------------------------------------------------

bool IsEnabled() {
    return CVarGetInteger(CVAR_ROOM_NAV_ENABLED, 0) != 0;
}

static bool IsAutoScanEnabled() {
    // AutoScan defaults ON when master is on — see plan §3.
    return CVarGetInteger(CVAR_ROOM_NAV_AUTO_SCAN, 1) != 0;
}

// ---------------------------------------------------------------------------
// In-memory cache. Survives within a session. Keyed on packed (scene, room)
// uint32 — high 16 bits = sceneNum, low 8 bits = roomNum. Negative values
// are clamped to 0xFFFF / 0xFF (sentinel; matches how OoT signals invalid
// scene/room in transient state).
// ---------------------------------------------------------------------------

static uint32_t MakeCacheKey(int16_t sceneNum, int8_t roomNum) {
    uint32_t scenePart = (uint32_t)((uint16_t)sceneNum) << 16;
    uint32_t roomPart  = (uint32_t)((uint8_t)roomNum) & 0xFF;
    return scenePart | roomPart;
}

static std::unordered_map<uint32_t, RoomNavData> sCache;

// ---------------------------------------------------------------------------
// Public API. GetForRoom now returns from the in-memory cache. Subsequent
// commits add disk-cache load (commit 7) and scan-and-persist (commits 3-7).
// ---------------------------------------------------------------------------

const RoomNavData* GetForRoom(int16_t sceneNum, int8_t roomNum) {
    auto it = sCache.find(MakeCacheKey(sceneNum, roomNum));
    if (it == sCache.end()) {
        return nullptr;
    }
    return &it->second;
}

int FindNearestNode(const RoomNavData* data, const Vec3f& /*pos*/) {
    if (data == nullptr || data->nodes.empty()) {
        return -1;
    }
    // Commit 1: stub — actual nearest-node lookup lands in commit 8.
    return -1;
}

int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int /*fromIdx*/,
                                  const Vec3f& /*targetPos*/) {
    if (data == nullptr) {
        return -1;
    }
    // Commit 1: stub — BFS lands in Phase 2 when nav system integration ships.
    return -1;
}

// ---------------------------------------------------------------------------
// Scan dispatch. Commit 2 implements the lookup-then-scan FLOW; the actual
// scan logic lands in commit 3 (multi-cast + node classification). Until
// then, ScanRoom is a stub that creates an empty RoomNavData entry to
// register the room as "attempted" and avoid re-attempting every frame.
// ---------------------------------------------------------------------------

static void TryLoadFromDisk(int16_t /*sceneNum*/, int8_t /*roomNum*/, RoomNavData* /*out*/) {
    // Commit 7 implements binary file I/O. Until then this is a no-op:
    // disk cache always misses, scan path always fires.
}

static void ScanRoom(int16_t sceneNum, int8_t roomNum, PlayState* /*play*/, RoomNavData* out) {
    // Commit 3 implements the multi-cast scan. Until then, populate header
    // fields only so subsequent calls hit the in-memory cache.
    out->sceneNum = sceneNum;
    out->roomNum  = roomNum;
    out->scanTimestamp = 0; // commit 7 fills this from wall-clock seconds.
    SPDLOG_INFO("[RoomNav] ScanRoom stub: scene={} room={} (commit 3 will populate)",
                sceneNum, (int)roomNum);
}

// Top-level lookup-then-scan dispatch. Called once per (scene, room)
// transition by OnGameFrameTick.
static void OnRoomEntered(int16_t sceneNum, int8_t roomNum, PlayState* play) {
    uint32_t key = MakeCacheKey(sceneNum, roomNum);

    // Step 1: in-memory cache. Already-loaded rooms early-return.
    if (sCache.count(key)) {
        return;
    }

    // Step 2: disk cache (stub until commit 7).
    RoomNavData fresh{};
    TryLoadFromDisk(sceneNum, roomNum, &fresh);
    if (fresh.sceneNum == sceneNum && fresh.roomNum == roomNum && !fresh.nodes.empty()) {
        sCache.emplace(key, std::move(fresh));
        SPDLOG_INFO("[RoomNav] Loaded cached scene={} room={} from disk", sceneNum, (int)roomNum);
        return;
    }

    // Step 3: scan + persist (scan stubbed in commit 2; populated in commits 3-6;
    // disk-write stubbed until commit 7).
    if (!IsAutoScanEnabled()) {
        // AutoScan off: never scan, even if no cached data exists. Used for
        // "play with this exact baked set, don't generate more" mode.
        return;
    }

    RoomNavData scanned{};
    ScanRoom(sceneNum, roomNum, play, &scanned);
    sCache.emplace(key, std::move(scanned));
    // Commit 7 will add: SaveToDisk(sceneNum, roomNum, sCache.at(key));
}

// ---------------------------------------------------------------------------
// OnGameFrameUpdate hook — polling trigger. Detects (sceneNum, roomNum)
// delta and dispatches OnRoomEntered. Scan-skip conditions verified
// against soh/include/z64.h and soh/include/z64save.h.
// ---------------------------------------------------------------------------

static int16_t sLastScene = -1;
static int8_t  sLastRoom  = -1;

static void OnGameFrameTick() {
    if (!IsEnabled()) {
        return;
    }
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }

    // Scan-skip conditions — see plan §2 trigger semantics.
    if (play->csCtx.state != CS_STATE_IDLE)            return; // cutscene active
    if (play->transitionTrigger != TRANS_TRIGGER_OFF)  return; // mid-scene-transition
    if (gSaveContext.gameMode != GAMEMODE_NORMAL)      return; // file-select / title-screen
    if (gSaveContext.fileNum < 0)                       return; // no save loaded

    int16_t currentScene = play->sceneNum;
    int8_t  currentRoom  = (int8_t)play->roomCtx.curRoom.num;

    if (currentScene == sLastScene && currentRoom == sLastRoom) {
        return; // unchanged; nothing to do
    }
    sLastScene = currentScene;
    sLastRoom  = currentRoom;

    OnRoomEntered(currentScene, currentRoom, play);
}

// ---------------------------------------------------------------------------
// Registration. Single ShipInit entry point; called once at startup.
// ---------------------------------------------------------------------------

static void RegisterRoomNavData() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameTick);
}

} // namespace Anchor::Nav::Room

static RegisterShipInitFunc registerRoomNavData(Anchor::Nav::Room::RegisterRoomNavData);
