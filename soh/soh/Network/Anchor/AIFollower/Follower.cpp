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
#include "soh/cvar_prefixes.h"
#include "../Common/ActorSyncHelpers.h"
#include "../Common/PlayerLookup.h"
#include "../Common/SceneAuthority.h"
#include "../Common/ItemEligibility.h"
#include "../Common/PauseLinkBuffer.h"
#include "../Common/ActorSyncScope.h"
#include "../WorldStateSync/WorldStateSync.h"
#include "soh/ShipInit.hpp"

#include <chrono>
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/frame_interpolation.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "z64.h"
#include "variables.h"
#include "functions.h"  // Math_Atan2S, Math_StepToS, etc.
#include "macros.h"     // BTN_*, ITEM_*, SLOT_*, GET_PLAYER, AMMO/CUR_CAPACITY/INV_CONTENT
// Decomp actor headers used by the moved follower body — bombwall / breakwall /
// shutter / door / climb / push-block / per-enemy struct branches. Mirrored
// from HookHandlers.cpp so the follower body's references resolve.
#include "src/overlays/actors/ovl_Bg_Bombwall/z_bg_bombwall.h"
#include "src/overlays/actors/ovl_Bg_Breakwall/z_bg_breakwall.h"
#include "src/overlays/actors/ovl_Bg_Haka_Zou/z_bg_haka_zou.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hamstep/z_bg_hidan_hamstep.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hrock/z_bg_hidan_hrock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Shelter/z_bg_ice_shelter.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombchuiwa/z_bg_jya_bombchuiwa.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombiwa/z_bg_jya_bombiwa.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Bwall/z_bg_mizu_bwall.h"
#include "src/overlays/actors/ovl_Bg_Spot08_Bakudankabe/z_bg_spot08_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot11_Bakudankabe/z_bg_spot11_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot17_Bakudankabe/z_bg_spot17_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Maruta/z_bg_ydan_maruta.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Si/z_en_si.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_Item_B_Heart/z_item_b_heart.h"
#include "src/overlays/actors/ovl_Obj_Bombiwa/z_obj_bombiwa.h"
#include "src/overlays/actors/ovl_Obj_Hamishi/z_obj_hamishi.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Dalm/z_bg_hidan_dalm.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Kowarerukabe/z_bg_hidan_kowarerukabe.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
#include "src/overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
#include "src/overlays/actors/ovl_En_Hintnuts/z_en_hintnuts.h"
#include "src/overlays/actors/ovl_En_St/z_en_st.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
#include "src/overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "src/overlays/actors/ovl_Obj_Oshihiki/z_obj_oshihiki.h"

extern PlayState* gPlayState;
extern MapData*   gMapData;

// Forward-decls for static decomp functions referenced by the follower
// body (mirrored from HookHandlers.cpp).
void func_8086ED70(BgBombwall* bgBombwall, PlayState* play);
void BgBreakwall_Wait(BgBreakwall* bgBreakwall, PlayState* play);
void func_80883000(BgHakaZou* bgHakaZou, PlayState* play);
void func_808887C4(BgHidanHamstep* bgHidanHamstep, PlayState* play);
void func_808896B8(BgHidanHrock* bgHidanHrock, PlayState* play);
void BgIceShelter_Idle(BgIceShelter* bgIceShelter, PlayState* play);
void BgIceShelter_SetupMelt(BgIceShelter* bgIceShelter);
void ObjBombiwa_Break(ObjBombiwa* objBombiwa, PlayState* play);
void ObjHamishi_Break(ObjHamishi* objHamishi, PlayState* play);
void BgJyaBombchuiwa_WaitForExplosion(BgJyaBombchuiwa* bgJyaBombchuiwa, PlayState* play);
void BgMizuBwall_Idle(BgMizuBwall* bgMizuBwall, PlayState* play);
void func_808B6BC0(BgSpot17Bakudankabe* bgSpot17Bakudankabe, PlayState* play);
void func_808BF078(BgYdanMaruta* bgYdanMaruta, PlayState* play);
void BgYdanSp_FloorWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_WallWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_BurnWeb(BgYdanSp* bgYdanSp, PlayState* play);
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
float OTRGetDimensionFromLeftEdge(float v);
float OTRGetDimensionFromRightEdge(float v);
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
// File-scope tunables and helpers shared between TickFollower and the
// per-state handler methods peeled off in Phase 1 commit 6+. Promoted
// from local statics inside the original TickFollower lambda. Anonymous
// namespace gives internal linkage so symbols don't leak to other TUs.
// Values must stay in sync with their counterparts elsewhere — the
// tunables-cleanup commit folds the in-TickFollower locals away once
// every state has been peeled into its own helper.
// ---------------------------------------------------------------------------

namespace {

// Yaw toward (dx, dz). Math_Atan2S(z, x) per OoT convention. Was a local
// lambda inside TickFollower; promoted to file scope so per-state handlers
// (HandleStateBlock, future HandleStateAttack/Engage/etc.) can call it
// without re-declaring or capturing.
inline s16 YawToward(f32 dx, f32 dz) {
    return Math_Atan2S(dz, dx); // z first, x second — OoT convention
}

// Frames per ATTACK / BLOCK / STANDBY cycle. Was `static constexpr int
// kAttackDuration = 60` inside TickFollower; promoted so per-state
// handlers reference the same value.
static constexpr int kAttackDuration = 60;

} // anonymous namespace

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

