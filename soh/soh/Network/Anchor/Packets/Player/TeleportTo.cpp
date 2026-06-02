#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/AnchorSwitchAge.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"

extern "C" {
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * TELEPORT_TO
 *
 * See REQUEST_TELEPORT for more information, this is the second part of the process.
 */

void Anchor::SendPacket_TeleportTo(uint32_t clientId) {
    if (!IsSaveLoaded()) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);

    nlohmann::json payload;
    payload["type"] = TELEPORT_TO;
    payload["targetClientId"] = clientId;
    payload["entranceIndex"] = gSaveContext.entranceIndex;
    payload["roomIndex"] = gPlayState->roomCtx.curRoom.num;
    payload["posRot"] = player->actor.world;
    // Pillar B Phase 5 — schema 2: requester needs the source's linkAge to
    // detect a cross-timeline teleport, and sceneNum so it can apply the
    // §5 castle-exit override before arming the transition.
    payload["linkAge"]  = (s32)gSaveContext.linkAge;
    payload["sceneNum"] = (s16)gPlayState->sceneNum;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TeleportTo(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    s32 entranceIndex = payload.at("entranceIndex").get<s32>();
    s8 roomIndex = payload.at("roomIndex").get<s8>();
    PosRot posRot = payload.at("posRot").get<PosRot>();

    // Pillar B Phase 5 — cross-timeline detection. Schema-2 senders include
    // linkAge + sceneNum. Schema-1 senders default to local linkAge (treated
    // as same-timeline so behaviour is unchanged for legacy peers).
    s32 sourceLinkAge  = payload.value("linkAge",  (s32)gSaveContext.linkAge);
    s16 sourceSceneNum = payload.value("sceneNum", (s16)gPlayState->sceneNum);

    if (sourceLinkAge != gSaveContext.linkAge) {
        // Cross-timeline route — switch age + teleport in one transition.
        // AnchorSwitchAge applies the TimeTravel CVar gate (Q 4.B.6) and
        // the castle-exit override (Q 4.B.7). On refusal, no game state is
        // mutated and the player stays put with no error notification —
        // a TODO for the Q 4.B.8 confirm-dialog polish pass.
        AnchorSwitchAge::SwitchAgeAndTeleport(sourceLinkAge, sourceSceneNum,
                                              entranceIndex, roomIndex,
                                              posRot.pos, posRot.rot.y);
        return;
    }

    // Same-timeline path — original behaviour preserved.
    gPlayState->nextEntranceIndex = entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = entranceIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = roomIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = posRot.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = posRot.rot.y;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0xDFF;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.respawnFlag = 1;
    static HOOK_ID hookId = 0;
    hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
        *should = false;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
    });
}
