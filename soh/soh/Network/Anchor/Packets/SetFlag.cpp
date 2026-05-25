#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "z64.h"
#include "functions.h"

extern PlayState* gPlayState;
}

/**
 * SET_FLAG
 *
 * Fired when a flag is set in the save context
 */

void Anchor::SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = SET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetFlag(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    s16 flag = payload.at("flag").get<s16>();

    if (sceneNum == SCENE_ID_MAX) {
        auto effect = new GameInteractionEffect::SetFlag();
        effect->parameters[0] = flagType;
        effect->parameters[1] = flag;
        effect->Apply();

        // Special case: If King Zora moved, and the player has Ruto's Letter, convert it to an empty bottle
        if (flagType == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
            Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
            Inventory_ReplaceItem(gPlayState, ITEM_LETTER_RUTO, ITEM_BOTTLE);
        }
    } else {
        // Special case: Ignore water temple water level flags, stored at 0x1C, 0x1D, 0x1E.
        if (sceneNum == SCENE_WATER_TEMPLE && flagType == FLAG_SCENE_SWITCH &&
            (flag == 0x1C || flag == 0x1D || flag == 0x1E)) {
            return;
        }

        // Special case: Ignore forest temple elevator flag, stored at 0x1B.
        if (sceneNum == SCENE_FOREST_TEMPLE && flagType == FLAG_SCENE_SWITCH && flag == 0x1B) {
            return;
        }

        auto effect = new GameInteractionEffect::SetSceneFlag();
        effect->parameters[0] = sceneNum;
        effect->parameters[1] = flagType;
        effect->parameters[2] = flag;
        effect->Apply();

        // #193 fix 2 — active-despawn pass for static collectibles.
        //
        // SetSceneFlag::_Apply updates the runtime + persisted flag
        // bitmasks, but the `VB_ITEM00_DESPAWN` check at
        // z_en_item00.c:373 only fires inside `EnItem00_Init`. An
        // EN_ITEM00 actor that's already alive when the flag arrives
        // keeps rendering and remains pickable — so a peer who picked
        // up a static heart in another scene works fine via late
        // re-entry (Init re-checks), but simultaneous same-room pickup
        // leaves the second client with a live stale copy.
        //
        // Walk ACTORCAT_MISC, find EnItem00 actors whose
        // `collectibleFlag` matches, and kill them. This is the
        // mirror of the local pickup path (z_en_item00.c:969,
        // 980, 989) which calls both `Flags_SetCollectible` AND
        // `Actor_Kill` atomically.
        //
        // FLAG_SCENE_TREASURE (chest open) is intentionally NOT
        // mirrored here. Chests don't despawn on open — they animate
        // into an open pose and stay in the scene. Cross-client chest
        // open sync needs a separate animation/contents-distribution
        // story (design doc Q on shared vs per-player chest contents).
        if (sceneNum == gPlayState->sceneNum &&
            flagType == FLAG_SCENE_COLLECTIBLE) {
            Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
            while (a != nullptr) {
                Actor* next = a->next;
                if (a->id == ACTOR_EN_ITEM00) {
                    EnItem00* it = (EnItem00*)a;
                    if (it->collectibleFlag == flag) {
                        Actor_Kill(a);
                    }
                }
                a = next;
            }
        }
    }
}
