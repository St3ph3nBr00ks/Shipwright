// TIME_SYNC packet (issue #63) — periodic + edge-triggered time-of-day
// broadcast.
//
// Wire format (schema=1):
//   { "type": "TIME_SYNC",
//     "schema": 1,
//     "dayTime": <u16>,
//     "nightFlag": <s32>,
//     "reason": "periodic" | "cs_start" | "cs_end" | "scene_transition" }
//
// Send triggers (all in HookHandlers.cpp OnGameFrameUpdate):
//   - Periodic: CVAR-configurable interval (default 5s, range 1-60s).
//   - Cutscene-start edge: csCtx.state IDLE -> non-IDLE.
//   - Cutscene-end edge: csCtx.state non-IDLE -> IDLE.
//   - Scene-transition edge: transitionTrigger OFF -> TRANS_TRIGGER_START.
//
// Receive: forward-only bidirectional reconciliation via
// AnchorTimeOfDay::ApplyIfAhead (shared with HandlePacket_UpdateClientState).
//
// Schema-driven version-bypass (Pillar F, Anchor.cpp:447-466) is automatic
// because we include the "schema" field — no allowlist entry needed.
//
// See Claude/Plans/time_of_day_periodic_sync_plan_2026-06-16.md and
// Claude/Analysis/time_of_day_sync_implementation_analysis_2026-06-16.md.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketSchemas.h"
#include "soh/Network/Anchor/Common/TimeOfDayReconcile.h"

#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "variables.h"  // gSaveContext (Pitfall 9)
}

void Anchor::SendPacket_TimeSync(const char* reason) {
    if (!isConnected) return;

    // #63 — frozen-scene broadcast suppression (added 2026-06-16 after
    // log 537 field test surfaced day-cycle-skipping behavior).
    //
    // Vanilla freezes dayTime in dungeons / boss rooms / Lon Lon Ranch
    // via envCtx.timeIncrement = 0 (z_scene.c:354-368). Option A
    // (commit c1d1d77f8) gated the RECEIVE side so the frozen-scene
    // player wouldn't accept incoming syncs — but frozen-scene clients
    // kept BROADCASTING their stuck values. Advancing-clock receivers
    // (gTimeIncrement != 0) eventually accept those broadcasts via the
    // modular-distance check: as the receiver's clock sweeps the
    // circular day, it MUST land within forwardDist < 0x8000 of any
    // frozen peer once per cycle, at which point the apply yanks the
    // receiver's clock forward — skipping morning / noon / afternoon.
    //
    // Field test log 537 captured this in a single apply line:
    //   dayTime 14098 (0x3712, night) -> 44794 (0xAEFA, late afternoon)
    //   forwardDist=30696 (just under 0x8000 = 32768)
    //
    // Suppress all TIME_SYNC sends when local clock is frozen. Frozen
    // clients stop polluting the network; advancing clients' clocks
    // evolve naturally. On scene exit, gTimeIncrement becomes non-zero
    // and broadcasts resume.
    //
    // gTimeIncrement is declared in z_kankyo.c:61, exposed via
    // soh/include/variables.h:80 (extern). Already in scope through
    // the extern "C" #include "variables.h" at file top.
    if (gTimeIncrement == 0) {
        return;
    }

    nlohmann::json payload;
    payload["type"]      = TIME_SYNC;
    // Schema field enables Pillar F bypass of the clientVersion check
    // (Anchor.cpp:447-466). Without "schema", legacy version-mismatch
    // filtering would silently drop the packet between mismatched peers.
    payload["schema"]    = PacketSchemas::GetPacketSchema(TIME_SYNC);
    payload["dayTime"]   = (u16)gSaveContext.dayTime;
    payload["nightFlag"] = gSaveContext.nightFlag;
    payload["reason"]    = (reason != nullptr) ? reason : "unknown";

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TimeSync(nlohmann::json payload) {
    // Receive runs on the game thread via ProcessIncomingPacketQueue
    // (Anchor.cpp:473-479). Safe to read/write gSaveContext directly —
    // no race with vanilla z_kankyo.c:957-959 increments.

    // Defensive: don't apply when no save is loaded (file-select, name-
    // entry, etc.). The send-side gate already filters these but a peer
    // running a different gate config could still send to us.
    if (!IsSaveLoaded()) {
        return;
    }

    if (!payload.contains("dayTime") || !payload.contains("nightFlag")) {
        SPDLOG_WARN("[TimeSync] dropped malformed payload (missing fields)");
        return;
    }

    const u16 receivedDayTime   = (u16)payload.value("dayTime", 0);
    const s32 receivedNightFlag = payload.value("nightFlag", 0);
    const std::string reason    = payload.value("reason", std::string("unknown"));

    // Helper applies forward-only check + writes both dayTime AND skyboxTime
    // (so the visual sky doesn't lag the synced time by a frame).
    AnchorTimeOfDay::ApplyIfAhead(
        receivedDayTime, receivedNightFlag, reason.c_str());
}
