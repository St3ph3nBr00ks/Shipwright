#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

// Walk the syncable actor categories looking for one whose EnemyNetId
// extension matches `netId`. Returns nullptr when no match. Used by the
// held-actor sync release-edge path to read throw velocity from a rock
// that was just detached from the local player.
static Actor* AnchorFindActorByNetId(uint32_t netId) {
    if (netId == 0 || gPlayState == nullptr) return nullptr;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (a != nullptr) {
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(a);
            if (ext != nullptr && ext->netId == netId) return a;
            a = a->next;
        }
    }
    return nullptr;
}

// File-static tracker for the local player's heldActor netId at the last
// SendPacket_PlayerUpdate. Used to detect release-edge transitions.
// Persists across reconnects — false-positive release events on stale
// netIds resolve to "actor not found" in AnchorFindActorByNetId and are
// silently dropped on the wire.
static uint32_t sLastLocalHeldActorNetId = 0;
// File-static tracker for the local player's last-broadcast sceneNum.
// Combined with sLastLocalHeldActorNetId to detect scene-exit-while-
// carrying: when the holder transitions scenes, the held actor is
// destroyed by scene unload (no AnchorFindActorByNetId hit on
// the new scene). Without intervention, the static placement re-spawns
// on scene re-entry — visible "the pot I took is back" bug (log 318).
// We record the netId in HostBookkeeping::SceneDeaths so the existing
// IsSceneDeath suppression at HookHandlers.cpp OnActorSpawn kills the
// re-spawn before any client sees it. Sentinel -1 marks pre-first-send.
static int16_t sLastLocalSceneNum = -1;

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

    Player* player = GET_PLAYER(gPlayState);

    // Carry-exit detection runs BEFORE the no-peers-in-scene early-return
    // below. Otherwise a holder walking alone into a different scene skips
    // the carry-exit notification entirely (their PLAYER_UPDATE is gated
    // off because no peer is in the new scene). We send via
    // SendPacket_EnemyRemovedFromScene which broadcasts independent of
    // scene, so the SceneDeath record propagates regardless of where peers
    // currently are. Log 319 root cause: this block previously lived below
    // the early-return and never fired on the holder's lone visit.
    uint32_t currentHeldActorNetId = 0;
    if (player->heldActor != nullptr) {
        const EnemyNetId* ext =
            ObjectExtension::GetInstance().Get<EnemyNetId>(player->heldActor);
        if (ext != nullptr) currentHeldActorNetId = ext->netId;
    }
    const int16_t currentScene = (int16_t)gPlayState->sceneNum;
    const bool sceneJustChanged =
        (sLastLocalSceneNum != -1) && (sLastLocalSceneNum != currentScene);

    bool didSendCarryExit = false;
    if (sLastLocalHeldActorNetId != 0 &&
        currentHeldActorNetId != sLastLocalHeldActorNetId &&
        sceneJustChanged &&
        AnchorFindActorByNetId(sLastLocalHeldActorNetId) == nullptr) {
        SendPacket_EnemyRemovedFromScene(sLastLocalHeldActorNetId, sLastLocalSceneNum);
        didSendCarryExit = true;
    }
    sLastLocalSceneNum = currentScene;

    uint32_t currentPlayerCount = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            currentPlayerCount++;
        }
    }
    if (currentPlayerCount == 0) {
        // Keep sLastLocalHeldActorNetId in sync so a future frame (when
        // peers re-join our scene) doesn't re-trigger the carry-exit
        // detection on a stale comparison.
        sLastLocalHeldActorNetId = currentHeldActorNetId;
        return;
    }

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

    // Held-actor sync (§3.2). currentHeldActorNetId was computed at the
    // top of the function (above the early-return) for carry-exit
    // detection; broadcast it here too so observers' DummyPlayer attach
    // edge-trigger has the value.
    payload["heldActorNetId"] = currentHeldActorNetId;

    // Throw release-edge: actor still alive in current scene; ship throw
    // velocity so observer's local copy starts the same trajectory.
    // Carry-exit was handled above the early-return — skip if it already
    // fired so we don't re-broadcast something that's already gone.
    if (!didSendCarryExit && sLastLocalHeldActorNetId != 0 &&
        currentHeldActorNetId != sLastLocalHeldActorNetId) {
        Actor* released = AnchorFindActorByNetId(sLastLocalHeldActorNetId);
        if (released != nullptr) {
            payload["releasedActorNetId"] = sLastLocalHeldActorNetId;
            payload["releaseSpeedXZ"]     = released->speedXZ;
            payload["releaseVelocityY"]   = released->velocity.y;
            payload["releaseYaw"]         = (int)released->world.rot.y;
        }
    }
    sLastLocalHeldActorNetId = currentHeldActorNetId;
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

        // Held-actor sync (§3.2). Update the netId; DummyPlayer_Update
        // performs the actual attach / detach edge-trigger using this
        // value (it has the Player* + can write player->heldActor).
        client.heldActorNetId = payload.value("heldActorNetId", (uint32_t)0);

        // Release-edge throw velocity. When the owner just released a
        // held actor, the payload carries the speedXZ / velocity.y / yaw
        // the vanilla throw code wrote to it. Apply to our local copy so
        // its actionFunc (already transitioning to a Thrown state) reads
        // the matching trajectory inputs and produces a symmetric arc.
        if (payload.contains("releasedActorNetId")) {
            uint32_t releasedNetId = payload["releasedActorNetId"].get<uint32_t>();
            Actor* released = AnchorFindActorByNetId(releasedNetId);
            if (released != nullptr) {
                released->speedXZ    = payload["releaseSpeedXZ"].get<f32>();
                released->velocity.y = payload["releaseVelocityY"].get<f32>();
                released->world.rot.y = (s16)payload["releaseYaw"].get<int>();
            }
        }

    }
}
