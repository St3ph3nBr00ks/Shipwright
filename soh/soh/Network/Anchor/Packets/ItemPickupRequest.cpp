#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"  // kSyncableActorCategories — Phase 3 associated-actor lookup
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"
#include "soh/ObjectExtension/ObjectExtension.h"
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
 * ITEM_PICKUP_REQUEST — peer → room host (targeted).
 *
 * #193 race A mitigation: simultaneous-pickup post-window double-credit.
 *
 * Pre-mitigation: each peer ran its local pickup gate independently.
 * Two peers walking over the same drop in the same frame after the
 * 3s killer-exclusivity window both passed Layer 1+2, both credited
 * their local gSaveContext, both broadcast ITEM_COLLECTED. By the
 * time each receiver processed the other's broadcast, both clients
 * had already credited. Drop counted twice.
 *
 * Post-mitigation: peer's gate sets `pickupState = Pending` on the
 * ItemDropNetId extension and broadcasts this packet. Vanilla pickup
 * is suppressed (`*should = false`). Host walks for the matching
 * netId; if alive, kills the drop on host (claim) and broadcasts
 * ITEM_COLLECTED with the requesting peer's clientId. If host's local
 * actor is dead (host already collected, or another peer's request
 * arrived first), drops the request silently — the loser sees their
 * drop disappear via the winner's ITEM_COLLECTED broadcast.
 *
 * Race-loss UX: the loser's pickup never credits, drop disappears.
 * Winner's gate re-fires on the next frame after `pickupState`
 * transitions to `Granted`; vanilla pickup body runs and credits.
 *
 * Wire shape:
 *   {
 *     "type":           "ITEM_PICKUP_REQUEST",
 *     "schema":         1,
 *     "sceneNum":       17,
 *     "timeline":       0,
 *     "targetClientId": <effective room host>,
 *     "netId":          0xNNNNNNNN
 *   }
 *
 * Bandwidth: ~80 bytes per pickup attempt; well within budget.
 * Pickup attempts are user-input-rate, not per-frame.
 */

void Anchor::SendPacket_ItemPickupRequest(uint32_t itemNetId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;  // host doesn't request from itself

    uint32_t targetId = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    if (targetId == 0 || targetId == ownClientId) return;

    nlohmann::json payload;
    payload["type"]           = ITEM_PICKUP_REQUEST;
    payload["targetClientId"] = targetId;
    payload["sceneNum"]       = (int)gPlayState->sceneNum;
    payload["netId"]          = itemNetId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[ItemPickupRequest] Sending netId={} target={} sceneNum={}",
                itemNetId, targetId, (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ItemPickupRequest(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    // Host-only handler.
    if (!::SceneAuthority::IsMyCurrentRoomHost()) return;

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[ItemPickupRequest] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[ItemPickupRequest] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    uint32_t itemNetId      = (uint32_t)payload.value("netId", (uint32_t)0);
    uint32_t senderClientId = (uint32_t)payload.value("clientId", (uint32_t)0);
    if (itemNetId == 0 || senderClientId == 0) {
        SPDLOG_WARN("[ItemPickupRequest] Drop — malformed payload");
        return;
    }

    // Walk ACTORCAT_MISC for the matching local EnItem00. Host-side claim
    // succeeds iff the actor is still alive on host. If host already
    // picked up the drop locally (vanilla pickup body ran) the actor's
    // update is NULL and the request silently fails — the loser sees
    // the drop disappear via host's local ITEM_COLLECTED broadcast.
    Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
    while (it != nullptr) {
        if (it->id == ACTOR_EN_ITEM00 && it->update != nullptr) {
            const ItemDropNetId* ext =
                ObjectExtension::GetInstance().Get<ItemDropNetId>(it);
            if (ext != nullptr && ext->netId == itemNetId) {
                // Phase 3 C-hybrid: look up the decorative offering
                // actor near the EN_ITEM00 (same proximity-walk as
                // host-self pickup at HookHandlers.cpp). Embed its
                // netId so receivers Actor_Kill the decoration at
                // the moment of pickup.
                //
                // ITEM_COLLECTED broadcasts do NOT echo to sender —
                // host must ALSO dismiss the associated actor locally
                // (verified log 290 P1 AnchorProfile: rx_pps=0 for
                // ITEM_COLLECTED after host's own grant broadcast).
                uint32_t assocActorNetId = 0;
                Actor*   assocActor      = nullptr;
                {
                    constexpr float kAssocRadius   = 200.0f;
                    constexpr float kAssocRadiusSq = kAssocRadius * kAssocRadius;
                    float bestDistSq = kAssocRadiusSq;
                    for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
                        Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
                        while (a != nullptr) {
                            const EnemyNetId* nidExt =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                            if (nidExt != nullptr && nidExt->netId != 0 &&
                                nidExt->phase != EnemyStateSync::LifecyclePhase::Alive &&
                                a->update != nullptr) {
                                const float dx = a->world.pos.x - it->world.pos.x;
                                const float dy = a->world.pos.y - it->world.pos.y;
                                const float dz = a->world.pos.z - it->world.pos.z;
                                const float dSq = dx * dx + dy * dy + dz * dz;
                                if (dSq < bestDistSq) {
                                    bestDistSq      = dSq;
                                    assocActorNetId = nidExt->netId;
                                    assocActor      = a;
                                }
                            }
                            a = a->next;
                        }
                    }
                }

                SPDLOG_INFO("[ItemPickupRequest] Granted netId={} to clientId={} "
                            "assocActorNetId={} — broadcasting ITEM_COLLECTED",
                            itemNetId, senderClientId, assocActorNetId);
                // Claim: kill the drop on host so future requests find
                // it dead.
                Actor_Kill(it);
                // Local dismissal of associated actor (host doesn't
                // get its own ITEM_COLLECTED echo).
                if (assocActor != nullptr) {
                    SPDLOG_INFO("[ItemPickupRequest] dismissing associated actor netId={} "
                                "locally on host (no own-echo for ITEM_COLLECTED)",
                                assocActorNetId);
                    Actor_Kill(assocActor);
                }
                // Broadcast ITEM_COLLECTED with the granted clientId
                // (relay overwrites the sender's clientId field, but
                // we want the WINNER's id, so set it explicitly via a
                // dedicated field).
                nlohmann::json bcast;
                bcast["type"]                 = ITEM_COLLECTED;
                bcast["targetTeamId"]         = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
                bcast["sceneNum"]             = (int)gPlayState->sceneNum;
                bcast["netId"]                = itemNetId;
                bcast["winnerClientId"]       = senderClientId;
                bcast["associatedActorNetId"] = assocActorNetId;
                PacketTimeline::SetTimelineField(bcast);
                SendJsonToRemote(bcast);
                return;
            }
        }
        it = it->next;
    }

    SPDLOG_INFO("[ItemPickupRequest] Drop — netId={} no longer alive on host (already collected)",
                itemNetId);
}
