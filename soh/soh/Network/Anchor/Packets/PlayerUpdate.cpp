#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * PLAYER_UPDATE
 *
 * Contains real-time data necessary to update other clients in the same scene as the player
 *
 * Sent every frame to other clients within the same scene
 *
 * Note: This packet is sent _a lot_, so please do not include any unnecessary data in it
 */

void Anchor::SendPacket_PlayerUpdate() {
    if (!IsSaveLoaded()) {
        return;
    }

    uint32_t currentPlayerCount = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            currentPlayerCount++;
        }
    }
    if (currentPlayerCount == 0) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    nlohmann::json payload;

    payload["type"] = PLAYER_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["entranceIndex"] = gSaveContext.entranceIndex;
    payload["linkAge"] = gSaveContext.linkAge;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;

    // Active camera state — read by host's AIDirector PickSpawnPosition
    // to gate spawns against every peer's actual view, not just Link's
    // position. Camera eye = world-space camera origin; at = look-at
    // point. Forward direction = normalize(at - eye).
    Camera* activeCam = GET_ACTIVE_CAM(gPlayState);
    if (activeCam != nullptr) {
        payload["cameraEye"] = activeCam->eye;
        payload["cameraAt"]  = activeCam->at;
    }
    std::vector<int> jointArray;
    for (size_t i = 0; i < 24; i++) {
        Vec3s joint = player->skelAnime.jointTable[i];
        jointArray.push_back(joint.x);
        jointArray.push_back(joint.y);
        jointArray.push_back(joint.z);
    }
    payload["prevTransl"] = player->skelAnime.prevTransl;
    payload["movementFlags"] = player->skelAnime.movementFlags;
    payload["jointTable"] = jointArray;
    // Upper-body anim joint table — carry pose only. Sent only when the
    // owner is actively carrying an actor; the field's absence in the
    // packet tells the observer "do not merge upper limbs this frame",
    // letting the main jointTable render unmodified for walk / run /
    // attack / etc. Vanilla's merge gate is "upperActionFunc returned
    // non-zero" (z_player.c:3617); we use PLAYER_STATE1_CARRYING_ACTOR
    // as the conservative approximation.
    // See Plans/carry_held_actor_sync.md §3.1.
    if (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) {
        std::vector<int> upperJointArray;
        for (size_t i = 0; i < 24; i++) {
            Vec3s joint = player->upperSkelAnime.jointTable[i];
            upperJointArray.push_back(joint.x);
            upperJointArray.push_back(joint.y);
            upperJointArray.push_back(joint.z);
        }
        payload["upperJointTable"] = upperJointArray;
    }
    payload["upperLimbRot"] = player->upperLimbRot;
    payload["currentBoots"] = player->currentBoots;
    payload["currentShield"] = player->currentShield;
    payload["currentTunic"] = player->currentTunic;
    payload["stateFlags1"] = player->stateFlags1;
    payload["stateFlags2"] = player->stateFlags2 & ~PLAYER_STATE2_DISABLE_DRAW;
    payload["buttonItem0"] = gSaveContext.equips.buttonItems[0];
    payload["itemAction"] = player->itemAction;
    payload["heldItemAction"] = player->heldItemAction;
    payload["modelGroup"] = player->modelGroup;
    payload["invincibilityTimer"] = player->invincibilityTimer;
    payload["unk_862"] = player->unk_862;
    payload["unk_85C"] = player->unk_85C;
    payload["unk_860"] = player->unk_860;  // Deku-Stick burning timer
    payload["actionVar1"] = player->av1.actionVar1;
    // Multi-player dialogue redesign (#191 follow-up) — peer's csCtx.state
    // for "alone in cutscene" detection in Anchor_ShouldAdvanceCutsceneTextLocal.
    payload["csCtxState"] = (int)gPlayState->csCtx.state;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_PlayerUpdate(nlohmann::json payload) {
    uint32_t clientId = payload["clientId"].get<uint32_t>();

    if (clients.contains(clientId)) {
        auto& client = clients[clientId];

        if (client.linkAge != payload.value("linkAge", (s32)LINK_AGE_ADULT)) {
            shouldRefreshActors = true;
        }

        client.sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
        client.entranceIndex = payload.value("entranceIndex", (s32)0);
        client.linkAge = payload.value("linkAge", (s32)LINK_AGE_ADULT);
        client.posRot = payload.value("posRot", PosRot{ 0 });
        client.cameraEye = payload.value("cameraEye", Vec3f{ 0.0f, 0.0f, 0.0f });
        client.cameraAt  = payload.value("cameraAt",  Vec3f{ 0.0f, 0.0f, 0.0f });
        std::vector<int> jointArray = payload.value("jointTable", std::vector<int>{});
        jointArray.resize(24 * 3); // Ensure it has enough elements, in case of missing data
        for (int i = 0; i < 24; i++) {
            client.jointTable[i].x = jointArray[i * 3];
            client.jointTable[i].y = jointArray[i * 3 + 1];
            client.jointTable[i].z = jointArray[i * 3 + 2];
        }
        // Upper-body anim joint table — per-frame gate. The owner only
        // includes the field when actively carrying an actor; otherwise
        // we skip the observer-side merge so the main jointTable renders
        // unmodified for walk / run / attack / etc.
        if (payload.contains("upperJointTable")) {
            std::vector<int> upperJointArray = payload["upperJointTable"].get<std::vector<int>>();
            upperJointArray.resize(24 * 3);
            for (int i = 0; i < 24; i++) {
                client.upperJointTable[i].x = upperJointArray[i * 3];
                client.upperJointTable[i].y = upperJointArray[i * 3 + 1];
                client.upperJointTable[i].z = upperJointArray[i * 3 + 2];
            }
            client.upperMergeActiveThisFrame = true;
        } else {
            client.upperMergeActiveThisFrame = false;
        }
        client.movementFlags = payload.value("movementFlags", (u8)0);
        client.prevTransl = payload.value("prevTransl", Vec3s{ 0 });
        client.upperLimbRot = payload.value("upperLimbRot", Vec3s{ 0 });
        client.currentBoots = payload.value("currentBoots", (s8)0);
        client.currentShield = payload.value("currentShield", (s8)0);
        client.currentTunic = payload.value("currentTunic", (s8)0);
        client.stateFlags1 = payload.value("stateFlags1", (u32)0);
        client.stateFlags2 = payload.value("stateFlags2", (u32)0);
        client.buttonItem0 = payload.value("buttonItem0", (u8)0);
        client.itemAction = payload.value("itemAction", (s8)0);
        client.heldItemAction = payload.value("heldItemAction", (s8)0);
        client.modelGroup = payload.value("modelGroup", (u8)0);
        client.invincibilityTimer = payload.value("invincibilityTimer", (s8)0);
        client.unk_862 = payload.value("unk_862", (s16)0);
        client.unk_85C = payload.value("unk_85C", (f32)0);
        client.unk_860 = payload.value("unk_860", (s16)0);
        client.actionVar1 = payload.value("actionVar1", (s8)0);
        // Pre-update peers default to CS_STATE_IDLE (0) — treated as
        // out-of-cutscene for the multi-player dialogue detection.
        client.csCtxState = (s8)payload.value("csCtxState", 0);
    }
}
