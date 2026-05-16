/**
 * Invader — implementation (v1 step 15a scaffold + Phase B equipment swap).
 *
 * Anchor_TickInvaderActor is a no-op for v1 (combat AI blocked on
 * #208). The draw-context flag (sCurrentlyDrawingInvader) is the
 * one piece of real logic — used by the VB_APPLY_TUNIC_COLOR hook
 * to apply hostile-black tint instead of inheriting the previous
 * draw's env color.
 *
 * Phase B equipment swap (step 15b): around the Player_DrawImpl call
 * inside EnInvader_Draw, the Begin/End pair save the local Player's
 * current model state, force PLAYER_MODELGROUP_SWORD_AND_SHIELD via
 * Player_SetModels, then restore at End. This makes the Invader
 * appear permanently armed with sword + shield, independent of what
 * Link is actually holding. For v1 the Invader is always armed —
 * later combat-AI commits may switch to a state-driven model pick
 * (mirroring NpcStateToModelGroup in FollowerNPC.cpp).
 *
 * Canonical pattern lives in AIFollowerNPC/FollowerNPC.cpp lines
 * 422-526; the bow/slingshot quirk-fix from that file is not needed
 * here because v1 Invader has no ranged state. See
 * Plans/actor_feature_translation_guide.md §6.
 *
 * Future combat AI lives here, mirroring AIFollowerNPC/FollowerNPC.cpp's
 * structure (state dispatch, G-guards, recovery harness). See plan §0.5
 * for the "clone, modify, then extract once the second consumer is
 * functional" sequencing.
 */

#include "Invader.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"   // gPlayState
#include "functions.h"   // Player_SetModels
#include "macros.h"      // GET_PLAYER
#include "z64.h"         // Player, Gfx, PLAYER_MODELGROUP_SWORD_AND_SHIELD (via z64player.h)
extern PlayState* gPlayState;
}

namespace {
// Set during EnInvader_Draw's Player_DrawImpl call; cleared after.
// Read by the VB_APPLY_TUNIC_COLOR hook to know which actor's tunic
// is being rendered, so it can apply the black-tint override. File-
// scope static rather than class member because actor draws don't
// nest — the gfx context is single-threaded and each draw completes
// before the next starts.
static Actor* sCurrentlyDrawingInvader = nullptr;

// Phase B equipment swap — file-scope save slot. Single instance is
// safe because actor draws don't nest (same justification as in
// FollowerNPC.cpp's equipment-swap save slot). If two actors ever
// share the same gfx context concurrently, this becomes a stack.
//
// sEquipmentSwapActive controls whether End() should restore — set
// true in Begin() only when a real swap happened. Defensive: lets
// End() be a no-op when Begin() short-circuited (e.g. gPlayState
// null, localPlayer null, or already at the intended group).
static bool  sEquipmentSwapActive       = false;
static s32   sSavedPlayerModelGroup     = 0;
static u8    sSavedPlayerLeftHandType   = 0;
static u8    sSavedPlayerRightHandType  = 0;
static u8    sSavedPlayerSheathType     = 0;
static Gfx** sSavedPlayerLeftHandDLists  = nullptr;
static Gfx** sSavedPlayerRightHandDLists = nullptr;
static Gfx** sSavedPlayerSheathDLists    = nullptr;
static Gfx** sSavedPlayerWaistDLists     = nullptr;
}  // namespace

extern "C" void Anchor_TickInvaderActor(Actor* invader, PlayState* play) {
    // v1: no-op. Combat AI state machine (post-#208) populates this.
    (void)invader;
    (void)play;
}

extern "C" void Anchor_InvaderDrawBegin(Actor* invader) {
    sCurrentlyDrawingInvader = invader;

    // Phase B equipment swap. Save Player's current model state and
    // force the Invader's intended model (SWORD_AND_SHIELD for v1).
    // End() restores.
    sEquipmentSwapActive = false;
    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    // v1: Invader is always armed. Future combat AI may swap based on
    // an Invader state-machine — mirror NpcStateToModelGroup at that
    // point.
    const s32 intendedGroup = PLAYER_MODELGROUP_SWORD_AND_SHIELD;

    // No-op if we're already at the intended group (saves a redundant
    // SetModels call when Player is in fighter stance — common in
    // combat scenarios where the Invader is most likely to appear).
    if (intendedGroup == localPlayer->modelGroup) {
        return;
    }

    // Save. We save BOTH the modelGroup (for the canonical
    // Player_SetModels restore) AND the raw DList/type fields
    // (defensive — in case Player_SetModels's EquipmentAlwaysVisible
    // CVar branches resolve slightly different DLists on restore than
    // the user originally had).
    sSavedPlayerModelGroup       = localPlayer->modelGroup;
    sSavedPlayerLeftHandType     = localPlayer->leftHandType;
    sSavedPlayerRightHandType    = localPlayer->rightHandType;
    sSavedPlayerSheathType       = localPlayer->sheathType;
    sSavedPlayerLeftHandDLists   = localPlayer->leftHandDLists;
    sSavedPlayerRightHandDLists  = localPlayer->rightHandDLists;
    sSavedPlayerSheathDLists     = localPlayer->sheathDLists;
    sSavedPlayerWaistDLists      = localPlayer->waistDLists;

    // Apply Invader's intended model. Player_SetModels writes
    // leftHandType/DLists, rightHandType/DLists, sheathType/DLists,
    // waistDLists from sPlayerDListGroups[type][linkAge]. Does NOT
    // write modelGroup itself — the trailing assignment handles that
    // (matches FollowerNPC.cpp:489-490).
    Player_SetModels(localPlayer, intendedGroup);
    localPlayer->modelGroup = (u8)intendedGroup;
    sEquipmentSwapActive = true;
}

extern "C" void Anchor_InvaderDrawEnd(void) {
    sCurrentlyDrawingInvader = nullptr;

    if (!sEquipmentSwapActive) return;
    sEquipmentSwapActive = false;

    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    // Restore. Both the canonical SetModels call (so any internal
    // bookkeeping inside Player_SetModels remains consistent) AND
    // the raw fields (defensive — the EquipmentAlwaysVisible branches
    // inside Player_SetModels may pick slightly different DLists than
    // the user originally had).
    Player_SetModels(localPlayer, sSavedPlayerModelGroup);
    localPlayer->modelGroup      = (u8)sSavedPlayerModelGroup;
    localPlayer->leftHandType    = sSavedPlayerLeftHandType;
    localPlayer->rightHandType   = sSavedPlayerRightHandType;
    localPlayer->sheathType      = sSavedPlayerSheathType;
    localPlayer->leftHandDLists  = sSavedPlayerLeftHandDLists;
    localPlayer->rightHandDLists = sSavedPlayerRightHandDLists;
    localPlayer->sheathDLists    = sSavedPlayerSheathDLists;
    localPlayer->waistDLists     = sSavedPlayerWaistDLists;
}

extern "C" Actor* Anchor_GetCurrentlyDrawingInvader(void) {
    return sCurrentlyDrawingInvader;
}
