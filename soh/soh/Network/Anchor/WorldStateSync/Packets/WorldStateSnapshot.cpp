#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/WorldStateSync/WorldStateSync.h"

#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

/**
 * WORLD_STATE_REQUEST + WORLD_STATE_SNAPSHOT — Pillar C v1 late-join.
 *
 * On Anchor::OnConnected the joining client sends WORLD_STATE_REQUEST to
 * its team. Each team-mate responds with a WORLD_STATE_SNAPSHOT carrying
 * the full mSetFlags entries. Joiner merges (idempotent) and applies any
 * newly-arrived entries that match the currently-loaded scene.
 *
 * Mirrors the existing REQUEST_TEAM_STATE / UPDATE_TEAM_STATE pattern.
 *
 * Size estimate (design doc): ~100 scenes × ~10 flags × ~30 bytes JSON
 * = ~30 KB worst case. Acceptable for one-time on-join transfer.
 */

void Anchor::HandlePacket_WorldStateRequest(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    if (roomState.syncItemsAndFlags == 0) return;

    // Routed packet — only respond if the request was targeted at us OR
    // is team-broadcast and the requester is on our team. The relay's
    // targetTeamId routing already filters team-broadcasts before they
    // reach us, so by the time we get this we should respond.

    nlohmann::json reply;
    reply["type"]    = WORLD_STATE_SNAPSHOT;
    reply["entries"] = WorldStateSync::BuildSnapshotPayload();

    // Reply directly to the requester. Relay-enriched payload["clientId"]
    // is the sender of the request (server-side, unforgeable).
    uint32_t requesterId = payload.value("clientId", (uint32_t)0);
    if (requesterId == 0) return;

    reply["targetClientId"] = requesterId;
    SendJsonToRemote(reply);
    SPDLOG_INFO("[WorldStateSync] Sent WORLD_STATE_SNAPSHOT to clientId={} ({} entries)",
                requesterId, reply["entries"].size());
}

void Anchor::HandlePacket_WorldStateSnapshot(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    if (roomState.syncItemsAndFlags == 0) return;

    if (!payload.contains("entries") || !payload["entries"].is_array()) {
        SPDLOG_WARN("[WorldStateSync] WORLD_STATE_SNAPSHOT missing 'entries' array — ignoring");
        return;
    }

    SPDLOG_INFO("[WorldStateSync] Received WORLD_STATE_SNAPSHOT with {} entries",
                payload["entries"].size());
    WorldStateSync::ApplySnapshotPayload(payload["entries"]);
}
