/*
 * HORSE_DESPAWN — owner → all peers (room broadcast).
 *
 * Sent when the owner's local in-scope horse instance leaves the sync
 * pipeline: dismount-then-vanish, scene-transition out, owner client
 * disconnect, or CVar-disable runtime flip. Receivers find the local
 * replica by netId and call KillNetworkActorSilently (Anchor.h:676) so
 * no ENEMY_DEFEATED-style broadcast echoes back.
 *
 * Plan: Plans/horse_sync_plan.md §"HORSE_DESPAWN" / §"Despawn paths".
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/HorseNetId.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

void Anchor::SendPacket_HorseDespawn(uint32_t netId, uint8_t reason) {
    if (!isConnected) return;
    if (!IsHorseSyncEnabled()) return;
    if (netId == 0) return;

    nlohmann::json payload;
    payload["type"]   = HORSE_DESPAWN;
    payload["netId"]  = netId;
    payload["reason"] = (int)reason;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[HorseDespawn] Broadcasting netId={} reason={}",
                netId, (int)reason);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_HorseDespawn(nlohmann::json payload) {
    if (!IsHorseSyncEnabled()) return;
    if (gPlayState == nullptr) return;

    uint32_t netId = payload.value("netId", (uint32_t)0);
    if (netId == 0) return;

    // Find the local replica. Prefer the cached map; fall back to
    // category walk so a despawn racing a scene-transition (cached
    // pointer already invalidated by the new scene's actor-list reset)
    // still cleans up if the replica is in the actor list under the
    // same netId.
    Actor* horse = nullptr;
    auto it = mPeerHorses.find(netId);
    if (it != mPeerHorses.end() && it->second != nullptr &&
        it->second->update != nullptr) {
        horse = it->second;
    } else {
        horse = FindActorByNetId(gPlayState, netId);
    }

    if (horse != nullptr) {
        SPDLOG_INFO("[HorseDespawn] Killing replica netId={}", netId);
        KillNetworkActorSilently(horse);
    } else {
        SPDLOG_DEBUG("[HorseDespawn] no replica found for netId={} (already gone)",
                     netId);
    }

    mPeerHorses.erase(netId);
}
