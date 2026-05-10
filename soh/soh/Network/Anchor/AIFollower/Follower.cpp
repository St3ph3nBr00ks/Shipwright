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
#include "FollowerRecorder.h"
#include "../Anchor.h"
#include "soh/cvar_prefixes.h"
#include "../Common/ActorSyncHelpers.h"
#include "../Common/PlayerLookup.h"
#include "../Common/SceneAuthority.h"
#include "../Common/ItemEligibility.h"
#include "../Common/PauseLinkBuffer.h"
#include "../Common/ActorSyncScope.h"
#include "../Common/NavTraits.h"        // AnchorNav::IsNavSystemEnabled — Phase 2 master gate
#include "../Common/JumpResolver.h"     // AnchorNav::ResolveLedgeAhead — Phase 2 STUCK consumer
#include "../Common/VerticalTeleport.h" // AnchorNav::IsShapeAEligible — Phase 2 CLIMBING consumer
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // FindClimbAnchorAbove — autonomous climb
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

// ---------------------------------------------------------------------------
// Phase 2 — nav substrate consumer gate.
//
// Phase 2 of the SRP refactor (#173 / #169) replaces the bespoke pursuit /
// target-selection / steering code in the per-state handlers
// (HandleStateFollow, HandleStateEngage, HandleStateReturn, HandleStateStuck,
// HandleStateClimbing) with calls to the Anchor nav substrate
// (ActorTrail::ComputePathTo, TargetSelection::ChooseTarget,
// GroundFollowing::GetGroundFollowingBearing, JumpResolver::ResolveLedgeAhead,
// VerticalTeleport's Shape A wrapper). Plan refs:
// Plans/anchor_code_decoupling.md (#173) +
// Plans/nav_system_implementation_plan.md §Phase 2 consumer wiring.
//
// This gate sits on top of the master Nav CVar so a user enabling Nav.* for
// debug-overlay viewing or other consumers does NOT inadvertently change
// follower behaviour. The Phase 2 substrate-consumer path engages only when
// BOTH gates are on:
//   gEnhancements.Nav.Enabled              (master)
//   gEnhancements.Nav.AiFollowerConsumer   (this gate)
//
// Default off; ships and stays off permanently per Flotilla policy for
// vanilla-altering features (memory: feedback_vanilla_altering_default_off.md).
// Per-state handlers branch on this predicate to pick between the legacy
// bespoke code path (when off — current default) and nav substrate calls
// (when on — Phase 2 work in progress).
// ---------------------------------------------------------------------------

#define CVAR_NAV_AI_FOLLOWER_CONSUMER CVAR_ENHANCEMENT("Nav.AiFollowerConsumer")

bool IsAiFollowerNavSubstrateEnabled() {
    return AnchorNav::IsNavSystemEnabled() &&
           CVarGetInteger(CVAR_NAV_AI_FOLLOWER_CONSUMER, 0) != 0;
}

