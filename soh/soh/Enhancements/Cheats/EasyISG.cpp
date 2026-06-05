#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/ShipInit.hpp"

extern "C" {
extern PlayState* gPlayState;
#include "macros.h"
}

#define CVAR_EASY_ISG_NAME CVAR_CHEAT("EasyISG")
#define CVAR_EASY_ISG_DEFAULT 0
// Settings-sync v2 — host-authoritative.
#define CVAR_EASY_ISG_VALUE AnchorCVarSync::GetEnforcedInt(CVAR_EASY_ISG_NAME, CVAR_EASY_ISG_DEFAULT)

void OnGameFrameUpdateEasyISG() {
    if (!GameInteractor::IsSaveLoaded(true)) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    player->meleeWeaponState = 1;
}

void RegisterEasyISG() {
    COND_HOOK(OnGameFrameUpdate, CVAR_EASY_ISG_VALUE, OnGameFrameUpdateEasyISG);
}

static RegisterShipInitFunc initFunc(RegisterEasyISG, { CVAR_EASY_ISG_NAME });
