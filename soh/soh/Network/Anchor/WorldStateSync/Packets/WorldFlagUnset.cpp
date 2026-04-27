#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/WorldStateSync/WorldStateSync.h"

#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

/**
 * WORLD_FLAG_UNSET — Pillar C v1 (post-Phase-3 unset symmetry).
 *
 * Mirror of WORLD_FLAG_SET for unset events. Broadcast by
 * WorldStateSync::OnLocalFlagUnset (fired from the OnSceneFlagUnset
 * hook → Flags_UnsetSwitch at z_actor.c:700). Receiver removes the
 * entry from sSetFlags and applies via Flags_UnsetSwitch with
 * sApplyingNetworkFlag=true to suppress the echo.
 *
 * Conflict policy: last-write-wins by relay arrival order. TCP through
 * the relay serialises each client's send queue, so the most recent
 * set/unset packet for a given key is the authoritative one. Sufficient
 * for OoT's puzzle use cases (no sub-frame contention on the same flag).
 *
 * Reconnect edge case (known limitation): if peer A sets X, A
 * disconnects, peer B unsets X while A is gone, A reconnects and
 * requests snapshot — the snapshot omits X (it's unset on B), but A's
 * local sSetFlags still has X from before. ApplySnapshotPayload uses
 * insert-merge so A's stale entry isn't dropped. To be revisited if
 * a real bug surfaces; the simple fix is replace-merge on snapshot.
 */

void Anchor::HandlePacket_WorldFlagUnset(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    if (roomState.syncItemsAndFlags == 0) return;

    int16_t sceneNum = payload.value("sceneNum", (int16_t)-1);
    int16_t flagType = payload.value("flagType", (int16_t)0);
    int16_t flag     = payload.value("flag",     (int16_t)0);
    uint8_t timeline = (uint8_t)payload.value("timeline", (uint8_t)0);

    if (sceneNum < 0) return;

    SPDLOG_INFO("[WorldStateSync] Received WORLD_FLAG_UNSET sceneNum={} timeline={} flagType={} flag=0x{:02X}",
                sceneNum, (int)timeline, flagType, flag);

    WorldStateSync::ReceiveFlagUnset(sceneNum, timeline, flagType, flag);
}