void RegisterFollowerModule() {
    // No-op log. The two follower hooks (OnGameFrameUpdate state-machine
    // driver + ShouldActorUpdate input injection) are (re-)registered from
    // Anchor::RegisterFollowerHooks per-connect, not from this ShipInit
    // boot-time entry, because they need Anchor:: state access and
    // isConnected re-registration. Kept so the namespace + ShipInit boot
    // path stays exercised; serves as the natural extension point if the
    // module ever needs boot-time setup independent of Anchor enable/disable.
    SPDLOG_DEBUG("[AiFollower] Follower module loaded; per-connect hooks live in Anchor::RegisterFollowerHooks");
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

// Distance at which FOLLOW switches back to IDLE (and RETURN settles).
// Was static constexpr inside TickFollower; promoted for HandleStateReturn.
static constexpr f32 kFollowThreshold = 100.0f;

// P3.10 (user 2026-05-09 — "if the leader is standing on a platform,
// the AI Follower will stand under the leader instead of walking up a
// sloped surface to get onto the platform"): vertical gate for the
// FOLLOW→IDLE / RETURN→IDLE transitions. When |dy| to the final goal
// exceeds this threshold, the follower hasn't really arrived even if
// XZ distance is small — it's standing UNDER the leader rather than
// AT the leader. Keep FOLLOW/RETURN active so the path consumer keeps
// trying to find a slope / stairs / ladder route. G10 / G14 leash-
// timeout safety nets eventually fire if no path exists.
static constexpr f32 kFollowYThreshold = 50.0f;

// Phase 2 — NavPath subgoal-reach threshold (XZ distance). When the
// follower is closer than this to the current subgoal, advance the path
// cursor to the next subgoal. Tighter than kFollowThreshold so intermediate
// subgoals are visited en route, not skipped.
static constexpr f32 kNavPathSubgoalReach    = 40.0f;
// Phase 2 — distance at which the held NavPath is considered stale due to
// the leader having drifted far from where the path was originally captured.
// 50u matches a single FOLLOW-step's typical movement; once the leader has
// moved that far, recompute rather than chasing the captured target.
static constexpr f32 kNavPathTargetDriftRefresh = 50.0f;

// Frames the dismount-forward-hold counter is armed for after CLIMBING→IDLE
// (Bug C, log 69). Was static constexpr inside TickFollower; promoted for
// HandleStateClimbing.
static constexpr int kClimbDismountHoldFrames = 9;

// STUCK fallback-nudge tunables. Promoted for HandleStateStuck.
static constexpr f32 kMoveSpeed     = 4.0f;  // units/frame for the position-override nudge
static constexpr int kStuckRecovery = 25;    // frames of nudge before retrying FOLLOW

// Y-axis floor-difference threshold — reject targets / items more than
// this many units above or below the follower. Promoted for
// HandleStateCollectItem. Multiple TickFollower references will continue
// to resolve here once the local declaration is removed.
static constexpr f32 kMaxYDelta = 120.0f;

// Leader leash — abandon COLLECT_ITEM when the leader strays beyond
// this distance. Promoted for HandleStateCollectItem.
static constexpr f32 kMaxLeash = 800.0f;

// Enemy detection radius (XZ) for IDLE→ENGAGE transitions. Promoted for
// HandleStateIdle.
static constexpr f32 kEngageRange = 350.0f;

// Item-pickup tunables — XZ scan radius, grace-period (human-first-pick
// window), and the COLLECT_ITEM walking timeout. Promoted for
// HandleStateIdle / HandleStateFollow / HandleStateCollectItem.
static constexpr f32 kItemProximity      = 200.0f;
static constexpr int kItemGraceFrames    = 180;
static constexpr int kItemCollectTimeout = 300;

// Stuck-detection tunables for HandleStateFollow.
static constexpr int kStuckCheckInterval = 20;     // frames between progress checks
static constexpr f32 kStuckMinProgress   = 5.0f;   // min units travelled per interval
static constexpr int kStuckCycleWindow   = 300;    // G12 cycle-count reset window (frames)

// Combat tunables for HandleStateEngage / HandleStateAttack.
// kSwordVerticalReach — Link's effective vertical sword reach. Above this,
// ranged-required enemies are routed to RANGED_ATTACK instead of ATTACK.
// kSwingReach          — sword arc XZ reach. Standoff = attackRange - kSwingReach.
static constexpr f32 kSwordVerticalReach = 40.0f;
static constexpr f32 kSwingReach         = 50.0f;

// Per-enemy combat policy (G4 / G6-G8). Promoted from TickFollower-local
// arrays so HandleStateEngage / HandleStateAttack can reach them.
//   kShieldReflectEnemyIds — ENGAGE routes to BLOCK (shield reflect).
//   kRangedRequiredEnemyIds — ENGAGE routes to RANGED_ATTACK when target
//                             is also above kSwordVerticalReach.
static constexpr s16 kShieldReflectEnemyIds[] = { ACTOR_EN_DEKUNUTS };
static constexpr s16 kRangedRequiredEnemyIds[] = {
    ACTOR_BOSS_GOMA, // Queen Gohma — ceiling phase
    ACTOR_EN_GOMA,   // Gohma larvae on the ceiling
    ACTOR_EN_SW,     // Skullwalltula on a wall vine
    ACTOR_EN_ST,     // Skulltula hanging from ceiling
};

inline bool IsShieldReflectEnemy(s16 id) {
    for (s16 e : kShieldReflectEnemyIds) { if (e == id) return true; }
    return false;
}

inline bool IsRangedRequiredEnemy(s16 id) {
    for (s16 e : kRangedRequiredEnemyIds) { if (e == id) return true; }
    return false;
}

// Per-enemy attack-range override. Default kAttackRange (80) stops Link
// at sword-tip contact, which walks the follower into the lunge arc of
// enemies whose damage volume sits ahead of world.pos (Karebaba head,
// Deku Baba stem-tip, Bari body-AoE). Promoted from a TickFollower-local
// lambda; was [] empty-capture so the move is trivial.
inline f32 GetAttackRangeForEnemy(s16 id) {
    switch (id) {
        case ACTOR_EN_KAREBABA: return 100.0f; // head lunges ~40 u
        case ACTOR_EN_DEKUBABA: return  90.0f; // stem-tip head
        case ACTOR_EN_VALI:     return 120.0f; // body AoE discharge
        default:                return  80.0f; // kAttackRange
    }
}

// Item filter — does the follower's character class want this drop?
// Was a parent-function lambda with `[]` empty capture (no parent-state
// dependencies); promoted to a file-scope free function so per-state
// handlers + Anchor::ScanForItemCandidate can call it.
inline bool FollowerWantsItem(Actor* item) {
    if (item == nullptr || item->id != ACTOR_EN_ITEM00 ||
        item->update == nullptr) {
        return false;
    }
    s16 itemType = (s16)(item->params & 0xFF);
    return ItemEligibility::CanPlayerCollectItem00(
        itemType, /*walletCapAware=*/false);
}

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
        followerAutonomousClimb = false;
        followerAutonomousClimbFrames = 0;
        SPDLOG_INFO("[Follower] Activated (menu)");
    } else {
        hasPendingTransition = false;
        pendingTransitionTimeoutFrames = 0;
        followerDoorHandoff = false;
        followerDoorHandoffFrames = 0;
        followerClimbDismountFrames = 0;
        followerCloseFailBaseline = 0.0f;
        followerCloseFailFrames = 0;
        followerAutonomousClimb = false;
        followerAutonomousClimbFrames = 0;
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
// Anchor::TickFollower — per-frame follower state-machine body. Originally
// moved verbatim from HookHandlers.cpp's OnGameFrameUpdate lambda body in
// Phase 1 commit 4 of the SRP refactor (#173 / #169); dedented to standard
// 4-space function-body depth in Phase 1 commit 14. The body uses unqualified
// member access for Anchor:: state (followerActive, clients, followerAIState,
// etc.); these resolve through 'this' since this is an Anchor:: method, just
// as they resolved through the lambda's 'this' capture before the move.
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
        //
        // P3.7 (user 2026-05-09 — "When the AI Follower starts to
        // climb onto a ledge, the AI Follower system appears to
        // disable"): also mask during the three Player_State1 climb
        // flags. The hoist transition fires this race:
        //   Frame N    : DO_ACTION_CLIMB set; we inject BTN_A; mask
        //                exempts BTN_A from deactivation. ✓
        //   Frame N+1  : Link enters CLIMBING_LEDGE animation;
        //                DO_ACTION_CLIMB clears. press.button may
        //                still carry BTN_A residue OR OoT itself may
        //                synthesise an input as part of the hoist.
        //                Without the climb-flag mask below, the
        //                check sees BTN_A unmasked → deactivate.
        // Adding HANGING_OFF_LEDGE | CLIMBING_LEDGE | CLIMBING_LADDER
        // to the mask covers the residual-press race AND any in-
        // climb state where OoT internally consumes A.
        if (player != nullptr &&
            (followerDoorPressCooldown > 0 ||
             followerClimbExitCooldown > 0 ||
             (player->stateFlags2 &
              (PLAYER_STATE2_DO_ACTION_CLIMB | PLAYER_STATE2_DO_ACTION_ENTER)) ||
             (player->stateFlags1 &
              (PLAYER_STATE1_HANGING_OFF_LEDGE |
               PLAYER_STATE1_CLIMBING_LEDGE |
               PLAYER_STATE1_CLIMBING_LADDER)) ||
             (player->doorType != PLAYER_DOORTYPE_NONE &&
              player->doorType != PLAYER_DOORTYPE_FAKE))) {
            deactivateCheck &= ~BTN_A;
        }
        // Hang-state resolution mask (originally Phase 1 commit 6c,
        // ungated in P3.4): the hang-state input-injection block in
        // TickFollowerInput injects BTN_A (climb-up) or BTN_B (drop)
        // based on Δy to leader. P3.4 ungated the INJECTION from the
        // Nav.VerticalTeleport CVar, but the corresponding mask
        // here was left gated — so when the gate is off and BTN_B
        // injection fires, the deactivate-check sees BTN_B unmasked
        // and turns the follower off (user 2026-05-09 follow-up
        // report: "AI Follower is still turning off when the
        // follower climbs onto a ledge"). Mirror the ungating here.
        //
        // Also extend the mask to cover the entire climb-state
        // family (HANGING_OFF_LEDGE | CLIMBING_LEDGE |
        // CLIMBING_LADDER) — same race as BTN_A in P3.7: any BTN_B
        // injected during hang may persist in press.button into
        // the CLIMBING_LEDGE / CLIMBING_LADDER frames.
        if (player != nullptr &&
            (player->stateFlags1 &
             (PLAYER_STATE1_HANGING_OFF_LEDGE |
              PLAYER_STATE1_CLIMBING_LEDGE |
              PLAYER_STATE1_CLIMBING_LADDER))) {
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
    if (followerClimbExitCooldown > 0) {
        followerClimbExitCooldown--;
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
    // kFollowThreshold moved to file-scope anonymous namespace
    // (Phase 1 commit 7) so per-state handlers can reference it.
    // kEngageRange moved to file-scope anonymous namespace
    // (Phase 1 commit 10) — used by HandleStateIdle.
    static constexpr f32 kAttackRange        = 80.0f;  // melee-contact radius (XZ)
    // kMaxYDelta moved to file-scope anonymous namespace
    // (Phase 1 commit 9) — used by HandleStateCollectItem.
    // kMaxLeash moved to file-scope anonymous namespace
    // (Phase 1 commit 9) — used by HandleStateCollectItem.
    // kMoveSpeed moved to file-scope anonymous namespace
    // (Phase 1 commit 8) — used by HandleStateStuck.
    // kStuckCheckInterval / kStuckMinProgress moved to file-scope
    // anonymous namespace (Phase 1 commit 11) — used by HandleStateFollow.
    // kStuckRecovery moved to file-scope anonymous namespace
    // (Phase 1 commit 8) — used by HandleStateStuck.
    // kAttackDuration moved to file-scope anonymous namespace
    // (Phase 1 commit 6) so per-state handlers can reference it.
    // G10 — leash-timeout teleport thresholds.
    static constexpr f32 kTeleportThreshold   = 1200.0f; // sustained XZ overrun that triggers teleport
    static constexpr int kTeleportDelayFrames = 120;     // ~2s at 60fps; debounces brief overshoots
    // G12 — STUCK escalation: N STUCK entries within window → teleport.
    static constexpr int kStuckCycleEscalation = 3;     // count threshold
    // kStuckCycleWindow moved to file-scope anonymous namespace
    // (Phase 1 commit 11) — used by HandleStateFollow.
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
    // kClimbDismountHoldFrames moved to file-scope anonymous
    // namespace (Phase 1 commit 7) so HandleStateClimbing can
    // reference it.
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
    // kItemProximity / kItemGraceFrames / kItemCollectTimeout
    // moved to file-scope anonymous namespace (Phase 1 commit 10).
    // G13 — boss scenes that warrant pre-emptive teleport on leader entry.
    // Only Deku Tree boss is in scope for the first dungeon demo (#167);
    // extend this list as later dungeons land.
    static constexpr s16 kBossScenes[] = { /* SCENE_DEKU_TREE_BOSS */ 0x11 };
    auto IsBossScene = [&](s16 scene) -> bool {
        for (s16 s : kBossScenes) { if (s == scene) return true; }
        return false;
    };
    // Combat helpers (IsShieldReflectEnemy, IsRangedRequiredEnemy,
    // GetAttackRangeForEnemy) and constants (kShieldReflectEnemyIds,
    // kRangedRequiredEnemyIds, kSwordVerticalReach, kSwingReach)
    // moved to file-scope anonymous namespace (Phase 1 commit 12)
    // so HandleStateEngage / HandleStateAttack can reach them.

    // Item pickup — need-gated whitelist via shared helper
    // (#193 Phase 0). Reserved for the human leader: progression
    // items, shields, tunics, keys, heart pieces — all return
    // false. AI follower keeps the legacy "rupees always" rule
    // (`walletCapAware = false`) since vanilla truncates surplus
    // and the follower acts in the local player's stead.
    // FollowerWantsItem + ScanForItemCandidate moved (Phase 1
    // commit 10): the wants-filter is now a file-scope free
    // function in the anonymous namespace; the scan is now
    // Anchor::ScanForItemCandidate. Both are called via
    // member-function syntax / file-scope name lookup at the
    // existing call sites.

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
        AnchorFollower::QueueRecorderEvent(std::string("teleport:") + reason);
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
            // P3.9 (user 2026-05-09 — "NPCs need the ability to target
            // the last door a player went into open it to pursue"):
            // pre-fix, the pending transition timed out at 720 frames
            // (~12 s @ 60 fps) and DEACTIVATED the follower. That was
            // too aggressive — if the follower couldn't reach the door
            // within the timeout (long room, blocked path, slow
            // pursuit), the follower simply gave up and turned off
            // rather than persisting toward the door.
            //
            // New behaviour: keep the pending transition active
            // indefinitely until one of these clears it:
            //   (a) follower's scene matches leader's destination
            //       (we already crossed the boundary somehow);
            //   (b) follower reaches the trigger and fires the
            //       transition (path (b) above);
            //   (c) a NEW SCENE_TRANSITION_HANDOFF arrives (newer
            //       leader transition supersedes — handled in the
            //       packet receive site, not here);
            //   (d) follower deactivates for an unrelated reason
            //       (manual stop, joystick cancel, etc).
            // Existing G10 leash + G14 close-fail timeouts still fire
            // when the follower is genuinely stuck; those are the
            // correct safety nets for "can't reach trigger." The
            // counter is kept for diagnostic logging only.
            if (pendingTransitionTimeoutFrames > 0) {
                pendingTransitionTimeoutFrames--;
                if (pendingTransitionTimeoutFrames == 0) {
                    SPDLOG_INFO("[Follower] Pending transition timeout window "
                                "elapsed; persisting pursuit (G10/G14 safety "
                                "nets handle genuinely stuck cases).");
                    // Intentionally do NOT clear hasPendingTransition or
                    // deactivate. The follower keeps pathing toward
                    // pendingTransitionPos until it succeeds or a safety
                    // net fires.
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
                    // P3.5 part 3 (user 2026-05-09 follow-up — "the AI
                    // Follower should not teleport when the leader
                    // crosses threshold into a different room, the AI
                    // Follower should continue navigating to the leader.
                    // Teleporting should only be used when is not
                    // possible to navigate to the leader"): drop the
                    // arm-edge teleport entirely. P3.5 part 2 still
                    // fired it within 30u as a "fine alignment" snap;
                    // the user's feedback says even that is wrong. The
                    // follower should always navigate to the door via
                    // FOLLOW. Only G10 leash overrun / G14 close-fail
                    // / G15 hang-state safety nets should ever
                    // teleport — those are "navigation actually
                    // impossible" detectors.
                    //
                    // doorTarget is still set below for FOLLOW to
                    // path toward; only the BONUS instant-snap is
                    // removed.
                    SPDLOG_INFO("[Follower] Leader crossed room boundary (ours={} leader={}) "
                                "— door handoff armed; teleport=NEVER(P3.5p3) "
                                "last-pos=({:.0f},{:.0f},{:.0f}) last-room={} yaw={} "
                                "target={:.0f},{:.0f},{:.0f} {} timeout={} frames",
                                (int)ourRoom, (int)leaderRoom,
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
                    // Bug 2 fix 2 (user 2026-05-10): suspend the G11
                    // countdown while the substrate has a non-empty
                    // path. The path-routing change in Bug 2 fix 1
                    // gives the follower a multi-waypoint route to
                    // the door — but the route may take longer than
                    // kDoorHandoffTimeout (6 sec) to traverse if the
                    // follower starts far from the door or the route
                    // is obstacle-rich. Pre-fix, G11 fired regardless
                    // of substrate progress, forcing a teleport that
                    // would have been unnecessary if given more time.
                    // Post-fix, G11 only counts down when there's no
                    // path to walk — preserves G11 as a "navigation
                    // actually impossible" guard while letting the
                    // substrate drive the common case.
                    const bool substrateHasPath =
                        AnchorFollower::IsAiFollowerNavSubstrateEnabled() &&
                        !followerNavPath.Empty();
                    if (!substrateHasPath) {
                        followerDoorHandoffFrames--;
                    }
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

    // G15 — hang-state safety teleport (P3.3, user 2026-05-09).
    // When the follower is in PLAYER_STATE1_HANGING_OFF_LEDGE without
    // transitioning into the CLIMBING_LEDGE hoist for an extended
    // period, the hang-state resolution (BTN_A climb-up / BTN_B drop)
    // either never fired or couldn't make progress. Teleporting to
    // the leader breaks the deadlock. Faster than G10 (no leash
    // distance threshold) since hanging is a known broken-state
    // signal in itself; G14's progress-delta heuristic also wouldn't
    // fire because the follower's XYZ is locked while hanging.
    //
    // Counter is gated so an actively-hoisting follower (CLIMBING_LEDGE
    // set) doesn't accumulate — the hoist is the desired transition.
    static constexpr int kHangTimeoutFrames = 180; // ~3 s at 60 fps
    if (player != nullptr) {
        const u32 sf1 = player->stateFlags1;
        const bool hanging  = (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) != 0;
        const bool hoisting = (sf1 & PLAYER_STATE1_CLIMBING_LEDGE)   != 0;
        if (hanging && !hoisting) {
            followerHangFrames++;
            if (followerHangFrames >= kHangTimeoutFrames) {
                bool triggered = TeleportToLeader("G15 hang-state timeout");
                followerHangFrames = 0;
                followerAIState    = FollowerAIState::IDLE;
                followerStateFrames = 0;
                if (triggered) {
                    return;
                }
            }
        } else {
            followerHangFrames = 0;
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
    //
    // P3.5 (user 2026-05-09 — "AI Follower does not need to snap to
    // the position of the player to enter doors, crawlspaces, and
    // ladders anymore. NPCs should snap the correct position when
    // they are already within ~30 units of the climbable surface
    // they intend to use"): gate the snap on XZ proximity to the
    // leader's position. When far, defer CLIMBING entry and let the
    // FOLLOW state drive the follower toward the ladder base via
    // stick injection / nav substrate. CLIMBING will enter on a
    // subsequent tick once the follower is within reach. Removes
    // the across-scene "leader started climbing → instant teleport"
    // behaviour the user objected to.
    {
        auto it = clients.find(followerLeaderClientId);
        if (it != clients.end() && it->second.isClimbing &&
            followerAIState != FollowerAIState::CLIMBING) {
            constexpr f32 kClimbApproachRadius = 30.0f;
            f32 dx = leaderPos.x - p2Pos.x;
            f32 dz = leaderPos.z - p2Pos.z;
            f32 distSq = dx * dx + dz * dz;
            if (distSq <= kClimbApproachRadius * kClimbApproachRadius) {
                // Within proximity — snap to ladder XZ and enter CLIMBING.
                // Snap stays as-is (was Bug 2's solution): puts follower
                // adjacent to the ladder collider so the next stick_y
                // injection actually attaches.
                Vec3f ladderXz = { leaderPos.x, p2Pos.y, leaderPos.z };
                player->actor.world.pos = ladderXz;
                player->actor.prevPos   = ladderXz;
                followerAIState     = FollowerAIState::CLIMBING;
                followerStateFrames = 0;
                SPDLOG_INFO("[Follower] Leader climbing + within {:.0f}u → CLIMBING "
                            "(snap to ladder XZ at {:.0f},{:.0f},{:.0f})",
                            sqrtf(distSq),
                            ladderXz.x, ladderXz.y, ladderXz.z);
                // Refresh p2Pos snapshot since we just moved.
                p2Pos = player->actor.world.pos;
            } else {
                // Too far from the ladder base — point FOLLOW toward
                // it and let the substrate path / stick injection bring
                // us close. CLIMBING entry retried on next tick.
                followerMoveTarget = leaderPos;
                if (followerStateFrames % 60 == 0) {
                    SPDLOG_INFO("[Follower] Leader climbing but follower "
                                "{:.0f}u from ladder base — approaching "
                                "before CLIMBING entry",
                                sqrtf(distSq));
                }
            }
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
            // Body extracted to Anchor::HandleStateIdle
            // (Phase 1 commit 10 of the SRP refactor).
            HandleStateIdle(player, dummyActor, sideTarget, p2Pos);
            break;
        }

        case FollowerAIState::FOLLOW: {
            // Body extracted to Anchor::HandleStateFollow
            // (Phase 1 commit 11 of the SRP refactor).
            HandleStateFollow(player, sideTarget, p2Pos);
            break;
        }

        case FollowerAIState::STUCK: {
            // Body extracted to Anchor::HandleStateStuck
            // (Phase 1 commit 8 of the SRP refactor). Phase 2 commit 5
            // added leaderPos / p2Pos so JumpResolver can evaluate the
            // gap with the right reference points.
            HandleStateStuck(player, leaderPos, p2Pos);
            break;
        }

        case FollowerAIState::ENGAGE: {
            // Body extracted to Anchor::HandleStateEngage
            // (Phase 1 commit 12 of the SRP refactor).
            HandleStateEngage(player, leaderPos, p2Pos);
            break;
        }

        case FollowerAIState::ATTACK: {
            // Body extracted to Anchor::HandleStateAttack
            // (Phase 1 commit 12 of the SRP refactor).
            HandleStateAttack(player, p2Pos);
            break;
        }

        case FollowerAIState::RETURN: {
            // Body extracted to Anchor::HandleStateReturn
            // (Phase 1 commit 7 of the SRP refactor).
            HandleStateReturn(player, sideTarget, p2Pos);
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
            // Body extracted to Anchor::HandleStateClimbing
            // (Phase 1 commit 7 of the SRP refactor).
            HandleStateClimbing(player, leaderPos, leaderActor);
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
            // Body extracted to Anchor::HandleStateRangedAttack
            // (Phase 1 commit 8 of the SRP refactor).
            HandleStateRangedAttack(player, p2Pos);
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
            // Body extracted to Anchor::HandleStateCollectItem
            // (Phase 1 commit 9 of the SRP refactor).
            HandleStateCollectItem(player, leaderPos, p2Pos);
            break;
        }
    }

    // End-of-block position override was intentionally removed when
    // the follower switched to stick-input movement. The only path
    // that now writes to player->actor.world.pos is the STUCK state
    // fallback above — see that case's comment block for rationale.

    AnchorFollower::CaptureFrame(ctx);
}

// ---------------------------------------------------------------------------
// Anchor::TickFollowerInput — per-frame follower input-injection body.
// Originally moved verbatim from HookHandlers.cpp's ShouldActorUpdate lambda
// body in Phase 1 commit 5 of the SRP refactor (#173 / #169); dedented to
// standard 4-space function-body depth in Phase 1 commit 14. The hook fires
// immediately BEFORE the player actor's update(), so input written here is
// consumed by Player_Update on the same frame — that's why locomotion /
// swing / climb can be driven by writing to input[0] from this site.
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

    // --- Hang-state resolution (P3.4, originally Phase 1 commit 6c) ---
    // When the follower enters PLAYER_STATE1_HANGING_OFF_LEDGE, decide
    // whether to climb up (BTN_A) or drop down (BTN_B). The existing
    // DO_ACTION_CLIMB path above handles climb-up but not drop-down,
    // so a follower hanging from a ledge with the leader BELOW would
    // climb up (wrong direction) instead of letting go.
    //
    // Thresholds:
    //   target Y > follower Y + 30  → climb up   (BTN_A)
    //   target Y < follower Y - 80  → drop down  (BTN_B)
    //   otherwise                   → BTN_A (default — bias
    //                                  toward climb up).
    // BTN_A path overlaps DO_ACTION_CLIMB above; idempotent.
    // BTN_B is the case the previous gate-on-only design left
    // unhandled.
    //
    // P3.4 (user 2026-05-09 — "NPCs in the hold-on/hang-on/ledge
    // state need to climb up or let go and drop to pursue a
    // target"): originally gated behind gEnhancements.Nav.Enabled +
    // Nav.VerticalTeleport. That made this basic NPC capability
    // dependent on enabling experimental nav CVars. Now always-on
    // for the AI Follower — hang-state resolution is core to "follow
    // the leader through vertical terrain". Future per-NPC opt-out
    // can route through NavTraits when AI Invader / ally NPCs join.
    {
        const bool hangFlag = (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) != 0;
        static bool sWasHanging = false;
        if (hangFlag) {
            constexpr f32 kHangResolveAboveThreshold = 30.0f;
            constexpr f32 kHangResolveBelowThreshold = 80.0f;
            // User 2026-05-10: previously used followerMoveTarget.y
            // here, which carries the IMMEDIATE subgoal (often at
            // follower's current Y level, especially when the substrate
            // path's next waypoint is the ledge-bottom node the
            // follower is hanging from). That gave dy≈0 across hang
            // events even when the leader was clearly above on the
            // ledge — climb-up was selected by default-bias but the
            // log made the picture confusing. Switch to the leader's
            // ACTUAL Y so the dy reading reflects "where do we need
            // to end up vertically" instead of "where's the next
            // waypoint." When climbing the ledge IS the way to reach
            // the leader, leader.y > follower.y → climb-up. When the
            // leader is below us (we hung off a ledge by mistake),
            // dy < -80 → drop down.
            //
            // leaderPos itself is only in scope at the top-level
            // frame block. Use the synced AnchorClient.posRot.pos
            // for the leader instead — same data, different access
            // path. Falls back to followerMoveTarget.y when the
            // leader entry isn't found (rare; safety net).
            f32 targetY = followerMoveTarget.y;
            auto it = clients.find(followerLeaderClientId);
            if (it != clients.end()) {
                targetY = it->second.posRot.pos.y;
            }
            f32 dy       = targetY - player->actor.world.pos.y;
            bool dropDown = (dy < -kHangResolveBelowThreshold);
            // High-confidence climb-up: leader is meaningfully above the
            // hang position (>+30u). In addition to BTN_A, inject forward
            // stick so OoT's hang-action handler reads "intent to stay
            // grabbed and climb" rather than "neutral input → auto-drop"
            // (suspected cause of the 4-6-frame brief grabs in log 24).
            // Skip forward-stick in the BTN_A default-bias range
            // (-80 < dy < +30) so incidental wall-brushes during pursuit
            // still drop naturally — only commit to forward-stick when
            // the leader is clearly above and climbing IS the intent.
            bool climbUp = (!dropDown && dy > kHangResolveAboveThreshold);
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
            if (climbUp) {
                // Forward stick using the same camera-relative inversion
                // as the ground-walking block at line 1996-2001. World
                // direction we want = Link's facing (shape.rot.y) which
                // points INTO the wall during hang. We already have the
                // angle so no Math_Atan2S step is needed (vs the
                // ground-walking case which derives the angle from a
                // dx/dz target delta).
                Camera* cam = GET_ACTIVE_CAM(gPlayState);
                s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                s16 worldYaw    = player->actor.shape.rot.y;
                s16 stickAngle  = worldYaw - inputDirYaw;
                s8  stickY = (s8)( Math_CosS(stickAngle) * 127.0f);
                s8  stickX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                input.cur.stick_x = stickX;
                input.cur.stick_y = stickY;
                input.rel.stick_x = stickX;
                input.rel.stick_y = stickY;
            }
            if (!sWasHanging) {
                SPDLOG_INFO("[Follower] BTN_{} hang-state resolution "
                            "(leaderY={:.1f} followerY={:.1f} dy={:.1f}, "
                            "above={:.1f}, below={:.1f}, forwardStick={})",
                            dropDown ? "B" : "A",
                            targetY, player->actor.world.pos.y, dy,
                            kHangResolveAboveThreshold,
                            kHangResolveBelowThreshold,
                            climbUp ? "yes" : "no");
                sWasHanging = true;
            }
        } else if (sWasHanging) {
            sWasHanging = false;
        }
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

void Anchor::HandleStateReturn(Player* player, const Vec3f& sideTarget, const Vec3f& p2Pos) {
    // RETURN: walk back to leader's side after combat. Bug 2 fix 1
    // (user 2026-05-10): mirrors FOLLOW's door-handoff substrate routing.
    // When followerDoorHandoff is active, the safety-net block has set
    // followerMoveTarget to the door target; use that as the substrate's
    // finalGoal so the path plans toward the door (not the leader who's
    // in another room). TrailKey=0 for the door target since a static
    // position has no trail; ComputePathTo's Layer 1 LOS + Layer 3 graph
    // handle the path.
    const bool useDoorTarget = followerDoorHandoff;
    const Vec3f finalGoal    = useDoorTarget ? followerMoveTarget : sideTarget;
    Vec3f returnTarget       = finalGoal;
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled()) {
        AnchorNav::TrailKey targetKey = useDoorTarget
            ? AnchorNav::TrailKey{0}
            : AnchorNav::TrailKeyForPlayer((uint8_t)followerLeaderClientId);
        const bool sceneChanged = (followerNavPath.sceneNum != gPlayState->sceneNum);
        const bool keyChanged   = (followerNavPathTargetKey != targetKey);
        const f32 driftDx = finalGoal.x - followerNavPathLastTarget.x;
        const f32 driftDz = finalGoal.z - followerNavPathLastTarget.z;
        const bool targetDrifted =
            (driftDx * driftDx + driftDz * driftDz) >
            (kNavPathTargetDriftRefresh * kNavPathTargetDriftRefresh);
        const bool needsRefresh = followerNavPath.Empty() || sceneChanged ||
                                   keyChanged || targetDrifted;
        if (needsRefresh) {
            followerNavPath.Reset();
            // Same Layer 1 LOS skip as HandleStateFollow's door-handoff
            // path — see that site for rationale.
            bool gotPath = AnchorNav::ActorTrail::GetInstance().ComputePathTo(
                targetKey, &player->actor, finalGoal, gPlayState, followerNavPath,
                /*skipLayer1LOS=*/useDoorTarget);
            followerNavPathTargetKey  = targetKey;
            followerNavPathLastTarget = finalGoal;
            if (!gotPath) {
                SPDLOG_DEBUG("[Follower] RETURN NavPath empty (ComputePathTo "
                             "returned false; falling back to direct finalGoal "
                             "useDoorTarget={})", useDoorTarget);
            }
        }
        if (!followerNavPath.Empty()) {
            Vec3f sg = followerNavPath.CurrentSubgoal();
            f32   sgDx = sg.x - p2Pos.x;
            f32   sgDz = sg.z - p2Pos.z;
            if (sgDx * sgDx + sgDz * sgDz <
                kNavPathSubgoalReach * kNavPathSubgoalReach) {
                followerNavPath.Advance();
            }
            if (!followerNavPath.Empty()) {
                returnTarget = followerNavPath.CurrentSubgoal();
            }
        }
    }
    followerMoveTarget = returnTarget;
    f32 distToReturnTarget = sqrtf(SQ(returnTarget.x - p2Pos.x) +
                                    SQ(returnTarget.z - p2Pos.z));
    // Stick injection in TickFollowerInput drives actual movement; we
    // face the immediate move target so Player_Update's auto-rotate
    // aligns with stick direction.
    if (distToReturnTarget > 0.001f) {
        player->actor.shape.rot.y = YawToward(
            returnTarget.x - player->actor.world.pos.x,
            returnTarget.z - player->actor.world.pos.z);
    }
    // RETURN→IDLE gates on distance to the FINAL goal (sideTarget), not
    // the immediate subgoal. With nav substrate off this is identical
    // (returnTarget == sideTarget); with nav substrate on it prevents
    // RETURN→IDLE from firing when the follower reaches an intermediate
    // breadcrumb en route to the leader.
    //
    // P3.10: also gate on |Δy| so the follower doesn't settle into IDLE
    // while standing UNDER the leader (e.g., leader on a platform above).
    // See kFollowYThreshold doc.
    f32 distToFinalGoal = sqrtf(SQ(sideTarget.x - p2Pos.x) +
                                 SQ(sideTarget.z - p2Pos.z));
    f32 dyToFinalGoal = fabsf(sideTarget.y - p2Pos.y);
    if (distToFinalGoal < kFollowThreshold &&
        dyToFinalGoal    < kFollowYThreshold) {
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] RETURN→IDLE dist={:.1f} dy={:.1f}",
                    distToFinalGoal, dyToFinalGoal);
    }
}

void Anchor::HandleStateClimbing(Player* player, const Vec3f& leaderPos, Actor* leaderActor) {
    // G1/G2 — leader is climbing. Bug 2 redesign (2026-04-22): instead
    // of writing world.pos = leaderPos every frame (which fights gravity
    // between actor-update and our hook, producing the "hover slightly
    // below leader" symptom), we point followerMoveTarget at leader's
    // XZ at follower's current Y (the ladder base / current rung) and
    // let TickFollowerInput's stick injection drive Link.
    //
    // Once Link's PLAYER_STATE1_CLIMBING_LADDER fires (Link physically
    // grabbed the ladder), stick_y direction toggles based on Δy to
    // leader. OoT plays the real climb animation natively.
    //
    // Exit when leader's isClimbing flips back to false. Bug C (log 69)
    // — arm the dismount-forward-hold so the next-frame state machine
    // doesn't immediately point Link backward off the rim.
    //
    // ── Phase 2: this state IS the Shape A reference implementation ──
    // VerticalTeleport.h:90-118 documents Shape A as "the existing
    // follower CLIMBING pipeline" — the input-injection climb that
    // produces the real animation, real physics, and vine lateral
    // tracking. The plan-doc deliberately does not re-implement this
    // mechanism in VerticalTeleport.cpp; rewriting would risk
    // regressing those behaviours. Instead, AnchorNav::IsShapeAEligible
    // exposes a discoverable predicate so OTHER navigators (synced
    // enemies, AI Invader) can decide whether to use this style of
    // climb (Shape A, for Link-rigged actors) or a direct world.pos
    // teleport (Shape B, for non-Link actors).
    //
    // We diagnostic-check IsShapeAEligible here when the substrate
    // gate is on. The follower is always Shape A-eligible by
    // construction (Player actor, eligibleForVerticalTeleport=true
    // by default), so a false return surfaces a config inconsistency:
    // Nav.VerticalTeleport CVar off while AiFollowerConsumer is on,
    // or a NavTraits override has been added that disables the
    // follower. Log-only — never blocks the existing pipeline,
    // because the existing pipeline IS the reference implementation
    // of Shape A. The legacy follower CLIMBING runs untouched
    // regardless of the substrate gate.
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled() &&
        !AnchorNav::IsShapeAEligible(&player->actor)) {
        static bool sShapeAWarned = false;
        if (!sShapeAWarned) {
            SPDLOG_WARN("[Follower] CLIMBING: IsShapeAEligible=false despite "
                        "substrate gate on. Check Nav.VerticalTeleport CVar / "
                        "NavTraits eligibleForVerticalTeleport for ACTOR_PLAYER. "
                        "Following pipeline runs unchanged.");
            sShapeAWarned = true;
        }
    }

    // P3.8 part 2 / P3.6: autonomous climb path. Entered from
    // HandleStateFollow when the follower is at a climb anchor and
    // the path goes up. Target is followerClimbTopTarget (set on
    // entry) instead of the leader's position. Exits when the
    // follower reaches the top tolerance OR when a safety counter
    // elapses (anchor was bad / can't make progress).
    if (followerAutonomousClimb) {
        followerAutonomousClimbFrames++;
        constexpr f32 kAutonomousClimbReachY = 16.0f;       // within 16u Y of top → done
        constexpr int kAutonomousClimbMaxFrames = 600;      // ~10s safety
        bool reachedTop =
            (player->actor.world.pos.y >= followerClimbTopTarget.y - kAutonomousClimbReachY);
        bool timedOut = (followerAutonomousClimbFrames >= kAutonomousClimbMaxFrames);
        if (reachedTop || timedOut) {
            followerClimbDismountYaw    = player->actor.shape.rot.y;
            followerClimbDismountFrames = kClimbDismountHoldFrames;
            followerAIState     = FollowerAIState::IDLE;
            followerStateFrames = 0;
            followerAutonomousClimb       = false;
            followerAutonomousClimbFrames = 0;
            SPDLOG_INFO("[Follower] CLIMBING→IDLE (autonomous {}); "
                        "follower y={:.0f} top={:.0f}",
                        reachedTop ? "reached top" : "TIMEOUT",
                        player->actor.world.pos.y,
                        followerClimbTopTarget.y);
            return;
        }
        // followerMoveTarget = climb-top position. TickFollowerInput's
        // CLIMBING-aware injection reads this for stick_y direction
        // and for walk-to-ladder approach when not yet on the climb
        // collider. No leader-yaw match — leader may be far away.
        followerMoveTarget = followerClimbTopTarget;
        return;
    }

    auto it = clients.find(followerLeaderClientId);
    if (it == clients.end() || !it->second.isClimbing) {
        followerClimbDismountYaw    = player->actor.shape.rot.y;
        followerClimbDismountFrames = kClimbDismountHoldFrames;
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] CLIMBING→IDLE (leader stopped climbing); "
                    "armed dismount forward-hold {} frames at yaw={}",
                    kClimbDismountHoldFrames,
                    (int)followerClimbDismountYaw);
        return;
    }
    // followerMoveTarget = leader's XZ at the leader's current Y.
    // TickFollowerInput's CLIMBING-aware injection reads this for
    // direction (leader.y vs p2Pos.y).
    followerMoveTarget = leaderPos;
    // P3.12 (user 2026-05-09 — "when climbing, NPCs should stop
    // attempting to face toward the target they are pursuing. When
    // climbing, NPCs must face the surface geometry that they are
    // climbing on"): do NOT overwrite shape.rot.y per-frame during
    // CLIMBING. OoT's ladder/vine code sets shape.rot.y to the
    // surface normal when Link grabs the climbable, and re-orients
    // automatically as the climb progresses. The pre-existing
    // "Match leader's facing so dismount looks clean" write fought
    // OoT's correct orientation every frame, producing the
    // visible "follower facing away from the wall while climbing"
    // bug. The dismount-forward-hold still captures shape.rot.y
    // AT EXIT (followerClimbDismountYaw) — by then OoT has
    // already oriented the player for the dismount frame, so the
    // captured yaw is the right thing to hold.
    (void)leaderActor;  // referenced via signature; explicit no-op now
}

void Anchor::HandleStateStuck(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos) {
    // STUCK: stick-input hit a wall / corner / doorway / void the
    // simulation can't navigate. Apply a small position nudge directly
    // toward followerMoveTarget for up to kStuckRecovery frames.
    // Bypasses Link's physics just enough to get past the obstacle.
    // Stick injection stays active during this state (see
    // TickFollowerInput) so Link's legs still try to walk — the nudge
    // is additive, not a replacement.
    //
    // This is the ONLY path in the follower state machine that writes
    // to player->actor.world.pos in the stick-input design.
    //
    // Phase 2 — when the nav substrate consumer gate is on, evaluate
    // the obstacle on the FIRST stuck frame via
    // JumpResolver::ResolveLedgeAhead and dispatch:
    //   SafeTerrain     → STUCK→FOLLOW immediately (the desired step
    //                     was actually safe; STUCK was a false alarm,
    //                     let the next FOLLOW tick retry).
    //   JumpAcross      → write world.pos = landingPos, exit STUCK.
    //                     Crosses the gap in one frame (matches the
    //                     existing "STUCK is the only world.pos
    //                     write" pattern, just a longer nudge).
    //   TrailContinues  → same as JumpAcross — landing chosen by the
    //                     target's breadcrumb evidence rather than a
    //                     forward fan probe; mechanically identical
    //                     for the follower.
    //   PathAround      → adopt result.pathAround into followerNavPath
    //                     so FOLLOW's substrate path consumer picks
    //                     it up on the next tick. STUCK→FOLLOW.
    //   Retreat         → write world.pos = retreatPos. Mirrors the
    //                     existing G10/G12/G14 teleport-back-to-leader
    //                     safety net, scoped to one step. Exit STUCK.
    //
    // The legacy nudge (one step toward followerMoveTarget per tick
    // for kStuckRecovery frames) remains the default and the gate-off
    // fallback. JumpResolver runs only on entry frame 0 — subsequent
    // STUCK frames continue with the legacy nudge. Frames-zero gate
    // ensures the predicate runs once per stuck cycle, not every frame.
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled() &&
        followerStuckFrames == 0) {
        AnchorNav::TrailKey leaderKey =
            AnchorNav::TrailKeyForPlayer((uint8_t)followerLeaderClientId);
        AnchorNav::JumpResolutionResult r =
            AnchorNav::ResolveLedgeAhead(&player->actor,
                                          leaderPos,
                                          followerMoveTarget,
                                          leaderKey,
                                          /*navigatorKey=*/0,
                                          gPlayState);
        switch (r.kind) {
            case AnchorNav::JumpResolution::SafeTerrain:
                SPDLOG_INFO("[Follower] STUCK JumpResolver: SafeTerrain — "
                            "returning to FOLLOW (false alarm)");
                followerAIState     = FollowerAIState::FOLLOW;
                followerLastPos     = p2Pos;
                followerStateFrames = 0;
                return;
            case AnchorNav::JumpResolution::JumpAcross:
            case AnchorNav::JumpResolution::TrailContinues:
                SPDLOG_INFO("[Follower] STUCK JumpResolver: {} → landing "
                            "({:.0f},{:.0f},{:.0f})",
                            r.kind == AnchorNav::JumpResolution::JumpAcross
                                ? "JumpAcross" : "TrailContinues",
                            r.landingPos.x, r.landingPos.y, r.landingPos.z);
                player->actor.world.pos = r.landingPos;
                followerAIState     = FollowerAIState::FOLLOW;
                followerLastPos     = r.landingPos;
                followerStateFrames = 0;
                // Drop any held NavPath — its waypoints don't account
                // for our teleport. FOLLOW will recompute on next tick.
                followerNavPath.Reset();
                return;
            case AnchorNav::JumpResolution::PathAround: {
                SPDLOG_INFO("[Follower] STUCK JumpResolver: PathAround "
                            "({} waypoints) — adopting into NavPath",
                            (int)r.pathAround.size());
                followerNavPath.Reset();
                followerNavPath.waypoints       = std::move(r.pathAround);
                followerNavPath.cursorIdx       = 0;
                followerNavPath.sceneNum        = (int16_t)gPlayState->sceneNum;
                followerNavPath.capturedTargetPos = leaderPos;
                followerNavPathTargetKey  = leaderKey;
                followerNavPathLastTarget = leaderPos;
                followerAIState     = FollowerAIState::FOLLOW;
                followerLastPos     = p2Pos;
                followerStateFrames = 0;
                return;
            }
            case AnchorNav::JumpResolution::Retreat:
                SPDLOG_INFO("[Follower] STUCK JumpResolver: Retreat → "
                            "({:.0f},{:.0f},{:.0f})",
                            r.retreatPos.x, r.retreatPos.y, r.retreatPos.z);
                player->actor.world.pos = r.retreatPos;
                followerAIState     = FollowerAIState::FOLLOW;
                followerLastPos     = r.retreatPos;
                followerStateFrames = 0;
                followerNavPath.Reset();
                return;
        }
        // Fall through if a future enum variant is added without a
        // case — the legacy nudge below still applies.
    }

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
}

void Anchor::HandleStateRangedAttack(Player* player, const Vec3f& p2Pos) {
    // G6/G7/G8 — ranged attack. Inject BTN_Z + BTN_A while ENGAGE
    // target is a known ranged-required class (Gohma ceiling, larvae,
    // Skullwalltulas on vines). Movement freezes (no stick) so Link
    // can aim. Item-override system (FollowerRestoreItems) restores
    // the player's C-button loadout on every exit path.
    if (followerTargetEnemy == nullptr ||
        followerTargetEnemy->update == nullptr) {
        FollowerRestoreItems();
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (target gone)");
        return;
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
        return;
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
}

void Anchor::HandleStateCollectItem(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos) {
    // COLLECT_ITEM: opportunistic pickup of En_Item00 drops after an
    // enemy kill. Walks toward followerTargetItem until pickup fires
    // (En_Item00's own collision handler attaches to Link on contact —
    // no BTN_A or other interaction needed for pickup).
    //
    // Exit paths:
    //   - target actor gone (collected by us OR by leader) → RETURN
    //   - leader beyond leash → RETURN (follow takes priority)
    //   - item on a different floor (|Δy| ≥ kMaxYDelta) → RETURN
    //   - timeout elapsed (couldn't reach) → RETURN
    if (followerTargetItem == nullptr ||
        followerTargetItem->update == nullptr) {
        SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item gone — collected or unloaded)");
        followerTargetItem  = nullptr;
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        return;
    }
    // Leader leash — don't stray too far from the leader just for a rupee.
    {
        f32 lx = leaderPos.x - p2Pos.x;
        f32 lz = leaderPos.z - p2Pos.z;
        if (lx * lx + lz * lz > kMaxLeash * kMaxLeash) {
            SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (leader beyond leash)");
            followerTargetItem  = nullptr;
            followerAIState     = FollowerAIState::RETURN;
            followerStateFrames = 0;
            return;
        }
    }
    // Y-gate — item ended up on a different floor (bounce off a ledge
    // between grace expiry and pickup start).
    if (fabsf(followerTargetItem->world.pos.y - p2Pos.y) >= kMaxYDelta) {
        SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item off-floor)");
        followerTargetItem  = nullptr;
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        return;
    }
    // Timeout — couldn't reach the item in kItemCollectTimeout frames
    // (geometry / collision mishap).
    if (followerCollectItemTimeoutFrames > 0) {
        followerCollectItemTimeoutFrames--;
        if (followerCollectItemTimeoutFrames == 0) {
            SPDLOG_WARN("[Follower] COLLECT_ITEM→RETURN (timeout)");
            followerTargetItem  = nullptr;
            followerAIState     = FollowerAIState::RETURN;
            followerStateFrames = 0;
            return;
        }
    }
    // Drive TickFollowerInput toward the item.
    followerMoveTarget = followerTargetItem->world.pos;
    {
        f32 idx = followerTargetItem->world.pos.x - p2Pos.x;
        f32 idz = followerTargetItem->world.pos.z - p2Pos.z;
        if (idx * idx + idz * idz > 1.0f) {
            player->actor.shape.rot.y = YawToward(idx, idz);
        }
    }
}

