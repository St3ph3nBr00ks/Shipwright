#include "soh/Network/Anchor/Anchor.h"
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
 * ENEMY_DEFEATED
 *
 * Sent by the host when an enemy fires OnEnemyDefeat (at the end of its death
 * animation, just before Actor_Kill). Non-host clients receive this and call
 * Actor_Kill on the matching local actor so the enemy disappears on all clients.
 *
 * Note: the non-host enemy will not play a death animation — it disappears
 * immediately. Full death animation sync is deferred to a later phase.
 */

void Anchor::SendPacket_EnemyDefeated(uint32_t netId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DEFEATED;
    payload["netId"] = netId;

    SPDLOG_INFO("[EnemyDefeated] Sending defeat for netId={}", netId);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Host does not apply defeat packets — it is the authority.
    if (roomState.ownerClientId == ownClientId) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);

    SPDLOG_INFO("[EnemyDefeated] Received defeat for netId={}", netId);

    // Search ACTORCAT_ENEMY and ACTORCAT_BOSS for the matching actor.
    for (int cat : { (int)ACTORCAT_ENEMY, (int)ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            // Grab next before Actor_Kill to avoid touching freed memory.
            Actor* next = actor->next;
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext != nullptr && ext->netId == netId) {
                SPDLOG_INFO("[EnemyDefeated] Killing actor id={} netId={}", actor->id, netId);
                Actor_Kill(actor);
                return;
            }
            actor = next;
        }
    }

    SPDLOG_WARN("[EnemyDefeated] No actor found for netId={} — possible netId mismatch or already dead",
                netId);
}
