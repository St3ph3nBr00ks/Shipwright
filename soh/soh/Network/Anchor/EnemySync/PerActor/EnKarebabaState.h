#pragma once

// Refactor A.7 — per-actor state extract for ACTOR_EN_KAREBABA.
//
// First per-actor extract under EnemySync/PerActor/. Lifts truly
// Karebaba-specific fields off the catch-all `EnemyNetId` struct into a
// dedicated nested struct. Future regrow-class enemies (En_Dekubaba
// state-machine sync, En_St, En_Sw, En_Dekunuts) should follow the same
// shape: a dedicated header under `PerActor/` with a tightly-scoped
// struct nested into `EnemyNetId`.
//
// See Plans/decoupling_gap_audit_2026-05-16.md §16 + Plans/refactor_
// sequenced_landing_plan_2026-05-29.md Stage 3.7.
//
// Scope-narrowing note (verified 2026-05-31): the earlier audit drafts
// listed `netStateIndex` as Karebaba-only — incorrect. `netStateIndex`
// is shared by 9+ actor types (Karebaba, Dekubaba, EnGoma, Dekunuts,
// Hintnuts, EnSt, EnSw, BossGoma, EnGoroiwa) and stays on `EnemyNetId`.
// Only `netActorParams` is genuinely Karebaba-specific (Karebaba's
// vanilla DeadItemDrop branching reads off the params bit, and no
// other actor mirrors actor->params over the wire). The four other
// fields in the original audit list (`pendingNaturalDeath`,
// `stalledKillPending`, `deferredDeadItemDrop`, `pendingKillNetIds`)
// had already been migrated during C2 Phase 1/2 — phase-predicate
// reads + `EnemyStateSync::HostBookkeeping`.

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"  // s16
}

namespace EnemySync {

struct EnKarebabaState {
    // Mirror of `actor->params` at host-send time. Karebaba reads its
    // own `params` to branch between DeadItemDrop (params == 1) and the
    // Dead → Regrow path (params == 0); non-host's `ApplyNetState`
    // applies this mirror BEFORE calling the actor's actionFunc so the
    // two paths stay in lock-step with the host.
    s16 netActorParams = 0;

    // Pillar 5 (GH #310) — geyser AoE Spin enhancement flag from most
    // recent host broadcast. Peer's HookHandlers forwards this to the
    // EnKarebabaDescriptor via Anchor_Enhance_EnKarebaba_ApplyPeerEnhancedFlag
    // BEFORE calling EnKarebaba_ApplyNetState, so peer's local
    // SetupSpin sees the correct flag when transitioning to state 4.
    bool netEnhancedSpin = false;

    // V6 — telegraph flag. True when host's Karebaba is in Ready
    // charge state (about to fire acid on next in-range spin).
    // Peer applies via Anchor_Enhance_EnKarebaba_ApplyPeerChargedFlag
    // so peer's OnUprightTick renders the 1.25× head + mouth spit.
    bool netCharged      = false;
};

}  // namespace EnemySync
