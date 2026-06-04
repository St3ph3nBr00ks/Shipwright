#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/ShipInit.hpp"

extern "C" PlayState* gPlayState;

#define CVAR_UNRESTRICTED_ITEMS_NAME CVAR_CHEAT("NoRestrictItems")
#define CVAR_UNRESTRICTED_ITEMS_DEFAULT 0
// Settings-sync v1 — NoRestrictItems is class B (drift causes unfairness
// — host uses Master Sword as child, peer cannot). Reads route through
// AnchorCVarSync; falls back to local on disconnect.
#define CVAR_UNRESTRICTED_ITEMS_VALUE AnchorCVarSync::GetEnforcedInt(CVAR_UNRESTRICTED_ITEMS_NAME, CVAR_UNRESTRICTED_ITEMS_DEFAULT)

void OnGameFrameUpdateUnrestrictedItems() {
    if (!GameInteractor::IsSaveLoaded(true)) {
        return;
    }

    // do not allow the use of sun's song even with the cheat
    u8 sunsBackup = gPlayState->interfaceCtx.restrictions.sunsSong;
    memset(&gPlayState->interfaceCtx.restrictions, 0, sizeof(gPlayState->interfaceCtx.restrictions));
    gPlayState->interfaceCtx.restrictions.sunsSong = sunsBackup;
}

void RegisterUnrestrictedItems() {
    COND_HOOK(OnGameFrameUpdate, CVAR_UNRESTRICTED_ITEMS_VALUE, OnGameFrameUpdateUnrestrictedItems);
    COND_VB_SHOULD(VB_SHOULD_LOAD_BG_IMAGE, CVAR_UNRESTRICTED_ITEMS_VALUE, {
        int32_t* camId = va_arg(args, int*);
        if (*camId == -1) {
            *should = false;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterUnrestrictedItems, { CVAR_UNRESTRICTED_ITEMS_NAME });
