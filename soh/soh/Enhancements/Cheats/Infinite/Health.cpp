#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/ShipInit.hpp"
#include "z64save.h"

extern "C" SaveContext gSaveContext;

#define CVAR_INFINITE_HEALTH_NAME CVAR_CHEAT("InfiniteHealth")
#define CVAR_INFINITE_HEALTH_DEFAULT 0
// Settings-sync v2 — host-authoritative; falls back to local on disconnect.
#define CVAR_INFINITE_HEALTH_VALUE AnchorCVarSync::GetEnforcedInt(CVAR_INFINITE_HEALTH_NAME, CVAR_INFINITE_HEALTH_DEFAULT)

void OnGameFrameUpdateInfiniteHealth() {
    if (!GameInteractor::IsSaveLoaded(true)) {
        return;
    }

    gSaveContext.health = gSaveContext.healthCapacity;
}

void RegisterInfiniteHealth() {
    COND_HOOK(OnGameFrameUpdate, CVAR_INFINITE_HEALTH_VALUE, OnGameFrameUpdateInfiniteHealth);
}

static RegisterShipInitFunc initFunc(RegisterInfiniteHealth, { CVAR_INFINITE_HEALTH_NAME });
