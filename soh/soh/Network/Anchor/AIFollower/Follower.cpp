/**
 * AiFollower / Follower — implementation. Phase 1 commit 1: scaffolding.
 *
 * Empty stub. Subsequent commits move the AI Follower state machine,
 * helper methods, hook bodies, and tunable constants out of
 * HookHandlers.cpp into this file.
 *
 * This file's mere existence is the value-add of commit 1: verifies the
 * new directory under soh/soh/Network/Anchor/AiFollower/ is picked up by
 * the CMake GLOB_RECURSE in soh/CMakeLists.txt (after a reconfigure)
 * and that the namespace + ShipInit boot path work end-to-end. Future
 * move-commits drop into a working build infrastructure rather than
 * doing scaffolding + logic-move atomically (which is harder to
 * verify and harder to revert).
 */

#include "Follower.h"
#include "../Anchor.h"
#include "soh/cvar_prefixes.h"        // CVAR_REMOTE_ANCHOR
#include "soh/ShipInit.hpp"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "macros.h"     // BTN_*, ITEM_*, SLOT_*, GET_PLAYER
#include "variables.h"  // gSaveContext + other game globals
extern PlayState* gPlayState;  // required despite variables.h include — see Pitfall 9
}

namespace AnchorFollower {

void RegisterFollowerModule() {
    // Stub. Subsequent Phase 1 commits register OnGameFrameUpdate /
    // ShouldActorUpdate hooks here, mirroring (and eventually
    // replacing) the registrations currently performed inside
    // Anchor::RegisterHooks for follower-specific behaviour.
    SPDLOG_DEBUG("[AiFollower] Follower module scaffolded (no hooks yet)");
}

// Phase 1 commit 3 stub. Phase 1 commit 4 will populate this with the
// follower body lifted from HookHandlers.cpp's OnGameFrameUpdate
// callback. Until then, calling TickFollower is a no-op — the existing
// lambda body in HookHandlers.cpp continues to drive follower behaviour
// unchanged.
void TickFollower(FollowerFrameContext& /*ctx*/) {
    // Intentionally empty. Implementation arrives in Phase 1 commit 4.
}

} // namespace AnchorFollower

// ---------------------------------------------------------------------------
// Anchor:: follower-specific member implementations. Declarations stay in
// Anchor.h; bodies relocated here from HookHandlers.cpp per Phase 1
// commit 2 of the SRP refactor (#173 / #169). Behaviour preserved
// byte-for-byte from the originals.
//
// Method bodies are defined under `Anchor::` qualifier in this TU; private-
// member access works the same as before (member-function scope is private,
// not file-private, in C++).
// ---------------------------------------------------------------------------

// Option B — follower item override system. See Anchor.h for full
// design. Touches gSaveContext.equips.{buttonItems[1..3], cButtonSlots[0..2]}.
// B-slot (sword) is never modified.
u8 Anchor::FollowerTryEquipRangedWeapon() {
    // Gate on CVar.
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems"), 0)) {
        return 0xFF;
    }
    // Idempotent — if already overridden, just report the active slot.
    if (followerItemOverrideActive) {
        return followerActiveCSlot;
    }
    // Pick slingshot (child) or bow (adult), whichever is in inventory.
    u8 item = ITEM_NONE;
    u8 invSlot = 0;
    if (gSaveContext.inventory.items[SLOT_SLINGSHOT] == ITEM_SLINGSHOT) {
        item = ITEM_SLINGSHOT;
        invSlot = SLOT_SLINGSHOT;
    } else if (gSaveContext.inventory.items[SLOT_BOW] == ITEM_BOW) {
        item = ITEM_BOW;
        invSlot = SLOT_BOW;
    } else {
        SPDLOG_INFO("[Follower] FollowerTryEquipRangedWeapon: no slingshot or bow in inventory");
        return 0xFF;
    }
    // Snapshot C-button loadout (indices 1..3 of buttonItems; indices 0..2
    // of cButtonSlots). Skip B-button.
    for (int i = 1; i <= 3; i++) {
        savedButtonItems[i] = gSaveContext.equips.buttonItems[i];
    }
    for (int i = 0; i < 3; i++) {
        savedCButtonSlots[i] = gSaveContext.equips.cButtonSlots[i];
    }
    // Override C-left (buttonItems index 1; cButtonSlots index 0).
    gSaveContext.equips.buttonItems[1]  = item;
    gSaveContext.equips.cButtonSlots[0] = invSlot;
    followerItemOverrideActive          = true;
    followerActiveCSlot                 = 0; // C-left
    SPDLOG_INFO("[Follower] Item override: equipped {} (invSlot={}) to C-left; "
                "saved prior C-items ({:#04x},{:#04x},{:#04x})",
                (item == ITEM_SLINGSHOT ? "slingshot" : "bow"), (int)invSlot,
                (int)savedButtonItems[1], (int)savedButtonItems[2], (int)savedButtonItems[3]);
    return 0;
}

