// EquipmentSwap — implementation. See EquipmentSwap.h for design
// rationale. Tier 1 refactor (2026-06-04) extracted from byte-
// identical Draw{Begin,End} bodies in NPC Follower / NPC Invader.

#include "EquipmentSwap.h"

extern "C" {
#include "variables.h"  // gSaveContext, gItemSlots — INV_CONTENT macro
                        // expands to gSaveContext.inventory.items[
                        // gItemSlots[item]] (Pitfall 9).
#include "functions.h"  // Player_SetModels
#include "macros.h"     // INV_CONTENT, ITEM_BOW/SLINGSHOT/NONE, PLAYER_IA_*
#include "z64player.h"  // PLAYER_MODELGROUP_BOW_SLINGSHOT
}

namespace AnchorEquipmentSwap {

void ApplySwap(Player* localPlayer, s32 intendedModelGroup,
               EquipmentSwapState& state) {
    if (localPlayer == nullptr) {
        state.active = false;
        return;
    }

    // No-op when Player is already at the intended group (saves a
    // redundant SetModels call when NPC is IDLE and Player has
    // nothing special equipped — the most common case).
    if (intendedModelGroup == localPlayer->modelGroup) {
        state.active = false;
        return;
    }

    // Save state. Both modelGroup (for the canonical SetModels
    // restore) AND raw per-limb fields (defensive — see
    // RestoreSwap).
    state.savedModelGroup       = localPlayer->modelGroup;
    state.savedLeftHandType     = localPlayer->leftHandType;
    state.savedRightHandType    = localPlayer->rightHandType;
    state.savedSheathType       = localPlayer->sheathType;
    state.savedLeftHandDLists   = localPlayer->leftHandDLists;
    state.savedRightHandDLists  = localPlayer->rightHandDLists;
    state.savedSheathDLists     = localPlayer->sheathDLists;
    state.savedWaistDLists      = localPlayer->waistDLists;

    // Bow / slingshot heldItemAction override. See header for the
    // rationale (log 163 child-Link slingshot issue).
    state.savedHeldItemActionActive = false;
    if (intendedModelGroup == PLAYER_MODELGROUP_BOW_SLINGSHOT) {
        const u8 bowSlot   = INV_CONTENT(ITEM_BOW);
        const u8 slingSlot = INV_CONTENT(ITEM_SLINGSHOT);
        const bool hasBow       = (bowSlot   != ITEM_NONE);
        const bool hasSlingshot = (slingSlot != ITEM_NONE);
        s8 desired = -1;
        if (hasSlingshot && !hasBow) {
            desired = PLAYER_IA_SLINGSHOT;
        } else if (hasBow && !hasSlingshot) {
            desired = PLAYER_IA_BOW;
        }  // both / neither → no override (let Player's natural state pick)
        if (desired >= 0 && localPlayer->heldItemAction != desired) {
            state.savedHeldItemAction       = localPlayer->heldItemAction;
            state.savedHeldItemActionActive = true;
            localPlayer->heldItemAction     = desired;
        }
    }

    // Apply NPC's intended model. Player_SetModels writes
    // leftHandType/DLists, rightHandType/DLists, sheathType/DLists,
    // waistDLists from sPlayerDListGroups[type][linkAge]. Does NOT
    // write modelAnimType (each NPC controls that separately via
    // currentAnimType).
    Player_SetModels(localPlayer, intendedModelGroup);
    localPlayer->modelGroup = intendedModelGroup;  // SetModels doesn't touch this
    state.active = true;
}

void RestoreSwap(Player* localPlayer, EquipmentSwapState& state) {
    if (!state.active) return;
    state.active = false;

    if (localPlayer == nullptr) return;

    // Canonical re-bind via Player_SetModels (so any internal
    // bookkeeping is consistent) AND raw fields (defensive — the
    // EquipmentAlwaysVisible CVar branches inside Player_SetModels
    // may pick slightly different DLists than the user originally
    // had).
    Player_SetModels(localPlayer, state.savedModelGroup);
    localPlayer->modelGroup      = state.savedModelGroup;
    localPlayer->leftHandType    = state.savedLeftHandType;
    localPlayer->rightHandType   = state.savedRightHandType;
    localPlayer->sheathType      = state.savedSheathType;
    localPlayer->leftHandDLists  = state.savedLeftHandDLists;
    localPlayer->rightHandDLists = state.savedRightHandDLists;
    localPlayer->sheathDLists    = state.savedSheathDLists;
    localPlayer->waistDLists     = state.savedWaistDLists;
    if (state.savedHeldItemActionActive) {
        localPlayer->heldItemAction      = state.savedHeldItemAction;
        state.savedHeldItemActionActive  = false;
    }
}

void ResetForSceneTransition(EquipmentSwapState& state) {
    state.active                    = false;
    state.savedHeldItemActionActive = false;
    // Saved-pointer slots intentionally NOT cleared — they may hold
    // dangling pointers to freed prior-scene resources; the next
    // ApplySwap will overwrite them before any code reads (per the
    // Pitfall 22 invariant documented in session_state.md).
}

}  // namespace AnchorEquipmentSwap
