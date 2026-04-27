#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/WorldStateSync/WorldStateSync.h"

#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

/**
 * WORLD_FLAG_SET — Pillar C v1.
 *
 * Broadcast when a local FLAG_SCENE_SWITCH set has fired in
 * Flags_SetSwitch (z_actor.c:675). Carries the flag's coordinates in the
 * Pillar C key space:
 *   sceneNum, timeline (linkAge), flagType (FLAG_SCENE_SWITCH for v1),
 *   flag (the bit index).
 *
 * Send is built inside WorldStateSync::OnLocalFlagSet — there's no
 * SendPacket_* in this file because the dispatch is one-way (hook → send).
 *
 * Receive: stores the entry in WorldStateSync's local sSetFlags and, if
 * the receiver is in the same (sceneNum, timeline), applies via
 * Flags_SetSwitch with sApplyingNetworkFlag=true to suppress the echo.
 */

void Anchor::HandlePacket_WorldFlagSet(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;

    // Pillar B: timeline is intentionally NOT used as a filter here.
    // Cross-timeline state is meaningful — it's part of the WorldState
    // key. The receiver stores both timelines in its replicated map and
    // only applies the entry whose timeline matches the local linkAge.
    if (roomState.syncItemsAndFlags == 0) return;

    int16_t sceneNum = payload.value("sceneNum", (int16_t)-1);
    int16_t flagType = payload.value("flagType", (int16_t)0);
    int16_t flag     = payload.value("flag",     (int16_t)0);
    uint8_t timeline = (uint8_t)payload.value("timeline", (uint8_t)0);

    if (sceneNum < 0) return;

    SPDLOG_INFO("[WorldStateSync] Received WORLD_FLAG_SET sceneNum={} timeline={} flagType={} flag=0x{:02X}",
                sceneNum, (int)timeline, flagType, flag);

    WorldStateSync::ReceiveFlagSet(sceneNum, timeline, flagType, flag);
}
