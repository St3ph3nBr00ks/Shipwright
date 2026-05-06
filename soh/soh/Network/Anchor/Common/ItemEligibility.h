#pragma once

// Per-player eligibility gate for ACTOR_EN_ITEM00 pickups.
//
// Extracted from HookHandlers.cpp's `FollowerWantsItem` lambda
// (#173 / #193 Phase 0). Used by:
//   - AI Follower opportunistic item pickup scan (#172)
//   - MP item-drop sync per-player pickup gate (#193)
//
// Both callers want the same predicate shape: "given the local
// player's gSaveContext and a resolved ITEM00_* type, can the
// player benefit from picking this up right now?"
//
// "Resolved" meaning the actor->params has already been masked
// down to its post-Init ITEM00_* enum value (see
// z_en_item00.c:363 — `this->actor.params &= 0xFF`). Callers
// should not pass FLEXIBLE / unmasked-with-flags values; the
// helper treats any unknown discriminant as ineligible.

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

namespace ItemEligibility {

// Returns true when the local player (read via global gSaveContext)
// can benefit from picking up an EN_ITEM00 with the given resolved
// ITEM00_* type. Returns false for:
//   - Reserved-for-progression types (heart pieces, heart containers,
//     small keys, shields, tunics, song chest items).
//   - Capacity-capped items the local player is already capped on
//     (full HP for hearts, full quiver for arrows, etc.).
//   - Unrecognised discriminants (defensive default).
//
// Today's behaviour matches the pre-#193 AI Follower whitelist exactly,
// EXCEPT for rupees: the AI Follower allowed rupees unconditionally
// (silently truncated by vanilla); #193's per-player share semantics
// gate them on `gSaveContext.rupees < CUR_CAPACITY(UPG_WALLET)`.
//
// `walletCapAware = true` enables the wallet-cap rupee gate (used by
// #193 pickup gate so an over-capped teammate can defer to others).
// `walletCapAware = false` keeps the legacy "always allowed" rupee
// behaviour (used by AI Follower).
bool CanPlayerCollectItem00(s16 item00Type, bool walletCapAware);

}  // namespace ItemEligibility