void Anchor::FollowerRestoreItems() {
    if (!followerItemOverrideActive) { return; }
    for (int i = 1; i <= 3; i++) {
        gSaveContext.equips.buttonItems[i] = savedButtonItems[i];
    }
    for (int i = 0; i < 3; i++) {
        gSaveContext.equips.cButtonSlots[i] = savedCButtonSlots[i];
    }
    followerItemOverrideActive = false;
    followerActiveCSlot        = 0xFF;
    SPDLOG_INFO("[Follower] Item override: restored original C-button loadout");
}

void Anchor::SetFollowerActive(bool active) {
    bool changed = (followerActive != active);
    followerActive = active;
    if (active) {
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        followerStuckFrames = 0;
        followerTargetEnemy = nullptr;
        followerLeaderClientId = 0;
        followerOverrunFrames = 0;
        followerStuckCycleCount = 0;
        followerStuckCycleResetFrames = 0;
        hasPendingTransition = false;
        pendingTransitionTimeoutFrames = 0;
        followerDoorHandoff = false;
        followerDoorHandoffFrames = 0;
        followerClimbDismountFrames = 0;
        followerCloseFailBaseline = 0.0f;
        followerCloseFailFrames = 0;
        SPDLOG_INFO("[Follower] Activated (menu)");
    } else {
        hasPendingTransition = false;
        pendingTransitionTimeoutFrames = 0;
        followerDoorHandoff = false;
        followerDoorHandoffFrames = 0;
        followerClimbDismountFrames = 0;
        followerCloseFailBaseline = 0.0f;
        followerCloseFailFrames = 0;
        // Safety: always restore the player's C-button loadout on any
        // deactivation path (menu toggle, joystick cancel, scene boundary,
        // leash timeout, …). FollowerRestoreItems is a no-op when no
        // override is active.
        FollowerRestoreItems();
        // Bug 8 — defensive input cleanup. The follower hook OR-s buttons into
        // input each frame while active; if deactivation happens mid-frame
        // (after injection but before Player_Update consumes them), the
        // residual bits can trigger a stray action on Link. Clear every
        // button and stick axis the follower ever injects.
        if (gPlayState != nullptr) {
            Input& input = gPlayState->state.input[0];
            constexpr u16 kFollowerButtons = BTN_A | BTN_B | BTN_Z | BTN_R |
                                             BTN_CLEFT | BTN_CDOWN | BTN_CRIGHT;
            input.press.button &= ~kFollowerButtons;
            input.cur.button   &= ~kFollowerButtons;
            input.press.stick_x = 0;
            input.press.stick_y = 0;
            input.cur.stick_x   = 0;
            input.cur.stick_y   = 0;
        }
        SPDLOG_INFO("[Follower] Deactivated (menu)");
    }
    if (changed && isConnected) {
        SendPacket_UpdateClientState();
    }
}

// ShipInit hook — fires once at boot. Mirrors the pattern used by
// every other Anchor::Common module (NavTraits, ActorTrail,
// LeashRespawn, etc.).
static RegisterShipInitFunc registerFollowerModule(AnchorFollower::RegisterFollowerModule);