Actor* Anchor::ScanForItemCandidate(Player* player) {
    // Item pickup — scan ACTORCAT_MISC for eligible En_Item00 drops.
    // Maintains itemFirstSeenFrame (grace-period tracker) and returns
    // the nearest eligible in-range item whose grace window has elapsed.
    // Called once per tick from IDLE / FOLLOW state bodies. Pointer-
    // reuse is handled by purging entries whose key is no longer in
    // the current MISC list.
    //
    // Was a parent-function lambda inside TickFollower; promoted to an
    // Anchor:: method so per-state handlers can call it. Caller passes
    // `player` for the proximity reference position.
    //
    // Pass 1: collect current live EN_ITEM00 pointers.
    std::unordered_set<Actor*> liveItems;
    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
    while (cand != nullptr) {
        if (cand->id == ACTOR_EN_ITEM00 && cand->update != nullptr) {
            liveItems.insert(cand);
        }
        cand = cand->next;
    }
    // Pass 2: purge itemFirstSeenFrame entries whose key is no longer
    // in the MISC list (item was collected / unloaded).
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
        // Test 5 diagnostics — log item type at the first post-grace
        // scan for each actor, sparse per type.
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
        f32 dx = item->world.pos.x - selfPos.x;
        f32 dz = item->world.pos.z - selfPos.z;
        f32 d2 = dx * dx + dz * dz;
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            bestItem   = item;
        }
    }
    return bestItem;
}

