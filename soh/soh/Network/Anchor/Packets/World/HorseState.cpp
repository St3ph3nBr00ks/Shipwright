/*
 * HORSE_STATE — owner → all peers (room broadcast, ~10 Hz throttled).
 *
 * Per-tick state stream for in-scope horse instances owned by the local
 * client. Receivers apply pos / shape.rot / numBoosts directly to the
 * matching local horse instance; actionState routes through the
 * EnemyNetId-style cached state pattern (lastAppliedActionState on
 * HorseNetId), with vanilla state-machine code playing the corresponding
 * animation locally on receivers.
 *
 * Critical-edge: actionState changes bypass the throttle so animation
 * transitions land immediately. Matches PLAYER_UPDATE's heldActorNetId
 * critical-edge pattern.
 *
 * Plan: Plans/horse_sync_plan.md §"HORSE_STATE" / §"Animation sync
 * model".
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
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
}

void Anchor::SendPacket_HorseState(Actor* horse) {
    if (!isConnected) return;
    if (!IsHorseSyncEnabled()) return;
    if (horse == nullptr) return;

    const HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(horse);
    if (ext == nullptr || ext->netId == 0) return;
    // Owners only — peers never send authoritative state for horses
    // they don't own.
    if (ext->isPeerOwned) return;

    EnHorse* en          = (EnHorse*)horse;
    int32_t  actionState = (int32_t)en->action;
    uint8_t  numBoosts   = en->numBoosts;

    nlohmann::json payload;
    payload["type"]        = HORSE_STATE;
    payload["netId"]       = ext->netId;
    payload["actionState"] = actionState;
    payload["speedXZ"]     = horse->speedXZ;
    payload["pos"]         = { horse->world.pos.x, horse->world.pos.y, horse->world.pos.z };
    payload["shapeRot"]    = { (int)horse->shape.rot.x,
                                (int)horse->shape.rot.y,
                                (int)horse->shape.rot.z };
    payload["numBoosts"]   = (int)numBoosts;
    payload["quiet"]       = true;  // suppress relay-side logging for high-rate packet
    PacketTimeline::SetTimelineField(payload);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_HorseState(nlohmann::json payload) {
    if (!IsHorseSyncEnabled()) return;
    if (gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    uint32_t netId = payload.value("netId", (uint32_t)0);
    if (netId == 0) return;

    // Find the local replica for this netId. mPeerHorses is keyed by
    // netId; we also fall through to a category walk via
    // FindActorByNetId so that mid-scene-transition + late-join races
    // (where the replica exists in the actor list but mPeerHorses
    // hasn't been re-populated yet) still apply state.
    Actor* horse = nullptr;
    auto it = mPeerHorses.find(netId);
    if (it != mPeerHorses.end() && it->second != nullptr &&
        it->second->update != nullptr) {
        horse = it->second;
    } else {
        horse = FindActorByNetId(gPlayState, netId);
        // Cache for future ticks.
        if (horse != nullptr) mPeerHorses[netId] = horse;
    }
    if (horse == nullptr) return;  // late-join race; HORSE_SPAWN will materialise it

    HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(horse);
    if (ext == nullptr) return;

    Vec3f pos = horse->world.pos;
    auto posArr = payload.value("pos", std::vector<float>{});
    if (posArr.size() >= 3) {
        pos.x = posArr[0]; pos.y = posArr[1]; pos.z = posArr[2];
    }
    horse->world.pos = pos;

    auto rotArr = payload.value("shapeRot", std::vector<int>{});
    if (rotArr.size() >= 3) {
        horse->shape.rot.x = (s16)rotArr[0];
        horse->shape.rot.y = (s16)rotArr[1];
        horse->shape.rot.z = (s16)rotArr[2];
    }

    horse->speedXZ = payload.value("speedXZ", 0.0f);

    ext->lastAppliedActionState = (int16_t)payload.value("actionState", -1);

    EnHorse* en = (EnHorse*)horse;
    en->numBoosts = (uint8_t)payload.value("numBoosts", 0);
}
