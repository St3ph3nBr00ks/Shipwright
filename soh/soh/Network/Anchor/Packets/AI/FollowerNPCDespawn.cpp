/*
 * FOLLOWER_NPC_DESPAWN — broadcast on owner-side despawn (CVar toggle
 * off, owner disconnect, owner scene change without auto-respawn, etc.).
 * Peers receive → Actor_Kill the local replica and drop from
 * mPeerFollowerNpcs.
 *
 * Plan: Plans/npc_follower_plan.md §2.6 / §2.9.
 * Phase 3 of the NPC Follower work.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

void Anchor::SendPacket_FollowerNpcDespawn(uint32_t netId, uint8_t reason) {
    if (!isConnected) return;

    nlohmann::json payload;
    payload["type"]          = FOLLOWER_NPC_DESPAWN;
    payload["netId"]         = netId;
    payload["ownerClientId"] = ownClientId;
    payload["reason"]        = (int)reason;  // 0=cvar_off 1=died 2=scene_change 3=owner_disconnect
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[FollowerNpcDespawn] Broadcasting netId={} reason={}",
                netId, (int)reason);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_FollowerNpcDespawn(nlohmann::json payload) {
    uint32_t ownerClientId = payload.value("ownerClientId", (uint32_t)0);

    // Authority gate: we ARE the owner; ignore.
    if (ownerClientId == ownClientId) return;

    auto it = mPeerFollowerNpcs.find(ownerClientId);
    if (it == mPeerFollowerNpcs.end()) {
        // No replica tracked — already despawned, or we never saw a
        // SPAWN for this owner (cross-scene). Silently OK.
        return;
    }
    Actor* replica = it->second;
    if (replica != nullptr && replica->update != nullptr) {
        Actor_Kill(replica);
    }
    mPeerFollowerNpcs.erase(it);
    SPDLOG_INFO("[FollowerNpcDespawn] Killed replica for owner={}", ownerClientId);
}