void Anchor::HandleStateIdle(Player* player, Actor* dummyActor, const Vec3f& sideTarget, const Vec3f& p2Pos) {
    // IDLE: hold position next to leader. Drift back to side-target if
    // leader moved out of FollowThreshold; else scan for nearby enemies
    // → ENGAGE; else scan for eligible item drops → COLLECT_ITEM; else
    // match leader facing.
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
        return;
    }
    // Scan for the nearest live enemy within ENGAGE range. Reject
    // enemies on a different vertical level (XZ-only follower can't
    // reach them). Target blacklist: scrub-puzzle actors that can only
    // be defeated by shield-reflect — follower can't perform reflect
    // and would otherwise run at empty Hintnut nests.
    auto IsScrubPuzzleActor = [](int16_t id) -> bool {
        return id == ACTOR_EN_HINTNUTS ||
               id == ACTOR_EN_DEKUNUTS;
    };
    Actor* nearest    = nullptr;
    f32    nearDistSq = kEngageRange * kEngageRange;
    Actor* eActor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (eActor != nullptr) {
        if (eActor->update != nullptr &&
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
        return;
    }
    // Item pickup — no enemy to engage; scan for eligible drops.
    {
        Actor* item = this->ScanForItemCandidate(player);
        if (item != nullptr) {
            followerTargetItem = item;
            followerCollectItemTimeoutFrames = kItemCollectTimeout;
            followerAIState     = FollowerAIState::COLLECT_ITEM;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] IDLE→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                        (int)(item->params & 0xFF),
                        item->world.pos.x, item->world.pos.y, item->world.pos.z);
            return;
        }
    }
    // Per user 2026-05-09: do NOT match leader's facing in IDLE — the
    // synchronised swivel looked unnatural. Follower keeps whatever yaw
    // it last had (from arriving in IDLE), which is more natural and
    // matches how an actual companion would idle. The only intentional
    // facing reset is on FOLLOW/ENGAGE/RETURN entry where the move
    // direction supplies a sensible yaw. (Suppress the unused parameter
    // warning explicitly so the dummyActor reference is preserved for
    // future re-use without a signature change.)
    (void)dummyActor;
    // Pre-populate move target so the first FOLLOW frame's
    // TickFollowerInput sees the correct direction immediately. Test 8
    // — during door handoff the G11 block already set followerMoveTarget
    // to the door centerline; don't overwrite with side-offset.
    if (!followerDoorHandoff) {
        followerMoveTarget = sideTarget;
    }
}