// ---------------------------------------------------------------------------
// Anchor::TickFollower — per-frame follower state-machine body. Moved verbatim
// from HookHandlers.cpp's OnGameFrameUpdate lambda body. Phase 1 commit 4 of
// the SRP refactor (#173 / #169). The body uses unqualified member access for
// Anchor:: state (followerActive, clients, followerAIState, etc.); these
// resolve through 'this' since this is an Anchor:: method, just as they
// resolved through the lambda's 'this' capture before the move.
//
// Indentation preserved at the lambda's original 16-space depth so the diff
// against pre-commit-4 HookHandlers.cpp is line-aligned. A future cleanup
// commit can dedent to standard 4-space function-body depth.
// ---------------------------------------------------------------------------
void Anchor::TickFollower(AnchorFollower::FollowerFrameContext& ctx) {
    Player* player = ctx.player;
    (void)ctx;  // ctx populated minimally in this commit; future commits expand


                // Any real input cancels follower mode.
                // During ATTACK the animation hook injects BTN_B into press.button —
                // exclude it so our own injection doesn't cancel follower mode.
                //
                // INVARIANT — every `input.press.button |= X` site in this file MUST
                // have a matching mask entry below for the state(s) that inject X.
                // If a new injection is added without its mask, the follower will
                // self-cancel on the frame it fires. Symptom: log shows
                // `Deactivated (input pressed=0xNNNN state=...)` with the NNNN bit
                // matching the newly injected button and the state being one that
                // just started injecting it (Test 4 log 70 caught this for BTN_Z
                // in ENGAGE/ATTACK when Bug D added the lock-on hold without the
                // mask; fixed by the block below).
                //
                // Current mask table:
                //   ENGAGE/ATTACK         → BTN_Z   (lock-on tap, Bug D / Test 6)
                //   ATTACK                → BTN_A | BTN_B | BTN_R (jump/swing/shield)
                //   BLOCK                 → BTN_R   (shield plant)
                //   RANGED_ATTACK         → BTN_Z | C-slot (BTN_A fire removed Test 6)
                //   GETTING_ITEM/TALKING  → BTN_A   (text-box dismiss)
                //   DO_ACTION_CLIMB/ENTER → BTN_A   (ladder + crawlspace)
                //   doorType != NONE/FAKE → BTN_A   (door auto-press, Test 7)
                if (followerActive) {
                    u16 pressed = gPlayState->state.input[0].press.button;
                    u16 deactivateCheck = pressed;
                    // Mask off buttons WE inject — otherwise our own input would
                    // cancel follower mode the frame after we inject it.
                    if (followerAIState == FollowerAIState::ATTACK) {
                        // Swing frames inject BTN_B (normal) or BTN_A (jump-
                        // attack when locked + too far for regular swing,
                        // Test 6 fix). Non-swing frames inject BTN_R for the
                        // between-swings shield (Test 5 fix).
                        deactivateCheck &= ~(BTN_A | BTN_B | BTN_R);
                    }
                    if (followerAIState == FollowerAIState::BLOCK) {
                        deactivateCheck &= ~BTN_R;
                    }
                    // Bug D — BTN_Z lock-on edge-pressed on ENGAGE entry (and
                    // held via cur through ATTACK). Mask from cancel-check
                    // so the ENGAGE entry frame doesn't self-cancel.
                    if (followerAIState == FollowerAIState::ENGAGE ||
                        followerAIState == FollowerAIState::ATTACK) {
                        deactivateCheck &= ~BTN_Z;
                    }
                    // Item pickup — while OoT is showing an item-get text box
                    // (PLAYER_STATE1_GETTING_ITEM or PLAYER_STATE1_TALKING),
                    // we inject BTN_A every 20 frames to dismiss. Mask so
                    // our own press doesn't self-cancel follower mode.
                    if (player != nullptr &&
                        (player->stateFlags1 &
                         (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_TALKING))) {
                        deactivateCheck &= ~BTN_A;
                    }
                    if (followerAIState == FollowerAIState::RANGED_ATTACK) {
                        deactivateCheck &= ~(BTN_Z | BTN_A);
                        // If we also injected a C-button for item draw
                        // (Option B), mask that too so our own press doesn't
                        // cancel follower mode.
                        switch (followerActiveCSlot) {
                            case 0: deactivateCheck &= ~BTN_CLEFT;  break;
                            case 1: deactivateCheck &= ~BTN_CDOWN;  break;
                            case 2: deactivateCheck &= ~BTN_CRIGHT; break;
                            default: break;
                        }
                    }
                    // DO_ACTION_CLIMB triggers BTN_A injection regardless of state
                    // (ledge-hang and water-exit climb-out). DO_ACTION_ENTER
                    // covers crawlspaces. doorType (!= NONE) covers doors
                    // (En_Door / Door_Shutter / etc.) — Phase A injects
                    // BTN_A for all three. Mask BTN_A in any of these
                    // cases so our own injection doesn't cancel follower.
                    //
                    // Test 10 (log 79, Bug 1) — `followerDoorPressCooldown`
                    // covers the mid-frame race: Phase A injects this frame,
                    // Player_Update consumes + clears doorType same frame,
                    // deactivate-check then reads press.button with no
                    // matching mask condition. Counter armed on injection,
                    // decremented below in the post-check tick.
                    if (player != nullptr &&
                        (followerDoorPressCooldown > 0 ||
                         (player->stateFlags2 &
                          (PLAYER_STATE2_DO_ACTION_CLIMB | PLAYER_STATE2_DO_ACTION_ENTER)) ||
                         (player->doorType != PLAYER_DOORTYPE_NONE &&
                          player->doorType != PLAYER_DOORTYPE_FAKE))) {
                        deactivateCheck &= ~BTN_A;
                    }
                    // Nav system Shape A hang-state resolution (commit 6c).
                    // The hang-state injection block in ShouldActorUpdate
                    // injects BTN_A or BTN_B based on Δy to leader; mask
                    // both from the deactivate check so our own press
                    // doesn't self-cancel follower mode while resolving
                    // the hang.
                    if (player != nullptr &&
                        (player->stateFlags1 & PLAYER_STATE1_HANGING_OFF_LEDGE) &&
                        CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0 &&
                        CVarGetInteger(CVAR_ENHANCEMENT("Nav.VerticalTeleport"), 0) != 0) {
                        deactivateCheck &= ~(BTN_A | BTN_B);
                    }
                    if (deactivateCheck != 0) {
                        // Include state name + the UNMASKED residue so future
                        // self-cancel regressions are easy to diagnose: any
                        // bit in `check` is a button we either didn't mask or
                        // user genuinely pressed. If the bit matches a known
                        // injection (BTN_Z, BTN_A, BTN_B, BTN_R, C-slot) and
                        // the state should be injecting that button, the mask
                        // table above is missing an entry for that state.
                        const char* stateStr = "?";
                        switch (followerAIState) {
                            case FollowerAIState::IDLE:          stateStr = "IDLE";          break;
                            case FollowerAIState::FOLLOW:        stateStr = "FOLLOW";        break;
                            case FollowerAIState::STUCK:         stateStr = "STUCK";         break;
                            case FollowerAIState::ENGAGE:        stateStr = "ENGAGE";        break;
                            case FollowerAIState::ATTACK:        stateStr = "ATTACK";        break;
                            case FollowerAIState::RETURN:        stateStr = "RETURN";        break;
                            case FollowerAIState::CLIMBING:      stateStr = "CLIMBING";      break;
                            case FollowerAIState::BLOCK:         stateStr = "BLOCK";         break;
                            case FollowerAIState::RANGED_ATTACK: stateStr = "RANGED_ATTACK"; break;
                            case FollowerAIState::STANDBY:       stateStr = "STANDBY";       break;
                            case FollowerAIState::COLLECT_ITEM:  stateStr = "COLLECT_ITEM";  break;
                        }
                        SetFollowerActive(false);
                        SPDLOG_INFO("[Follower] Deactivated (input pressed=0x{:04X} check=0x{:04X} state={})",
                                    pressed, deactivateCheck, stateStr);
                        return;
                    }
                }

                if (!followerActive) { return; }

                // Monotonic per-Anchor tick counter. Advances once per
                // follower-active OnGameFrameUpdate tick. Used for
                // grace-period tracking in the item-pickup scan — must
                // not be followerStateFrames (which resets on state
                // change).
                followerTickCounter++;

                // Test 10 (log 79, Bug 1) — tick down door-press BTN_A
                // cooldown. Mask reads `> 0` each frame; decrement here
                // AFTER the mask has had its chance to strip BTN_A.
                if (followerDoorPressCooldown > 0) {
                    followerDoorPressCooldown--;
                }

                // G18 — full cutscene suspension. csCtx.state == CS_STATE_IDLE means
                // no cutscene; anything else is an active CS frame and we must not
                // touch the player's state machine. Stick suppression alone (in
                // ShouldActorUpdate) is not enough — running the state machine here
                // can still write shape.rot.y or trigger state transitions that
                // collide with cutscene scripts.
                if (gPlayState->csCtx.state != CS_STATE_IDLE) {
                    return;
                }

                // G12 — tick the stuck-cycle reset window. When the window expires,
                // the cycle counter clears so isolated STUCK events don't accumulate
                // across long sessions. Counter is incremented at FOLLOW→STUCK below.
                if (followerStuckCycleResetFrames > 0) {
                    followerStuckCycleResetFrames--;
                    if (followerStuckCycleResetFrames == 0) {
                        followerStuckCycleCount = 0;
                    }
                }

                // --- AI follower state machine ---
                // Test 10 (log 79, Bug 2 mitigation) — tightened from 50 u
                // to 25 u. User had reserved this as a Test 8 fallback.
                // Larger offsets put the follower's sideTarget adjacent to
                // holes/ledges the leader was walking beside (e.g. Deku
                // Tree Mad Scrub hole) — follower walks to sideTarget and
                // falls in. 25 u keeps the follower visibly offset without
                // straying as far into hazardous geometry.
                static constexpr f32 kFollowOffset       = 25.0f;  // world +X from leader
                static constexpr f32 kFollowThreshold    = 100.0f; // dist to switch FOLLOW↔IDLE
                static constexpr f32 kEngageRange        = 350.0f; // enemy detection radius (XZ)
                static constexpr f32 kAttackRange        = 80.0f;  // melee-contact radius (XZ)
                static constexpr f32 kMaxYDelta          = 120.0f; // reject enemies on a different floor
                static constexpr f32 kMaxLeash           = 800.0f; // abandon ENGAGE if leader this far
                static constexpr f32 kMoveSpeed          = 4.0f;   // units/frame for STUCK fallback nudge only
                static constexpr int kStuckCheckInterval = 20;     // frames between stuck checks
                static constexpr f32 kStuckMinProgress   = 5.0f;   // min units per check interval
                static constexpr int kStuckRecovery      = 25;     // frames of strafe before retry
                // kAttackDuration moved to file-scope anonymous namespace
                // (Phase 1 commit 6) so per-state handlers can reference it.
                // G10 — leash-timeout teleport thresholds.
                static constexpr f32 kTeleportThreshold   = 1200.0f; // sustained XZ overrun that triggers teleport
                static constexpr int kTeleportDelayFrames = 120;     // ~2s at 60fps; debounces brief overshoots
                // G12 — STUCK escalation: N STUCK entries within window → teleport.
                static constexpr int kStuckCycleEscalation = 3;     // count threshold
                static constexpr int kStuckCycleWindow     = 300;   // frames; resets count if exceeded
                // Phase B (Bug 7) — door handoff timeout. After leader crosses a
                // room boundary, the follower has this many frames to navigate
                // to the door / cross the threshold itself. On timeout, teleport.
                static constexpr int kDoorHandoffTimeout   = 360;   // ~6 s at 60fps
                // Bug C (log 69) — dismount forward-hold. After CLIMBING→IDLE,
                // hold stick forward at the climb-exit yaw for this many frames
                // so Link walks inward past the rim before other state machine
                // logic can re-point him backward toward a leader standing at
                // the edge. Tuning history:
                //   Test 5 (log 71): 60 (1 s) — too long, follower overshot.
                //   Test 6 (log 74): 15 (0.25 s) — better but still overshoot.
                //   Test 9 (log 78): 9 (0.15 s) — current, per user feedback.
                // Covers both ladder dismount and vine top-rim climb-over
                // (HANGING_OFF_LEDGE / CLIMBING_LEDGE clears fire the same
                // CLIMBING→IDLE arm path).
                static constexpr int kClimbDismountHoldFrames = 9;
                // Item pickup (Claude/Plans/ai_follower_item_pickup.md).
                // kItemProximity — XZ radius of the ACTORCAT_MISC scan.
                //     User-specified 200 units: far enough to catch most
                //     enemy-drop distances, short enough not to distract.
                // kItemGraceFrames — human-first-pick window. A drop isn't
                //     eligible until it has been observed for this many
                //     ticks; lets the leader grab it if they want to.
                // kItemCollectTimeout — walking timeout inside COLLECT_ITEM.
                //     Walking from kEngageRange (350) → item at ~4 u/frame
                //     is < 90 frames; 300 gives plenty of slack for
                //     collision mishaps. Drops back to RETURN on expiry.
                static constexpr f32 kItemProximity      = 200.0f;
                static constexpr int kItemGraceFrames    = 180;
                static constexpr int kItemCollectTimeout = 300;
                // G13 — boss scenes that warrant pre-emptive teleport on leader entry.
                // Only Deku Tree boss is in scope for the first dungeon demo (#167);
                // extend this list as later dungeons land.
                static constexpr s16 kBossScenes[] = { /* SCENE_DEKU_TREE_BOSS */ 0x11 };
                auto IsBossScene = [&](s16 scene) -> bool {
                    for (s16 s : kBossScenes) { if (s == scene) return true; }
                    return false;
                };
                // G4 — enemies that require shield-reflect to defeat. ENGAGE routes
                // to BLOCK instead of ATTACK when the target is one of these.
                static constexpr s16 kShieldReflectEnemyIds[] = { ACTOR_EN_DEKUNUTS };
                auto IsShieldReflectEnemy = [&](s16 id) -> bool {
                    for (s16 e : kShieldReflectEnemyIds) { if (e == id) return true; }
                    return false;
                };
                // G6/G7/G8 — enemies that require ranged attack (slingshot/bow).
                // ENGAGE routes to RANGED_ATTACK when the target is one of these
                // AND the target is above Link's sword vertical reach (see Fix 2,
                // 2026-04-22). Previously gated on |Δy| >= kMaxYDelta=120, which
                // was far too loose — Link's sword vertical reach is ~30 units,
                // so a Skullwalltula at Δy=118 still slipped through into ATTACK
                // and the follower swung at empty air for 60 frames (P2 log 67,
                // 15:21:03).
                static constexpr f32 kSwordVerticalReach = 40.0f;
                static constexpr s16 kRangedRequiredEnemyIds[] = {
                    ACTOR_BOSS_GOMA, // Queen Gohma — ceiling phase
                    ACTOR_EN_GOMA,   // Gohma larvae on the ceiling
                    ACTOR_EN_SW,     // Skullwalltula on a wall vine
                    ACTOR_EN_ST,     // Skulltula hanging from ceiling on its thread (Fix 2)
                };
                auto IsRangedRequiredEnemy = [&](s16 id) -> bool {
                    for (s16 e : kRangedRequiredEnemyIds) { if (e == id) return true; }
                    return false;
                };

                // Bug D (combat upgrade) — per-enemy approach distance.
                // Default kAttackRange (80) stops Link at sword-tip contact,
                // which is fine for Stalfos-class melee but walks the
                // follower straight into the lunge arc of enemies whose
                // damage volume sits ahead of world.pos (Karebaba head,
                // Deku Baba stem-tip, Bari body-AoE). Override per actor id.
                // The override is used BOTH for ENGAGE→ATTACK admission and
                // for the point-blank shield trigger (see SwingReach below).
                auto GetAttackRangeForEnemy = [](s16 id) -> f32 {
                    // Test 5 (log 71) tuning: user reported "10 units closer"
                    // for Karebaba + Dekubaba — swings were reaching but
                    // sword often whiffed because standoff put Link just
                    // outside arc. Karebaba 110→100, Dekubaba 100→90.
                    switch (id) {
                        case ACTOR_EN_KAREBABA: return 100.0f; // head lunges ~40 u
                        case ACTOR_EN_DEKUBABA: return  90.0f; // stem-tip head
                        case ACTOR_EN_VALI:     return 120.0f; // body AoE discharge
                        default:                return  80.0f; // kAttackRange
                    }
                };
                // Sword arc reach — Link's effective swing distance. Inside
                // this radius we switch to shield-up-between-swings; outside
                // it, the follower walks forward during the swing-cycle gap.
                static constexpr f32 kSwingReach = 50.0f;

                // Item pickup — need-gated whitelist via shared helper
                // (#193 Phase 0). Reserved for the human leader: progression
                // items, shields, tunics, keys, heart pieces — all return
                // false. AI follower keeps the legacy "rupees always" rule
                // (`walletCapAware = false`) since vanilla truncates surplus
                // and the follower acts in the local player's stead.
                auto FollowerWantsItem = [](Actor* item) -> bool {
                    if (item == nullptr || item->id != ACTOR_EN_ITEM00 ||
                        item->update == nullptr) {
                        return false;
                    }
                    s16 itemType = (s16)(item->params & 0xFF);
                    return ItemEligibility::CanPlayerCollectItem00(
                        itemType, /*walletCapAware=*/false);
                };

                // Item pickup — scan ACTORCAT_MISC for eligible En_Item00
                // drops. Maintains itemFirstSeenFrame (grace-period tracker)
                // and returns the nearest eligible in-range item whose
                // grace window has elapsed. Called once per tick from the
                // IDLE/FOLLOW state bodies. Pointer-reuse is handled by
                // purging entries whose key is no longer in the current
                // MISC list.
                auto ScanForItemCandidate = [&]() -> Actor* {
                    // Pass 1: collect current live EN_ITEM00 pointers.
                    std::unordered_set<Actor*> liveItems;
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
                    while (cand != nullptr) {
                        if (cand->id == ACTOR_EN_ITEM00 && cand->update != nullptr) {
                            liveItems.insert(cand);
                        }
                        cand = cand->next;
                    }
                    // Pass 2: purge itemFirstSeenFrame entries whose key is
                    // no longer in the MISC list (item was collected / unloaded).
                    for (auto it = itemFirstSeenFrame.begin();
                         it != itemFirstSeenFrame.end();) {
                        if (liveItems.find(it->first) == liveItems.end()) {
                            it = itemFirstSeenFrame.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    // Pass 3: register newly-seen items + evaluate eligibility.
                    Vec3f selfPos = player->actor.world.pos;
                    Actor* bestItem  = nullptr;
                    f32    bestDistSq = kItemProximity * kItemProximity;
                    for (Actor* item : liveItems) {
                        auto firstIt = itemFirstSeenFrame.find(item);
                        if (firstIt == itemFirstSeenFrame.end()) {
                            itemFirstSeenFrame[item] = followerTickCounter; // arm grace
                            continue;
                        }
                        // Grace check first — cheap int compare before physics math.
                        if (followerTickCounter - firstIt->second < kItemGraceFrames) {
                            continue;
                        }
                        // Test 5 diagnostics — log item type at the first
                        // post-grace scan for each actor, sparse per type.
                        // Lets us see why sticks/seeds/etc. don't engage.
                        bool wants = FollowerWantsItem(item);
                        if (followerTickCounter - firstIt->second == kItemGraceFrames) {
                            s16 itemType = (s16)(item->params & 0xFF);
                            SPDLOG_INFO("[Follower] item grace expired ptr=0x{:x} type=0x{:02X} "
                                        "wants={} y-delta={:.0f}",
                                        (uintptr_t)item, (int)itemType, wants ? 1 : 0,
                                        item->world.pos.y - selfPos.y);
                        }
                        if (!wants) {
                            continue;
                        }
                        // Same-floor gate (mirrors enemy-target Y gate).
                        if (fabsf(item->world.pos.y - selfPos.y) >= kMaxYDelta) {
                            continue;
                        }
                        // Room-equality check disabled: player->actor.room is
                        // stale across room transitions. See earlier banner.
                        f32 dx = item->world.pos.x - selfPos.x;
                        f32 dz = item->world.pos.z - selfPos.z;
                        f32 d2 = dx * dx + dz * dz;
                        if (d2 < bestDistSq) {
                            bestDistSq = d2;
                            bestItem   = item;
                        }
                    }
                    return bestItem;
                };

                // -----------------------------------------------------------------
                // Room-equality check — DISABLED 2026-04-21.
                //
                // What it did (four sites: IsEligibleLeader, IDLE enemy scan,
                // ENGAGE off-floor/room guard, ATTACK off-floor/room guard):
                // reject any candidate whose actor->room did not match the local
                // player's actor->room. Added originally alongside kMaxYDelta to
                // keep the follower from targeting enemies in a different logical
                // room — e.g. an enemy in the pit beneath the Great Deku Tree
                // entrance, where XZ distance is short but they are physically
                // unreachable.
                //
                // Why it broke combat:
                // OoT's Actor_Spawn (z_actor.c:3394) assigns actor->room =
                // roomCtx.curRoom.num AT SPAWN TIME and never updates it. The
                // Player actor is spawned once per scene and persists across
                // TransitionActor room changes — nothing in the decomp writes
                // to player->actor.room after the initial spawn (verified by
                // searching soh/src for any such assignment: zero hits). So
                // in any multi-room scene (Hyrule Field quadrants, most
                // dungeons past room 0), the Player's room number is stale
                // the moment the player walks through the first transition,
                // and every enemy spawned in a subsequent room fails the
                // equality test. Observed regression: Hyrule Field with 5
                // Karebabas within 80-unit attack range, zero IDLE→ENGAGE
                // events (P2 log 52, 2026-04-21).
                //
                // The kMaxYDelta gate alone handles the original floor-below
                // bug that motivated this check — OoT floor-to-floor vertical
                // separation is always ≫ 120 units in practice.
                //
                // When it would be useful again: single-floor scenes where
                // two rooms are physically adjacent at the same Y level and
                // could be mistakenly targeted through a thin wall within
                // the 350-unit engage range. If such a case surfaces, the
                // correct fix is to compare against a live room source,
                // NOT player->actor.room. Candidates:
                //   - gPlayState->roomCtx.curRoom.num (authoritative current
                //     room number; accept actor->room == -1 as well since
                //     that is the documented "persistent across rooms"
                //     sentinel — see z64actor.h:215).
                //   - A SoH-side room tracker updated from a TransitionActor
                //     hook, stored on the Anchor instance.
                // With either, the four sites below should read e.g.:
                //   s8 curRoom = (s8)gPlayState->roomCtx.curRoom.num;
                //   bool roomOk = (cand->room == curRoom || cand->room == -1);
                // Until then, the lines are commented out rather than
                // deleted so the intent and re-enable path stay discoverable.
                // -----------------------------------------------------------------

                // Movement is driven by stick input injected in ShouldActorUpdate
                // (mirrors how BTN_B drives sword swings). Link's own Player_Update
                // then handles locomotion, wall collisions, ledge-climb, swim,
                // cutscene suspension, etc. The state machine here only computes
                // `followerMoveTarget` — the world-space point ShouldActorUpdate
                // steers toward — and never writes to player->actor.world.pos
                // except in the STUCK fallback (see that case below for rationale).

                // --- Pick a leader DummyPlayer ---
                // Prefer the previously chosen leader (stickiness) if it is still
                // eligible; otherwise scan the DummyPlayer list for the nearest
                // eligible one. Eligibility: same room as the follower, within the
                // vertical gate, not parked out-of-scene at (-9999,-9999,-9999),
                // and the remote client is not itself in follower mode.
                auto IsEligibleLeader = [&](Actor* cand) -> bool {
                    if (cand == nullptr || cand->update != (ActorFunc)DummyPlayer_Update) {
                        return false;
                    }
                    if (cand->id != ACTOR_EN_OE2) { return false; }
                    if (cand->world.pos.x < -9000.0f) { return false; } // out-of-scene sentinel
                    // Room-equality check DISABLED — see banner note above the state machine.
                    // if (cand->room != player->actor.room) { return false; }
                    uint32_t cid = GetDummyPlayerClientId(cand);
                    if (cid == 0) { return false; }
                    auto it = clients.find(cid);
                    if (it != clients.end() && it->second.followerActive) {
                        return false; // don't follow another follower
                    }
                    // Bug 6 (2026-04-22) — Y-eligibility check REMOVED.
                    // Previously gated on |Δy| < kMaxYDelta with a Fix 1
                    // carve-out for isClimbing leaders. The carve-out was
                    // necessary because tall ladders/vines lift the leader
                    // out of the band within ~1 s, dropping the leader and
                    // stranding the follower (log 67). Removing the gate
                    // entirely is cleaner: the follower's stick-driven
                    // navigation will hit walls / floors / ceilings naturally
                    // (Link's collisions stop him), and G10 (now 3D-distance)
                    // / G12 (stuck-cycle) catch the unreachable case.
                    // kMaxYDelta still gates ENGAGE/IDLE enemy targeting.
                    return true;
                };

                Actor* leaderActor = nullptr;
                if (followerLeaderClientId != 0) {
                    // Sticky path: re-find last leader's actor and check eligibility.
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (cand != nullptr) {
                        if (cand->id == ACTOR_EN_OE2 &&
                            cand->update == (ActorFunc)DummyPlayer_Update &&
                            GetDummyPlayerClientId(cand) == followerLeaderClientId) {
                            if (IsEligibleLeader(cand)) { leaderActor = cand; }
                            break;
                        }
                        cand = cand->next;
                    }
                    if (leaderActor == nullptr) {
                        followerLeaderClientId = 0; // release stickiness, re-scan below
                    }
                }
                if (leaderActor == nullptr) {
                    // Scan for nearest eligible DummyPlayer (any client, not just host).
                    Actor* nearestLeader  = nullptr;
                    f32    nearestDistSq  = 1.0e18f; // effectively unbounded for XZ world distances
                    Vec3f  selfPos        = player->actor.world.pos;
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (cand != nullptr) {
                        if (IsEligibleLeader(cand)) {
                            f32 dx = cand->world.pos.x - selfPos.x;
                            f32 dz = cand->world.pos.z - selfPos.z;
                            f32 d2 = dx * dx + dz * dz;
                            if (d2 < nearestDistSq) {
                                nearestDistSq = d2;
                                nearestLeader = cand;
                            }
                        }
                        cand = cand->next;
                    }
                    if (nearestLeader != nullptr) {
                        leaderActor = nearestLeader;
                        followerLeaderClientId = GetDummyPlayerClientId(nearestLeader);
                        SPDLOG_INFO("[Follower] Leader selected clientId={} pos=({:.0f},{:.0f},{:.0f})",
                                    followerLeaderClientId,
                                    nearestLeader->world.pos.x,
                                    nearestLeader->world.pos.y,
                                    nearestLeader->world.pos.z);
                    }
                }
                // No eligible leader — stay in IDLE and wait. Do not cancel
                // follower mode; the user may be the only active player.
                if (leaderActor == nullptr) {
                    if (followerAIState != FollowerAIState::IDLE) {
                        followerAIState     = FollowerAIState::IDLE;
                        followerStateFrames = 0;
                        SPDLOG_INFO("[Follower] No eligible leader — reverting to IDLE");
                    }
                    return;
                }

                Actor* dummyActor = leaderActor;                          // preserved name for downstream reads
                Vec3f  leaderPos  = leaderActor->world.pos;
                // Test 5 (log 71) — crawlspace fix. When leader is crawling,
                // sideTarget (+kFollowOffset on X) lands the follower beside
                // the crawlspace hole rather than on its centerline, so
                // DO_ACTION_ENTER never fires for the follower and Phase A's
                // BTN_A injection has nothing to press. Use leaderPos
                // directly to stay on-axis.
                bool leaderCrawling = false;
                {
                    auto it = clients.find(followerLeaderClientId);
                    if (it != clients.end()) { leaderCrawling = it->second.isCrawling; }
                }
                Vec3f  sideTarget = leaderCrawling
                    ? leaderPos
                    : Vec3f{ leaderPos.x + kFollowOffset, leaderPos.y, leaderPos.z };

                // YawToward moved to file-scope anonymous namespace
                // (Phase 1 commit 6) so per-state handlers can call it.

                // Bug B (log 69) — cross-room teleport helper. Plain
                // world.pos=leaderPos teleports do not update OoT's
                // roomCtx.curRoom.num, so teleporting into a different room
                // leaves the game's collision/actor context stuck in the
                // old room. Every subsequent frame G11 re-detects the
                // divergence and re-arms the handoff, producing the
                // infinite-loop symptom from log 69.
                //
                // This helper decides:
                //   (a) Same room or no leader clients entry — plain
                //       world.pos write suffices (no room transition needed).
                //   (b) Different room / scene — drive OoT through its
                //       respawn pipeline (RESPAWN_MODE_DOWN + respawnFlag=1 +
                //       same-scene TRANS_TRIGGER_START). func_8009728C reads
                //       roomIndex from respawn[respawnFlag-1], and
                //       Player_Init copies pos/yaw. Well-exercised engine
                //       path (void-out / Farore's Wind). Handles Deku Tree
                //       basement / Mad Scrub / other non-entrance-accessible
                //       rooms that a raw entrance-index reload cannot reach.
                //
                // Returns true if a scene transition was triggered (caller
                // should return from the follower hook since OoT owns the
                // next frames).
                // Test 5 (log 71) — centralise post-teleport resets so every
                // caller (G10 / G11 / G12) gets the same behaviour. Without
                // this, different call sites reset different counters (G10
                // didn't clear stuck-cycle, G11 didn't clear overrun), which
                // masked the Kokiri Forest 26-teleport loop until the hold
                // counter broke the cycle.
                static constexpr int kPostTeleportHoldFrames = 30;
                auto TeleportToLeader = [&](const char* reason) -> bool {
                    Vec3f destPos = leaderPos;
                    s16   destYaw = leaderActor->shape.rot.y;
                    auto  it      = clients.find(followerLeaderClientId);
                    s8    ourRoom    = (s8)gPlayState->roomCtx.curRoom.num;
                    s8    leaderRoom = (it != clients.end()) ? it->second.curRoomNum : ourRoom;
                    bool  roomsDiffer = (leaderRoom != -1 && ourRoom != -1 && leaderRoom != ourRoom);
                    // Nav system Shape A hang-state guard (commit 6c).
                    // Suppress same-room world.pos writes while Link is in
                    // PLAYER_STATE1_HANGING_OFF_LEDGE — writing under that
                    // state produces the "slide while hanging" residual
                    // (#169 hang-on-ledge has-no-exit-logic). The hang-
                    // state resolution below injects BTN_A or BTN_B
                    // instead so OoT's own state machine resolves the
                    // hang in the right direction. Cross-room teleport
                    // stays unaffected — it goes through RESPAWN_MODE_TOP
                    // / scene-reload, not a direct world.pos write.
                    if (!roomsDiffer && (player->stateFlags1 & PLAYER_STATE1_HANGING_OFF_LEDGE) &&
                        CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0 &&
                        CVarGetInteger(CVAR_ENHANCEMENT("Nav.VerticalTeleport"), 0) != 0) {
                        SPDLOG_INFO("[Follower] Teleport SUPPRESSED ({}) — hang-state guard "
                                    "(stateFlags1 & HANGING_OFF_LEDGE)",
                                    reason);
                        return false;
                    }
                    // Reset ALL the "how long have I been unable to reach
                    // the leader" counters on every teleport. Also arm the
                    // post-teleport hold so ShouldActorUpdate zeroes stick
                    // for a short window — prevents immediate sideTarget
                    // walk-into-wall (Test 5 "stuck in wall" symptom).
                    followerOverrunFrames         = 0;
                    followerStuckFrames           = 0;
                    followerStuckCycleCount       = 0;
                    followerStuckCycleResetFrames = 0;
                    followerPostTeleportFrames    = kPostTeleportHoldFrames;
                    followerCloseFailBaseline     = 0.0f;
                    followerCloseFailFrames       = 0;
                    if (!roomsDiffer) {
                        player->actor.world.pos = destPos;
                        player->actor.prevPos   = destPos;
                        SPDLOG_INFO("[Follower] Teleport world.pos ({}) — same room {} pos={:.0f},{:.0f},{:.0f} "
                                    "(hold {} frames)",
                                    reason, (int)ourRoom, destPos.x, destPos.y, destPos.z,
                                    kPostTeleportHoldFrames);
                        return false;
                    }
                    SPDLOG_WARN("[Follower] Teleport scene-reload ({}) — ours-room={} leader-room={} "
                                "pos={:.0f},{:.0f},{:.0f}",
                                reason, (int)ourRoom, (int)leaderRoom,
                                destPos.x, destPos.y, destPos.z);
                    // Test 5 (log 71) — switched from RESPAWN_MODE_DOWN to
                    // RESPAWN_MODE_TOP. DOWN is the void-out pipeline;
                    // z_player.c:10853 inflicts void damage via
                    // `GameInteractor_Should(VB_INFLICT_VOID_DAMAGE, ...)`
                    // when `respawnFlag == 1 || respawnFlag == -1` —
                    // which matched our DOWN+1 flag. User reported P2 took
                    // damage and died on the second teleport. TOP is the
                    // Farore's Wind pipeline; it reads the same pos/yaw/
                    // roomIndex fields but isn't in the void-damage
                    // predicate, so teleport no longer damages Link.
                    //
                    // Play_SetRespawnData is static to z_play.c; inline the
                    // struct writes rather than plumb a forward declaration.
                    RespawnData* rd = &gSaveContext.respawn[RESPAWN_MODE_TOP];
                    rd->entranceIndex    = gSaveContext.entranceIndex;
                    rd->roomIndex        = (s16)leaderRoom;
                    rd->pos              = destPos;
                    rd->yaw              = destYaw;
                    rd->playerParams     = 0x0DFF;  // normal-spawn player-params
                    rd->tempSwchFlags    = gPlayState->actorCtx.flags.tempSwch;
                    rd->tempCollectFlags = gPlayState->actorCtx.flags.tempCollect;
                    gSaveContext.respawnFlag        = 3; // RESPAWN_MODE_TOP + 1
                    gPlayState->transitionTrigger   = TRANS_TRIGGER_START;
                    gPlayState->nextEntranceIndex   = gSaveContext.entranceIndex;
                    gPlayState->transitionType      = TRANS_TYPE_FADE_BLACK;
                    return true;
                };

                // p2Pos is a READ-ONLY snapshot of the follower's current position,
                // taken at the top of the state-machine block for distance/transition
                // checks. Under stick-input movement, the state machine no longer
                // writes p2Pos back to player->actor.world.pos — Link's own
                // Player_Update moves him in response to the stick injected in
                // ShouldActorUpdate. The only path that now writes to
                // player->actor.world.pos is the STUCK fallback (see that case).
                Vec3f p2Pos = player->actor.world.pos;

                // -----------------------------------------------------------------
                // Top-of-hook safety nets (Batch A — G10, G11, G12 escalation, G13).
                //
                // Run BEFORE the state machine so they apply uniformly regardless
                // of which state the follower is in. Each writes player->actor.world.pos
                // directly under specific failure conditions — these are bounded
                // exceptions to the "STUCK is the only world.pos writer" rule
                // documented in the state machine block below.
                // -----------------------------------------------------------------

                // Phase C — pending SCENE_TRANSITION_HANDOFF replay.
                // Runs BEFORE G11 so the follower doesn't get deactivated while
                // navigating toward the trigger point. Three outcomes:
                //   (a) our sceneNum has already changed to (or past) the
                //       leader's — packet is stale; clear and fall through.
                //   (b) still in the from-scene AND within proximity of the
                //       trigger — fire our own transition (set
                //       nextEntranceIndex + transitionTrigger) and clear.
                //   (c) still in the from-scene but too far from the trigger —
                //       point followerMoveTarget at triggerPos so the state
                //       machine walks us there. Decrement timeout.
                bool pendingTransitionInFlight = false;
                if (hasPendingTransition) {
                    s16 ourScene = (s16)gPlayState->sceneNum;
                    if (ourScene != pendingTransitionFromScene) {
                        // We moved on without using the handoff (user walked
                        // manually, or we already fired the transition last
                        // frame). Drop it.
                        SPDLOG_INFO("[Follower] Pending transition cleared — scene already changed "
                                    "(ours=0x{:02X} packet.fromScene=0x{:02X})",
                                    (int)ourScene, (int)pendingTransitionFromScene);
                        hasPendingTransition           = false;
                        pendingTransitionTimeoutFrames = 0;
                    } else {
                        static constexpr f32 kHandoffProximity = 60.0f;
                        f32 dx = pendingTransitionPos.x - p2Pos.x;
                        f32 dz = pendingTransitionPos.z - p2Pos.z;
                        f32 d2 = dx * dx + dz * dz;
                        if (d2 < kHandoffProximity * kHandoffProximity) {
                            SPDLOG_INFO("[Follower] Pending transition firing — entering scene via "
                                        "entrance 0x{:04X} (from scene 0x{:02X})",
                                        (int)(u16)pendingTransitionEntrance,
                                        (int)pendingTransitionFromScene);
                            gPlayState->nextEntranceIndex = pendingTransitionEntrance;
                            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
                            hasPendingTransition           = false;
                            pendingTransitionTimeoutFrames = 0;
                            return; // scene load owns the next frames
                        } else {
                            // Navigate to the trigger. Force the state machine
                            // to walk toward the door/trigger point by routing
                            // through FOLLOW with an overridden move target.
                            pendingTransitionInFlight = true;
                            followerMoveTarget = pendingTransitionPos;
                            if (followerAIState == FollowerAIState::IDLE) {
                                followerAIState     = FollowerAIState::FOLLOW;
                                followerStateFrames = 0;
                                followerLastPos     = p2Pos;
                                SPDLOG_INFO("[Follower] IDLE→FOLLOW (toward pending transition trigger at "
                                            "{:.0f},{:.0f},{:.0f}, dist={:.0f})",
                                            pendingTransitionPos.x, pendingTransitionPos.y,
                                            pendingTransitionPos.z, sqrtf(d2));
                            }
                        }
                        if (pendingTransitionTimeoutFrames > 0) {
                            pendingTransitionTimeoutFrames--;
                            if (pendingTransitionTimeoutFrames == 0) {
                                SPDLOG_WARN("[Follower] Pending transition TIMEOUT — leader is gone, "
                                            "can't reach trigger. Deactivating.");
                                hasPendingTransition = false;
                                SetFollowerActive(false);
                                return;
                            }
                        }
                    }
                }

                // G11/G13 — leader crossed a scene or room boundary.
                // Leader's scene/room is broadcast via UPDATE_CLIENT_STATE; if it
                // diverges from ours, we either teleport (boss scene per G13),
                // deactivate (different scene per G11), or initiate a door-
                // handoff walk-through (same scene different room, Bug 7 phase B).
                //
                // SUPPRESSED when a pending SCENE_TRANSITION_HANDOFF is in
                // flight (phase C): the packet already tells us exactly where
                // to go and which entrance to use. Deactivating here would
                // stop the navigation before we reach the trigger.
                //
                // Test 14 (log 84) — also suppressed when OoT is mid-transition
                // (`transitionTrigger != TRANS_TRIGGER_OFF`). `hasPendingTransition`
                // is cleared the frame we fire the trigger, but `ourScene`
                // doesn't update until the scene load completes — a ~100-200 ms
                // window where the G13 scene-mismatch check fires and
                // deactivates the follower right in the middle of the
                // transition we just triggered. Gating on transitionTrigger
                // covers that window cleanly.
                bool sceneLoadInProgress =
                    (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF);
                if (!pendingTransitionInFlight && !sceneLoadInProgress) {
                    auto it = clients.find(followerLeaderClientId);
                    if (it != clients.end() && it->second.isSaveLoaded) {
                        s16 leaderScene = it->second.sceneNum;
                        s8  leaderRoom  = it->second.curRoomNum;
                        s16 ourScene    = (s16)gPlayState->sceneNum;
                        s8  ourRoom     = (s8)gPlayState->roomCtx.curRoom.num;

                        // Shadow-track the leader's position while we share a
                        // room. When they cross a door and leave the room, the
                        // follower walks toward this cached point to find the
                        // same door, then teleports on timeout if it fails.
                        if (leaderScene == ourScene && leaderRoom == ourRoom) {
                            followerLeaderLastInOurRoom       = leaderPos;
                            followerLeaderLastInOurRoomNumber = ourRoom;
                            // Rooms re-synced while a handoff was in flight:
                            // our follower crossed the door successfully.
                            if (followerDoorHandoff) {
                                SPDLOG_INFO("[Follower] Door handoff complete — room re-synced (ours={})",
                                            (int)ourRoom);
                                followerDoorHandoff       = false;
                                followerDoorHandoffFrames = 0;
                            }
                        }

                        if (leaderScene != ourScene) {
                            if (IsBossScene(leaderScene)) {
                                // G13 — historically we deactivated here. With
                                // SCENE_TRANSITION_HANDOFF active, the leader's
                                // handoff packet is what carries the follower
                                // through the boss door. G13 only fires now if
                                // the leader entered the boss scene WITHOUT
                                // the handoff packet reaching us (packet
                                // dropped, or leader's build predates the
                                // packet). In that case, deactivate with the
                                // same fallback behaviour as before.
                                SPDLOG_WARN("[Follower] Leader entered boss scene 0x{:02X} without handoff — "
                                            "deactivating (walk through the door manually, then re-enable)",
                                            leaderScene);
                            } else {
                                SPDLOG_WARN("[Follower] Leader in different scene (ours=0x{:02X} leader=0x{:02X}) "
                                            "— deactivating; walk through the door manually",
                                            ourScene, leaderScene);
                            }
                            SetFollowerActive(false);
                            return;
                        }

                        // Same scene, different room: Bug 7 phase B — initiate
                        // a door handoff. The follower walks toward the leader's
                        // last-seen position in our room (usually a door
                        // threshold) and Phase A's DO_ACTION_ENTER injection
                        // triggers the door animation. If we can't reach the
                        // door within kDoorHandoffTimeout frames, teleport to
                        // the leader (they may already be deep in the next room).
                        if (leaderRoom != ourRoom && leaderRoom != -1 && ourRoom != -1) {
                            // Test 6 (log 74) follow-up — scan for a
                            // transition actor (door / shutter) whose
                            // `sides[]` connect ourRoom ↔ leaderRoom.
                            // OoT's TransitionActorEntry carries the
                            // door's world position and the two rooms it
                            // bridges; using that position as the nav
                            // target puts the follower exactly on the
                            // door trigger so Phase A's DO_ACTION_ENTER
                            // BTN_A injection fires. Falls back to the
                            // shadow-tracked `followerLeaderLastInOurRoom`
                            // if no matching transition is found (e.g.
                            // fall-through holes like Deku Tree 0→10,
                            // where the "transition" is vertical gravity
                            // and no door actor exists on the centerline).
                            Vec3f doorTarget = followerLeaderLastInOurRoom;
                            bool  doorFound  = false;
                            {
                                TransitionActorContext* tac = &gPlayState->transiActorCtx;
                                for (s32 i = 0; i < tac->numActors; i++) {
                                    const TransitionActorEntry* e = &tac->list[i];
                                    bool matchesRoomPair =
                                        (e->sides[0].room == (s8)ourRoom    &&
                                         e->sides[1].room == (s8)leaderRoom) ||
                                        (e->sides[0].room == (s8)leaderRoom &&
                                         e->sides[1].room == (s8)ourRoom);
                                    if (matchesRoomPair) {
                                        doorTarget.x = (f32)e->pos.x;
                                        doorTarget.y = (f32)e->pos.y;
                                        doorTarget.z = (f32)e->pos.z;
                                        doorFound    = true;
                                        break;
                                    }
                                }
                            }

                            if (!followerDoorHandoff) {
                                followerDoorHandoff       = true;
                                followerDoorHandoffFrames = kDoorHandoffTimeout;
                                // Test 9 — on the arm edge (first frame rooms
                                // diverge), teleport follower to leader's
                                // last-same-room position + match rotation.
                                //
                                // Test 10 (log 79, Bug 2) — GUARD on the
                                // teleport. `followerLeaderLastInOurRoom`
                                // was recorded in room
                                // `followerLeaderLastInOurRoomNumber`.
                                // If follower's CURRENT room matches that
                                // number, the teleport is same-room: safe
                                // to write world.pos directly. If NOT
                                // (follower already crossed a room boundary
                                // themselves — Mad Scrub fall-through hole
                                // was observed looping at 20 Hz in log 79),
                                // writing world.pos to another room's
                                // coordinates produces a broken state
                                // where Link is visually in room A but
                                // roomCtx.curRoom.num is room B — eventually
                                // resolved by a void-out respawn back to
                                // room A, after which the follower walks
                                // back into the hole from sideTarget. Skip
                                // the teleport in that case; handoff nav +
                                // G10/G12 will handle via scene-reload.
                                bool teleportSafe =
                                    (followerLeaderLastInOurRoomNumber == ourRoom);
                                // Nav system Shape A hang-state guard (commit 6c).
                                // Same reasoning as TeleportToLeader's guard
                                // — suppress world.pos writes while Link is
                                // hanging off a ledge so the BTN_A/BTN_B
                                // resolution path can resolve the hang
                                // cleanly without sliding.
                                bool hangActive =
                                    (player->stateFlags1 & PLAYER_STATE1_HANGING_OFF_LEDGE) != 0 &&
                                    CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0 &&
                                    CVarGetInteger(CVAR_ENHANCEMENT("Nav.VerticalTeleport"), 0) != 0;
                                if (teleportSafe && !hangActive) {
                                    player->actor.world.pos = followerLeaderLastInOurRoom;
                                    player->actor.prevPos   = followerLeaderLastInOurRoom;
                                    player->actor.shape.rot.y = leaderActor->shape.rot.y;
                                    // Post-teleport hold: zero stick for
                                    // the hold window so Link settles
                                    // before the state machine resumes.
                                    followerPostTeleportFrames = 30;
                                    followerStuckCycleCount    = 0;
                                    followerStuckCycleResetFrames = 0;
                                    followerOverrunFrames      = 0;
                                }
                                SPDLOG_INFO("[Follower] Leader crossed room boundary (ours={} leader={}) "
                                            "— door handoff armed; teleport={} last-pos=({:.0f},{:.0f},{:.0f}) "
                                            "last-room={} yaw={} target={:.0f},{:.0f},{:.0f} {} "
                                            "timeout={} frames",
                                            (int)ourRoom, (int)leaderRoom,
                                            teleportSafe ? "fired" : "SKIPPED(room-mismatch)",
                                            followerLeaderLastInOurRoom.x,
                                            followerLeaderLastInOurRoom.y,
                                            followerLeaderLastInOurRoom.z,
                                            (int)followerLeaderLastInOurRoomNumber,
                                            (int)leaderActor->shape.rot.y,
                                            doorTarget.x, doorTarget.y, doorTarget.z,
                                            doorFound ? "transition-actor" : "shadow-position",
                                            kDoorHandoffTimeout);
                            }

                            // Route the follower to the transition actor
                            // position (preferred) or the shadow-tracked
                            // leader position (fallback). FOLLOW's approach
                            // logic handles the rest.
                            followerMoveTarget = doorTarget;
                            if (followerAIState == FollowerAIState::IDLE) {
                                followerAIState     = FollowerAIState::FOLLOW;
                                followerStateFrames = 0;
                                followerLastPos     = p2Pos;
                            }

                            if (followerDoorHandoffFrames > 0) {
                                followerDoorHandoffFrames--;
                                if (followerDoorHandoffFrames == 0) {
                                    SPDLOG_WARN("[Follower] Door handoff TIMEOUT "
                                                "(ours-room={} leader-room={})",
                                                (int)ourRoom, (int)leaderRoom);
                                    bool triggered = TeleportToLeader("G11 handoff timeout");
                                    followerDoorHandoff       = false;
                                    followerOverrunFrames     = 0;
                                    followerAIState           = FollowerAIState::IDLE;
                                    followerStateFrames       = 0;
                                    if (triggered) {
                                        return; // scene transition owns the next frames
                                    }
                                }
                            }
                        }
                    }
                }

                // G10 — leash-timeout teleport. If the follower has been more
                // than kTeleportThreshold from the leader for kTeleportDelayFrames
                // continuous frames, teleport to the leader. Catches stuck-in-
                // geometry / fell-behind / can't-traverse scenarios that the
                // state machine couldn't recover from.
                //
                // Bug 6 (2026-04-22): now uses 3D distance (was XZ-only).
                // With Y-eligibility removed from IsEligibleLeader, the
                // "leader on a different floor" case falls through to here
                // — Δy can be the dominant component. Treating XZ-only would
                // miss that case entirely (log 68: P1 on floor above P2,
                // 30 + s, no teleport fired).
                {
                    f32 dxL = leaderPos.x - p2Pos.x;
                    f32 dyL = leaderPos.y - p2Pos.y;
                    f32 dzL = leaderPos.z - p2Pos.z;
                    if (dxL * dxL + dyL * dyL + dzL * dzL > kTeleportThreshold * kTeleportThreshold) {
                        followerOverrunFrames++;
                        if (followerOverrunFrames >= kTeleportDelayFrames) {
                            // Bug B (log 69) — route through TeleportToLeader
                            // so cross-room overruns use the scene-reload path
                            // rather than a raw world.pos write.
                            bool triggered = TeleportToLeader("G10 3D leash overrun");
                            followerOverrunFrames = 0;
                            followerAIState       = FollowerAIState::IDLE;
                            followerStateFrames   = 0;
                            if (triggered) {
                                return;
                            }
                        }
                    } else {
                        followerOverrunFrames = 0;
                    }
                }

                // G12 — STUCK escalation teleport. If the follower has entered
                // STUCK kStuckCycleEscalation times within kStuckCycleWindow,
                // bail to a teleport. Counter is incremented at the FOLLOW→STUCK
                // transition below; window is reset when we reach IDLE cleanly.
                if (followerStuckCycleCount >= kStuckCycleEscalation) {
                    // Bug B (log 69) — route through TeleportToLeader for
                    // cross-room-safe teleport. Always return regardless of
                    // mode: STUCK escalation is a terminal reset.
                    TeleportToLeader("G12 stuck-cycle escalation");
                    followerStuckCycleCount       = 0;
                    followerStuckCycleResetFrames = 0;
                    followerOverrunFrames         = 0;
                    followerAIState               = FollowerAIState::IDLE;
                    followerStateFrames           = 0;
                    return;
                }

                // G14 — close-to-leader fail-timeout (Test 6, user request).
                // G10 catches hard leash overruns (> 1200 u for 2 s); G12
                // catches stuck-cycle loops. Between those two is a gap:
                // follower is 200-1200 u from leader, actively trying to
                // close, but terrain/state-machine churn prevents progress.
                // G14 measures baseline distance and fires a teleport when
                // the follower hasn't reduced distance by `kG14ProgressDelta`
                // in `kG14TimeoutFrames`.
                //
                // Reset the baseline whenever the follower makes progress
                // (delta > kG14ProgressDelta) OR leaves a "trying to move"
                // state (IDLE / CLIMBING / BLOCK / STANDBY / RANGED_ATTACK
                // / COLLECT_ITEM are excluded).
                static constexpr f32 kG14MinDistance     = 200.0f;   // below this, no teleport
                static constexpr f32 kG14ProgressDelta   = 30.0f;    // units of "progress"
                static constexpr int kG14TimeoutFrames   = 600;      // ~10 s at 60 fps
                {
                    bool actingToClose =
                        (followerAIState == FollowerAIState::FOLLOW  ||
                         followerAIState == FollowerAIState::STUCK   ||
                         followerAIState == FollowerAIState::ENGAGE  ||
                         followerAIState == FollowerAIState::ATTACK  ||
                         followerAIState == FollowerAIState::RETURN);
                    if (!actingToClose) {
                        // Non-closing state — reset so we don't inherit
                        // stale counter on next movement state entry.
                        followerCloseFailBaseline = 0.0f;
                        followerCloseFailFrames   = 0;
                    } else {
                        f32 dxCL = leaderPos.x - p2Pos.x;
                        f32 dyCL = leaderPos.y - p2Pos.y;
                        f32 dzCL = leaderPos.z - p2Pos.z;
                        f32 distToLeader = sqrtf(dxCL * dxCL + dyCL * dyCL + dzCL * dzCL);
                        if (distToLeader < kG14MinDistance) {
                            followerCloseFailBaseline = 0.0f;
                            followerCloseFailFrames   = 0;
                        } else {
                            if (followerCloseFailBaseline == 0.0f ||
                                distToLeader < followerCloseFailBaseline - kG14ProgressDelta) {
                                // First entry OR made progress — reset.
                                followerCloseFailBaseline = distToLeader;
                                followerCloseFailFrames   = 0;
                            } else {
                                followerCloseFailFrames++;
                                if (followerCloseFailFrames >= kG14TimeoutFrames) {
                                    SPDLOG_WARN("[Follower] G14 close-fail timeout "
                                                "(baseline={:.0f} now={:.0f} frames={})",
                                                followerCloseFailBaseline, distToLeader,
                                                followerCloseFailFrames);
                                    TeleportToLeader("G14 close-fail timeout");
                                    followerCloseFailBaseline = 0.0f;
                                    followerCloseFailFrames   = 0;
                                    followerAIState           = FollowerAIState::IDLE;
                                    followerStateFrames       = 0;
                                    return;
                                }
                            }
                        }
                    }
                }

                // G1/G2 — leader is climbing a vine/ladder. Bug 2 redesign
                // (2026-04-22): no longer teleport-and-ride. Instead, teleport
                // follower to leader's XZ (the ladder base) at follower's
                // current Y, then let the CLIMBING state inject stick_y so
                // Link's own state machine grabs the ladder and climbs
                // naturally. This produces the real climb animation, real
                // physics, and avoids the gravity-fight "hover slightly below
                // P1" symptom from log 68.
                //
                // Nav system Shape A (commit 6b): this block IS the
                // canonical Shape A — input-injection + XZ-only snap +
                // dismount forward-hold. AnchorNav::IsShapeAEligible
                // exposes the predicate that future Link-rigged ally
                // NPCs would consult before delegating into this path.
                // Per the plan §9, Shape A is preserved verbatim — any
                // rewrite risks regressing real climb animation /
                // physics / vine lateral tracking. The legacy follower
                // CVar (gRemoteAnchor.FollowerEnabled) remains the
                // master switch; nav.VerticalTeleport's master toggle
                // does NOT gate this code path because Shape A is
                // owned by the follower system, not the nav system.
                //
                // Edge-triggered: only enter CLIMBING if we aren't already
                // there. The XZ-only teleport makes follower adjacent to the
                // ladder rim; subsequent stick_y forward injection causes the
                // ladder collider to attach Link.
                {
                    auto it = clients.find(followerLeaderClientId);
                    if (it != clients.end() && it->second.isClimbing &&
                        followerAIState != FollowerAIState::CLIMBING) {
                        // Snap to leader's XZ but keep follower's Y. If
                        // follower is already higher (leader climbing down to
                        // us), we don't drop them; if follower is lower (the
                        // common case), they're now at the ladder base.
                        Vec3f ladderXz = { leaderPos.x, p2Pos.y, leaderPos.z };
                        player->actor.world.pos = ladderXz;
                        player->actor.prevPos   = ladderXz;
                        followerAIState     = FollowerAIState::CLIMBING;
                        followerStateFrames = 0;
                        SPDLOG_INFO("[Follower] Leader started climbing → CLIMBING "
                                    "(snap to ladder XZ at {:.0f},{:.0f},{:.0f})",
                                    ladderXz.x, ladderXz.y, ladderXz.z);
                        // Refresh p2Pos snapshot since we just moved.
                        p2Pos = player->actor.world.pos;
                    }
                }

                followerStateFrames++;

                // Periodic heartbeat: log state + positions every 60 frames.
                if (followerStateFrames % 60 == 0) {
                    f32 toTarget = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                    const char* stateStr = "?";
                    switch (followerAIState) {
                        case FollowerAIState::IDLE:          stateStr = "IDLE";          break;
                        case FollowerAIState::FOLLOW:        stateStr = "FOLLOW";        break;
                        case FollowerAIState::STUCK:         stateStr = "STUCK";         break;
                        case FollowerAIState::ENGAGE:        stateStr = "ENGAGE";        break;
                        case FollowerAIState::ATTACK:        stateStr = "ATTACK";        break;
                        case FollowerAIState::RETURN:        stateStr = "RETURN";        break;
                        case FollowerAIState::CLIMBING:      stateStr = "CLIMBING";      break;
                        case FollowerAIState::BLOCK:         stateStr = "BLOCK";         break;
                        case FollowerAIState::RANGED_ATTACK: stateStr = "RANGED_ATTACK"; break;
                        case FollowerAIState::STANDBY:       stateStr = "STANDBY";       break;
                        case FollowerAIState::COLLECT_ITEM:  stateStr = "COLLECT_ITEM";  break;
                    }
                    SPDLOG_INFO("[Follower] state={} p2=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) distToTarget={:.0f}",
                                stateStr,
                                p2Pos.x, p2Pos.y, p2Pos.z,
                                sideTarget.x, sideTarget.y, sideTarget.z,
                                toTarget);
                }

                switch (followerAIState) {

                    case FollowerAIState::IDLE: {
                        // Drift back to side-target if P1 moved.
                        f32 dx = sideTarget.x - p2Pos.x;
                        f32 dz = sideTarget.z - p2Pos.z;
                        if (dx * dx + dz * dz > kFollowThreshold * kFollowThreshold) {
                            followerAIState     = FollowerAIState::FOLLOW;
                            followerStateFrames = 0;
                            followerLastPos     = p2Pos;
                            SPDLOG_INFO("[Follower] IDLE→FOLLOW p2=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) dist={:.0f}",
                                        p2Pos.x, p2Pos.y, p2Pos.z,
                                        sideTarget.x, sideTarget.y, sideTarget.z,
                                        sqrtf(dx * dx + dz * dz));
                            break;
                        }
                        // Scan for the nearest live enemy within ENGAGE range.
                        // Reject enemies on a different vertical level — the
                        // follower only moves in XZ, so targets on another floor
                        // (e.g. a room below the Deku Tree entrance) otherwise
                        // cause it to walk into walls and swing at air.
                        // (Room-equality check disabled — see banner note above.)
                        //
                        // Target blacklist: actors that can only be defeated by
                        // shield-reflect of their own projectiles. The follower
                        // can't perform reflect (no shield input in current
                        // RANGED_ATTACK mode), and the underground "wait" state
                        // for these scrubs has the AC collider disabled +
                        // collider height shrunk to 5, so a melee swing hits
                        // nothing but the targeting logic still picks them up
                        // because ACTOR_FLAG_ATTENTION_ENABLED stays set.
                        // Symptom on the demo path: follower "runs at empty
                        // Hintnut nest" in Compound Room.
                        auto IsScrubPuzzleActor = [](int16_t id) -> bool {
                            return id == ACTOR_EN_HINTNUTS ||
                                   id == ACTOR_EN_DEKUNUTS;
                        };
                        Actor* nearest    = nullptr;
                        f32    nearDistSq = kEngageRange * kEngageRange;
                        Actor* eActor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
                        while (eActor != nullptr) {
                            if (eActor->update != nullptr &&
                                /* eActor->room == player->actor.room && */
                                !IsScrubPuzzleActor(eActor->id) &&
                                fabsf(eActor->world.pos.y - p2Pos.y) < kMaxYDelta) {
                                f32 edx     = eActor->world.pos.x - p2Pos.x;
                                f32 edz     = eActor->world.pos.z - p2Pos.z;
                                f32 eDistSq = edx * edx + edz * edz;
                                if (eDistSq < nearDistSq) {
                                    nearDistSq = eDistSq;
                                    nearest    = eActor;
                                }
                            }
                            eActor = eActor->next;
                        }
                        if (nearest != nullptr) {
                            followerTargetEnemy = nearest;
                            followerAIState     = FollowerAIState::ENGAGE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] IDLE→ENGAGE enemy id={} at ({:.0f},{:.0f},{:.0f}) dist={:.0f}",
                                        nearest->id,
                                        nearest->world.pos.x, nearest->world.pos.y, nearest->world.pos.z,
                                        sqrtf(nearDistSq));
                            break;
                        }
                        // Item pickup — no enemy to engage; scan for eligible drops.
                        // Grace/filter/Y-gate are all handled inside ScanForItemCandidate.
                        {
                            Actor* item = ScanForItemCandidate();
                            if (item != nullptr) {
                                followerTargetItem = item;
                                followerCollectItemTimeoutFrames = kItemCollectTimeout;
                                followerAIState     = FollowerAIState::COLLECT_ITEM;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] IDLE→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                                            (int)(item->params & 0xFF),
                                            item->world.pos.x, item->world.pos.y, item->world.pos.z);
                                break;
                            }
                        }
                        // In IDLE, match P1's facing direction.
                        player->actor.shape.rot.y = dummyActor->shape.rot.y;
                        // Pre-populate move target so the first FOLLOW frame's
                        // ShouldActorUpdate sees the correct direction immediately.
                        // Test 8 — during door handoff the G11 block above
                        // already set followerMoveTarget to the door
                        // centerline; don't overwrite with side-offset.
                        if (!followerDoorHandoff) {
                            followerMoveTarget = sideTarget;
                        }
                        break;
                    }

                    case FollowerAIState::FOLLOW: {
                        // Stuck detection: every kStuckCheckInterval frames check progress.
                        if (followerStateFrames % kStuckCheckInterval == 0) {
                            f32 progDx   = p2Pos.x - followerLastPos.x;
                            f32 progDz   = p2Pos.z - followerLastPos.z;
                            f32 progress = sqrtf(progDx * progDx + progDz * progDz);
                            f32 toTarget = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                            SPDLOG_INFO("[Follower] FOLLOW check: progress={:.1f} distToTarget={:.0f} "
                                        "p2=({:.0f},{:.0f}) last=({:.0f},{:.0f}) target=({:.0f},{:.0f})",
                                        progress, toTarget,
                                        p2Pos.x, p2Pos.z,
                                        followerLastPos.x, followerLastPos.z,
                                        sideTarget.x, sideTarget.z);
                            followerLastPos = p2Pos; // update checkpoint
                            if (progress < kStuckMinProgress) {
                                // Stick input failed to make progress. Enter the
                                // STUCK fallback, which nudges the follower
                                // directly toward followerMoveTarget via
                                // position override until kStuckRecovery frames
                                // elapse. (followerStuckDir is no longer used:
                                // the perpendicular strafe pattern was dropped
                                // when movement switched to stick input. Field
                                // kept in the header for a future strafe variant.)
                                followerAIState     = FollowerAIState::STUCK;
                                followerStuckFrames = 0;
                                followerStateFrames = 0;
                                // G12 — count this entry; arm the reset window.
                                // The top-of-hook check escalates to teleport when
                                // count >= kStuckCycleEscalation within the window.
                                followerStuckCycleCount++;
                                followerStuckCycleResetFrames = kStuckCycleWindow;
                                SPDLOG_INFO("[Follower] FOLLOW→STUCK (stick input stalled, cycle={})",
                                            followerStuckCycleCount);
                                break;
                            }
                        }
                        // Item pickup — scan every 10 frames inside FOLLOW (less
                        // frequent than IDLE; we're actively traversing so a
                        // tight scan window is less useful). On finding an
                        // eligible drop, abandon FOLLOW and divert to COLLECT_ITEM.
                        if (followerStateFrames % 10 == 0) {
                            Actor* item = ScanForItemCandidate();
                            if (item != nullptr) {
                                followerTargetItem = item;
                                followerCollectItemTimeoutFrames = kItemCollectTimeout;
                                followerAIState     = FollowerAIState::COLLECT_ITEM;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] FOLLOW→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                                            (int)(item->params & 0xFF),
                                            item->world.pos.x, item->world.pos.y, item->world.pos.z);
                                break;
                            }
                        }
                        // Test 8 (user report) — during door handoff, the
                        // G11 safety-net block above this switch sets
                        // followerMoveTarget to the transition-actor
                        // position (door centerline). FOLLOW was then
                        // overwriting that with sideTarget (+kFollowOffset
                        // on X), pushing the follower 50 u off the door
                        // centerline into the adjacent wall. Skip the
                        // overwrite while handoff is active — the handoff
                        // block owns the move target in that case. Same
                        // pattern crawlspaces already use via the leader-
                        // Crawling sideTarget collapse.
                        Vec3f followTarget = sideTarget;
                        if (!followerDoorHandoff) {
                            followerMoveTarget = followTarget;
                        } else {
                            // Route through the handoff target without
                            // offset; yaw/dist computations below use
                            // followerMoveTarget as the ground truth.
                            followTarget = followerMoveTarget;
                        }
                        {
                            f32 dist = sqrtf(SQ(followTarget.x - p2Pos.x) + SQ(followTarget.z - p2Pos.z));
                            // Stick injection in ShouldActorUpdate drives actual movement;
                            // here we just transition when we're close enough.
                            if (dist > 0.001f) {
                                player->actor.shape.rot.y = YawToward(
                                    followTarget.x - player->actor.world.pos.x,
                                    followTarget.z - player->actor.world.pos.z);
                            }
                            // Bug 2 (log 184 Karebaba corridor) — skip the
                            // FOLLOW→IDLE transition while a door handoff is
                            // armed. The handoff block at the top of the hook
                            // re-arms `followerDoorHandoff` every frame the
                            // rooms differ, and (when state was IDLE) flips
                            // back to FOLLOW on the same frame. Without this
                            // guard, FOLLOW→IDLE→FOLLOW oscillates at frame
                            // cadence whenever dist-to-door < kFollowThreshold,
                            // which is exactly the moment the follower is
                            // close enough for the BTN_A door-open injection
                            // to fire — and the oscillation prevents that
                            // injection from sticking.
                            //
                            // Stay in FOLLOW until either:
                            //   (a) follower crosses into leader's room (door
                            //       opens, walks through; rooms re-match and
                            //       the handoff clears at the top of the hook
                            //       at line 1449-1453), or
                            //   (b) followerDoorHandoffFrames hits zero
                            //       (timeout → fallback teleport).
                            if (dist < kFollowThreshold && !followerDoorHandoff) {
                                followerAIState     = FollowerAIState::IDLE;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] FOLLOW→IDLE dist={:.1f}", dist);
                            }
                        }
                        break;
                    }

                    case FollowerAIState::STUCK: {
                        // Fallback path: stick-input hit a wall / corner / doorway
                        // the simulation can't navigate. Apply a small position nudge
                        // directly toward followerMoveTarget for up to kStuckRecovery
                        // frames. This bypasses Link's physics just enough to get
                        // past the obstacle. Stick injection stays active in this
                        // state (see ShouldActorUpdate) so Link's legs still try to
                        // walk — the nudge is additive, not a replacement.
                        // This is the ONLY path in the follower state machine that
                        // writes to player->actor.world.pos in the stick-input design.
                        followerStuckFrames++;
                        f32 ndx = followerMoveTarget.x - player->actor.world.pos.x;
                        f32 ndz = followerMoveTarget.z - player->actor.world.pos.z;
                        f32 nd  = sqrtf(ndx * ndx + ndz * ndz);
                        if (nd > 0.001f) {
                            f32 step = (nd < kMoveSpeed) ? nd : kMoveSpeed;
                            player->actor.world.pos.x += ndx / nd * step;
                            player->actor.world.pos.z += ndz / nd * step;
                        }
                        if (followerStuckFrames >= kStuckRecovery) {
                            followerAIState     = FollowerAIState::FOLLOW;
                            followerLastPos     = player->actor.world.pos;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] STUCK→FOLLOW (fallback nudge complete)");
                        }
                        break;
                    }

                    case FollowerAIState::ENGAGE: {
                        // Abandon if leader is too far or target is gone.
                        {
                            f32 ldx = leaderPos.x - p2Pos.x;
                            f32 ldz = leaderPos.z - p2Pos.z;
                            if (ldx * ldx + ldz * ldz > kMaxLeash * kMaxLeash) {
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RETURN (leader too far)");
                                break;
                            }
                        }
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ENGAGE→RETURN (enemy gone)");
                            break;
                        }
                        // Vertical-reach handling. Three layered checks:
                        //  1. Cross-floor (|Δy| >= kMaxYDelta, 120 units): target
                        //     is on a different logical level. If it's ranged-
                        //     required, route to RANGED_ATTACK; otherwise bail.
                        //  2. Above sword reach but same-floor (Δy > kSwordVerticalReach,
                        //     40 units) AND ranged-required: route to RANGED_ATTACK.
                        //     Fix 2 (2026-04-22) — before this check existed, a
                        //     Skullwalltula at Δy=118 (just under kMaxYDelta) was
                        //     routed to ATTACK and the follower whiffed for the
                        //     full 60-frame cycle (P2 log 67, 15:21:03).
                        //  3. Otherwise fall through to XZ close + ATTACK.
                        // (Room-equality side of this check disabled — see banner note above.)
                        {
                            f32 dy = followerTargetEnemy->world.pos.y - p2Pos.y;
                            if (fabsf(dy) >= kMaxYDelta) {
                                if (IsRangedRequiredEnemy(followerTargetEnemy->id)) {
                                    FollowerTryEquipRangedWeapon();
                                    followerAIState     = FollowerAIState::RANGED_ATTACK;
                                    followerStateFrames = 0;
                                    SPDLOG_INFO("[Follower] ENGAGE→RANGED_ATTACK (off-floor target id={})",
                                                followerTargetEnemy->id);
                                    break;
                                }
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RETURN (enemy off-floor)");
                                break;
                            }
                            if (dy > kSwordVerticalReach &&
                                IsRangedRequiredEnemy(followerTargetEnemy->id)) {
                                FollowerTryEquipRangedWeapon();
                                followerAIState     = FollowerAIState::RANGED_ATTACK;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RANGED_ATTACK (above sword reach Δy={:.0f} target id={})",
                                            dy, followerTargetEnemy->id);
                                break;
                            }
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        f32   edx      = enemyPos.x - p2Pos.x;
                        f32   edz      = enemyPos.z - p2Pos.z;
                        f32   distSq   = edx * edx + edz * edz;
                        // Bug D — per-enemy attackRange keeps the follower
                        // outside lunge arcs of enemies whose damage volume
                        // sits ahead of world.pos.
                        f32   attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
                        if (distSq < attackRange * attackRange) {
                            // G4 — Mad Scrub class: shield first, then swing on
                            // the stunned scrub. BLOCK→ATTACK is wired in BLOCK.
                            if (IsShieldReflectEnemy(followerTargetEnemy->id)) {
                                followerAIState     = FollowerAIState::BLOCK;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→BLOCK (shield-reflect target id={})",
                                            followerTargetEnemy->id);
                                break;
                            }
                            followerAIState     = FollowerAIState::ATTACK;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ENGAGE→ATTACK enemy=({:.0f},{:.0f},{:.0f}) dist={:.0f} "
                                        "range={:.0f} id={}",
                                        enemyPos.x, enemyPos.y, enemyPos.z, sqrtf(distSq),
                                        attackRange, followerTargetEnemy->id);
                            break;
                        }
                        // Every 20 frames log distance to enemy so we can see approach progress.
                        if (followerStateFrames % 20 == 0) {
                            SPDLOG_INFO("[Follower] ENGAGE progress: distToEnemy={:.0f} p2=({:.0f},{:.0f})",
                                        sqrtf(distSq), p2Pos.x, p2Pos.z);
                        }
                        // Test 6 (log 74) — dangling Skulltula safety gap.
                        // User: "AI Follower gets too close to dangling
                        // Skulltulas and takes damage without waiting for
                        // them to reveal their weak spot." En_St on the
                        // ceiling drops on Link when he walks underneath;
                        // without state-machine sync (#90 pending) the
                        // follower can't tell if the Skulltula is safe to
                        // approach. Keep a 150 u XZ safety gap for any
                        // EN_ST target whose Y is well above the follower
                        // (ceiling/wall mounted). Pulls the move target
                        // back along the follower→enemy vector so Link
                        // stops at 150 u XZ even though the slingshot
                        // still has line-of-fire. ENGAGE→RANGED_ATTACK
                        // admission distance (350 u) covers this.
                        Vec3f navTarget = enemyPos;
                        if (followerTargetEnemy->id == ACTOR_EN_ST) {
                            f32 targetDy = enemyPos.y - p2Pos.y;
                            if (targetDy > 40.0f) {
                                // Test 7 (user): "extend 50 units" — 150→200
                                // so follower stands further back and the
                                // slingshot arc has a better downward angle
                                // to the ground Skulltula vs a Link that
                                // walked directly under it.
                                static constexpr f32 kEnStSafeStandoffXZ = 200.0f;
                                f32 distXZ = sqrtf(distSq);
                                if (distXZ > kEnStSafeStandoffXZ) {
                                    f32 shrink = (distXZ - kEnStSafeStandoffXZ) / distXZ;
                                    navTarget.x = p2Pos.x + edx * shrink;
                                    navTarget.z = p2Pos.z + edz * shrink;
                                } else {
                                    // Already inside safe gap — hold position
                                    // so we don't drift closer.
                                    navTarget.x = p2Pos.x;
                                    navTarget.z = p2Pos.z;
                                }
                                navTarget.y = enemyPos.y;
                            }
                        }
                        followerMoveTarget = navTarget;
                        // Stick injection in ShouldActorUpdate drives actual movement.
                        if (distSq > 1.0f) {
                            player->actor.shape.rot.y = YawToward(edx, edz);
                        }
                        break;
                    }

                    case FollowerAIState::ATTACK: {
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy gone)");
                            break;
                        }
                        // Task 3 — stop swinging when the target is defeated.
                        // Two complementary signals because OoT doesn't have one
                        // universal "dead" field:
                        //   (a) colChkInfo.health <= 0 — catches actors that
                        //       decrement their own health (Dekubaba, En_Ba,
                        //       most bosses — ~14 overlays total).
                        //   (b) EnemyNetId::hasLocalDeath / pendingNaturalDeath —
                        //       covers the AC_HIT-only pattern (Karebaba,
                        //       En_Firefly, En_St, most Phase-4A enemies) where
                        //       health is initialised once in sColCheckInfoInit
                        //       and never written again. Their death is signalled
                        //       by the collision AC_HIT flag driving SetupDying,
                        //       and our OnEnemyDefeat / HandlePacket_EnemyDefeated
                        //       paths flip these flags on the EnemyNetId extension.
                        // Initial Task 3 implementation used only (a) and was a
                        // no-op for Karebaba (health stays at 1 through the entire
                        // Dying cycle, P2 log 62 2026-04-21).
                        bool targetDefeated = (followerTargetEnemy->colChkInfo.health <= 0);
                        if (!targetDefeated) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr) {
                                EnemyStateSync::AuditBooleansVsPhase(*ext, "Follower.targetDefeatedCheck.A");
                                if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) ||
                                    EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase)) {
                                    targetDefeated = true;
                                }
                            }
                        }
                        if (targetDefeated) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy dead)");
                            break;
                        }
                        // Room-equality side of this check disabled — see banner note above.
                        if (/* followerTargetEnemy->room != player->actor.room || */
                            fabsf(followerTargetEnemy->world.pos.y - p2Pos.y) >= kMaxYDelta) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy off-floor)");
                            break;
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        // Bug D — point followerMoveTarget at a standoff
                        // offset from enemyPos instead of enemyPos itself.
                        // Stopping radius is attackRange - kSwingReach:
                        // sword can still reach (kSwingReach), but Link
                        // holds outside the enemy's damage volume. For
                        // Karebaba (range=110, swing=50), standoff is 60 u
                        // from root — outside the head's lunge arc. For
                        // Stalfos-class (range=80, swing=50), standoff is
                        // 30 u — the original sword-tip contact distance.
                        f32 attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
                        f32 standoff    = attackRange - kSwingReach;
                        if (standoff < 20.0f) standoff = 20.0f; // sanity floor
                        {
                            f32 edx      = enemyPos.x - p2Pos.x;
                            f32 edz      = enemyPos.z - p2Pos.z;
                            f32 enemyDistSq = edx * edx + edz * edz;
                            f32 enemyDist   = sqrtf(enemyDistSq);
                            if (enemyDist > 1.0f) {
                                // Move target = enemyPos pulled back toward
                                // the follower by `standoff` units. Avoids
                                // walking into the damage volume even while
                                // the enemy walks toward us.
                                f32 shrink = (enemyDist > standoff)
                                             ? (enemyDist - standoff) / enemyDist
                                             : 0.0f;
                                followerMoveTarget.x = p2Pos.x + edx * shrink;
                                followerMoveTarget.y = enemyPos.y;
                                followerMoveTarget.z = p2Pos.z + edz * shrink;
                            } else {
                                followerMoveTarget = enemyPos;
                            }
                            if (followerStateFrames % 10 == 0) {
                                SPDLOG_INFO("[Follower] ATTACK frame={} distToEnemy={:.0f} "
                                            "standoff={:.0f} p2=({:.0f},{:.0f})",
                                            followerStateFrames, enemyDist, standoff,
                                            p2Pos.x, p2Pos.z);
                            }
                            if (enemyDistSq > 1.0f) {
                                player->actor.shape.rot.y = YawToward(edx, edz);
                            }
                        }
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (cycle complete)");
                        }
                        break;
                    }

                    case FollowerAIState::RETURN: {
                        // Test 8 — same door-handoff carve-out as FOLLOW:
                        // preserve the handoff's door-centerline target.
                        Vec3f returnTarget = sideTarget;
                        if (!followerDoorHandoff) {
                            followerMoveTarget = returnTarget;
                        } else {
                            returnTarget = followerMoveTarget;
                        }
                        f32 dist = sqrtf(SQ(returnTarget.x - p2Pos.x) + SQ(returnTarget.z - p2Pos.z));
                        // Stick injection in ShouldActorUpdate drives actual movement.
                        if (dist > 0.001f) {
                            player->actor.shape.rot.y = YawToward(
                                returnTarget.x - player->actor.world.pos.x,
                                returnTarget.z - player->actor.world.pos.z);
                        }
                        if (dist < kFollowThreshold) {
                            followerAIState     = FollowerAIState::IDLE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RETURN→IDLE dist={:.1f}", dist);
                        }
                        break;
                    }

                    // G1/G2 — leader is climbing. Bug 2 redesign (2026-04-22):
                    // instead of writing world.pos = leaderPos every frame
                    // (which fights gravity between actor-update and our hook,
                    // producing the "hover slightly below leader" symptom),
                    // we point followerMoveTarget at leader's XZ at follower's
                    // current Y (the ladder base / current rung) and let the
                    // ShouldActorUpdate stick injection drive Link.
                    //
                    // The state machine sets a flag (followerOnLadderTarget)
                    // so the stick-inject hook knows to use raw stick_y for
                    // up/down rather than camera-relative XZ projection.
                    // Once Link's PLAYER_STATE1_CLIMBING_LADDER fires (Link
                    // physically grabbed the ladder), stick_y direction
                    // toggles based on Δy to leader: positive (up) if leader
                    // is higher, negative (down) if lower, zero when within
                    // tolerance. OoT plays the real climb animation natively.
                    //
                    // Exit when leader's isClimbing flips back to false. The
                    // top-of-hook re-arm only fires on rising edge so we
                    // don't loop back into CLIMBING if leader's still
                    // sticky-eligible.
                    case FollowerAIState::CLIMBING: {
                        auto it = clients.find(followerLeaderClientId);
                        if (it == clients.end() || !it->second.isClimbing) {
                            // Bug C (log 69) — arm the dismount-forward-hold.
                            // Immediately after CLIMBING→IDLE, follower is on
                            // the rim of the top/bottom floor. Without this
                            // hold, the next-frame state machine recomputes
                            // the move target around leaderPos — and leader
                            // often stands right at the climb exit, so the
                            // follower's stick-math points BACKWARD off the
                            // rim. Snapshot the current facing (set every
                            // frame during CLIMBING to leaderActor->shape.rot.y),
                            // arm the hold counter, then IDLE.
                            followerClimbDismountYaw    = player->actor.shape.rot.y;
                            followerClimbDismountFrames = kClimbDismountHoldFrames;
                            followerAIState     = FollowerAIState::IDLE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] CLIMBING→IDLE (leader stopped climbing); "
                                        "armed dismount forward-hold {} frames at yaw={}",
                                        kClimbDismountHoldFrames,
                                        (int)followerClimbDismountYaw);
                            break;
                        }
                        // followerMoveTarget = leader's XZ at the leader's
                        // current Y. ShouldActorUpdate's CLIMBING-aware
                        // injection reads this for direction (leader.y vs
                        // p2Pos.y).
                        followerMoveTarget = leaderPos;
                        // Match leader's facing so dismount looks clean.
                        player->actor.shape.rot.y = leaderActor->shape.rot.y;
                        break;
                    }

                    // G4 — shield reflect. Inject BTN_R while ENGAGE target is a
                    // known shield-reflect class (Mad Scrub). Movement freezes
                    // (no stick) so Link plants the shield. Returns to ENGAGE
                    // when target leaves the reflect-class window or is defeated.
                    case FollowerAIState::BLOCK: {
                        // Body extracted to Anchor::HandleStateBlock
                        // (Phase 1 commit 6 of the SRP refactor).
                        HandleStateBlock(player, p2Pos);
                        break;
                    }

                    // G6/G7/G8 — ranged attack. Inject BTN_Z + BTN_A while ENGAGE
                    // target is a known ranged-required class (Gohma ceiling, larvae,
                    // Skullwalltulas on vines). Movement freezes so Link aims.
                    case FollowerAIState::RANGED_ATTACK: {
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (target gone)");
                            break;
                        }
                        // Two-signal defeat check, mirroring the ATTACK state.
                        bool defeated = (followerTargetEnemy->colChkInfo.health <= 0);
                        if (!defeated) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr) {
                                EnemyStateSync::AuditBooleansVsPhase(*ext, "Follower.targetDefeatedCheck.B");
                                if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) ||
                                    EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase)) {
                                    defeated = true;
                                }
                            }
                        }
                        if (defeated) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (target dead)");
                            break;
                        }
                        // Face the target so the slingshot aim line is correct.
                        f32 ex = followerTargetEnemy->world.pos.x - p2Pos.x;
                        f32 ez = followerTargetEnemy->world.pos.z - p2Pos.z;
                        if (ex * ex + ez * ez > 1.0f) {
                            player->actor.shape.rot.y = YawToward(ex, ez);
                        }
                        if (followerStateFrames >= kAttackDuration) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (cycle complete)");
                        }
                        break;
                    }

                    // Reserved — placeholder for G19 (Gohma weak-point window).
                    // No transitions wired today; ENGAGE never picks STANDBY.
                    case FollowerAIState::STANDBY: {
                        // Body extracted to Anchor::HandleStateStandby
                        // (Phase 1 commit 6 of the SRP refactor).
                        HandleStateStandby();
                        break;
                    }

                    // Item pickup (Claude/Plans/ai_follower_item_pickup.md).
                    // Walks toward followerTargetItem until pickup fires
                    // (En_Item00 is collision-triggered; contact → collect).
                    // Exit paths:
                    //   - target actor gone (collected by us OR by leader) → RETURN
                    //   - timeout elapsed (couldn't reach) → RETURN
                    //   - leader beyond leash → RETURN (follow takes priority)
                    //   - leader started climbing → let top-of-hook G1/G2 take over
                    //   - item on a different floor (|Δy| ≥ kMaxYDelta) → RETURN
                    case FollowerAIState::COLLECT_ITEM: {
                        if (followerTargetItem == nullptr ||
                            followerTargetItem->update == nullptr) {
                            SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item gone — collected or unloaded)");
                            followerTargetItem  = nullptr;
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            break;
                        }
                        // Leader leash — don't stray too far from the leader
                        // just for a rupee.
                        {
                            f32 lx = leaderPos.x - p2Pos.x;
                            f32 lz = leaderPos.z - p2Pos.z;
                            if (lx * lx + lz * lz > kMaxLeash * kMaxLeash) {
                                SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (leader beyond leash)");
                                followerTargetItem  = nullptr;
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                break;
                            }
                        }
                        // Y-gate — item ended up on a different floor (bounce
                        // off a ledge between grace expiry and pickup start).
                        if (fabsf(followerTargetItem->world.pos.y - p2Pos.y) >= kMaxYDelta) {
                            SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item off-floor)");
                            followerTargetItem  = nullptr;
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            break;
                        }
                        // Timeout — couldn't reach the item in kItemCollectTimeout
                        // frames (geometry / collision mishap).
                        if (followerCollectItemTimeoutFrames > 0) {
                            followerCollectItemTimeoutFrames--;
                            if (followerCollectItemTimeoutFrames == 0) {
                                SPDLOG_WARN("[Follower] COLLECT_ITEM→RETURN (timeout)");
                                followerTargetItem  = nullptr;
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                break;
                            }
                        }
                        // Drive ShouldActorUpdate toward the item. En_Item00's
                        // own collision handler attaches to Link on contact —
                        // no BTN_A or other interaction needed for pickup.
                        followerMoveTarget = followerTargetItem->world.pos;
                        {
                            f32 idx = followerTargetItem->world.pos.x - p2Pos.x;
                            f32 idz = followerTargetItem->world.pos.z - p2Pos.z;
                            if (idx * idx + idz * idz > 1.0f) {
                                player->actor.shape.rot.y = YawToward(idx, idz);
                            }
                        }
                        break;
                    }
                }

                // End-of-block position override was intentionally removed when
                // the follower switched to stick-input movement. The only path
                // that now writes to player->actor.world.pos is the STUCK state
                // fallback above — see that case's comment block for rationale.
}

