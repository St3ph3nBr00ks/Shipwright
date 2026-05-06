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

// ---------------------------------------------------------------------------
// Public API stubs. Subsequent commits populate these with real logic.
// ---------------------------------------------------------------------------

const RoomNavData* GetForRoom(int16_t /*sceneNum*/, int8_t /*roomNum*/) {
    // Commit 1: stub — no rooms scanned yet.
    return nullptr;
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
// OnGameFrameUpdate hook — polling trigger. Commit 1 stub: master-CVar
// gate only; no room-change detection or scan dispatch yet (those land
// in commit 2).
// ---------------------------------------------------------------------------

static void OnGameFrameTick() {
    if (!IsEnabled()) {
        return;
    }
    // Commit 2 will add: scan-skip conditions, (sceneNum, roomNum) delta
    // detection, and OnRoomEntered() dispatch.
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