void Anchor::HandleStateFollow(Player* player, const Vec3f& sideTarget, const Vec3f& p2Pos) {
    // FOLLOW: stick-driven movement toward leader's side. Periodically
    // checks progress (every kStuckCheckInterval frames); enters STUCK
    // if no progress AND increments G12 stuck-cycle counter for the
    // teleport-escalation safety net. Scans for opportunistic item
    // drops every 10 frames. Transitions to IDLE when within
    // kFollowThreshold of the target — except during door handoff,
    // where the FOLLOW→IDLE→FOLLOW oscillation would prevent the
    // BTN_A door-open injection from sticking.
    //
    // P3.8 part 2 / P3.6 (user 2026-05-09): autonomous CLIMBING
    // engagement. When the substrate gate is on AND the final goal
    // (sideTarget) is significantly above the follower AND there's a
    // climb anchor (vine wall / ladder / climbable surface) within
    // ~30u XZ whose top is also above, transition to CLIMBING with
    // followerAutonomousClimb=true. The existing CLIMBING input-
    // injection (which uses followerMoveTarget for stick_y direction)
    // drives the upward climb from there. Throttled to every 10
    // frames to keep the RoomNavData query off the per-frame critical
    // path.
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled() &&
        followerAIState != FollowerAIState::CLIMBING &&
        !followerAutonomousClimb &&
        (followerStateFrames % 10 == 0)) {
        constexpr f32 kAutonomousClimbXZRadius = 30.0f;
        constexpr f32 kAutonomousClimbMinHeight = 50.0f; // top must be at least this high
        // Only consider engaging when the FINAL goal is meaningfully
        // above the follower — avoids climbing for siblings on the
        // same floor where the substrate path happens to pass near a
        // climb base en route to a non-climbable destination.
        if (sideTarget.y > p2Pos.y + kAutonomousClimbMinHeight) {
            const ::AnchorNavRoom::RoomNavData* navData =
                ::AnchorNavRoom::GetForRoom((int16_t)gPlayState->sceneNum,
                                             (int8_t)gPlayState->roomCtx.curRoom.num);
            if (navData != nullptr) {
                Vec3f anchorBase, anchorTop;
                if (::AnchorNavRoom::FindClimbAnchorAbove(
                        navData, p2Pos,
                        kAutonomousClimbXZRadius,
                        kAutonomousClimbMinHeight,
                        anchorBase, anchorTop)) {
                    followerAutonomousClimb       = true;
                    followerClimbTopTarget        = anchorTop;
                    followerAutonomousClimbFrames = 0;
                    followerAIState               = FollowerAIState::CLIMBING;
                    followerStateFrames           = 0;
                    SPDLOG_INFO("[Follower] FOLLOW→CLIMBING (autonomous; "
                                "anchor base=({:.0f},{:.0f},{:.0f}) "
                                "top=({:.0f},{:.0f},{:.0f}) "
                                "follower=({:.0f},{:.0f},{:.0f}))",
                                anchorBase.x, anchorBase.y, anchorBase.z,
                                anchorTop.x, anchorTop.y, anchorTop.z,
                                p2Pos.x, p2Pos.y, p2Pos.z);
                    return;
                }
            }
        }
    }
    //
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
            // Stick input failed to make progress. Enter STUCK fallback.
            followerAIState     = FollowerAIState::STUCK;
            followerStuckFrames = 0;
            followerStateFrames = 0;
            // G12 — count this entry; arm the reset window. The
            // top-of-hook check escalates to teleport when count >=
            // kStuckCycleEscalation within the window.
            followerStuckCycleCount++;
            followerStuckCycleResetFrames = kStuckCycleWindow;
            SPDLOG_INFO("[Follower] FOLLOW→STUCK (stick input stalled, cycle={})",
                        followerStuckCycleCount);
            return;
        }
    }
    // Item pickup — scan every 10 frames inside FOLLOW. On finding an
    // eligible drop, abandon FOLLOW and divert to COLLECT_ITEM.
    if (followerStateFrames % 10 == 0) {
        Actor* item = this->ScanForItemCandidate(player);
        if (item != nullptr) {
            followerTargetItem = item;
            followerCollectItemTimeoutFrames = kItemCollectTimeout;
            followerAIState     = FollowerAIState::COLLECT_ITEM;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] FOLLOW→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                        (int)(item->params & 0xFF),
                        item->world.pos.x, item->world.pos.y, item->world.pos.z);
            return;
        }
    }
    // Bug 2 fix 1 (user 2026-05-10): during door handoff, route substrate
    // pathfinding to the door target instead of skipping substrate entirely.
    // The G11 safety-net block above sets followerMoveTarget to the
    // transition-actor position (door centerline) when handoff is active.
    // Pre-fix, FOLLOW used the door target directly via legacy direct-yaw
    // steering — couldn't path around obstacles between follower and door.
    // Post-fix, ComputePathTo plans a multi-waypoint route to the door
    // through the navigable graph; the subgoal drives stick injection so
    // the follower walks AROUND obstacles to reach the door, then OoT's
    // transitionTrigger fires naturally when the follower hits the trigger
    // volume. G11 timeout becomes a "navigation actually impossible" guard
    // instead of a "we ran out of patience" forced teleport.
    //
    // TrailKey: leader's trail when targeting sideTarget (Layer 2 trail-
    // walk applies); kTrailKeyNone (=0) when targeting door (no trail
    // for a static position; ComputePathTo's Layer 1 LOS + Layer 3 graph
    // fallback handle it).
    const bool useDoorTarget = followerDoorHandoff;
    const Vec3f finalGoal    = useDoorTarget ? followerMoveTarget : sideTarget;
    Vec3f followTarget       = finalGoal;
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled()) {
        AnchorNav::TrailKey targetKey = useDoorTarget
            ? AnchorNav::TrailKey{0}
            : AnchorNav::TrailKeyForPlayer((uint8_t)followerLeaderClientId);
        const bool sceneChanged = (followerNavPath.sceneNum != gPlayState->sceneNum);
        const bool keyChanged   = (followerNavPathTargetKey != targetKey);
        const f32 driftDx = finalGoal.x - followerNavPathLastTarget.x;
        const f32 driftDz = finalGoal.z - followerNavPathLastTarget.z;
        const bool targetDrifted =
            (driftDx * driftDx + driftDz * driftDz) >
            (kNavPathTargetDriftRefresh * kNavPathTargetDriftRefresh);
        const bool needsRefresh = followerNavPath.Empty() || sceneChanged ||
                                   keyChanged || targetDrifted;
        if (needsRefresh) {
            followerNavPath.Reset();
            // Bug 2 fix 1 follow-up (user 2026-05-10): skip Layer 1 LOS
            // for door-handoff targets. ComputePathTo's Layer 1 returns
            // pathLen=1 (direct vector to target) when MovementClear's
            // pelvis-line says clear — but for a door target across
            // village geometry, the pelvis-line passes over short walls
            // / through gaps that the follower can't actually walk.
            // Forcing Layer 1 skip drops to Layer 3 BFS through the
            // RoomNavData graph (16k+ nodes for Kokiri Forest) whose
            // edges were built with MovementClearAtPosition + step-up
            // gates at scan time.
            bool gotPath = AnchorNav::ActorTrail::GetInstance().ComputePathTo(
                targetKey, &player->actor, finalGoal, gPlayState, followerNavPath,
                /*skipLayer1LOS=*/useDoorTarget);
            followerNavPathTargetKey  = targetKey;
            followerNavPathLastTarget = finalGoal;
            if (!gotPath) {
                // Nothing reachable per ComputePathTo's three layers.
                // Leave path empty; the !Empty() check below falls
                // through to direct finalGoal for this tick.
                SPDLOG_DEBUG("[Follower] FOLLOW NavPath empty (ComputePathTo "
                             "returned false; falling back to direct finalGoal "
                             "useDoorTarget={})", useDoorTarget);
            }
        }
        if (!followerNavPath.Empty()) {
            Vec3f sg = followerNavPath.CurrentSubgoal();
            f32   sgDx = sg.x - p2Pos.x;
            f32   sgDz = sg.z - p2Pos.z;
            if (sgDx * sgDx + sgDz * sgDz <
                kNavPathSubgoalReach * kNavPathSubgoalReach) {
                followerNavPath.Advance();
            }
            if (!followerNavPath.Empty()) {
                followTarget = followerNavPath.CurrentSubgoal();
            }
        }
    }
    followerMoveTarget = followTarget;
    f32 distToFollowTarget = sqrtf(SQ(followTarget.x - p2Pos.x) + SQ(followTarget.z - p2Pos.z));
    // Stick injection in TickFollowerInput drives actual movement;
    // here we just face the immediate move target. Under Phase 2 nav
    // substrate the immediate target may be an intermediate subgoal —
    // facing that subgoal is the right thing for stick injection.
    if (distToFollowTarget > 0.001f) {
        player->actor.shape.rot.y = YawToward(
            followTarget.x - player->actor.world.pos.x,
            followTarget.z - player->actor.world.pos.z);
    }
    // Bug 2 (log 184 Karebaba corridor) — skip the FOLLOW→IDLE
    // transition while a door handoff is armed. Without this guard,
    // FOLLOW→IDLE→FOLLOW oscillates at frame cadence and the BTN_A
    // door-open injection never sticks. Stay in FOLLOW until either
    // the follower crosses into leader's room (handoff clears at the
    // top of the hook) or followerDoorHandoffFrames hits zero
    // (timeout → fallback teleport).
    //
    // Phase 2: gate on distance to the final goal (sideTarget), not the
    // immediate subgoal. With nav substrate off this is identical
    // (followTarget == sideTarget); with nav substrate on it prevents
    // FOLLOW→IDLE from firing when the follower merely reaches an
    // intermediate breadcrumb en route to the leader.
    //
    // P3.10 (user 2026-05-09): also gate on |Δy|. Pre-fix, the
    // follower would settle into IDLE under the leader's platform —
    // visible "follower walks under target and stops" symptom. With
    // the Y gate, FOLLOW persists; the substrate path consumer keeps
    // trying to find a route up (slope / stairs / ladder via Layer 2
    // breadcrumbs). G10/G14 leash teleport eventually fires if no
    // route is found — safer than locking-in on the wrong altitude.
    f32 distToFinalGoal = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
    f32 dyToFinalGoal = fabsf(sideTarget.y - p2Pos.y);
    if (distToFinalGoal < kFollowThreshold &&
        dyToFinalGoal    < kFollowYThreshold &&
        !followerDoorHandoff) {
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] FOLLOW→IDLE dist={:.1f} dy={:.1f}",
                    distToFinalGoal, dyToFinalGoal);
    }
}

