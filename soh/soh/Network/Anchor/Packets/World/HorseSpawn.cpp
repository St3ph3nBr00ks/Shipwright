/*
 * HORSE_SPAWN — owner → all peers (room broadcast).
 *
 * Sent when the owner client's OnActorSpawn admits an in-scope
 * ACTOR_EN_HORSE instance to the sync pipeline. Receivers spawn a
 * local replica via Anchor_SpawnPeerHorse, attach a HorseNetId
 * extension keyed to the same netId, and from that point on apply
 * HORSE_STATE updates as the authoritative source for that horse's
 * pos/rot/animation.
 *
 * Authority gate: receivers whose ownClientId matches ownerClientId
 * ignore the packet (their local actor IS the source of truth — the
 * actor that fired the originating broadcast).
 *
 * Plan: Plans/horse_sync_plan.md §"Wire format" / §"Spawn paths".
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/HorseNetId.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"  // CVAR_ENHANCEMENT — #264 diagnostic gate

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

extern "C" Actor* Anchor_SpawnPeerHorse(uint32_t netId, uint32_t ownerClientId,
                                         int16_t variantParams,
                                         Vec3f pos, int16_t rotY);

void Anchor::SendPacket_HorseSpawn(uint32_t netId, int16_t variantParams,
                                     int16_t sceneNum, const Vec3f& pos,
                                     int16_t rotY) {
    if (!isConnected) return;
    if (!IsHorseSyncEnabled()) return;

    nlohmann::json payload;
    payload["type"]          = HORSE_SPAWN;
    payload["netId"]         = netId;
    payload["ownerClientId"] = ownClientId;
    payload["variantParams"] = (int)variantParams;
    payload["sceneNum"]      = (int)sceneNum;
    payload["pos"]           = { pos.x, pos.y, pos.z };
    payload["rotY"]          = (int)rotY;
    PacketTimeline::SetTimelineField(payload);

    // #264 diag — explicit `senderProcessOwn` tag so the SOURCE client is
    // unambiguous even when logs are cross-read. In owner-authoritative
    // model, `owner` field IS `ownClientId` of the sender, so they will
    // always match; the redundancy makes log forensics easier.
    SPDLOG_INFO("[HorseSpawn] Broadcasting netId={} owner={} senderProcessOwn={} "
                "params={} scene={} pos=({:.0f},{:.0f},{:.0f}) rotY={}",
                netId, ownClientId, ownClientId, (int)variantParams, (int)sceneNum,
                pos.x, pos.y, pos.z, (int)rotY);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_HorseSpawn(nlohmann::json payload) {
    const bool diagOn =
        CVarGetInteger(CVAR_ENHANCEMENT("DebugHorseSyncDiag"), 0) != 0;

    if (!IsHorseSyncEnabled()) return;
    if (gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    uint32_t ownerClientId = payload.value("ownerClientId", (uint32_t)0);

    // #264 diag — log the receive event with explicit receiver identity
    // BEFORE any of the gates fire. Authority gate skip is logged so we
    // can tell from logs whether the packet arrived and was self-rejected
    // vs. never arrived.
    if (diagOn) {
        SPDLOG_INFO("[HorseSpawn.diag] HandlePacket received: receiver={} "
                    "ownerClientId={} authoritySelf={}",
                    ownClientId, ownerClientId,
                    ownerClientId == ownClientId);
    }
    if (ownerClientId == ownClientId) return;  // authority gate

    uint32_t netId         = payload.value("netId", (uint32_t)0);
    int16_t  variantParams = (int16_t)payload.value("variantParams", 0);
    int16_t  sceneNum      = (int16_t)payload.value("sceneNum", -1);
    int16_t  rotY          = (int16_t)payload.value("rotY", 0);

    // Same-scene gate: peer horses only render when the receiver is in
    // the owner's scene. If we're elsewhere, ignore — when the owner
    // next sends from our scene (or we transition to theirs), their next
    // HORSE_SPAWN re-establishes the replica. Owner re-sends on their
    // OnSceneSpawnActors so this is reliable.
    if (gPlayState->sceneNum != sceneNum) {
        if (diagOn) {
            SPDLOG_INFO("[HorseSpawn.diag] receiver={} netId={} "
                        "REJECTED scene-mismatch (their scene={} != our scene={})",
                        ownClientId, netId,
                        (int)sceneNum, (int)gPlayState->sceneNum);
        }
        SPDLOG_DEBUG("[HorseSpawn] ignoring (their scene={} != our scene={})",
                     (int)sceneNum, (int)gPlayState->sceneNum);
        return;
    }

    // Defensive: avoid double-spawn for an already-tracked netId.
    auto it = mPeerHorses.find(netId);
    if (it != mPeerHorses.end() && it->second != nullptr &&
        it->second->update != nullptr) {
        if (diagOn) {
            SPDLOG_INFO("[HorseSpawn.diag] receiver={} netId={} "
                        "REJECTED already-tracked-dedup (existing actor at {:p})",
                        ownClientId, netId,
                        (void*)it->second);
        }
        SPDLOG_DEBUG("[HorseSpawn] netId={} already has a tracked replica; "
                     "ignoring duplicate spawn",
                     netId);
        return;
    }

    Vec3f pos{ 0.0f, 0.0f, 0.0f };
    auto posArr = payload.value("pos", std::vector<float>{ 0.0f, 0.0f, 0.0f });
    if (posArr.size() >= 3) { pos.x = posArr[0]; pos.y = posArr[1]; pos.z = posArr[2]; }

    Actor* horse = Anchor_SpawnPeerHorse(netId, ownerClientId, variantParams,
                                          pos, rotY);
    if (horse == nullptr) {
        SPDLOG_ERROR("[HorseSpawn] Anchor_SpawnPeerHorse failed for netId={} owner={}",
                     netId, ownerClientId);
        return;
    }

    mPeerHorses[netId] = horse;
    // #264 diag — confirm the post-spawn world.pos on the receiver to
    // detect if Anchor_SpawnPeerHorse mutated coords or if a follow-up
    // tick relocated the actor (would point at Hypothesis A — receiver-
    // side culling rather than sender-side stale pos).
    if (diagOn) {
        SPDLOG_INFO("[HorseSpawn.diag] receiver={} netId={} owner={} "
                    "spawn-applied: requested pos=({:.0f},{:.0f},{:.0f}) "
                    "actor->world.pos=({:.0f},{:.0f},{:.0f}) "
                    "actor->home.pos=({:.0f},{:.0f},{:.0f}) ourScene={}",
                    ownClientId, netId, ownerClientId,
                    pos.x, pos.y, pos.z,
                    horse->world.pos.x, horse->world.pos.y, horse->world.pos.z,
                    horse->home.pos.x, horse->home.pos.y, horse->home.pos.z,
                    (int)gPlayState->sceneNum);
    }
    SPDLOG_INFO("[HorseSpawn] Spawned replica netId={} owner={} at "
                "({:.0f},{:.0f},{:.0f})",
                netId, ownerClientId, pos.x, pos.y, pos.z);
}