// ---------------------------------------------------------------------------
// Anchor::TickFollowerInput — per-frame follower input-injection body. Moved
// verbatim from HookHandlers.cpp's ShouldActorUpdate lambda body. Phase 1
// commit 5 of the SRP refactor (#173 / #169). The hook fires immediately
// BEFORE the player actor's update(), so input written here is consumed by
// Player_Update on the same frame — that's why locomotion / swing / climb
// can be driven by writing to input[0] from this site.
//
// Indentation preserved at the lambda's original 20-space depth so the diff
// against pre-commit-5 HookHandlers.cpp is line-aligned. A future cleanup
// commit can dedent to standard 4-space function-body depth alongside the
// equivalent dedent of TickFollower.
// ---------------------------------------------------------------------------
void Anchor::TickFollowerInput(Actor* actor) {
    (void)actor;  // body uses player/actor as locals declared inline below

                    Input& input = gPlayState->state.input[0];
                    // States where we drive locomotion via stick input.
                    // ATTACK included: under stick-input movement the follower
                    // needs to close the last few tens of units between
                    // kAttackRange (80) and actual sword reach (~40). Without
                    // stick injection during ATTACK the follower stops at 80
                    // and swings into empty air (observed 2026-04-21, P2 log 64
                    // — 60-frame cycles with distToEnemy 75-85). Stick points
                    // at enemyPos, so the swing also goes toward the enemy
                    // — OoT reads stick on the BTN_B edge-press frame to set
                    // swing direction, which agrees with the approach direction.
                    bool isMoving = (followerAIState == FollowerAIState::FOLLOW       ||
                                     followerAIState == FollowerAIState::STUCK        ||
                                     followerAIState == FollowerAIState::ENGAGE       ||
                                     followerAIState == FollowerAIState::ATTACK       ||
                                     followerAIState == FollowerAIState::RETURN       ||
                                     followerAIState == FollowerAIState::CLIMBING     ||
                                     followerAIState == FollowerAIState::COLLECT_ITEM);

                    // --- Joystick cancel ---
                    // Read hardware values BEFORE we inject anything. OoT resets input.cur
                    // from hardware at the start of each frame, so these are the real values.
                    {
                        s8 hwX = input.cur.stick_x;
                        s8 hwY = input.cur.stick_y;
                        if ((s32)hwX * hwX + (s32)hwY * hwY > 25 * 25) {
                            SetFollowerActive(false);
                            SPDLOG_INFO("[Follower] Deactivated (joystick hw=({}, {}))", hwX, hwY);
                            return;
                        }
                    }

                    // --- State guard — don't inject stick while Link is in a
                    // non-walkable state. Injecting during these can corrupt the
                    // ladder/cutscene state machines. Button presses (BTN_A for
                    // climb) are handled below, separately from stick.
                    //
                    // IN_WATER is intentionally NOT blocked — swimming uses the
                    // same camera-relative stick input as walking, and the
                    // follower needs to be able to swim forward into a ledge
                    // to trigger the water-exit climb-out animation. (Observed
                    // 2026-04-21: blocking IN_WATER left the follower sliding
                    // along the water's edge unable to exit.)
                    Player* player = (Player*)actor;
                    u32 sf1 = player->stateFlags1;
                    u32 sf2 = player->stateFlags2;
                    bool nowOnLadder = (sf1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
                    // CLIMBING_LADDER is normally blocked (stick during a real
                    // climb would spam OoT's input). Bug 2 (2026-04-22): when
                    // our follower state is CLIMBING, we WANT stick injection
                    // to drive Link up/down the ladder. The CLIMBING-aware
                    // injection block below handles it via a different code
                    // path; here we just exempt CLIMBING_LADDER from the
                    // blocked list when we're actively driving.
                    bool blockedByPlayerState =
                        (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) ||
                        (sf1 & PLAYER_STATE1_CLIMBING_LEDGE)    ||
                        (sf1 & PLAYER_STATE1_IN_CUTSCENE)       ||
                        (sf1 & PLAYER_STATE1_DAMAGED)           ||
                        (sf1 & PLAYER_STATE1_TALKING)           ||
                        (sf1 & PLAYER_STATE1_INPUT_DISABLED);
                    if (nowOnLadder && followerAIState != FollowerAIState::CLIMBING) {
                        // On a ladder but our state machine isn't in CLIMBING:
                        // user manually grabbed it, or we mis-entered from a
                        // non-climbing state. Block stick injection — let the
                        // human resume control via the joystick-cancel path.
                        blockedByPlayerState = true;
                    }

                    // --- Walk/run: camera-relative stick toward followerMoveTarget ---
                    // OoT's movement pipeline: worldYaw = Camera_GetInputDirYaw(cam) + stickAngle,
                    // where stickAngle = Math_Atan2S(relY, -relX).  To move in world direction
                    // (dx, dz), invert that pipeline:
                    //   worldYaw    = Math_Atan2S(dz, dx)          [OoT convention: z first]
                    //   stickAngle  = worldYaw - inputDirYaw
                    //   relY        = Math_CosS(stickAngle) * mag
                    //   relX        = -Math_SinS(stickAngle) * mag
                    // Magnitude is distance-scaled: sprint when far, walk when
                    // close, zero within the stop radius so Link's own
                    // deceleration carries him the last few units.
                    static bool sAnimHookLogged = false;
                    if (!sAnimHookLogged) {
                        SPDLOG_INFO("[Follower] animHook firing for ACTOR_PLAYER");
                        sAnimHookLogged = true;
                    }

                    // --- Crawlspace override (2026-04-22) ---
                    // When Link is in PLAYER_STATE2_CRAWLING, the camera is
                    // locked to the tunnel axis and input is simplified to
                    // forward/back along that axis. Our camera-relative stick
                    // projection may or may not land on that axis cleanly, so
                    // we hardcode full forward (stick_y = 127) while the flag
                    // is set — crawlspaces in OoT are always "push forward to
                    // advance, press backward to back out". Zero X because X
                    // input during crawl is ignored anyway.
                    //
                    // Edge-logged: one log entry on entry into CRAWLING, one
                    // on exit — so we can tell from the test log whether this
                    // path fired. Not per-frame (would flood the log).
                    static bool sWasCrawling = false;
                    bool nowCrawling = (sf2 & PLAYER_STATE2_CRAWLING) != 0;
                    if (nowCrawling && !sWasCrawling) {
                        SPDLOG_INFO("[Follower] Crawlspace override ENTER "
                                    "(PLAYER_STATE2_CRAWLING set) — forcing stick_y=127");
                    } else if (!nowCrawling && sWasCrawling) {
                        SPDLOG_INFO("[Follower] Crawlspace override EXIT "
                                    "(PLAYER_STATE2_CRAWLING cleared)");
                    }
                    sWasCrawling = nowCrawling;

                    if (followerPostTeleportFrames > 0) {
                        // Test 5 post-teleport hold. Zero the stick (and
                        // press-button stick bits) for kPostTeleportHoldFrames
                        // after any teleport so Link settles at leaderPos
                        // before state-machine movement drives him toward
                        // sideTarget (which can be inside a wall if the
                        // leader was standing next to one).
                        input.cur.stick_x = 0;
                        input.cur.stick_y = 0;
                        input.rel.stick_x = 0;
                        input.rel.stick_y = 0;
                        followerPostTeleportFrames--;
                        if (followerPostTeleportFrames == 0) {
                            SPDLOG_INFO("[Follower] Post-teleport hold complete");
                        }
                    } else if (isMoving && nowCrawling) {
                        input.cur.stick_x = 0;
                        input.cur.stick_y = 127;
                        input.rel.stick_x = 0;
                        input.rel.stick_y = 127;
                    } else if (followerClimbDismountFrames > 0 && !blockedByPlayerState) {
                        // Bug C (log 69) — ladder/vine dismount forward-hold.
                        // Project the held world-space yaw (captured at the
                        // CLIMBING→IDLE transition as Link's shape.rot.y,
                        // which matches the leader's facing per the CLIMBING
                        // state body) into camera-relative stick axes. Full
                        // magnitude so Link walks briskly inward past the
                        // ledge rim. Counter decrements every frame; when it
                        // reaches zero, the normal move logic resumes.
                        Camera* cam = GET_ACTIVE_CAM(gPlayState);
                        s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                        s16 stickAngle  = followerClimbDismountYaw - inputDirYaw;
                        s8  stickY = (s8)( Math_CosS(stickAngle) * 127.0f);
                        s8  stickX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                        input.cur.stick_x = stickX;
                        input.cur.stick_y = stickY;
                        input.rel.stick_x = stickX;
                        input.rel.stick_y = stickY;
                        followerClimbDismountFrames--;
                        if (followerClimbDismountFrames == 0) {
                            SPDLOG_INFO("[Follower] Dismount forward-hold complete");
                        }
                    } else if (followerAIState == FollowerAIState::CLIMBING) {
                        // Bug 2 (2026-04-22): natural ladder grab + climb.
                        // Two phases:
                        //   (a) Not on ladder yet (nowOnLadder == false):
                        //       follower is approaching the ladder from the
                        //       side. Drive stick forward toward
                        //       followerMoveTarget (= leader's XZ at leader's
                        //       Y) using the standard camera-relative
                        //       projection so OoT's collision sees Link
                        //       walking into the ladder face-first and
                        //       attaches him.
                        //   (b) On ladder (nowOnLadder == true): OoT uses
                        //       raw stick_y for vertical motion. Direction
                        //       comes from comparing leader.y to follower.y:
                        //       leader higher → up, lower → down, within
                        //       tolerance → zero (we've reached them).
                        //       Stick_x is irrelevant during climb.
                        Vec3f p2w = actor->world.pos;
                        if (nowOnLadder) {
                            // Vertical: compare leader Y to follower Y.
                            f32 dyL = followerMoveTarget.y - p2w.y;
                            static constexpr f32 kClimbYTolerance = 8.0f;
                            s8  ladderY = 0;
                            if (dyL >  kClimbYTolerance)      ladderY =  127;
                            else if (dyL < -kClimbYTolerance) ladderY = -127;
                            // Test 6 (log 74) — lateral tracking on vine
                            // walls. OoT's ladder climb code ignores
                            // stick_x (ladder is single-column), but vine
                            // climb uses stick_x for lateral movement along
                            // the wall face. Inject a camera-relative
                            // horizontal component from the XZ delta so the
                            // follower tracks the leader sideways; on
                            // ladders this is a no-op (OoT clamps it), on
                            // vines it slides Link along the vine face.
                            //
                            // Gate on Δxz > tolerance so idle stand-still
                            // climbs (follower holding at leader Y) don't
                            // emit phantom lateral input.
                            f32 dxL = followerMoveTarget.x - p2w.x;
                            f32 dzL = followerMoveTarget.z - p2w.z;
                            f32 dxzSq = dxL * dxL + dzL * dzL;
                            s8  ladderX = 0;
                            static constexpr f32 kClimbXzTolerance = 10.0f;
                            if (dxzSq > kClimbXzTolerance * kClimbXzTolerance) {
                                Camera* cam = GET_ACTIVE_CAM(gPlayState);
                                s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                                s16 worldYaw    = Math_Atan2S(dzL, dxL);
                                s16 stickAngle  = worldYaw - inputDirYaw;
                                ladderX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                            }
                            input.cur.stick_x = ladderX;
                            input.cur.stick_y = ladderY;
                            input.rel.stick_x = ladderX;
                            input.rel.stick_y = ladderY;
                        } else {
                            // Walk toward ladder. Reuse the standard
                            // camera-relative inversion (smaller copy here so
                            // we can ignore the magnitude curve — full
                            // forward into the ladder gets the grab).
                            f32 dx = followerMoveTarget.x - p2w.x;
                            f32 dz = followerMoveTarget.z - p2w.z;
                            if (dx * dx + dz * dz > 1.0f) {
                                Camera* cam = GET_ACTIVE_CAM(gPlayState);
                                s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                                s16 worldYaw    = Math_Atan2S(dz, dx);
                                s16 stickAngle  = worldYaw - inputDirYaw;
                                s8  stickY = (s8)( Math_CosS(stickAngle) * 127.0f);
                                s8  stickX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                                input.cur.stick_x = stickX;
                                input.cur.stick_y = stickY;
                                input.rel.stick_x = stickX;
                                input.rel.stick_y = stickY;
                            } else {
                                input.cur.stick_x = 0; input.cur.stick_y = 0;
                                input.rel.stick_x = 0; input.rel.stick_y = 0;
                            }
                        }
                    } else if (isMoving && !blockedByPlayerState) {
                        Vec3f p2w = actor->world.pos;
                        f32 dx = followerMoveTarget.x - p2w.x;
                        f32 dz = followerMoveTarget.z - p2w.z;
                        f32 distSq = dx * dx + dz * dz;
                        if (distSq > 1.0f) {
                            f32 dist = sqrtf(distSq);
                            f32 magF;
                            if      (dist > 250.0f) magF = 127.0f; // sprint — leader far ahead
                            else if (dist >  60.0f) magF = 100.0f; // run
                            else if (dist >  30.0f) magF =  60.0f; // walk (decelerate)
                            else                    magF =   0.0f; // coast to a stop
                            Camera* cam = GET_ACTIVE_CAM(gPlayState);
                            s16 inputDirYaw  = Camera_GetInputDirYaw(cam);
                            s16 worldYaw     = Math_Atan2S(dz, dx); // z first per OoT convention
                            s16 stickAngle   = worldYaw - inputDirYaw;
                            s8  stickY = (s8)( Math_CosS(stickAngle) * magF);
                            s8  stickX = (s8)(-Math_SinS(stickAngle) * magF);
                            input.cur.stick_x = stickX;
                            input.cur.stick_y = stickY;
                            input.rel.stick_x = stickX;
                            input.rel.stick_y = stickY;
                        } else {
                            // Already at target — no stick
                            input.cur.stick_x = 0; input.cur.stick_y = 0;
                            input.rel.stick_x = 0; input.rel.stick_y = 0;
                        }
                    } else {
                        input.cur.stick_x = 0; input.cur.stick_y = 0;
                        input.rel.stick_x = 0; input.rel.stick_y = 0;
                    }

                    // --- Auto-press A when the "Climb" action is available ---
                    // PLAYER_STATE2_DO_ACTION_CLIMB is the flag the engine uses
                    // to show "Climb" on the A-button prompt. It covers:
                    //   - Link hanging off a land ledge (PLAYER_STATE1_HANGING_OFF_LEDGE
                    //     is also set; the two flags agree).
                    //   - Link swimming at a water-exit ledge where the engine
                    //     accepts an A-press to climb out of the water.
                    // Injecting BTN_A whenever DO_ACTION_CLIMB is set handles
                    // both cases without needing to distinguish land vs water.
                    // (Observed 2026-04-21: relying on HANGING_OFF_LEDGE alone
                    // left the follower stuck at the water's edge.)
                    if (sf2 & PLAYER_STATE2_DO_ACTION_CLIMB) {
                        input.press.button |= BTN_A;
                        input.cur.button   |= BTN_A;
                        SPDLOG_INFO("[Follower] BTN_A climb (DO_ACTION_CLIMB)");
                    }

                    // --- Nav system Shape A hang-state resolution (commit 6c) ---
                    // Closes the documented #169 residual: when the follower
                    // enters PLAYER_STATE1_HANGING_OFF_LEDGE, no system decided
                    // whether to climb up (BTN_A) or drop down (BTN_B). The
                    // existing DO_ACTION_CLIMB path handles climb-up but not
                    // drop-down. With nav.VerticalTeleport on, we read leader
                    // altitude vs follower altitude and inject the right
                    // button. Plan §9 thresholds:
                    //   target Y > follower Y + 30  → climb up   (BTN_A)
                    //   target Y < follower Y - 80  → drop down  (BTN_B)
                    //   otherwise                   → BTN_A (default — bias
                    //                                  toward climb up).
                    // BTN_A path overlaps DO_ACTION_CLIMB above; idempotent.
                    // BTN_B is the new edge case.
                    {
                        const bool hangFlag = (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) != 0;
                        const bool navOn    =
                            CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0 &&
                            CVarGetInteger(CVAR_ENHANCEMENT("Nav.VerticalTeleport"), 0) != 0;
                        static bool sWasHanging = false;
                        if (hangFlag && navOn) {
                            constexpr f32 kHangResolveAboveThreshold = 30.0f;
                            constexpr f32 kHangResolveBelowThreshold = 80.0f;
                            // followerMoveTarget carries the follower's
                            // current navigation goal (leader pos in
                            // FOLLOW; tracked target in ENGAGE/ATTACK).
                            // leaderPos itself is only in scope at the
                            // top-level frame block — not here inside
                            // the CLIMBING-state input-injection branch.
                            f32 targetY  = followerMoveTarget.y;
                            f32 dy       = targetY - player->actor.world.pos.y;
                            bool dropDown = (dy < -kHangResolveBelowThreshold);
                            if (dropDown) {
                                input.press.button |= BTN_B;
                                input.cur.button   |= BTN_B;
                            } else {
                                // BTN_A injection above (DO_ACTION_CLIMB) covers
                                // climb-up. If the prompt isn't showing for some
                                // reason, force the press anyway so the
                                // resolution still fires.
                                input.press.button |= BTN_A;
                                input.cur.button   |= BTN_A;
                            }
                            if (!sWasHanging) {
                                SPDLOG_INFO("[Follower] BTN_{} hang-state resolution "
                                            "(dy={:.1f}, above={:.1f}, below={:.1f})",
                                            dropDown ? "B" : "A", dy,
                                            kHangResolveAboveThreshold,
                                            kHangResolveBelowThreshold);
                                sWasHanging = true;
                            }
                        } else if (sWasHanging) {
                            sWasHanging = false;
                        }
                        (void)hangFlag;  // suppress unused-warning when navOn is false
                    }

                    // --- Phase A — auto-press A when OoT prompts "Enter" ---
                    // PLAYER_STATE2_DO_ACTION_ENTER is the flag the engine
                    // sets to display "Enter" on the A-button prompt — fires
                    // whenever Link is adjacent to an openable door / passage
                    // that accepts A. OoT handles the actor-specific detection
                    // (En_Door trigger volume, Door_Shutter cylinder, grotto
                    // Door_Ana, certain transition actors) for us; we just
                    // inject the press.
                    //
                    // Doesn't solve the G11 "leader in different room"
                    // deactivation on its own — Phase B (#169, deferred) is
                    // the handoff that keeps the follower active long enough
                    // to WALK to the door. Phase A is still valuable standalone:
                    // any time the follower is naturally near an openable
                    // passage (FOLLOW toward a leader beside an open doorway,
                    // RETURN pathway, or manual user re-activate after G11
                    // placed the follower near a door), the door opens without
                    // user intervention.
                    //
                    // Edge-logged so the test log shows when this fires. Not
                    // per-frame (would flood when follower is idle near a door).
                    {
                        // Test 7 (user report) — Phase A was catching
                        // crawlspaces only. Doors set a different field:
                        // player->doorType becomes nonzero when Link is
                        // adjacent to an openable door (En_Door /
                        // Door_Shutter / Door_Toki — anything with a
                        // transition actor collider). PLAYER_DOORTYPE_FAKE
                        // (=3) is the trap-door variant that damages Link;
                        // explicitly exclude it so the follower doesn't
                        // self-inflict.
                        bool enterPromptActive = (sf2 & PLAYER_STATE2_DO_ACTION_ENTER) != 0;
                        bool doorInRange       = (player->doorType != PLAYER_DOORTYPE_NONE) &&
                                                 (player->doorType != PLAYER_DOORTYPE_FAKE);
                        bool promptActive      = enterPromptActive || doorInRange;
                        static bool sWasAtDoor = false;
                        if (promptActive && !sWasAtDoor) {
                            SPDLOG_INFO("[Follower] BTN_A door prompt ({}{})",
                                        enterPromptActive ? "DO_ACTION_ENTER" : "",
                                        doorInRange
                                            ? (enterPromptActive ? " + doorType" : "doorType")
                                            : "");
                        } else if (!promptActive && sWasAtDoor) {
                            SPDLOG_INFO("[Follower] BTN_A door EXIT (prompt cleared)");
                        }
                        sWasAtDoor = promptActive;
                        if (promptActive) {
                            input.press.button |= BTN_A;
                            input.cur.button   |= BTN_A;
                            // Test 10 (log 79, Bug 1) — arm cooldown.
                            // Player_Update consumes this press THIS FRAME
                            // to start the door-open animation and clears
                            // doorType in the same pass, before OnGameFrame-
                            // Update's deactivate-check reads press.button.
                            // Without this counter, the mask's doorType
                            // condition has already flipped to NONE by
                            // deactivate-check time → BTN_A in press,
                            // mask doesn't strip, follower self-cancels
                            // ("state=IDLE press=0x8000").
                            static constexpr int kDoorPressCooldownFrames = 5;
                            followerDoorPressCooldown = kDoorPressCooldownFrames;
                        }
                    }

                    // Test 6 (log 74) — BTN_Z tap-to-refresh (was hold).
                    // User reported: a held Z without an acquirable target
                    // just locks the CAMERA (OoT's fallback when no lock
                    // candidate is in the attention cone), which keeps Link
                    // facing whichever direction he was looking when Z went
                    // down. Wall Skulltulas above/beside the follower never
                    // enter the cone, so the camera-hold makes the problem
                    // worse — follower can't face targets above him.
                    //
                    // New pattern: edge-press Z once every kZTapIntervalFrames
                    // (0.5 s at 60 fps; scales naturally at P2's 20 fps). The
                    // press is consumed by OoT's target-scan each time — if
                    // the enemy drifts into range (leader walks closer, or
                    // pitch injection lands the cone on target) a later tap
                    // catches it. No cur.button hold, so the camera is free
                    // to adjust between taps.
                    //
                    // RANGED_ATTACK keeps its own Z cycle (below) — first-
                    // person aim mode does need Z held. The two states are
                    // mutually exclusive so there's no double-fire.
                    static constexpr int kZTapIntervalFrames = 30;
                    if (followerAIState == FollowerAIState::ENGAGE ||
                        followerAIState == FollowerAIState::ATTACK) {
                        if (followerStateFrames % kZTapIntervalFrames == 0) {
                            input.press.button |= BTN_Z;
                            input.cur.button   |= BTN_Z;
                        }
                    }

                    // --- Attack: face enemy + inject BTN_B at start of each charge phase ---
                    if (followerAIState == FollowerAIState::ATTACK) {
                        // Keep shape.rot.y facing the enemy here (BEFORE Player_Update) so
                        // that when BTN_B is processed by OoT this frame, the swing direction
                        // is current.  OnGameFrameUpdate also sets it (after Player_Update) to
                        // maintain facing during the animation; both assignments are consistent.
                        // Task 3: suppress both the facing update and the BTN_B injection
                        // when the target is dead or dying. The state-machine RETURN
                        // transition above catches it one frame earlier on the next
                        // OnGameFrameUpdate; this gate prevents a final rogue swing in the
                        // gap between the killing hit and the state transition.
                        //
                        // Mirrors the two-signal check in the ATTACK state (see banner
                        // comment there): colChkInfo.health catches actors that decrement
                        // their own health; EnemyNetId::hasLocalDeath / pendingNaturalDeath
                        // catches AC_HIT-only actors whose health never moves (Karebaba
                        // and most simple enemies).
                        bool targetAlive = (followerTargetEnemy != nullptr &&
                                            followerTargetEnemy->update != nullptr &&
                                            followerTargetEnemy->colChkInfo.health > 0);
                        if (targetAlive) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr) {
                                EnemyStateSync::AuditBooleansVsPhase(*ext, "Follower.targetAliveCheck");
                                if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) ||
                                    EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase)) {
                                    targetAlive = false;
                                }
                            }
                        }
                        if (targetAlive) {
                            f32 ex = followerTargetEnemy->world.pos.x - actor->world.pos.x;
                            f32 ez = followerTargetEnemy->world.pos.z - actor->world.pos.z;
                            f32 eDistSq = ex * ex + ez * ez;
                            if (eDistSq > 1.0f) {
                                actor->shape.rot.y = Math_Atan2S(ez, ex); // z first per OoT convention
                            }
                            if (followerStateFrames % 20 == 0) {
                                // Test 6 (log 74) — sword-range jump-attack
                                // gate. Vanilla sword reach is ~50 u (kSwingReach);
                                // when standoff puts the follower at 60-96 u
                                // from enemy, BTN_B swings whiff. Jump attack
                                // (locked + A + stick-forward) lunges Link
                                // ~80+ u and connects at the end of the swing.
                                //
                                // Gate BTN_A on PLAYER_STATE1_HOSTILE_LOCK_ON
                                // because A without lock-on triggers a roll
                                // instead of jump-attack. If we're not yet
                                // locked, fall back to BTN_B — worse reach
                                // but no wrong-input risk (Z-tap cadence
                                // above will retry the lock next cycle).
                                bool locked = (sf1 & PLAYER_STATE1_HOSTILE_LOCK_ON) != 0;
                                bool tooFarForSwing = (eDistSq > 50.0f * 50.0f); // kSwingReach²
                                if (locked && tooFarForSwing) {
                                    input.press.button |= BTN_A;
                                    input.cur.button   |= BTN_A;
                                    SPDLOG_INFO("[Follower] ATTACK jump-slash BTN_A "
                                                "(dist={:.0f} locked)", sqrtf(eDistSq));
                                } else {
                                    input.press.button |= BTN_B;
                                    input.cur.button   |= BTN_B;
                                    SPDLOG_INFO("[Follower] ATTACK injecting BTN_B (stateFrames={} "
                                                "dist={:.0f} locked={})",
                                                followerStateFrames, sqrtf(eDistSq), locked ? 1 : 0);
                                }
                            } else {
                                // Bug D / Test 5 (log 71) — shield between
                                // swings. Previous gate `eDistSq < 50*50`
                                // almost never fired: typical ATTACK frames
                                // sit at 69-96 u from enemy (user's test had
                                // zero BTN_R log lines). Any time we're in
                                // ATTACK state (already inside attackRange)
                                // and NOT on a swing frame, shield up.
                                // BLOCK pattern below sets both press+cur
                                // every frame; R-hold is continuous so
                                // edge-replay is harmless (unlike BTN_A
                                // which would mis-interpret as new press).
                                input.press.button |= BTN_R;
                                input.cur.button   |= BTN_R;
                                if (followerStateFrames % 20 == 10) {
                                    SPDLOG_INFO("[Follower] ATTACK shield BTN_R "
                                                "(stateFrames={} distToEnemy={:.0f})",
                                                followerStateFrames, sqrtf(eDistSq));
                                }
                            }
                        }
                    }

                    // G4 — BLOCK: hold BTN_R to plant the shield. OoT treats
                    // R-hold as a continuous shielding input, so set both .cur
                    // and .press every frame.
                    if (followerAIState == FollowerAIState::BLOCK) {
                        input.press.button |= BTN_R;
                        input.cur.button   |= BTN_R;
                    }

                    // Item pickup — dismiss item-get and talking text boxes
                    // with BTN_A every 20 frames. PLAYER_STATE1_GETTING_ITEM
                    // is set during the "raised-item" cutscene (first-time
                    // pickups of bombs, arrows, keys, heart pieces); TALKING
                    // catches the text-advance portion. Matches the BTN_B
                    // swing cadence so we're not slamming BTN_A every frame.
                    // Fires regardless of follower state (pickup can occur
                    // during COLLECT_ITEM, but also in any other state if
                    // Link steps on an item by accident).
                    if (sf1 & (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_TALKING)) {
                        if (followerStateFrames % 20 == 0) {
                            input.press.button |= BTN_A;
                            input.cur.button   |= BTN_A;
                        }
                    }

                    // G6/G7/G8 — RANGED_ATTACK: draw weapon, aim, release-to-fire.
                    //
                    // Bug 4 (2026-04-22) — release-to-fire cycle. Prior code
                    // pressed Z + C-button + A every frame. Three problems:
                    //   1. Setting input.press.button every frame = OoT sees
                    //      "just pressed" every frame, so the slingshot draw
                    //      animation never settles into ready-to-fire.
                    //   2. A-press before Link is fully drawn = roll/jump
                    //      attack, not fire (matches user's "rolled instead").
                    //   3. The natural OoT firing path is "release the
                    //      C-button to auto-fire the primed shot" — A-press
                    //      is the secondary path.
                    //
                    // New cycle: hold Z + C-button (cur only, press on entry
                    // edge), drop the C-button for one frame every kFireCycleFrames
                    // to trigger auto-fire. Re-press the next frame to re-draw.
                    //
                    // Option B — the C-button press is only meaningful if
                    // followerActiveCSlot != 0xFF (CVar enabled AND player has
                    // a slingshot/bow). Otherwise the C-button block is
                    // skipped; Z is still held but no fire happens.
                    if (followerAIState == FollowerAIState::RANGED_ATTACK) {
                        static constexpr int kFireCycleFrames = 60;
                        // Z: edge-press on entry, hold via cur thereafter.
                        if (followerStateFrames == 0) {
                            input.press.button |= BTN_Z;
                        }
                        input.cur.button |= BTN_Z;

                        u16 cBtn = 0;
                        switch (followerActiveCSlot) {
                            case 0: cBtn = BTN_CLEFT;  break;
                            case 1: cBtn = BTN_CDOWN;  break;
                            case 2: cBtn = BTN_CRIGHT; break;
                            default: break;
                        }
                        if (cBtn != 0) {
                            int phase = followerStateFrames % kFireCycleFrames;
                            // Phase 0: edge-press the C-button (start draw)
                            // Phase 1 .. (kFireCycleFrames-3): hold via cur (aim/prime)
                            // Phase (kFireCycleFrames-2, -1): RELEASE for TWO frames
                            //   (don't set cur/press). Test 5 (log 71) — early
                            //   cycles missed the fire entirely despite the
                            //   release-to-fire path; one-frame release was
                            //   too short for OoT to consume as a fire event.
                            //   Two frames is more reliable; later cycles in
                            //   the same log did fire successfully, so the
                            //   pattern works once OoT's state settles.
                            if (phase == 0) {
                                input.press.button |= cBtn;
                                input.cur.button   |= cBtn;
                                SPDLOG_INFO("[Follower] RANGED_ATTACK draw cycle (cSlot={})",
                                            (int)followerActiveCSlot);
                            } else if (phase >= kFireCycleFrames - 2) {
                                // Release window — do NOT add cBtn to cur
                                // or press for these two frames.
                                if (phase == kFireCycleFrames - 2) {
                                    SPDLOG_INFO("[Follower] RANGED_ATTACK release-to-fire "
                                                "(cSlot={} 2-frame window)",
                                                (int)followerActiveCSlot);
                                }
                            } else {
                                input.cur.button |= cBtn;
                            }
                        }

                        // Test 5 (log 71) — aim-pitch injection. First-person
                        // aim (slingshot/bow drawn) uses stick_y for camera
                        // pitch. Ceiling Skulltulas (target Δy > ~60 u)
                        // require aiming UP — without this injection Link
                        // fires forward into nothing when unlocked.
                        //
                        // Test 7 (user report): "When locked on, aiming is
                        // automatic, no additional input should be required."
                        // OoT's Z-lock drives the camera onto the target
                        // directly; injecting stick_y on top overrides that
                        // and points aim elsewhere. Gate on HOSTILE_LOCK_ON
                        // being CLEAR — inject pitch only as a fallback
                        // when lock-on hasn't acquired the target.
                        //
                        // Sign convention: OoT first-person aim uses
                        // positive stick_y = look up.
                        bool lockedOn = (sf1 & PLAYER_STATE1_HOSTILE_LOCK_ON) != 0;
                        if (!lockedOn &&
                            followerTargetEnemy != nullptr &&
                            followerTargetEnemy->update != nullptr) {
                            f32 dy = followerTargetEnemy->world.pos.y - actor->world.pos.y;
                            s8  pitchY = 0;
                            if      (dy >  60.0f) pitchY =  64;  // look up
                            else if (dy < -60.0f) pitchY = -64;  // look down
                            if (pitchY != 0) {
                                input.cur.stick_x = 0;
                                input.cur.stick_y = pitchY;
                                input.rel.stick_x = 0;
                                input.rel.stick_y = pitchY;
                                if (followerStateFrames % 20 == 0) {
                                    SPDLOG_INFO("[Follower] RANGED_ATTACK aim-pitch (no lock) "
                                                "stick_y={} dy={:.0f}",
                                                (int)pitchY, dy);
                                }
                            }
                        }

                        // Test 6 (log 74) — BTN_A "backup fire" path REMOVED.
                        // User reported follower rolling into walls when
                        // engaging Skullwalltulas; we guarded BTN_A on
                        // PLAYER_STATE1_READY_TO_FIRE but that flag has a
                        // transient window where Link's pose is still
                        // "walking with slingshot drawn" rather than
                        // "aim stance primed". BTN_A in that window triggers
                        // a jump-roll forward, which is exactly the wall-
                        // dive symptom. The C-button release-to-fire cycle
                        // above fires reliably once draw completes, so the
                        // A-press backup isn't needed.
                    }
}

