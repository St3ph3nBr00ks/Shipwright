#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "z64save.h"
extern SaveContext gSaveContext;
}

#define CVAR_INFINITE_MAGIC_NAME CVAR_CHEAT("InfiniteMagic")
#define CVAR_INFINITE_MAGIC_DEFAULT 0
// Settings-sync v2 — host-authoritative.
#define CVAR_INFINITE_MAGIC_VALUE AnchorCVarSync::GetEnforcedInt(CVAR_INFINITE_MAGIC_NAME, CVAR_INFINITE_MAGIC_DEFAULT)

void OnGameFrameUpdateInfiniteMagic() {
    if (!GameInteractor::IsSaveLoaded(true)) {
        return;
    }

    gSaveContext.magic = gSaveContext.magicLevel * MAGIC_NORMAL_METER;
}

void RegisterInfiniteMagic() {
    COND_HOOK(OnGameFrameUpdate, CVAR_INFINITE_MAGIC_VALUE, OnGameFrameUpdateInfiniteMagic);
}

static RegisterShipInitFunc initFunc(RegisterInfiniteMagic, { CVAR_INFINITE_MAGIC_NAME });
