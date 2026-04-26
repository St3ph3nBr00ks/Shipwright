#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_SPAWN
 *
 * Sent by the host when a dynamic enemy spawns at runtime (after the initial
 * scene actor batch has loaded). Common sources: En_Encount1 spawning Stalchildren,
 * Wolfos, Leevers, and Tektites; Peahat spawning larvae; Floormaster splitting.
 *
 * Non-host clients suppress locally-generated dynamic spawns (in OnActorSpawn)
 * and instead spawn the host's canonical actor on receipt of this packet.
 * Both clients compute the same deterministic netId from the same spawn position,
 * so the actor immediately participates in ENEMY_UPDATE tracking.
 *
 * Packet fields:
 *   sceneNum - guards against cross-scene application
 *   actorId  - actor type to spawn (s16)
 *   pos      - spawn world position (Vec3f)
 *   rot      - spawn rotation (Vec3s)
 *   params   - actor params (s16); encodes variant/strength for some enemies
 */

void Anchor::SendPacket_EnemySpawn(Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Broadcast to all clients in the room. The sceneNum field in the payload
    // and the guard in HandlePacket_EnemySpawn ensure non-hosts only apply the
    // spawn if they are in the same scene. Omitting a hasRemoteInScene pre-check
    // here avoids the race window where a client's sceneNum in our local table
    // is stale (not yet updated by their UpdateClientState) even though they are
    // already in the same scene — which would cause ENEMY_SPAWN to be silently
    // dropped and the enemy to exist only on the host.

    nlohmann::json payload;
    payload["type"]     = ENEMY_SPAWN;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"]  = actor->id;
    payload["pos"]      = actor->home.pos;
    payload["rot"]      = actor->home.rot;
    payload["params"]   = actor->params;

    SPDLOG_INFO("[EnemySpawn] Sending spawn actorId={} pos=({:.1f},{:.1f},{:.1f}) params={}",
                actor->id, actor->home.pos.x, actor->home.pos.y, actor->home.pos.z, actor->params);

    SendJsonToRemote(payload); // broadcast (no targetClientId — all clients in room)
}

void Anchor::HandlePacket_EnemySpawn(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)-1);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return; // different scene — ignore
    }

    s16    actorId = payload.value("actorId", (s16)0);
    Vec3f  pos     = payload["pos"].get<Vec3f>();
    Vec3s  rot     = payload["rot"].get<Vec3s>();
    s16    params  = payload.value("params", (s16)0);

    SPDLOG_INFO("[EnemySpawn] Received spawn actorId={} pos=({:.1f},{:.1f},{:.1f}) params={}",
                actorId, pos.x, pos.y, pos.z, params);

    // Set the guard so OnActorSpawn allows this actor through without suppressing it.
    isSpawningNetworkActor = true;
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId,
                                 pos.x, pos.y, pos.z,
                                 rot.x, rot.y, rot.z, params);
    isSpawningNetworkActor = false;

    if (spawned == nullptr) {
        SPDLOG_WARN("[EnemySpawn] Actor_Spawn failed for actorId={} — actor limit reached or invalid id",
                    actorId);
    }
}