// ---------------------------------------------------------------------------
// Per-state handlers (Phase 1 commit 6+) — peeled off TickFollower's switch.
// Each handler reads / writes Anchor:: state directly through `this` and takes
// explicit parameters for parent-function locals it needs (player, p2Pos,
// etc.). Handlers don't see the lambda-locals (leaderPos / leaderActor /
// distToLeader) unless explicitly passed. Future commits expand the set as
// more states are extracted.
// ---------------------------------------------------------------------------

void Anchor::HandleStateStandby() {
    // STANDBY: hold position for kAttackDuration frames, then drop to
    // ENGAGE so the next swing fires. Reserved for the G19 Gohma weak-
    // point window; currently entered only as a placeholder. No locals
    // from TickFollower required.
    if (followerStateFrames >= kAttackDuration) {
        followerAIState     = FollowerAIState::ENGAGE;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] STANDBY→ENGAGE (window expired)");
    }
}

void Anchor::HandleStateBlock(Player* player, const Vec3f& p2Pos) {
    // BLOCK: shield up, faces the incoming projectile. Used against
    // Mad Scrub (En_Dekunuts) deku-nut shots to reflect them. Holds for
    // kAttackDuration frames per cycle, then drops to ATTACK to swing
    // on the (now-stunned) scrub. Bails to RETURN if target despawns.
    if (followerTargetEnemy == nullptr ||
        followerTargetEnemy->update == nullptr) {
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] BLOCK→RETURN (target gone)");
        return;
    }
    // shape.rot.y points at target so the shield faces the incoming
    // projectile. Position is held by zeroed stick (see TickFollowerInput
    // isMoving exclusion).
    f32 ex = followerTargetEnemy->world.pos.x - p2Pos.x;
    f32 ez = followerTargetEnemy->world.pos.z - p2Pos.z;
    if (ex * ex + ez * ez > 1.0f) {
        player->actor.shape.rot.y = YawToward(ex, ez);
    }
    // Hold the shield for kAttackDuration frames per cycle, then drop
    // to ATTACK to swing on the (now-stunned) scrub.
    if (followerStateFrames >= kAttackDuration) {
        followerAIState     = FollowerAIState::ATTACK;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] BLOCK→ATTACK (shield cycle complete)");
    }
}
