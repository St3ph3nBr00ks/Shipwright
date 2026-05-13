/*
 * FOLLOWER_NPC_STATE — periodic position/rotation/state update for
 * NPC Follower replicas. Sent from owner at ~10 Hz; receivers apply
 * to the local replica's actor fields.
 *
 * Plan: Plans/npc_follower_plan.md §2.6 / §2.9.
 * Phase 3 of the NPC Follower work — v1 carries pos/rot only; the
 * `health` / `deathFlag` fields are RESERVED for v2 combat redesign.
 * `jointTable` (animation sync) is deferred until the state machine
 * lands in Phase 4 (the owner doesn't have meaningful joint data
 * to broadcast yet beyond the idle wait animation, which the replica
 * plays locally).
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

// Throttle: send every kFollowerNpcStateMs of game time. 100ms ≈ 10Hz
// at 20fps (= MsToGameTicks → 2 ticks) and ≈ 10Hz at 60fps (6 ticks).
// Framerate-aware via the MsToGameTicks infrastructure.
static constexpr int kFollowerNpcStateMs = 100;

void Anchor::SendPacket_FollowerNpcState() {
    if (!isConnected) return;
    if (mFollowerNpcLocalActor == nullptr) return;
    if (mFollowerNpcLocalActor->update == nullptr) return;
    if (gPlayState == nullptr) return;

    // The throttle check is done by the caller (TickFollowerNpcStateBroadcast).

    Actor* npc = mFollowerNpcLocalActor;
    const Vec3f& pos = npc->world.pos;
    const Vec3s& rot = npc->world.rot;
    const Vec3s& sr  = npc->shape.rot;
    const Vec3f& sc  = npc->scale;

    nlohmann::json payload;
    payload["type"]          = FOLLOWER_NPC_STATE;
    payload["netId"]         = (uint32_t)ownClientId;  // v1: one NPC per client
    payload["ownerClientId"] = ownClientId;
    payload["quiet"]         = true;  // relay suppresses logging for high-frequency packets
    payload["pos"]           = { pos.x, pos.y, pos.z };
    payload["rot"]           = { rot.x, rot.y, rot.z };
    payload["shapeRot"]      = { sr.x, sr.y, sr.z };
    payload["scale"]         = { sc.x, sc.y, sc.z };
    payload["state"]         = 0;  // Phase 4 will populate from EnFollower's state
    payload["health"]        = (int)100;  // RESERVED for v2
    payload["deathFlag"]     = (int)0;    // RESERVED for v2
    PacketTimeline::SetTimelineField(payload);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_FollowerNpcState(nlohmann::json payload) {
    uint32_t ownerClientId = payload.value("ownerClientId", (uint32_t)0);

    // Authority gate: we ARE the owner; ignore the echo.
    if (ownerClientId == ownClientId) return;

    if (gPlayState == nullptr) return;
    if (!IsSaveLoaded()) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    auto it = mPeerFollowerNpcs.find(ownerClientId);
    if (it == mPeerFollowerNpcs.end()) {
        // No replica yet. Could happen if STATE arrived before SPAWN
        // (race) or after our local DESPAWN. Silently skip — the next
        // SPAWN will create the replica and subsequent STATE packets
        // catch up.
        return;
    }
    Actor* replica = it->second;
    if (replica == nullptr || replica->update == nullptr) {
        // Replica died (e.g. scene transition); drop from map. The
        // owner's next SPAWN (auto-respawn on scene change) will
        // recreate.
        mPeerFollowerNpcs.erase(it);
        return;
    }

    // Apply pos / rot / scale. State / health / deathFlag reserved
    // for Phase 4 / v2 — read but ignore for now.
    auto posArr = payload.value("pos",      std::vector<float>{ 0.0f, 0.0f, 0.0f });
    auto rotArr = payload.value("rot",      std::vector<int>{ 0, 0, 0 });
    auto srArr  = payload.value("shapeRot", std::vector<int>{ 0, 0, 0 });
    auto scArr  = payload.value("scale",    std::vector<float>{ 0.01f, 0.01f, 0.01f });
    if (posArr.size() >= 3) {
        replica->world.pos.x = posArr[0];
        replica->world.pos.y = posArr[1];
        replica->world.pos.z = posArr[2];
    }
    if (rotArr.size() >= 3) {
        replica->world.rot.x = (s16)rotArr[0];
        replica->world.rot.y = (s16)rotArr[1];
        replica->world.rot.z = (s16)rotArr[2];
    }
    if (srArr.size() >= 3) {
        replica->shape.rot.x = (s16)srArr[0];
        replica->shape.rot.y = (s16)srArr[1];
        replica->shape.rot.z = (s16)srArr[2];
    }
    if (scArr.size() >= 3) {
        replica->scale.x = scArr[0];
        replica->scale.y = scArr[1];
        replica->scale.z = scArr[2];
    }
}

void Anchor::TickFollowerNpcStateBroadcast() {
    if (!isConnected) return;
    if (mFollowerNpcLocalActor == nullptr) return;
    if (mFollowerNpcLocalActor->update == nullptr) return;

    const uint64_t curFrame = gameFrameCounter.load(std::memory_order_relaxed);
    const int      interval = MsToGameTicks(kFollowerNpcStateMs);
    if (interval <= 0) return;
    if (curFrame < mFollowerNpcStateLastFrame + (uint64_t)interval) return;
    mFollowerNpcStateLastFrame = curFrame;

    SendPacket_FollowerNpcState();
}