void Anchor::HandleStateEngage(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos) {
    // ENGAGE: walking toward the locked-on enemy. Bails to RETURN on
    // leader leash exceed, target loss, or target on a different floor
    // (unless ranged-required, then RANGED_ATTACK). When in attackRange,
    // routes to ATTACK or BLOCK depending on enemy class.
    //
    // Abandon if leader is too far.
    {
        f32 ldx = leaderPos.x - p2Pos.x;
        f32 ldz = leaderPos.z - p2Pos.z;
        if (ldx * ldx + ldz * ldz > kMaxLeash * kMaxLeash) {
            followerAIState     = FollowerAIState::RETURN;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] ENGAGE\u2192RETURN (leader too far)");
            return;
        }
    }
    if (followerTargetEnemy == nullptr ||
        followerTargetEnemy->update == nullptr) {
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] ENGAGE\u2192RETURN (enemy gone)");
        return;
    }
    // Vertical-reach handling. Three layered checks:
    //  1. Cross-floor (|dy| >= kMaxYDelta): target on a different
    //     logical level. If ranged-required, RANGED_ATTACK; else bail.
    //  2. Above sword reach but same-floor (dy > kSwordVerticalReach)
    //     AND ranged-required: route to RANGED_ATTACK.
    //  3. Otherwise fall through to XZ close + ATTACK.
    {
        f32 dy = followerTargetEnemy->world.pos.y - p2Pos.y;
        if (fabsf(dy) >= kMaxYDelta) {
            if (IsRangedRequiredEnemy(followerTargetEnemy->id)) {
                FollowerTryEquipRangedWeapon();
                followerAIState     = FollowerAIState::RANGED_ATTACK;
                followerStateFrames = 0;
                SPDLOG_INFO("[Follower] ENGAGE\u2192RANGED_ATTACK (off-floor target id={})",
                            followerTargetEnemy->id);
                return;
            }
            followerAIState     = FollowerAIState::RETURN;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] ENGAGE\u2192RETURN (enemy off-floor)");
            return;
        }
        if (dy > kSwordVerticalReach &&
            IsRangedRequiredEnemy(followerTargetEnemy->id)) {
            FollowerTryEquipRangedWeapon();
            followerAIState     = FollowerAIState::RANGED_ATTACK;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] ENGAGE\u2192RANGED_ATTACK (above sword reach \u0394y={:.0f} target id={})",
                        dy, followerTargetEnemy->id);
            return;
        }
    }
    Vec3f enemyPos = followerTargetEnemy->world.pos;
    f32   edx      = enemyPos.x - p2Pos.x;
    f32   edz      = enemyPos.z - p2Pos.z;
    f32   distSq   = edx * edx + edz * edz;
    // Bug D - per-enemy attackRange keeps the follower outside lunge
    // arcs of enemies whose damage volume sits ahead of world.pos.
    f32   attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
    if (distSq < attackRange * attackRange) {
        // G4 - Mad Scrub class: shield first, then swing on the stunned
        // scrub. BLOCK->ATTACK is wired in BLOCK.
        if (IsShieldReflectEnemy(followerTargetEnemy->id)) {
            followerAIState     = FollowerAIState::BLOCK;
            followerStateFrames = 0;
            SPDLOG_INFO("[Follower] ENGAGE\u2192BLOCK (shield-reflect target id={})",
                        followerTargetEnemy->id);
            return;
        }
        followerAIState     = FollowerAIState::ATTACK;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] ENGAGE\u2192ATTACK enemy=({:.0f},{:.0f},{:.0f}) dist={:.0f} "
                    "range={:.0f} id={}",
                    enemyPos.x, enemyPos.y, enemyPos.z, sqrtf(distSq),
                    attackRange, followerTargetEnemy->id);
        return;
    }
    if (followerStateFrames % 20 == 0) {
        SPDLOG_INFO("[Follower] ENGAGE progress: distToEnemy={:.0f} p2=({:.0f},{:.0f})",
                    sqrtf(distSq), p2Pos.x, p2Pos.z);
    }
    // Test 6 (log 74) - dangling Skulltula safety gap. Keep a 200u XZ
    // standoff for any EN_ST target whose Y is well above the follower.
    Vec3f navTarget = enemyPos;
    if (followerTargetEnemy->id == ACTOR_EN_ST) {
        f32 targetDy = enemyPos.y - p2Pos.y;
        if (targetDy > 40.0f) {
            static constexpr f32 kEnStSafeStandoffXZ = 200.0f;
            f32 distXZ = sqrtf(distSq);
            if (distXZ > kEnStSafeStandoffXZ) {
                f32 shrink = (distXZ - kEnStSafeStandoffXZ) / distXZ;
                navTarget.x = p2Pos.x + edx * shrink;
                navTarget.z = p2Pos.z + edz * shrink;
            } else {
                navTarget.x = p2Pos.x;
                navTarget.z = p2Pos.z;
            }
            navTarget.y = enemyPos.y;
        }
    }
    // Phase 2 — when the nav substrate consumer gate is on, plan a path
    // to the per-enemy navTarget (the standoff-adjusted enemy position)
    // rather than walking a straight line toward the enemy. The enemy's
    // own ActorTrail (keyed by EnemyNetId) feeds Layer 2 breadcrumbs;
    // Layer 3 RoomNavData provides BFS routing when the enemy is around
    // a corner. As in FOLLOW, ComputePathTo's three-layer fallback
    // gracefully degrades to direct-yaw when no substrate features are
    // active. On returned-empty path, we fall through to the bespoke
    // direct navTarget for this tick — never stalling.
    //
    // Sticky targeting (TargetSelection::ChooseTarget) is intentionally
    // NOT used here: the follower already has its own well-tuned target
    // stickiness via followerTargetEnemy (set in IDLE's enemy scan and
    // held until ENGAGE/ATTACK/RETURN cycle exits). TargetSelection's
    // per-navigator state lives on EnemyNetId, which the local Player
    // actor doesn't carry. Re-evaluating with TargetSelection here
    // would require a parallel state path; not worth the complexity
    // when the bespoke targeting already does the right thing.
    bool useSubgoalFacing = false;
    if (AnchorFollower::IsAiFollowerNavSubstrateEnabled()) {
        const EnemyNetId* targetExt =
            ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
        AnchorNav::TrailKey enemyKey =
            AnchorNav::TrailKeyForActor(targetExt != nullptr ? targetExt->netId : 0);
        const bool sceneChanged = (followerNavPath.sceneNum != gPlayState->sceneNum);
        const bool keyChanged   = (followerNavPathTargetKey != enemyKey);
        const f32 driftDx = navTarget.x - followerNavPathLastTarget.x;
        const f32 driftDz = navTarget.z - followerNavPathLastTarget.z;
        const bool targetDrifted =
            (driftDx * driftDx + driftDz * driftDz) >
            (kNavPathTargetDriftRefresh * kNavPathTargetDriftRefresh);
        const bool needsRefresh = followerNavPath.Empty() || sceneChanged ||
                                   keyChanged || targetDrifted;
        if (needsRefresh) {
            followerNavPath.Reset();
            bool gotPath = AnchorNav::ActorTrail::GetInstance().ComputePathTo(
                enemyKey, &player->actor, navTarget, gPlayState, followerNavPath);
            followerNavPathTargetKey  = enemyKey;
            followerNavPathLastTarget = navTarget;
            if (!gotPath) {
                SPDLOG_DEBUG("[Follower] ENGAGE NavPath empty (ComputePathTo "
                             "returned false; falling back to direct enemy yaw)");
            }
        }
        if (!followerNavPath.Empty()) {
            Vec3f sg = followerNavPath.CurrentSubgoal();
            f32   sgDx = sg.x - p2Pos.x;
            f32   sgDz = sg.z - p2Pos.z;
            if (sgDx * sgDx + sgDz * sgDz <
                kNavPathSubgoalReach * kNavPathSubgoalReach) {
                followerNavPath.Advance();
            }
            if (!followerNavPath.Empty()) {
                navTarget         = followerNavPath.CurrentSubgoal();
                useSubgoalFacing  = true;
            }
        }
    }
    followerMoveTarget = navTarget;
    // Facing: under nav substrate with an intermediate subgoal, face the
    // subgoal so Player_Update's auto-rotate aligns the body with stick
    // direction (reduces moonwalk-strafe visual). Otherwise face the
    // enemy directly — legacy behaviour, also covers En_St standoff
    // where navTarget != enemyPos but the body should still look at
    // the threat.
    if (useSubgoalFacing) {
        f32 aimDx = navTarget.x - p2Pos.x;
        f32 aimDz = navTarget.z - p2Pos.z;
        if (aimDx * aimDx + aimDz * aimDz > 1.0f) {
            player->actor.shape.rot.y = YawToward(aimDx, aimDz);
        } else if (distSq > 1.0f) {
            player->actor.shape.rot.y = YawToward(edx, edz);
        }
    } else if (distSq > 1.0f) {
        player->actor.shape.rot.y = YawToward(edx, edz);
    }
}

