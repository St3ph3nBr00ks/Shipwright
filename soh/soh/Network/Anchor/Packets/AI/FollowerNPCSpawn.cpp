/*
 * FOLLOWER_NPC_SPAWN — NPC Follower companion (Flotilla) spawn announce.
 *
 * Sent once when the owner client toggles its CVar on (or auto-respawns
 * after a scene transition). Peers receive → spawn a local read-only
 * replica at the broadcast pos. Authority gate: receivers whose own
 * clientId matches the packet's ownerClientId ignore the packet (they
 * ARE the owner; their local actor is the source of truth).
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
extern s16        gEnFollowerId;
}

void Anchor::SendPacket_FollowerNpcSpawn(uint32_t netId, const Vec3f& pos,
                                          const Vec3s& rot, int16_t sceneNum,
                                          int8_t roomNum, uint8_t linkAge) {
    if (!isConnected) return;

    nlohmann::json payload;
    payload["type"]          = FOLLOWER_NPC_SPAWN;
    payload["netId"]         = netId;
    payload["ownerClientId"] = ownClientId;
    payload["sceneNum"]      = (int)sceneNum;
    payload["roomNum"]       = (int)roomNum;
    payload["linkAge"]       = (int)linkAge;
    payload["pos"]           = { pos.x, pos.y, pos.z };
    payload["rot"]           = { rot.x, rot.y, rot.z };
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[FollowerNpcSpawn] Broadcasting netId={} pos=({:.0f},{:.0f},{:.0f}) "
                "sceneNum={} linkAge={}",
                netId, pos.x, pos.y, pos.z, (int)sceneNum, (int)linkAge);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_FollowerNpcSpawn(nlohmann::json payload) {
    uint32_t ownerClientId = payload.value("ownerClientId", (uint32_t)0);

    // Authority gate: we ARE the owner, our local actor is the source of
    // truth. Don't double-spawn a replica.
    if (ownerClientId == ownClientId) return;

    if (gPlayState == nullptr) return;
    if (!IsSaveLoaded()) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    uint32_t netId    = payload.value("netId", (uint32_t)0);
    int16_t  sceneNum = (int16_t)payload.value("sceneNum", (int)-1);

    // Replica only relevant if the sender is in the same scene as us.
    // If the owner is in a different scene, defer (no spawn yet); when
    // they enter our scene, their next periodic STATE packet will let
    // us know. Actually for cleanliness, only spawn when same-scene.
    if (gPlayState->sceneNum != sceneNum) {
        SPDLOG_DEBUG("[FollowerNpcSpawn] ignoring spawn from owner={} (their "
                     "scene={} != our scene={})",
                     ownerClientId, (int)sceneNum, (int)gPlayState->sceneNum);
        return;
    }

    // Avoid double-spawn if we already track a replica for this owner.
    auto it = mPeerFollowerNpcs.find(ownerClientId);
    if (it != mPeerFollowerNpcs.end() && it->second != nullptr &&
        it->second->update != nullptr) {
        SPDLOG_DEBUG("[FollowerNpcSpawn] replica for owner={} already exists; skipping",
                     ownerClientId);
        return;
    }

    if (gEnFollowerId == 0) {
        SPDLOG_WARN("[FollowerNpcSpawn] gEnFollowerId is 0; can't spawn replica");
        return;
    }

    Vec3f pos{ 0.0f, 0.0f, 0.0f };
    Vec3s rot{ 0, 0, 0 };
    auto posArr = payload.value("pos", std::vector<float>{ 0.0f, 0.0f, 0.0f });
    auto rotArr = payload.value("rot", std::vector<int>{ 0, 0, 0 });
    if (posArr.size() >= 3) { pos.x = posArr[0]; pos.y = posArr[1]; pos.z = posArr[2]; }
    if (rotArr.size() >= 3) { rot.x = (s16)rotArr[0]; rot.y = (s16)rotArr[1]; rot.z = (s16)rotArr[2]; }

    Actor* spawned = Actor_Spawn(
        &gPlayState->actorCtx, gPlayState,
        gEnFollowerId,
        pos.x, pos.y, pos.z,
        rot.x, rot.y, rot.z,
        0 /* params */);
    if (spawned == nullptr) {
        SPDLOG_ERROR("[FollowerNpcSpawn] Actor_Spawn failed for owner={} netId={}",
                     ownerClientId, netId);
        return;
    }

    mPeerFollowerNpcs[ownerClientId] = spawned;
    SPDLOG_INFO("[FollowerNpcSpawn] Spawned replica for owner={} netId={} at "
                "({:.0f},{:.0f},{:.0f})",
                ownerClientId, netId, pos.x, pos.y, pos.z);
}
