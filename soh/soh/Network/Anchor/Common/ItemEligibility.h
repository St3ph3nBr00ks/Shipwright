#pragma once

// Per-player eligibility gate for ACTOR_EN_ITEM00 pickups.
//
// Extracted from HookHandlers.cpp's `FollowerWantsItem` lambda
// (#173 / #193 Phase 0). Used by:
//   - AI Player Follower opportunistic item pickup scan (#172)
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
// Today's behaviour matches the pre-#193 AI Player Follower whitelist exactly,
// EXCEPT for rupees: the AI Player Follower allowed rupees unconditionally
// (silently truncated by vanilla); #193's per-player share semantics
// gate them on `gSaveContext.rupees < CUR_CAPACITY(UPG_WALLET)`.
//
// `walletCapAware = true` enables the wallet-cap rupee gate (used by
// #193 pickup gate so an over-capped teammate can defer to others).
// `walletCapAware = false` keeps the legacy "always allowed" rupee
// behaviour (used by AI Player Follower).
bool CanPlayerCollectItem00(s16 item00Type, bool walletCapAware);

// Phase 2 (spec §4 Phase 2) — eligibility-bitmap broadcast.
//
// Each client computes a uint32 bitmap of which ITEM00_* types they
// currently CAN benefit from (per CanPlayerCollectItem00 with
// walletCapAware=true) and broadcasts it on UPDATE_CLIENT_STATE.
// The Layer 2 pickup gate consults peer bitmaps to decide whether
// to defer pickup to a teammate.
//
// Returns the bit position for a given ITEM00_* type, or 0 if the
// type doesn't have a tracked eligibility bit. Returns a single-bit
// mask (1 << bit), not the bit index itself. Caller AND-tests against
// the bitmap directly.
//
// Bit layout (see spec for rationale):
//   bit 0  ITEM00_RUPEE_GREEN
//   bit 1  ITEM00_RUPEE_BLUE
//   bit 2  ITEM00_RUPEE_RED
//   bit 3  ITEM00_RUPEE_ORANGE
//   bit 4  ITEM00_RUPEE_PURPLE
//   bit 5  ITEM00_HEART
//   bit 6  ITEM00_MAGIC_SMALL
//   bit 7  ITEM00_MAGIC_LARGE
//   bit 8  ITEM00_STICK
//   bit 9  ITEM00_NUTS
//   bit 10 ITEM00_SEEDS
//   bit 11 ITEM00_ARROWS_* (any of SINGLE/SMALL/MEDIUM/LARGE — same bag)
//   bit 12 ITEM00_BOMBS_* (any of A/B/SPECIAL — same bag)
//   bit 13 ITEM00_BOMBCHU
uint32_t EligibilityBitForItem00(s16 item00Type);

// Compute the local player's full eligibility bitmap by walking each
// tracked ITEM00_* type and consulting CanPlayerCollectItem00 with
// walletCapAware=true. Called on save-load and on inventory changes.
uint32_t ComputeLocalEligibilityBitmap();

}  // namespace ItemEligibility