void Anchor::HandleStateAttack(Player* player, const Vec3f& p2Pos) {
    // ATTACK: 60-frame swing cycle (kAttackDuration). TickFollowerInput
    // injects BTN_B / BTN_R / BTN_A on cadence; this method tracks pose,
    // standoff target, and defeat / cycle-end transitions.
    //
    // Two-signal defeat check: colChkInfo.health <= 0 OR EnemyNetId
    // phase predicates (handles AC_HIT-only enemies whose health stays
    // at 1 through the whole death cycle).
    if (followerTargetEnemy == nullptr ||
        followerTargetEnemy->update == nullptr) {
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] ATTACK\u2192RETURN (enemy gone)");
        return;
    }
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
        SPDLOG_INFO("[Follower] ATTACK\u2192RETURN (enemy dead)");
        return;
    }
    if (fabsf(followerTargetEnemy->world.pos.y - p2Pos.y) >= kMaxYDelta) {
        followerAIState     = FollowerAIState::RETURN;
        followerStateFrames = 0;
        SPDLOG_INFO("[Follower] ATTACK\u2192RETURN (enemy off-floor)");
        return;
    }
    Vec3f enemyPos = followerTargetEnemy->world.pos;
    // Bug D - point followerMoveTarget at a standoff offset from
    // enemyPos. Stopping radius = attackRange - kSwingReach.
    f32 attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
    f32 standoff    = attackRange - kSwingReach;
    if (standoff < 20.0f) standoff = 20.0f; // sanity floor
    {
        f32 edx         = enemyPos.x - p2Pos.x;
        f32 edz         = enemyPos.z - p2Pos.z;
        f32 enemyDistSq = edx * edx + edz * edz;
        f32 enemyDist   = sqrtf(enemyDistSq);
        if (enemyDist > 1.0f) {
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
        SPDLOG_INFO("[Follower] ATTACK\u2192RETURN (cycle complete)");
    }
}

