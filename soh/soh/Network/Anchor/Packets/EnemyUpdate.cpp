#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_UPDATE
 *
 * Sent by the host every frame for each enemy actor in the current scene.
 * Non-host clients apply the position/rotation to the matching local actor
 * and have their enemy AI suppressed via ShouldActorUpdate.
 *
 * netId is deterministic: (sceneNum << 16) | (actorId << 8) | spawnIndex
 * so both clients independently assign the same id to the same enemy.
 */

void Anchor::SendPacket_EnemyUpdate(uint32_t netId, Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only send if at least one other client is in the same scene
    bool hasRemoteInScene = false;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            hasRemoteInScene = true;
            break;
        }
    }
    if (!hasRemoteInScene) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"] = netId;
    payload["pos"] = actor->world.pos;
    payload["rot"] = actor->world.rot;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only non-hosts apply incoming enemy positions
    if (roomState.ownerClientId == ownClientId) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);
    Vec3f pos = payload.value("pos", Vec3f{ 0, 0, 0 });
    Vec3s rot = payload.value("rot", Vec3s{ 0, 0, 0 });

    // Walk the enemy actor list and find the actor with a matching netId
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (actor != nullptr) {
        const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext != nullptr && ext->netId == netId) {
            actor->world.pos = pos;
            actor->world.rot = rot;
            actor->shape.rot = rot;
            break;
        }
        actor = actor->next;
    }
}
