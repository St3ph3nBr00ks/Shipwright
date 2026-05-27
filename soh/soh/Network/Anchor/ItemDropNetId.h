#pragma once

// Attached to ACTOR_EN_ITEM00 drops so the receive-side pickup gate can
// read host-authoritative drop metadata.
//
// Extracted from Anchor.h 2026-05-21 per Plans/decoupling_gap_audit_2026-05-16.md
// §3.3 / A.3. Anchor.h re-includes this header so existing consumers continue
// to compile unchanged.

#include <cstdint>
#include <string>

// #193 Phase 2 — attached to ACTOR_EN_ITEM00 actors so the receive-side
// pickup gate can read the host-authoritative drop metadata.
//   netId           — unique per-drop identifier (matches ITEM_DROP_SYNC).
//   killerClientId  — player who triggered the drop. Used by the 3s
//                     killer-exclusive window in the pickup gate.
//                     0 means unattributed (no exclusivity).
//   spawnTimeMs     — host's monotonic clock at drop time, in ms. Used
//                     by `now - spawnTimeMs < kKillerExclusiveMs` for
//                     the grace window. Stamped from the broadcast on
//                     receivers; stamped from `steady_clock::now()` on
//                     the host.
//   isFromBroadcast — true on receivers (drop was spawned from
//                     ITEM_DROP_SYNC). False on the host (local drop
//                     that the host then broadcasts). Used to suppress
//                     the OnActorSpawn-side broadcast on receivers so
//                     they don't echo the packet back.
// #193 race A mitigation — three-state pickup-claim machine for
// host-arbitrated pickup. Peer's local pickup gate transitions
// None → Pending when a request is sent. Host's ITEM_COLLECTED grant
// (winner == ownClientId) transitions Pending → Granted. Vanilla
// pickup runs on the next gate fire when state is Granted.
//
// Rationale: simultaneous-pickup post-window races could double-credit
// because each peer's local gate ran independently and credited
// gSaveContext before the other's broadcast arrived. Host arbitration
// serialises pickup so only one client's gSaveContext is credited.
enum class ItemPickupState : uint8_t {
    None     = 0,
    Pending  = 1,
    Granted  = 2,
    // #193 Phase 1 follow-up (log 287 Bug 2). Vanilla EnItem00 pickup
    // takes multiple frames to complete: first frame fires Item_Give /
    // Actor_OfferGetItemNearby, subsequent frames spin until
    // Actor_HasParent becomes true and Actor_Kill fires. The gate
    // fires every frame the player is in contact with the actor.
    // Without this terminal state, the Granted→None transition lets
    // the second gate-fire re-route through race A (sending a
    // spurious ITEM_PICKUP_REQUEST and returning *should=false),
    // which makes vanilla's pickup function return early and the
    // actor lives to its 220-frame unk_15A timeout (~3.7s @ 60fps).
    //
    // Consumed is terminal: once the grant has been applied locally
    // (first vanilla run), subsequent gate fires return *should=true
    // unconditionally so vanilla's give-item flow can complete.
    Consumed = 3,
};

struct ItemDropNetId {
    uint32_t        netId           = 0;
    uint32_t        killerClientId  = 0;
    int64_t         spawnTimeMs     = 0;
    bool            isFromBroadcast = false;
    ItemPickupState pickupState     = ItemPickupState::None;
    // #193 Phase 1 (spec Q2) — killer's team identity at drop time,
    // for the team-aware Layer 1 gate. Empty string when killer is
    // unattributed (killerClientId == 0) or killer's AnchorClient is
    // not in the local clients map. Layer 1 gate compares this to
    // the local player's TeamId CVar and bypasses the killer-
    // exclusive window for same-team players when TeamSharesPickups
    // is true.
    std::string     killerTeamId;
};
