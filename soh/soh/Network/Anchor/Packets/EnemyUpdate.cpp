#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_UPDATE
 *
 * Sent by the host every frame for each enemy actor in the current scene.
 * Non-host clients apply the received state to the matching local actor.
 * Enemy AI is suppressed on non-hosts via ShouldActorUpdate so enemies
 * are position/pose puppets driven entirely by the host.
 *
 * Packet fields:
 *   netId     - deterministic id assigned at spawn (see HookHandlers.cpp)
 *   sceneNum  - guards against cross-scene application
 *   pos       - actor->world.pos
 *   rot       - actor->world.rot
 *   shapeRot  - actor->shape.rot (may differ from world.rot for animated enemies)
 *   health    - actor->colChkInfo.health (enables death detection)
 *   jointTable - (optional) SkelAnime joint rotations; only present when the enemy
 *                has a supported skeleton. Drives the rendered pose on non-host.
 */

void Anchor::SendPacket_EnemyUpdate(uint32_t netId, Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only send if at least one other client is in the same scene AND room.
    // Room filtering prevents "No actor found" spam when one client loads a room
    // slightly ahead of another — the other client doesn't have the actors yet.
    s8 hostRoom = (s8)gPlayState->roomCtx.curRoom.num;
    bool hasRemoteInRoom = false;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.curRoomNum == hostRoom &&
            client.online && client.isSaveLoaded && !client.self) {
            hasRemoteInRoom = true;
            break;
        }
    }
    if (!hasRemoteInRoom) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"] = netId;
    payload["pos"] = actor->world.pos;
    payload["rot"] = actor->world.rot;
    payload["shapeRot"] = actor->shape.rot;
    payload["health"] = actor->colChkInfo.health;
    payload["scale"] = actor->scale;
    payload["quiet"] = true;

    // Karebaba: sync action state and params timer so non-host state machine matches host.
    // Non-host uses these to call ApplyNetState when the host's state differs from its own.
    if (actor->id == ACTOR_EN_KAREBABA) {
        payload["actionState"]  = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
        payload["actorParams"]  = (s16)actor->params;
    }

    // Include the joint/morph tables if this enemy has a supported skeleton.
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext != nullptr && ext->skelAnime != nullptr && ext->limbCount > 0) {
        nlohmann::json joints = nlohmann::json::array();
        nlohmann::json morphs = nlohmann::json::array();
        // limbCount + 1 entries: index 0 is the root translation, 1..limbCount are limb rotations.
        for (uint8_t i = 0; i <= ext->limbCount; i++) {
            joints.push_back(ext->skelAnime->jointTable[i]);
            if (ext->skelAnime->morphTable != nullptr) {
                morphs.push_back(ext->skelAnime->morphTable[i]);
            }
        }
        payload["jointTable"] = joints;
        if (!morphs.empty()) {
            payload["morphTable"] = morphs;
        }
    }

    SPDLOG_DEBUG("[EnemyUpdate] Send netId={} pos=({:.1f},{:.1f},{:.1f}) health={}",
                 netId, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                 (int)actor->colChkInfo.health);

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.curRoomNum == hostRoom &&
            client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only non-hosts apply incoming enemy state.
    if (roomState.ownerClientId == ownClientId) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);
    Vec3f pos      = payload.value("pos", Vec3f{ 0, 0, 0 });
    Vec3s rot      = payload.value("rot", Vec3s{ 0, 0, 0 });
    Vec3s shapeRot = payload.value("shapeRot", rot); // fall back to rot if field absent
    s8 health      = (s8)payload.value("health", 1);
    Vec3f scale    = payload.value("scale", Vec3f{ 1.0f, 1.0f, 1.0f });

    // Walk the enemy actor list and find the matching actor by netId.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (actor != nullptr) {
        EnemyNetId* ext = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
        if (ext != nullptr && ext->netId == netId) {
            // En_Karebaba: world.pos is computed each frame from home.pos + shape.rot
            // trig (same pattern as En_Dekubaba Fix 7). shape.rot is driven entirely
            // by the local state machine; overriding it with a stale host value causes
            // the next actor->update() to compute world.pos from the wrong angles,
            // making the stem base wobble. Skip both; jointTable sync handles visuals.
            if (actor->id != ACTOR_EN_DEKUBABA && actor->id != ACTOR_EN_KAREBABA) {
                actor->world.pos = pos;
                actor->shape.rot = shapeRot;
            }
            actor->world.rot         = rot;
            // Health sync rules:
            //   (a) hasLocalDeath: we already killed this enemy locally; ignore host
            //       health > 0 while our ENEMY_DEFEATED packet travels.
            //   (b) Multi-hit enemies: only apply if host's health is <= our current
            //       value (host has taken as much or more damage). Prevents the host's
            //       stale higher value from resetting locally-dealt damage on P2.
            if (!ext->hasLocalDeath && health <= actor->colChkInfo.health) {
                actor->colChkInfo.health = health;
                ext->netHealth           = health;
            }
            actor->scale             = scale;

            // Apply joint/morph tables if present in this packet and the actor has a skeleton.
            if (ext->skelAnime != nullptr && ext->limbCount > 0) {
                if (payload.contains("jointTable")) {
                    const auto& joints = payload["jointTable"];
                    uint8_t count = static_cast<uint8_t>(
                        std::min((size_t)(ext->limbCount + 1), joints.size()));
                    for (uint8_t i = 0; i < count; i++) {
                        ext->skelAnime->jointTable[i] = joints[i].get<Vec3s>();
                    }
                }
                if (payload.contains("morphTable") && ext->skelAnime->morphTable != nullptr) {
                    const auto& morphs = payload["morphTable"];
                    uint8_t count = static_cast<uint8_t>(
                        std::min((size_t)(ext->limbCount + 1), morphs.size()));
                    for (uint8_t i = 0; i < count; i++) {
                        ext->skelAnime->morphTable[i] = morphs[i].get<Vec3s>();
                    }
                }
            }

            // Cache state so OnActorUpdate can re-apply it after the enemy's own
            // update() runs (required for collision registration — Fix 4).
            // Note: netHealth is updated inside the hasLocalDeath guard above.
            ext->hasNetState  = true;
            ext->netPos       = pos;
            ext->netRot       = rot;
            ext->netShapeRot  = shapeRot;
            ext->netScale     = scale;

            // Karebaba: cache received state index so OnActorUpdate can drive
            // the local state machine to match the host's current state.
            // Skip when hasLocalDeath — we don't want to re-apply an Upright/Spin
            // state index that would oscillate with the dying actor's own state.
            if (actor->id == ACTOR_EN_KAREBABA && payload.contains("actionState") && !ext->hasLocalDeath) {
                ext->netStateIndex  = (s16)payload["actionState"].get<int>();
                ext->netActorParams = (s16)payload.value("actorParams", (int)0);
            }

            SPDLOG_DEBUG("[EnemyUpdate] Applied netId={} pos=({:.1f},{:.1f},{:.1f}) health={}",
                         netId, pos.x, pos.y, pos.z, (int)health);
            return;
        }
        actor = actor->next;
    }

    SPDLOG_WARN("[EnemyUpdate] No actor found for netId={} sceneNum={} — possible netId mismatch",
                netId, sceneNum);
}
