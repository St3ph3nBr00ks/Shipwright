#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "soh/Enhancements/randomizer/ShuffleTradeItems.h"
extern PlayState* gPlayState;
}

/**
 * UNSET_FLAG
 *
 * Fired when a flag is unset in the save context
 */

void Anchor::SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    // Mirror of SendPacket_SetFlag — no IsSaveLoaded gate. Payload doesn't
    // need gPlayState; unset happening during scene transition must still
    // propagate to peers. See Analysis/ocarina_send_side_silent_drop_2026-07-28.md.
    if (!roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = UNSET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UnsetFlag(nlohmann::json payload) {
    if (!roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    s16 flag = payload.at("flag").get<s16>();

    // sceneNum == SCENE_ID_MAX is a sentinel meaning "global flag" (handled below); only larger
    // values would index gSaveContext.sceneFlags out of bounds.
    if (sceneNum < 0 || sceneNum > SCENE_ID_MAX) {
        SPDLOG_ERROR("[Anchor] UNSET_FLAG: sceneNum {} out of range", sceneNum);
        return;
    }

    if (sceneNum == SCENE_ID_MAX) {
        // Global flag write via RawAction — always safe (no gPlayState
        // deref). Mirror of SetFlag receive-side bypass; see
        // Analysis/saria_bridge_crash_2026-07-28.md for the silent-drop
        // bug this pattern fixes.
        GameInteractor::RawAction::UnsetFlag(flagType, flag);

        // Special case: If an adult trade item flag is unset, replace the item if the player has it equipped.
        // Requires gPlayState + inventory access; keep behind IsSaveLoaded gate.
        if (IsSaveLoaded() && flagType == FLAG_RANDOMIZER_INF &&
            (flag >= RAND_INF_ADULT_TRADES_HAS_POCKET_EGG && flag <= RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK)) {
            u16 itemToReplace = ITEM_POCKET_EGG;
            switch (flag) {
                case RAND_INF_ADULT_TRADES_HAS_POCKET_EGG:
                    itemToReplace = ITEM_POCKET_EGG;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_POCKET_CUCCO:
                    itemToReplace = ITEM_POCKET_CUCCO;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_COJIRO:
                    itemToReplace = ITEM_COJIRO;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_ODD_MUSHROOM:
                    itemToReplace = ITEM_ODD_MUSHROOM;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_ODD_POTION:
                    itemToReplace = ITEM_ODD_POTION;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_SAW:
                    itemToReplace = ITEM_SAW;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_SWORD_BROKEN:
                    itemToReplace = ITEM_SWORD_BROKEN;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_PRESCRIPTION:
                    itemToReplace = ITEM_PRESCRIPTION;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_FROG:
                    itemToReplace = ITEM_FROG;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_EYEDROPS:
                    itemToReplace = ITEM_EYEDROPS;
                    break;
                case RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK:
                    itemToReplace = ITEM_CLAIM_CHECK;
                    break;
            }
            Inventory_ReplaceItem(gPlayState, itemToReplace, Randomizer_GetNextAdultTradeItem());
        }
    } else {
        // Scene-specific flag path — RawAction::UnsetSceneFlag dereferences
        // gPlayState. Retain the IsSaveLoaded gate until a deferred queue
        // is implemented (mirror of SetFlag scene-branch treatment).
        // TODO(#37 follow-up): deferred queue for scene-flag UNSET_FLAG
        // when !IsSaveLoaded, drained on OnSaveLoaded.
        if (!IsSaveLoaded()) {
            SPDLOG_WARN("[Anchor] UNSET_FLAG scene={} flagType={} flag={} dropped "
                        "(!IsSaveLoaded) — scene-flag deferred queue not yet implemented",
                        sceneNum, flagType, flag);
            return;
        }

        // Special case: Ignore water temple water level flags, stored at 0x1C, 0x1D, 0x1E.
        if (sceneNum == SCENE_WATER_TEMPLE && flagType == FLAG_SCENE_SWITCH &&
            (flag == 0x1C || flag == 0x1D || flag == 0x1E)) {
            return;
        }

        // Special case: Ignore forest temple elevator flag, stored at 0x1B.
        if (sceneNum == SCENE_FOREST_TEMPLE && flagType == FLAG_SCENE_SWITCH && flag == 0x1B) {
            return;
        }

        GameInteractor::RawAction::UnsetSceneFlag(sceneNum, flagType, flag);
    }
}
