#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * ITEM_COLLECTED — collector → all clients (team-broadcast).
 *
 * #193 Phase 1 — packet plumbing only. The pickup-gate sender + Actor_Kill
 * receiver land in Phase 3 (after the OnActorSpawn / ItemDropNetId
 * extension work in Phase 2). Send/Handle here are stubs that exercise
 * the wire format and dispatch chain.
 *
 * Wire fields:
 *   sceneNum     — sender's scene at send time. Receivers running in a
 *                  different scene drop the packet (the matching item
 *                  doesn't exist locally).
 *   timeline     — Pillar B linkAge.
 *   netId        — matches the ITEM_DROP_SYNC netId; receivers use this
 *                  to find the local EnItem00 in their actor list.
 *   collectorClientId — relay-derived; not trusted from sender, used
 *                  for diagnostic logging only.
 *   targetTeamId — team-scope routing.
 *
 * Receive flow (Phase 3):
 *   1. Walk ACTORCAT_MISC for an EnItem00 whose ItemDropNetId extension
 *      matches `netId`.
 *   2. Actor_Kill on hit. Vanilla EnItem00 lifecycle handles the
 *      sparkle/despawn animation.
 *   3. If no match (item never spawned locally, or already collected
 *      via local pickup before broadcast arrived), the packet is a
 *      no-op. Idempotent.
 */

void Anchor::SendPacket_ItemCollected(uint32_t itemNetId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = ITEM_COLLECTED;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["netId"]        = itemNetId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[ItemCollected] Sending netId={} sceneNum={}",
                itemNetId, (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ItemCollected(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[ItemCollected] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[ItemCollected] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    uint32_t itemNetId = (uint32_t)payload.value("netId", (uint32_t)0);
    if (itemNetId == 0) {
        SPDLOG_WARN("[ItemCollected] Drop — netId == 0");
        return;
    }

    // Winner resolution. Two broadcast paths:
    //  - Host's direct pickup: relay-injected `clientId` IS the winner
    //    (host picked up locally, broadcasts as itself).
    //  - Host's arbitration grant (race A): explicit `winnerClientId`
    //    field carries the requesting peer's id; the relay-injected
    //    `clientId` is the host (broadcaster), not the winner.
    // Prefer winnerClientId if present; fall back to clientId.
    uint32_t winnerClientId = (uint32_t)payload.value("winnerClientId", (uint32_t)0);
    if (winnerClientId == 0) {
        winnerClientId = (uint32_t)payload.value("clientId", (uint32_t)0);
    }

    // Walk ACTORCAT_MISC for the matching ItemDropNetId extension.
    Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
    while (it != nullptr) {
        if (it->id == ACTOR_EN_ITEM00 && it->update != nullptr) {
            const ItemDropNetId* ext =
                ObjectExtension::GetInstance().Get<ItemDropNetId>(it);
            if (ext != nullptr && ext->netId == itemNetId) {
                if (winnerClientId == ownClientId) {
                    // We won the arbitration. Two sub-cases:
                    //   - Our extension state was Pending (we sent
                    //     ITEM_PICKUP_REQUEST and host granted): transition
                    //     to Granted so the next pickup-gate fire allows
                    //     vanilla pickup. Don't kill the actor — vanilla
                    //     pickup body needs it alive to call Item_Give.
                    //   - We're the host echoing our own broadcast back:
                    //     extension state is None (host doesn't go through
                    //     Pending). Vanilla pickup already credited; the
                    //     drop's natural lifecycle (or our own Actor_Kill
                    //     in the pickup body) will clean up. Treat as
                    //     no-op.
                    if (ext->pickupState == ItemPickupState::Pending) {
                        ItemDropNetId* mut = const_cast<ItemDropNetId*>(ext);
                        mut->pickupState = ItemPickupState::Granted;
                        SPDLOG_INFO("[ItemCollected] rx netId={} GRANT for local — transitioning Pending → Granted",
                                    itemNetId);
                    }
                    // else: host-self-echo; no action.
                    return;
                }

                // Winner is someone else: Actor_Kill the local copy.
                SPDLOG_INFO("[ItemCollected] rx netId={} winner={} — killing local copy",
                            itemNetId, winnerClientId);
                Actor_Kill(it);
                return;
            }
        }
        it = it->next;
    }

    SPDLOG_DEBUG("[ItemCollected] rx netId={} winner={} — no local copy found",
                 itemNetId, winnerClientId);
}
