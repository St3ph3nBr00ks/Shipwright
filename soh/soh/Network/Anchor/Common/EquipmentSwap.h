/**
 * EquipmentSwap — shared Player-model save/swap/restore for AI actors
 *                 that render via Player_DrawImpl with a custom equipment
 *                 loadout (NPC Follower, NPC Invader).
 *
 * Both actors render Link's skeleton via Player_DrawImpl. To make them
 * visually wield their own weapon set (sword + shield in melee combat,
 * bow / slingshot in ranged combat) without permanently mutating the
 * local Player's equipment, each actor's Draw{Begin,End} brackets
 * Player_DrawImpl with a save / swap / restore of Player's model-group
 * + per-limb DList + heldItemAction state.
 *
 * Tier 1 refactor (2026-06-04) — both actors had byte-identical
 * ~70-line Draw{Begin,End} bodies with the only divergence being the
 * per-actor model-group selector callback and a per-actor draw-context
 * pointer (read by VB_APPLY_TUNIC_COLOR for owner-color attribution per
 * Pitfall 22). The shared mechanism lives here; per-actor wrappers
 * shrink to set-pointer + resolve-group + ApplySwap. See plan items
 * 4 + 5.
 *
 * Reset-on-scene-transition: each per-actor `Anchor_*DrawStateReset-
 * OnSceneTransition` shim still exists (called from the central
 * OnSceneInit handler in HookHandlers.cpp per Pitfall 22), now
 * forwarding to ResetForSceneTransition + clearing its own context
 * pointer. The OnSceneInit registration stays unchanged.
 */
#pragma once

extern "C" {
#include "z64.h"  // Player, Gfx, s32, s8, u8 (via ultratypes)
}

namespace AnchorEquipmentSwap {

// Saved Player state captured at swap-apply time. One instance is
// held file-static by each per-actor TU. Bow/slingshot override is
// folded in — see ApplySwap for the rationale.
struct EquipmentSwapState {
    // Whether a swap is currently active (i.e. ApplySwap saved into
    // this struct and Player's model fields are presently the NPC's
    // chosen loadout). When false, RestoreSwap is a no-op.
    bool   active                    = false;

    // Whether the bow/slingshot heldItemAction override fired in
    // ApplySwap (gated on intended-group + per-player inventory).
    // Tracked separately so RestoreSwap only restores the action
    // when it was actually mutated.
    bool   savedHeldItemActionActive = false;

    // Player's pre-swap equipment slots. Both canonical (modelGroup
    // — drives Player_SetModels' restore path) AND raw (defensive
    // re-binding in case anything between save and restore mutated
    // the per-limb pointers).
    s32    savedModelGroup           = 0;
    u8     savedLeftHandType         = 0;
    u8     savedRightHandType        = 0;
    u8     savedSheathType           = 0;
    Gfx**  savedLeftHandDLists       = nullptr;
    Gfx**  savedRightHandDLists      = nullptr;
    Gfx**  savedSheathDLists         = nullptr;
    Gfx**  savedWaistDLists          = nullptr;
    s8     savedHeldItemAction       = 0;
};

// Save Player's current equipment state into `state` and apply the
// NPC's intended model group. No-op (and `state.active = false`) when
// the intended group matches Player's current modelGroup — common
// case (NPC in IDLE, Player has nothing special equipped).
//
// Bow/slingshot heldItemAction override (folded in from both actors):
// Player_SetModels picks the bow vs slingshot DList variant via
// Player_HoldsSlingshot(this), which reads Player's heldItemAction.
// When NPC is the shooter, Player isn't actively holding either —
// default selection is bow. For child Link with only slingshot
// owned, that produces a visually-wrong bow on the NPC (reported
// in log 163). Override: when intended group is BOW_SLINGSHOT and
// Player has slingshot but not bow, temporarily set heldItemAction
// to PLAYER_IA_SLINGSHOT so the DList pick resolves correctly.
// Restore at End. Both / neither owned → no override.
void ApplySwap(Player* localPlayer, s32 intendedModelGroup,
               EquipmentSwapState& state);

// Restore Player's saved equipment state. No-op when `state.active`
// is false (ApplySwap either short-circuited or was never called).
// Defensive — runs both Player_SetModels (canonical re-bind) and the
// raw per-field writes (in case any code between Apply and Restore
// mutated the live state).
void RestoreSwap(Player* localPlayer, EquipmentSwapState& state);

// Defensive scene-transition reset. Clears `active` +
// `savedHeldItemActionActive` flags WITHOUT running the End-path
// restore. The saved Player DList pointers reference the prior
// scene's allocated resources, which may be freed during the
// transition — restoring them would write dangling pointers into
// Link's draw state and crash on the first Player_Draw of the new
// scene. Player_Init re-binds these naturally for the new scene;
// we just need to prevent the next ApplySwap from short-circuiting
// on a stale-active flag, and prevent the matching RestoreSwap (if
// any) from writing stale data on top of the new scene's freshly-
// initialized Player state.
//
// Called by each per-actor `Anchor_*DrawStateReset-
// OnSceneTransition` shim, which is registered to the central
// OnSceneInit handler in HookHandlers.cpp (Pitfall 22).
void ResetForSceneTransition(EquipmentSwapState& state);

}  // namespace AnchorEquipmentSwap
