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

    uint32_t itemNetId       = (uint32_t)payload.value("netId", (uint32_t)0);
    uint32_t collectorClientId = (uint32_t)payload.value("clientId", (uint32_t)0);

    // Phase 1 stub — Phase 3 will walk ACTORCAT_MISC for the matching
    // ItemDropNetId extension and Actor_Kill the local copy.
    SPDLOG_INFO("[ItemCollected] (Phase 1 stub) rx netId={} collector={}",
                itemNetId, collectorClientId);
}