// ---------------------------------------------------------------------------
// Anchor::RegisterFollowerHooks — (re-)register the two follower hooks
// (OnGameFrameUpdate state-machine driver + ShouldActorUpdate input injector).
//
// Phase 1 commit 13 of the SRP refactor (#173 / #169). Body moved verbatim
// from HookHandlers.cpp's Anchor::RegisterHooks. Called from RegisterHooks
// at the same point in the enable/disable cycle, so the hook IDs persist
// (function-scope statics) across re-registrations.
//
// Position source for the follower: the host's DummyPlayer actor (ACTORCAT_NPC,
// id=ACTOR_EN_OE2, update=DummyPlayer_Update, clientId==roomState.ownerClientId).
// Its world.pos is updated every frame by DummyPlayer_Update to the host's
// authoritative position. Activation: toggled via the Anchor settings menu
// (AI Follower checkbox). Any controller input while active immediately cancels
// it and returns manual control.
//
// Note: COND_HOOK cannot be used here — the registered lambdas (or the bodies
// they would inline if COND_HOOK macros expanded over them in the future)
// contain brace-initializer lists for Vec3f, and the C preprocessor does NOT
// treat {} as grouping, so their commas split the macro's argument list.
// ---------------------------------------------------------------------------
void Anchor::RegisterFollowerHooks(bool isConnected) {
    // Follower state-machine driver. Fires from OnGameFrameUpdate; reads
    // host position, transitions follower state, computes followerMoveTarget,
    // and (in some states) writes player->actor.world.pos directly.
    //
    // Early-out checks live in this lambda since OnGameFrameUpdate fires
    // every frame regardless of relevance; we short-circuit before the
    // TickFollower call when not on a follower-relevant frame.
    {
        static HOOK_ID followerHookId = 0;
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnGameFrameUpdate>(followerHookId);
        followerHookId = 0;
        if (isConnected) {
            followerHookId = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([&]() {
                // Only run on non-host clients with a save loaded.
                if (::SceneAuthority::IsEffectiveHost()) { return; }
                if (!IsSaveLoaded()) { return; }
                if (gPlayState == nullptr) { return; }

                Player* player = GET_PLAYER(gPlayState);
                if (player == nullptr) { return; }
                AnchorFollower::FollowerFrameContext ctx;
                ctx.play   = gPlayState;
                ctx.player = player;
                this->TickFollower(ctx);
            });
        }
    }

    // Follower input injection (non-host only).
    //
    // Fires via ShouldActorUpdate immediately BEFORE the player actor's update()
    // so the player's own action state machine sees synthetic input and moves /
    // swings / climbs in response. (OnGameFrameUpdate fires too late — after
    // update() — so inputs written there would miss the current frame.)
    //
    // This hook is the PRIMARY driver of follower movement. The state machine
    // in OnGameFrameUpdate computes `followerMoveTarget`; this hook projects
    // that target into camera-relative stick input and lets Link's own
    // Player_Update carry him there — respecting walls, slopes, ledges,
    // water, cutscenes, and every other state transition OoT handles natively.
    //
    // Walk/run: stick is deflected toward followerMoveTarget with magnitude
    // scaled by distance (sprint > 250 units, run > 60, walk > 30, zero
    // within 30 so Link's own deceleration handles the last few units).
    //
    // State guard: stick is zeroed when Link is in a state that can't accept
    // free movement (ladder climb, ledge hang / climb-up, water, cutscene,
    // hit-react, talking, input disabled). Injecting during these can corrupt
    // the associated state machine.
    //
    // Ledge-climb: BTN_A is injected whenever PLAYER_STATE1_HANGING_OFF_LEDGE
    // is set — the follower runs up to a tall ledge, Link hangs, we press A,
    // Link hoists up. This replaces the old position-override-through-geometry
    // behaviour that clipped through ledges.
    //
    // Attack: BTN_B as an edge-press every 20 frames while in ATTACK state.
    // Stick is ALSO driven during ATTACK so the follower keeps closing the
    // gap between kAttackRange (80) and actual sword reach (~30-40 units);
    // without it the follower stops at 80 and swings at empty air. The stick
    // points at enemyPos, agreeing with shape.rot.y, so swing direction is
    // unambiguous regardless of which field OoT consults on the BTN_B frame.
    //
    // Timing note: ShouldActorUpdate sees followerStateFrames from the PREVIOUS
    // OnGameFrameUpdate (one frame before the next increment).  BTN_B is injected
    // when followerStateFrames % 20 == 0, which corresponds to frame 1, 21, 41
    // inside the ATTACK state after the next increment. The sword swing takes
    // ~20 frames, matching the cycle period.
    {
        static HOOK_ID followerAnimHookId = 0;
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::ShouldActorUpdate>(followerAnimHookId);
        followerAnimHookId = 0;
        if (isConnected) {
            followerAnimHookId = GameInteractor::Instance->RegisterGameHook<GameInteractor::ShouldActorUpdate>(
                [&](void* refActor, bool* should) {
                    (void)should; // we never block; only inject input
                    if (!followerActive)        { return; }
                    if (gPlayState == nullptr)  { return; }
                    Actor* actor = static_cast<Actor*>(refActor);
                    if (actor->id != ACTOR_PLAYER) { return; }

                    this->TickFollowerInput(actor);
                });
        }
    }
}
