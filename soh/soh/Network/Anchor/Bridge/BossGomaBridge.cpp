// Refactor A.8 — Boss_Goma extern "C" shim bridge.
//
// Three C-linkage facades for Boss_Goma encounter coordination + the
// adjacent Mido post-Deku-Tree walk-away broadcast. Moved from
// HookHandlers.cpp on 2026-06-01 per Plans/A.8_design_review.md (DR-5)
// Stage 3.8.
//
// Domain: BossGoma sync (also covers Mido post-Deku-Tree walk per
// design doc Pillar G). No file-statics. All shims read EnemyNetId
// via ObjectExtension and send via Anchor::Instance->SendPacket_*.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <libultraship/libultraship.h>  // SPDLOG_INFO

extern "C" {
#include "z64.h"
}

// Sender wrapper — peer's BossGoma_Encounter case 3 calls this when
// its local actor.projectedPos check passes (peer is looking up at
// Goma during the intro). Host receives and increments its local
// Goma's lookedAtFrames so the fight progresses regardless of which
// player triggered the look. See Packets/BossGomaLookedAt.cpp + #67.
extern "C" void Anchor_NotifyBossGomaLookedAt(Actor* boss) {
    if (boss == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;  // host's own check fires the local path
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(boss);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_BossGomaLookedAt(ext->netId);
}

// Sender wrapper — dialog client's EnMd_BlockPath calls this when its
// transition to Walk fires for the post-Deku-Tree confrontation
// (DEKU_TREE_DEAD + !SPOKE + KOKIRI). Broadcasts to team so peers
// can transition their local Mido through the same Walk path and play
// the walk-away cinematic instead of despawning abruptly when the SPOKE
// flag syncs. See Packets/MidoPostDekuLeave.cpp + #184 follow-up.
extern "C" void Anchor_NotifyMidoPostDekuLeave(void) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    Anchor::Instance->SendPacket_MidoPostDekuLeave();
}

// Receive-side accessor — case 3 of BossGoma_Encounter calls this
// each frame. Returns 1 (and clears the flag) if a BOSS_GOMA_LOOKED_AT
// has been received during this encounter; the caller then fires
// BossGoma_SetupEncounterState4 immediately, skipping the local
// 15-frame frustum-check accumulator (which doesn't trip on host
// because host's camera isn't pointing at Goma in MP). Returns 0
// for single-player / disconnected / non-Boss_Goma callers, in
// which case case 3 falls through to its vanilla logic.
extern "C" int Anchor_BossGomaConsumePeerSignaled(Actor* boss) {
    if (boss == nullptr) return 0;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return 0;
    EnemyNetId* ext = const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(boss));
    if (ext == nullptr) return 0;
    if (!ext->bossGomaPeerSignaled) return 0;
    ext->bossGomaPeerSignaled = false;
    SPDLOG_INFO("[BossGoma] case-3 consumed peer-signal flag → firing eye-roll cinematic");
    return 1;
}
