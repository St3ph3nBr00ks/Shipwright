#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
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
                SPDLOG_INFO("[ItemPickupRequest] Granted netId={} to clientId={} — broadcasting ITEM_COLLECTED",
                            itemNetId, senderClientId);
                // Claim: kill the drop on host so future requests find
                // it dead.
                Actor_Kill(it);
                // Broadcast ITEM_COLLECTED with the granted clientId
                // (relay overwrites the sender's clientId field, but
                // we want the WINNER's id, so set it explicitly via a
                // dedicated field).
                nlohmann::json bcast;
                bcast["type"]              = ITEM_COLLECTED;
                bcast["targetTeamId"]      = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
                bcast["sceneNum"]          = (int)gPlayState->sceneNum;
                bcast["netId"]             = itemNetId;
                bcast["winnerClientId"]    = senderClientId;
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
