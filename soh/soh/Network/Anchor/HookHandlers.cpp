#include "Anchor.h"
#include "EnemyNetId.h"        // #243.7.2 — explicit (was transitive via Anchor.h)
#include "ItemDropNetId.h"     // #243.7.2 — explicit (was transitive via Anchor.h)
#include "AIDirector/Director.h"      // AnchorDirector::Director::Instance() (Director scaffold step 1)
#include "AIPlayerFollower/Follower.h"      // FollowerFrameContext for the OnGameFrameUpdate wrapper (Phase 1 commit 4)
#include "soh/cvar_prefixes.h"        // CVAR_REMOTE_ANCHOR / CVAR_ENHANCEMENT (Nav system commit 6c)
#include "Common/ActorSyncHelpers.h"  // GetEnemySkelAnime, IsSyncedWorldActor, IsSyncableActor
#include "Common/SkelAnimeWire.h"     // kExpectedLimbCount (#154 — defense-in-depth limb-count registry)
#include "Common/StaleHostGate.h"     // ShouldDeferToPeerLocalAI — host-freshness gate for per-actor sync
#include "Common/PlayerLookup.h"      // FindNearestPlayerActor
#include "Common/SceneAuthority.h"    // IsEffectiveHost (Pillar A Phase 1)
#include "Common/ItemEligibility.h"   // CanPlayerCollectItem00 (#193 Phase 0)
#include "Common/PauseLinkBuffer.h"   // Anchor_IsDrawingPauseLink (#182 follow-up)
#include "NPCFollower/FollowerNPC.h" // Anchor_GetCurrentlyDrawingFollowerNpc (NPC color fix)
#include "Common/AINavTest.h"          // Navigation Test Harness — Tick() driver
#include "NPCInvader/Invader.h"          // Anchor_GetCurrentlyDrawingInvader (black-tint color fix)
#include "Common/ActorSyncScope.h"    // ActorSyncScope (Generic NPC State Sync Phase 0/1)
#include "Common/SyncedClaimableDrop.h"     // Plan B (#193) — drop arbitration registry
#include "Common/DropAdapters/GroundDropAdapter.h"  // Plan B step 3 — ground-drop adapter
#include "Common/DropAdapters/ModalOfferAdapter.h"  // MODAL_OFFER_CLAIMED match — adapter-identity check
#include "Common/DropAdapters/ModalPhantomAdapter.h"  // Plan B step 5 — modal-phantom adapter (Bug B fix)
#include "WorldStateSync/WorldStateSync.h"  // Pillar C v1
#include "soh/Enhancements/audio/VoicePack.h"  // Anchor_RefreshVoicePackTuningMultiplier (#83/#84 α.7+)
#include <algorithm>  // std::sort, std::min — title-screen peer formation
#include <chrono>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/frame_interpolation.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h" // AMMO / CUR_CAPACITY / INV_CONTENT — follower item-pickup need-gating
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
// Enemy struct headers for SkelAnime offset exceptions (see GetEnemySkelAnime below)
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
#include "src/overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
#include "src/overlays/actors/ovl_En_Hintnuts/z_en_hintnuts.h"
#include "src/overlays/actors/ovl_En_St/z_en_st.h"
// en_skb_sync_plan — Stalchild (En_Skb) state-machine sync.
#include "src/overlays/actors/ovl_En_Skb/z_en_skb.h"
#include "src/overlays/actors/ovl_En_Ssh/z_en_ssh.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
// #47 / en_firefly_sync_plan.md — Keese (En_Firefly) state-machine sync.
#include "src/overlays/actors/ovl_En_Firefly/z_en_firefly.h"
// en_crow_sync_plan.md — Guay (En_Crow) state-machine sync.
#include "src/overlays/actors/ovl_En_Crow/z_en_crow.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
// feature/sync-en-tite — Tektite state-machine + animation sync.
#include "src/overlays/actors/ovl_En_Tite/z_en_tite.h"
// #102 / en_reeba_sync_plan.md — Leever (En_Reeba) state-machine sync.
#include "src/overlays/actors/ovl_En_Reeba/z_en_reeba.h"
// en_ik_sync_plan.md — Iron Knuckle (En_Ik) state-machine sync.
#include "src/overlays/actors/ovl_En_Ik/z_en_ik.h"
// #99 / en_poh_sync_plan.md — Poe (En_Poh) state-machine sync.
#include "src/overlays/actors/ovl_En_Poh/z_en_poh.h"

// #107 / en_peehat_sync_plan.md - Peahat (En_Peehat) state-machine sync.
#include "src/overlays/actors/ovl_En_Peehat/z_en_peehat.h"

// #137 / en_eiyer_sync_plan — Stinger (En_Eiyer) state-machine sync.
#include "src/overlays/actors/ovl_En_Eiyer/z_en_eiyer.h"

// #128 / en_bili_sync_plan.md — Biri jellyfish (En_Bili) state-machine sync.
#include "src/overlays/actors/ovl_En_Bili/z_en_bili.h"
// #126 — Bari big jellyfish (En_Vali) state-machine sync.
#include "src/overlays/actors/ovl_En_Vali/z_en_vali.h"
// En_Zf — Lizalfos + Dinolfos state-machine sync.
#include "src/overlays/actors/ovl_En_Zf/z_en_zf.h"
// En_Mb — Moblin (Club / SpearGuard / SpearPatrol) state-machine sync.
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
// En_Bigokuta — Big Octo miniboss state-machine sync (#130).
#include "src/overlays/actors/ovl_En_Bigokuta/z_en_bigokuta.h"
// En_Fd — Flare Dancer enflamed shell state-machine sync (#100).
#include "src/overlays/actors/ovl_En_Fd/z_en_fd.h"
// En_GeldB — Gerudo Thief state-machine sync.
#include "src/overlays/actors/ovl_En_GeldB/z_en_geldb.h"
// En_Po_Field — Field Poe (Hyrule Field night) state-machine sync.
#include "src/overlays/actors/ovl_En_Po_Field/z_en_po_field.h"
// En_Vm — Beamos turret state-machine sync.
#include "src/overlays/actors/ovl_En_Vm/z_en_vm.h"
// En_Fw — Flare Dancer core/wisp state-machine sync (#100).
#include "src/overlays/actors/ovl_En_Fw/z_en_fw.h"
// #129 / en_bb_sync_plan.md — Bubble (En_Bb) state-machine sync.
#include "src/overlays/actors/ovl_En_Bb/z_en_bb.h"

// en_honotrap_sync — Fake-eye fire/ice traps (Fire/Ice/Shadow Temples).
#include "src/overlays/actors/ovl_En_Honotrap/z_en_honotrap.h"
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
// Issue #153 — En_Goroiwa is ACTORCAT_PROP, the first non-ENEMY actor synced.
#include "src/overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
// Boss_Goma — minimal Encounter -> FloorMain bridge (boss-fight trigger sync).
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
// Push-block bidirectional sync — `Anchor_IsActorMidPush` detects local push
// across all supported pushable actors. See Common/PushableActorState.cpp.
#include "soh/Network/Anchor/Common/PushableActorState.h"
// Hyrule Castle Talon any-client wake state sync. EnTa_NetSync_GetStateIndex /
// EnTa_NetSync_ApplyState are C-linkage helpers. See Claude/Analysis/talon_castle_wake_sync_2026-06-17.md.
#include "src/overlays/actors/ovl_En_Ta/z_en_ta.h"

extern PlayState* gPlayState;
extern MapData* gMapData;

// #276 — read by OnActorKill broadcast site to suppress ENEMY_DEFEATED
// for Obj_Mure2's distance-cull of grass-cluster children. Defined in
// Bridge/EnvActorBridge.cpp; set/cleared from z_obj_mure2.c's
// ObjMure2_CleanupAndDie via Anchor_BeginObjMure2Cull / End.
extern "C" bool Anchor_IsObjMure2CullingChildren(void);

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

// Pillar 5 (GH #310) — Karebaba geyser enhancement peer-flag bridges.
// Called BEFORE EnKarebaba_ApplyNetState on peer so the descriptor's
// per-actor state carries the enhanced-spin + charged flags when
// the local SetupSpin fires from ApplyNetState case 4, and so
// OnUprightTick can render the telegraph on peer.
extern "C" void Anchor_Enhance_EnKarebaba_ApplyPeerEnhancedFlag(EnKarebaba* actor, int enhanced);
extern "C" void Anchor_Enhance_EnKarebaba_ApplyPeerChargedFlag(EnKarebaba* actor, int charged);

// GetEnemySkelAnime, IsSyncedWorldActor, IsSyncableActor moved to
// Common/ActorSyncHelpers.h in #173 Phase 1.
// FindNearestPlayerActor moved to Common/PlayerLookup.h.

// Anchor-core extern "C" shims (Anchor_GetNearestPlayerActor /
// Anchor_GetSyncedPlayerActors / Anchor_IsEffectiveHost /
// Anchor_IsCurrentRoomHost / Anchor_IsAnyPeerOnDyna) moved to
// Bridge/AnchorCoreBridge.cpp on 2026-06-01 per refactor A.8.

// Receive-side state-machine logging dedup.
// The OnActorUpdate driver blocks for En_Sw / En_St / En_Dekunuts run
// every frame and decide apply-vs-block on (curState, netStateIndex).
// During a sustained block (e.g. dormant-active filter holding a peer
// in lunge while net says idle), naive logging would emit the same
// "block" line every frame at 20-60 Hz. Dedup by netId — log only when
// the encoded (curState<<16 | netState<<8 | blocked) tuple changes.
//
// One global map; netIds are scene-globally unique so collisions across
// actor types are impossible.
namespace {
std::unordered_map<uint32_t, uint32_t> sLoggedStateEncoded;
bool ShouldLogStateChange(uint32_t netId, int16_t cur, int16_t net, bool blocked) {
    uint32_t encoded = ((uint32_t)(uint16_t)cur << 16)
                     | ((uint32_t)(uint16_t)net << 8)
                     | (blocked ? 1u : 0u);
    auto it = sLoggedStateEncoded.find(netId);
    if (it == sLoggedStateEncoded.end() || it->second != encoded) {
        sLoggedStateEncoded[netId] = encoded;
        return true;
    }
    return false;
}

// Enemies where vanilla damage-response transitions to a state machine
// (stun / talk / flee / puzzle-counter / animation) rather than calling
// Actor_Kill. B2-D's fundamental assumption — "if peer's damage would
// be lethal, host will eventually broadcast ENEMY_DEFEATED" — is wrong
// for these actors: host correctly does NOT broadcast defeat (because
// nothing died), and peer's 1-second timeout fires spuriously, killing
// the actor mid-state.
//
// Populated 2026-07-13 from a walk of `IsSyncedWorldActor` allowlist
// per Claude/Plans/b2d_composite_fix_2026-07-13.md §3a. See analysis
// Claude/Analysis/hintnut_dialogue_disappear_2026-07-13.md for the
// motivating regression (hint nut vanish mid-dialogue).
//
// Layer 1 of the composite fix (Layers 2 + 3 apply additional gates
// further down at the B2-D fire site). Even if the composite Layer 2
// (phase gate) is expected to catch novel additions, this list is
// kept explicit so the known-safe class is documented in code.
bool IsStunNotDieActor(int16_t actorId) {
    switch (actorId) {
        case ACTOR_EN_HINTNUTS:  // Deku Tree 2→3→1 puzzle scrubs (base case)
        case ACTOR_EN_SSH:       // Cursed Skulltula people — Actor_Kill only at Init
                                 // (gsTokens threshold), never damage-driven
        case ACTOR_EN_MD:        // Mido NPC (Kokiri Forest / Lost Woods)
        case ACTOR_EN_TA:        // Talon NPC (castle wake / cucco-throw sequence)
            return true;
        default:
            return false;
    }
}
}  // namespace


// Anchor_ShouldSuppressHintnutsLocalAI moved to Bridge/NPCAIBridge.cpp
// on 2026-06-01 per refactor A.8.

// Anchor_NotifyProjectileHitEnemy / Anchor_NotifyTalkRequest /
// Anchor_NotifyDialogEnd moved to Bridge/EnemySyncBridge.cpp on
// 2026-06-01 per refactor A.8.

// Anchor_NotifyBossGomaLookedAt + Anchor_NotifyMidoPostDekuLeave
// moved to Bridge/BossGomaBridge.cpp on 2026-06-01 per refactor A.8.

// Anchor_ShouldAdvanceCutsceneTextLocal moved to Bridge/CutsceneBridge.cpp
// on 2026-06-01 per refactor A.8.

// Anchor_BossGomaConsumePeerSignaled moved to Bridge/BossGomaBridge.cpp
// on 2026-06-01 per refactor A.8.

// Anchor_NotifyEnemyHitPlayer moved to Bridge/EnemySyncBridge.cpp on
// 2026-06-01 per refactor A.8.

// ItemDrop file-statics + shims moved to Bridge/ItemDropBridge.cpp on
// 2026-06-01 per refactor A.8. Storage definitions live in the bridge;
// HookHandlers references them via the `extern` declarations in
// Bridge/ItemDropBridgeState.h (included above through Anchor.h's chain
// or directly below for clarity).
//
// Shims moved:
//   - Anchor_SetPendingItemDropInvisibleDecorative
//   - Anchor_BeginItemDropLocalOnly / Anchor_EndItemDropLocalOnly
//   - Anchor_BeginItemDrop / Anchor_EndItemDrop
//   - Anchor_BeginItemDropForKiller
//   - Anchor_IsReceivingNetworkItemDrop (now ORs with EnvActor's
//     Anchor_IsHostingPeerEnvActorDrop accessor)
//   - Anchor_BeginNetworkItemDropSpawn / Anchor_EndNetworkItemDropSpawn
//     (C++ helpers used by ItemDropSync.cpp / ItemDropSnapshot.cpp).
//
// File-statics relocated (declared extern in ItemDropBridgeState.h):
//   - g_pendingItemDropKillerClientId / g_pendingItemDropKillerTeamId
//   - g_pendingItemDropSpawnTimeMs / g_pendingItemDropDepth
//   - g_isSpawningNetworkItemDrop / g_pendingNetworkItemDropNetId
//   - g_pendingItemDropInvisibleDecorative
//   - g_isLocalOnlyItemDrop
//   - g_pendingItemDropBroadcasts (vector<PendingItemDropBroadcast>)
#include "Bridge/ItemDropBridgeState.h"

// #193 Phase 4 v2 — env-actor drop wrapper, replaces direct
// `Item_DropCollectible(play, pos, params)` calls in env-actor
// destroy paths (En_Kusa, etc.). Behavioural matrix:
//
//   Disconnected:       call vanilla Item_DropCollectible.
//   Connected, host:    call vanilla Item_DropCollectible. The
//                       OnActorSpawn(EN_ITEM00) hook broadcasts
//                       ITEM_DROP_SYNC.
//   Connected, peer,
//   actor has netId:    suppress local drop; send ENV_ACTOR_DROP to
//                       host. Host runs the drop and broadcasts; peer
//                       receives the broadcast and spawns the drop.
//   Connected, peer,
//   actor has no netId: call vanilla (env actor not in the synced
//                       allowlist — fall back to v1 behaviour). Should
//                       not occur once Phase 4 v2 admits all four
//                       env-actor IDs to IsSyncedWorldActor.
//
// #193 Phase 4 v3 — return type changed from `void` to `EnItem00*` so
// capture-and-modify call sites (Bg_Haka_Tubo, Bg_Spot18_Basket — set
// velocity.y / shape.rot.y on the spawned actor) keep compiling. On
// the peer-suppress path the wrapper returns NULL; the caller's
// `if (collectible != NULL)` post-modify branch becomes a no-op,
// which is a cosmetic-only effect (peer's broadcast-spawned drops
// don't inherit the fan-out velocity but land at the right position
// and are pickable).
//
// #193 Phase 4 v3 race-D mitigation — peer-side scene check before
// sending. If host has left the scene (or no host found in clients
// map), the ENV_ACTOR_DROP would either be applied on host's wrong
// scene or silently dropped at the receive guard. Falling back to a
// local Item_DropCollectible preserves a visible drop on this client;
// same shape as the offline branch above (single client, single drop,
// no broadcast). Strictly better than silent loss.
//
// Anchor_DropCollectibleEnvActor + Anchor_DropCollectibleRandomEnvActor
// moved to Bridge/EnvActorBridge.cpp on 2026-06-01 per refactor A.8.
// Anchor_BeginNetworkItemDropSpawn + Anchor_EndNetworkItemDropSpawn
// moved to Bridge/ItemDropBridge.cpp on 2026-06-01 per refactor A.8.
// Anchor_IsReceivingNetworkItemDrop moved to Bridge/ItemDropBridge.cpp.
// Anchor_BeginHostingPeerEnvActorDrop / Anchor_EndHostingPeerEnvActorDrop
// moved to Bridge/EnvActorBridge.cpp.

// Anchor_BeginNetworkEnvActorDestroy / Anchor_EndNetworkEnvActorDestroy /
// Anchor_BroadcastEnvActorDestroy moved to Bridge/EnvActorBridge.cpp
// on 2026-06-01 per refactor A.8 (along with the
// `g_isApplyingNetworkEnvActorDestroy` thread_local they share).

bool Anchor::IsLocalPlayerClimbing() const {
    if (gPlayState == nullptr) { return false; }
    Player* p = GET_PLAYER(gPlayState);
    if (p == nullptr) { return false; }
    u32 sf1 = p->stateFlags1;
    return (sf1 & PLAYER_STATE1_CLIMBING_LADDER)   ||
           (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) ||
           (sf1 & PLAYER_STATE1_CLIMBING_LEDGE);
}

// Test 5 (log 71) — leader crawlspace sync. Broadcast via
// UPDATE_CLIENT_STATE so the follower can recognise when the
// leader is crawling and adjust its follow target accordingly
// (use leaderPos directly instead of +kFollowOffset sideTarget;
// sideTarget puts follower off-axis from the crawlspace hole
// and DO_ACTION_ENTER never fires).
bool Anchor::IsLocalPlayerCrawling() const {
    if (gPlayState == nullptr) { return false; }
    Player* p = GET_PLAYER(gPlayState);
    if (p == nullptr) { return false; }
    return (p->stateFlags2 & PLAYER_STATE2_CRAWLING) != 0;
}

// FollowerTryEquipRangedWeapon, FollowerRestoreItems, SetFollowerActive
// — moved to AIPlayerFollower/Follower.cpp per Phase 1 commit 2 of the SRP
// refactor. Declarations remain in Anchor.h.

// Forward decls for the per-module draw-state reset functions invoked
// from the OnSceneInit defensive guard below. Each lives in its own .cpp
// (Skeleton.cpp / FollowerNPC.cpp / Invader.cpp) and clears the
// associated equipment/face swap active-flag WITHOUT running its normal
// End-path restore — the saved-pointer slots may reference resources
// freed during the scene transition.
extern "C" {
    void Anchor_LocalPlayerFaceSwapResetOnSceneTransition(void);
    void Anchor_FollowerNpcDrawStateResetOnSceneTransition(void);
    void Anchor_InvaderDrawStateResetOnSceneTransition(void);
}

void Anchor::RegisterHooks() {

    // One-shot startup clear of the NavTest harness master CVar.
    // AI.NavTest.Enabled persists across game launches (libultraship
    // CVars are saved to disk); when left on from a prior dev session,
    // it silently suppresses combat across AI Player Follower / NPC
    // Follower / NPC Invader via IsCombatDisabled gates. Force-off at
    // boot matches the project rule that vanilla-altering features
    // ship default-off, permanently.
    AINavTest::ClearOnStartup();

    // Horse-sync hook registrations live in HorseSync/HorseHooks.cpp.
    RegisterHorseHooks();

    // #region Hooks that are required for basic Anchor functionality

    // Defensive scene-transition draw-state reset (2026-05-20, log 66
    // crash class). OnSceneInit fires after the old scene's actors
    // are destroyed but BEFORE the first Player_Draw of the new scene.
    // Clear any equipment/face-texture swap active-flags that may
    // have been left set by a swap whose Begin/End wasn't paired (e.g.,
    // mid-draw scene transition tear-down). Without this guard, the
    // first Player_Draw of the new scene could read sEyeTextures /
    // modelGroup / DList pointers that the previous swap "saved" from
    // a now-freed BakedPlayerModel — crashing in the skel/limb draw
    // chain.
    //
    // Unconditional registration (no isConnected gate): cosmetic packs
    // work in single-player too, so the local face-swap can be active
    // without a live connection. NPC/Invader swaps require connection
    // for the actors to exist; their reset functions are safe no-ops
    // when no swap was active.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(
        [](int16_t sceneNum) {
            // Defensive log — helps correlate crashes to the scene
            // transition tear-down/rebuild window when a minidump
            // + PDB isn't available. See Analysis/deku_tree_multi-
            // player_incidents_2026-07-13.md §3 (Incident 5).
            SPDLOG_INFO("[Anchor.OnSceneInit] sceneNum=0x{:04X}",
                        (unsigned)sceneNum);

            // Queue item 39 diagnostic — checkpoints 3 + 4: on Lost
            // Woods (0x5B) and Hyrule Field (0x51) scene init, dump
            // slot[SLOT_OCARINA] to trace where INV_CONTENT is lost
            // between Item_Give and pause-menu inspection. Gated on
            // gEnhancements.OcarinaInvDiag CVar.
            if (CVarGetInteger("gEnhancements.OcarinaInvDiag", 0)) {
                const char* label = nullptr;
                if (sceneNum == SCENE_LOST_WOODS) label = "CP3 Lost Woods";
                else if (sceneNum == SCENE_HYRULE_FIELD) label = "CP4 Hyrule Field";
                if (label != nullptr) {
                    SPDLOG_INFO("[OcarinaDiag {}] sceneNum=0x{:04X} slot[SLOT_OCARINA]=0x{:02X} "
                                "eventChkInf(SPOKE_TO_SARIA_ON_BRIDGE=0xC1)={}",
                                label,
                                (unsigned)sceneNum,
                                (unsigned)gSaveContext.inventory.items[SLOT_OCARINA],
                                Flags_GetEventChkInf(EVENTCHKINF_SPOKE_TO_SARIA_ON_BRIDGE) ? 1 : 0);
                }
            }

            // Defensive null-guards. The three per-actor reset
            // functions and Director::OnSceneInitFromHook are safe
            // no-ops when Anchor state isn't initialised, BUT the
            // hook fires during the scene tear-down/rebuild window
            // where downstream globals may be in transient states.
            // The Kokiri Forest crash of 2026-07-13 (log 692, RAX=0
            // during Cutscene_HandleConditionalTriggers) motivates
            // an explicit guard at every entry point registered on
            // OnSceneInit — belt-and-suspenders vs. any downstream
            // null-deref hazard the individual reset functions
            // might introduce in future edits.
            if (::Anchor::Instance == nullptr) {
                SPDLOG_INFO("[Anchor.OnSceneInit] Anchor::Instance null — skipping downstream resets");
                return;
            }

            Anchor_LocalPlayerFaceSwapResetOnSceneTransition();
            Anchor_FollowerNpcDrawStateResetOnSceneTransition();
            Anchor_InvaderDrawStateResetOnSceneTransition();
            // Queue item 40 (Option D) — clear aborted-catchup tracking
            // on scene init so revisiting the same cutscene entrance in
            // a fresh scene load gets a fresh Setup attempt. Prevents
            // the suppression state from becoming sticky across scenes.
            if (!::Anchor::Instance->abortedCatchups.empty()) {
                SPDLOG_INFO("[Anchor.OnSceneInit] Clearing abortedCatchups (size={}) — fresh scene entry allows new catchup attempts",
                            ::Anchor::Instance->abortedCatchups.size());
                ::Anchor::Instance->abortedCatchups.clear();
            }
            // Phase 3 / issue #237: stamp the scene-change frame so
            // Fix 1's grace period kicks in on same-scene reloads
            // (Game Over → Continue at the same scene). The polling
            // observer in Director::Tick uses curScene != prevScene
            // and misses same-scene transitions. OnSceneInit fires
            // unconditionally on every scene init regardless of
            // sceneNum change, so this is the reliable signal.
            AnchorDirector::Director::Instance().OnSceneInitFromHook(sceneNum);
        });

    // Pause-menu open analog of the OnSceneInit reset above. Pitfall 22
    // (session_state.md) flagged this hazard class: the pause-menu draw
    // chain runs Player_DrawImpl with the same shared sEye/sMouthTextures /
    // modelGroup globals that cosmetic-sync swaps save and restore, but
    // pause stays in the same scene so OnSceneInit doesn't fire.
    //
    // Crash class confirmed by log 268 (Tab-key in Inside Deku Tree):
    // OnLinkSkeletonInit for pauseCtx->playerSkelAnime triggered a
    // re-bake on pause open; ~60ms later the pause-menu draw frame hit
    // Exception 0xc0000005 walking through ControllerUnblockGameInput.
    //
    // Rising-edge fire only (state 0 → !=0). Idempotent if it fires
    // when no swap was active. Unconditional registration for the same
    // reason as the OnSceneInit hook above (cosmetic packs work offline).
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() {
            static int sLastPauseState = 0;
            if (gPlayState == nullptr) {
                sLastPauseState = 0;
                return;
            }
            const int curr = (int)gPlayState->pauseCtx.state;
            if (sLastPauseState == 0 && curr != 0) {
                Anchor_LocalPlayerFaceSwapResetOnSceneTransition();
                Anchor_FollowerNpcDrawStateResetOnSceneTransition();
                Anchor_InvaderDrawStateResetOnSceneTransition();
            }
            sLastPauseState = curr;
        });

    // HeldActor diagnostic (Analysis/rock_over_head_after_teleport_
    // 2026-07-09.md §6.1). Per-frame poll of the local player's
    // heldActor pointer. On any transition (NULL→X, X→Y, X→NULL), logs
    // a rich SPDLOG describing prior/new attachment, player state, and
    // interactRangeActor at the moment of change — enough to pinpoint
    // WHICH mechanism (vanilla Player_LiftActor, DummyPlayer sync,
    // scene reload, teleport, etc.) drove the write.
    //
    // Gated on gEnhancements.Anchor.HeldActorDiag (default 0). Enable
    // via console: `set gEnhancements.Anchor.HeldActorDiag 1`. Fires
    // one line per transition, not per frame — no log spam when steady.
    //
    // Unconditional hook registration (not COND_HOOK) so the CVar can be
    // toggled at runtime without reconnecting. Cheap when disabled:
    // one CVarGetInteger + one pointer compare + early-out.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() {
            if (CVarGetInteger(CVAR_ENHANCEMENT("Anchor.HeldActorDiag"), 0) == 0) {
                return;
            }
            if (gPlayState == nullptr) return;
            Player* player = GET_PLAYER(gPlayState);
            if (player == nullptr) return;

            static Actor* sLastHeldActor = nullptr;
            Actor* curHeldActor = player->heldActor;
            if (curHeldActor == sLastHeldActor) {
                return;  // no change
            }

            // Log the transition. Each actor gets its own SPDLOG_INFO
            // line so we can print the fields directly via SPDLOG's
            // fmtlib backend without pre-formatting to std::string.
            // Emit player context first, then prior, then current.
            const Vec3f pPos = player->actor.world.pos;
            SPDLOG_INFO("[HeldActorDiag] TRANSITION player.pos=({:.0f},{:.0f},{:.0f}) "
                        "stateFlags1=0x{:08X} stateFlags2=0x{:08X}",
                        pPos.x, pPos.y, pPos.z,
                        (unsigned)player->stateFlags1,
                        (unsigned)player->stateFlags2);
            if (sLastHeldActor == nullptr) {
                SPDLOG_INFO("[HeldActorDiag]   prior=NULL");
            } else if (sLastHeldActor->update == nullptr &&
                       sLastHeldActor->draw == nullptr) {
                SPDLOG_INFO("[HeldActorDiag]   prior=(stale?) ptr={}",
                            (const void*)sLastHeldActor);
            } else {
                SPDLOG_INFO("[HeldActorDiag]   prior id=0x{:04X} cat={} params=0x{:04X} "
                            "pos=({:.0f},{:.0f},{:.0f}) parent={}",
                            (unsigned)sLastHeldActor->id,
                            (int)sLastHeldActor->category,
                            (unsigned)sLastHeldActor->params,
                            sLastHeldActor->world.pos.x,
                            sLastHeldActor->world.pos.y,
                            sLastHeldActor->world.pos.z,
                            (const void*)sLastHeldActor->parent);
            }
            if (curHeldActor == nullptr) {
                SPDLOG_INFO("[HeldActorDiag]   current=NULL");
            } else if (curHeldActor->update == nullptr &&
                       curHeldActor->draw == nullptr) {
                SPDLOG_INFO("[HeldActorDiag]   current=(stale?) ptr={}",
                            (const void*)curHeldActor);
            } else {
                SPDLOG_INFO("[HeldActorDiag]   current id=0x{:04X} cat={} params=0x{:04X} "
                            "pos=({:.0f},{:.0f},{:.0f}) parent={}",
                            (unsigned)curHeldActor->id,
                            (int)curHeldActor->category,
                            (unsigned)curHeldActor->params,
                            curHeldActor->world.pos.x,
                            curHeldActor->world.pos.y,
                            curHeldActor->world.pos.z,
                            (const void*)curHeldActor->parent);
            }
            Actor* ira = player->interactRangeActor;
            if (ira == nullptr) {
                SPDLOG_INFO("[HeldActorDiag]   interactRangeActor=NULL");
            } else {
                SPDLOG_INFO("[HeldActorDiag]   interactRangeActor id=0x{:04X} cat={} "
                            "params=0x{:04X} pos=({:.0f},{:.0f},{:.0f})",
                            (unsigned)ira->id,
                            (int)ira->category,
                            (unsigned)ira->params,
                            ira->world.pos.x,
                            ira->world.pos.y,
                            ira->world.pos.z);
            }

            sLastHeldActor = curHeldActor;
        });

    // Phase α.7+ — voice-pack game-thread polls (unconditional; work
    // offline + multiplayer). Both calls are cheap no-ops in the steady
    // state:
    //   - RefreshVoicePackTuningMultiplier: one CVarGetFloat + atomic
    //     store; cached value drives the audio-thread multiply.
    //   - ReconcileLocalVoicePack: one CVarGetString + mutex acquire +
    //     string compare. Triggers OnAudioModChanged ONLY when divergent
    //     (startup with non-empty CVar restored from disk, console set,
    //     or any other path that bypasses the dropdown's CustomFunction).
    // See Claude/Analysis/voice_pack_startup_reconcile_2026-06-18.md.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() {
            Anchor_RefreshVoicePackTuningMultiplier();
            Anchor_ReconcileLocalVoicePack();
        });

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        // Phase 1.5 pendingMigrateBack — if this client is the original
        // owner returning mid-migration, clear the hold flag now that we've
        // physically entered a scene. Election re-runs; ownerClientId-if-
        // online rule flips effective host back to us. Runs BEFORE the
        // #63 pendingTimeSync detection block below so subsequent
        // SendPacket_UpdateClientState carries the corrected authority
        // state. No-op unless (a) we ARE the original owner and (b) the
        // flag was set by a prior ALL_CLIENT_STATE reappearance.
        Anchor::Instance->ClearPendingMigrateBackOnSceneEntry();

        // #63 — detect frozen→advancing scene transition for
        // pendingTimeSync. MUST run BEFORE SendPacket_UpdateClientState
        // (next statement) so PrepClientState sees the flag set and
        // omits stale dayTime carried from the just-exited frozen scene.
        //
        // Vanilla freezes dayTime in dungeons / boss rooms / Lon Lon
        // Ranch via envCtx.timeIncrement = 0 (z_scene.c:354-368). When
        // exiting one of those scenes into a time-advancing scene
        // (Hyrule Field etc.), Play_Init's CommandTimeSettings has
        // already set gTimeIncrement to the new scene's non-zero
        // value by the time this hook fires — but our local dayTime
        // is still the stale LLR-entry-time value. Without this
        // detection, the immediate SendPacket_UpdateClientState below
        // broadcasts the stale value; receivers in the destination
        // scene apply it via modular distance, yanking THEIR clocks
        // backward in time. Log 538 captured this: P1 in HF was
        // yanked from morning 0x5E40 to late-afternoon 0xB1A2 by
        // P2's exit-LLR UPDATE_CLIENT_STATE.
        //
        // Setting pendingTimeSync here:
        //   - PrepClientState below this point omits dayTime/nightFlag.
        //   - SendPacket_TimeSync would also short-circuit.
        //   - The first incoming TIME_SYNC or UPDATE_CLIENT_STATE with
        //     dayTime is applied unconditionally (bypassing modular
        //     check), then the flag clears.
        //   - 15s timeout (managed in OnGameFrameUpdate below) prevents
        //     permanent deadlock if no peer broadcasts.
        if (gPlayState != nullptr && gTimeIncrement != 0 &&
            lastSceneTimeIncrement == 0) {
            pendingTimeSync = true;
            pendingTimeSyncFrames = 0;
            SPDLOG_INFO("[TimeSync] pending sync flagged "
                        "(scene transition unfroze clock: was 0, now {})",
                        gTimeIncrement);
        }
        lastSceneTimeIncrement = gTimeIncrement;

        // Bump before sending so the host's HandlePacket_UpdateClientState sees the
        // new epoch and fires the dead-enemy replay even when sceneNum and isSaveLoaded
        // are both unchanged (Game Over continue, void-out in the same scene).
        sceneSpawnEpoch++;
        SendPacket_UpdateClientState();
        // Request current state from all other clients so we pick up their
        // dayTime if they are in a time-advancing scene and we were not.
        // Responses arrive as UpdateClientState packets and are applied via
        // the forward-only bidirectional time sync in HandlePacket_UpdateClientState.
        if (IsSaveLoaded()) {
            SendPacket_RequestTeamState();
        }
        if (IsSaveLoaded()) {
            // Multiplayer kill persistence (log 162 fix, follow-on to
            // `06f540f0f` / `f822c2dee`):
            //
            // mSceneDeaths and mDefeatBroadcasts persist for the entire
            // session. Earlier code wiped both whenever the host's scene
            // changed — a single-player assumption (scene reload = enemies
            // revive). In multiplayer it breaks the case where the host
            // briefly leaves a scene that a peer is still inside: the peer
            // continues to hold the dead state, but the host wipes its
            // record and lets every previously-killed enemy come back alive
            // on re-entry (log 162 — host left Inside Deku Tree, P2 stayed,
            // host re-entered → all Skullwalltulas alive on host again).
            //
            // Persistence is safe: mSceneDeaths is keyed by
            // (sceneNum, netId); the host respawn guard at OnActorSpawn:3142
            // re-applies kills to freshly-spawned actors with matching netIds.
            // The Karebaba natural-respawn path (OnActorUpdate detector
            // ~line 3322) clears its specific netId from both maps on revival.
            //
            // Stale damagers DO get GC'd because their relevance is bounded
            // by scene lifetime; ClearStaleDamagers is still called.
            auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
            if (::SceneAuthority::IsMyCurrentRoomHost()) {
                bookkeeping.ClearStaleDamagers((int16_t)gPlayState->sceneNum);
                // KB-18 (#177) Option 4 — schedule the host-authoritative
                // netId snapshot broadcast for next OnGameFrameUpdate. We
                // can't fire it here because the static-actor batch is
                // still loading; deferring one frame guarantees every
                // static actor has completed its Init + had its EnemyNetId
                // extension assigned before we serialise.
                pendingSceneActorNetIdsBroadcast = true;

                // Exit-gated vacancy detection (Test 1.5 fix follow-up,
                // host-side counterpart of UpdateClientState's exit check).
                // When the host transitions to a new scene, the OLD scene
                // is now potentially empty — check whether any peer is
                // still tracking it. If no, broadcast SCENE_DEATHS_CLEARED.
                // The static `sLastHostSceneEntered` survives across calls
                // and tracks the prior sceneNum; -1 sentinel for first
                // entry of session.
                static int16_t sLastHostSceneEntered = -1;
                const int16_t  curScene             = (int16_t)gPlayState->sceneNum;
                if (sLastHostSceneEntered >= 0 && sLastHostSceneEntered != curScene) {
                    if (!AnyPeerInScene(sLastHostSceneEntered) &&
                        Anchor::Instance != nullptr) {
                        Anchor::Instance->SendPacket_SceneDeathsCleared(
                            sLastHostSceneEntered, 0xFF);
                    }
                }
                sLastHostSceneEntered = curScene;
            }
            // Phase 5 #60 — clear the per-netId last-sent cache. netIds are reused
            // across scene visits (same posHash, same enemy), so a stale cached
            // snapshot from a previous visit would cause the predicate to skip
            // legit sends until the keepalive timer elapses.
            Anchor_ClearEnemyUpdateCache();
            // Clear buffered kills that belong to scenes OTHER than the one we are
            // entering.  Kills for the destination scene must survive so that
            // OnActorSpawn can call SetupDeadItemDrop when those actors spawn.
            // (Clearing unconditionally caused Fix 35 to fail when P2 entered the
            // target scene from a different scene — the pending kill was wiped just
            // before the Karebaba spawned, so it appeared alive.)
            {
                uint16_t newScene = gPlayState ? (uint16_t)gPlayState->sceneNum : 0xFFFF;
                bookkeeping.ClearStalePendingKillsFromOtherScenes(newScene);
            }
            RefreshClientActors();

            // Pillar C v1 — apply replicated WorldState entries for the
            // (sceneNum, timeline) we're entering. Picks up flags peers
            // set while we were elsewhere.
            WorldStateSync::ApplyKnownFlagsForScene(
                (int16_t)gPlayState->sceneNum,
                (uint8_t)gSaveContext.linkAge);

            // #191 vote-state ordering — reset the monotonic sequence
            // counters on scene load. Keeps the integers small and avoids
            // stale-rejection after any scene reload (Game Over → Continue,
            // void-out) that recycles a fresh vote-tally cycle. Both host
            // and peer counters are reset unconditionally; only whichever
            // role we're in reads its counter next.
            Anchor::Instance->voteStateSequence           = 0;
            Anchor::Instance->peerLastAppliedVoteStateSeq = 0;

            // Cutscene late-join detection (Plans/cutscene_late_join_plan.md
            // §3.3). Scan same-team peers for any who are mid-cutscene in
            // our scene AND timeline. If found, request catchup and enter
            // the pending state — the gate predicate
            // Anchor_ShouldSuppressLocalCutsceneEntry() will suppress any
            // local vanilla cutscene entry until the response arrives (or
            // the 2s deadline elapses).
            //
            // Team gating: the relay routes CUTSCENE_START by targetTeamId,
            // so we only see cutsceneStartActive entries broadcast by peers
            // on our team. Combined with the client.teamId == myTeamId
            // filter here, we never request catchup from a peer on a
            // different quest branch.
            //
            // Variant C.2.2 (2026-07-09) — arm the scene-entry request
            // delay gate instead of calling DetectAndRequestCutsceneCatchup
            // immediately. TickCutsceneCatchup polls the gate and fires
            // the detection scan once the configured delay (default
            // 1000 ms) has elapsed. Gives peer's scene-transition fade-in
            // + initial room render time to complete before catchup
            // pipeline engages. See Anchor.h catchupRequestGateArmedAt
            // for full rationale.
            //
            // Variant C.2.3 (2026-07-09) — also arm the fade-to-white
            // overlay state machine. Gated on HasSameSceneMidCsPeer() so
            // scenes without a mid-cutscene peer don't trigger a
            // spurious 1 s white flash. See Anchor.h catchupFadeState.
            if (Anchor::Instance->CutsceneCatchupEnabled() &&
                Anchor::Instance->HasSameSceneMidCsPeer()) {
                const auto now = std::chrono::steady_clock::now();
                Anchor::Instance->catchupRequestGateArmedAt = now;
                if (CVarGetInteger(CVAR_ENHANCEMENT(
                        "Anchor.CutsceneLateJoinFadeOverlay"), 1) != 0) {
                    Anchor::Instance->catchupFadeState =
                        Anchor::CatchupFadeState::FADING_TO_WHITE;
                    Anchor::Instance->catchupFadeStateChangedAt = now;
                    Anchor::Instance->catchupFadeHoldIdleSince =
                        std::chrono::steady_clock::time_point::min();
                }
            }

            // Also reset FRAME_SYNC seq counters on scene load — same
            // rationale as vote-state (keep small, avoid cross-scene
            // stale-rejection).
            Anchor::Instance->cutsceneFrameSyncSequence      = 0;
            Anchor::Instance->peerLastAppliedCutsceneFrameSeq = 0;

            // Phase 4a — defensive cleanup of wait-for-peer barriers on
            // scene transition. Barriers armed in the departed scene
            // are meaningless in the new scene (peers are elsewhere).
            // Under normal flow, Play_InCsMode freeze prevents scene
            // transitions during a wait; this cleanup handles edge
            // cases (crash-recovery, forced warp, timeout race).
            //
            // Plans/phase_4a_wire_first_consumer_design_2026-07-16.md
            // Change 6.
            Anchor::Instance->pendingCoordination.clear();
        }
    });

    // Title-screen peer actors (Phase 1 — Plans/title_screen_peer_actors.md).
    // Unconditional registration (NOT gated on isConnected) — title-screen
    // runs before any Anchor session is established and the gate inside the
    // lambda still requires connected peers, so no spurious spawns when
    // disconnected.
    //
    // Lifecycle: fires on every scene init. Always clears the cached map
    // first (cached pointers may be dangling after scene reload — per the
    // session_state.md "Scene-transition pointer cleanup" rule). Then if
    // gate (gameMode == TITLE_SCREEN && sceneNum == SCENE_HYRULE_FIELD &&
    // CVar on), spawns fresh peer actors for same-team connected peers,
    // capped at 3, alphabetical-by-name selection (Q1 locked recommendation).
    // Empty-team behaviour is strict — render nobody (Q3 locked).
    // CVar defaults on (Q2 locked).
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>([]() {
        Anchor::Instance->MaybeRebuildTitlePeers();
        // Issue #63 — reset the TIME_SYNC cutscene-edge detector's previous
        // state to CS_STATE_IDLE (0) on every scene load. Vanilla resets
        // csCtx.state to IDLE during Play_Init for the new scene; without
        // this matching reset on our tracker, a scene transitioned out of
        // mid-cutscene would leave prevCsState non-IDLE and produce a
        // spurious cs_end edge on the new scene's first frame.
        Anchor::Instance->prevCsState = 0;  // CS_STATE_IDLE
    });

    // Pillar C v1 — local FLAG_SCENE_SWITCH set fires this hook from
    // Flags_SetSwitch (z_actor.c:675). Broadcast via WorldStateSync
    // unless we're applying a network-driven set (echo guard).
    COND_HOOK(OnSceneFlagSet, isConnected, [&](int16_t sceneNum, int16_t flagType, int16_t flag) {
        if (flagType != FLAG_SCENE_SWITCH) return;  // v1 scope
        if (WorldStateSync::IsApplyingNetworkFlag()) return;
        WorldStateSync::OnLocalFlagSet(sceneNum, flagType, flag);
    });

    // Pillar C v1 unset symmetry — local FLAG_SCENE_SWITCH unset fires
    // OnSceneFlagUnset from Flags_UnsetSwitch (z_actor.c:700). Mirror of
    // the OnSceneFlagSet handler above.
    COND_HOOK(OnSceneFlagUnset, isConnected, [&](int16_t sceneNum, int16_t flagType, int16_t flag) {
        if (flagType != FLAG_SCENE_SWITCH) return;
        if (WorldStateSync::IsApplyingNetworkFlag()) return;
        WorldStateSync::OnLocalFlagUnset(sceneNum, flagType, flag);
    });

    COND_HOOK(OnPresentFileSelect, isConnected, [&]() { SendPacket_UpdateClientState(); });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;

        if (spawningDummyPlayerForClientId != 0) {
            SetDummyPlayerClientId(actor, spawningDummyPlayerForClientId);

            // By the time we get here, the actor was already added to the ACTORCAT_PLAYER list, so we need to move it
            Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
            actor->id = ACTOR_EN_OE2;
            actor->category = ACTORCAT_NPC;
            actor->init = DummyPlayer_Init;
            actor->update = DummyPlayer_Update;
            actor->draw = DummyPlayer_Draw;
            actor->destroy = DummyPlayer_Destroy;
        }
    });

    COND_HOOK(OnPlayerUpdate, isConnected, [&]() {
        if (justLoadedSave) {
            justLoadedSave = false;
            SendPacket_RequestTeamState();
        }

        if (shouldRefreshActors) {
            shouldRefreshActors = false;
            RefreshClientActors();
        }

        // Diagnostic: log when local player's skelAnime.skeleton pointer changes.
        // Helps identify if something overwrites the skeleton AFTER UpdateCustomSkeletons sets it.
        static void* sLastLocalSkeleton = nullptr;
        if (gPlayState != nullptr) {
            Player* localPlayer = GET_PLAYER(gPlayState);
            if (localPlayer != nullptr) {
                void* curSkel = (void*)localPlayer->skelAnime.skeleton;
                if (curSkel != sLastLocalSkeleton) {
                    SPDLOG_INFO("[CoopModel] LocalPlayer skelAnime.skeleton changed: {} -> {}",
                                sLastLocalSkeleton, curSkel);
                    sLastLocalSkeleton = curSkel;
                }
            }
        }

        // G1/G2 — broadcast climbing-state edge changes so remote followers can
        // teleport-and-ride. Edge-only (not every frame): UPDATE_CLIENT_STATE is
        // a heavy packet and climbing transitions are infrequent.
        static bool sLastClimbing = false;
        bool nowClimbing = IsLocalPlayerClimbing();
        if (nowClimbing != sLastClimbing) {
            sLastClimbing = nowClimbing;
            SPDLOG_INFO("[Follower] LocalPlayer isClimbing edge: {} -> {}",
                        !nowClimbing, nowClimbing);
            SendPacket_UpdateClientState();
            // Bug 1 fix — arm BTN_A mask hold on climb-exit edge. The
            // injected (or contact-grabbed) BTN_A can persist in
            // press.button into the frame AFTER stateFlags1 climb bits
            // clear; without this hold, the deactivate-check sees BTN_A
            // unmasked and turns the follower off. ~10 frames covers the
            // OoT input-clear race comfortably.
            if (!nowClimbing) {
                followerClimbExitCooldown = 10;
            }
        }

        SendPacket_PlayerUpdate();
    });

    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        ProcessIncomingPacketQueue();

        // #63 — pendingTimeSync timeout. Counter increments each frame
        // while the flag is set. If no incoming TIME_SYNC or
        // UPDATE_CLIENT_STATE with dayTime arrives within 15s, clear
        // the flag and resume normal behavior. Matches single-player
        // solo behavior (clock resumes from frozen-entry value).
        // Periodic TIME_SYNC fires every 5s default, so 15s = 3
        // expected intervals — generous slack for packet loss.
        if (pendingTimeSync) {
            pendingTimeSyncFrames++;
            static constexpr int kPendingTimeSyncTimeoutMs = 15000;
            const int timeoutTicks = MsToGameTicks(kPendingTimeSyncTimeoutMs);
            if (timeoutTicks > 0 && pendingTimeSyncFrames > timeoutTicks) {
                pendingTimeSync = false;
                pendingTimeSyncFrames = 0;
                SPDLOG_INFO("[TimeSync] pending sync timeout "
                            "(15s no incoming sync, resuming local)");
            }
        }

        // Game-tick interval measurement (2026-05-15 log 118 followup).
        // Sample wall-clock delta since the previous tick and EWMA-smooth
        // it into Anchor::mAvgGameTickMs. Consumers convert ms thresholds
        // to game-tick counts via Anchor::MsToGameTicks(ms). Cap implausible
        // deltas (load screens, scene transitions, hitches > 200ms) so
        // the rolling average stays stable at the real game-loop rate.
        {
            const uint64_t nowMs = (uint64_t)
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if (Anchor::Instance->mLastGameTickWallMs != 0) {
                const uint64_t delta = nowMs - Anchor::Instance->mLastGameTickWallMs;
                if (delta > 0 && delta < 200) {
                    // EWMA with alpha = 1/8 → ~24-tick (1.2s @ 20fps) response.
                    Anchor::Instance->mAvgGameTickMs =
                        (uint32_t)((Anchor::Instance->mAvgGameTickMs * 7 + (uint32_t)delta) / 8);
                }
            }
            Anchor::Instance->mLastGameTickWallMs = nowMs;
        }

        // Heartbeat liveness counter (#194 follow-up) — read by the
        // network thread when building the heartbeat payload. If this
        // hook stops firing (game thread frozen), the counter stops
        // advancing; peers detect a stale counter as IsClientGameFrozen
        // even though the network-thread heartbeat itself keeps flowing.
        Anchor::Instance->gameFrameCounter.fetch_add(
            1, std::memory_order_relaxed);

        // NPC Follower STATE broadcast (Phase 3) — runs only when
        // connected because there's nothing to broadcast otherwise.
        // The CVar polling driver lives in NpcFollowerInit.cpp under
        // an unconditional ShipInit hook so the NPC works in single-
        // player AND multiplayer.
        Anchor::Instance->TickFollowerNpcStateBroadcast();

        // Navigation Test Harness — DNF timeout + run-complete bookkeeping.
        // Tick is cheap (no-op when harness disabled or no run active).
        AINavTest::Tick();

        // #191 — host countdown for cutscene-textbox vote-skip. No-op
        // when no active textbox vote is in progress; broadcasts
        // CUTSCENE_TEXT_ADVANCED on timer-0.
        Anchor::Instance->TickCutsceneTextAdvance();

        // Cutscene late-join catchup tick (Plans/cutscene_late_join_plan.md).
        // 1Hz FRAME_SYNC emit (leader-only) + pending-catchup deadline
        // enforcement (peer-only). Both are internal gates; safe to
        // call every frame regardless of role.
        Anchor::Instance->TickCutsceneCatchup();

        // Generic CUTSCENE_START / CUTSCENE_END edge detector — watches
        // gSaveContext.cutsceneIndex transitions for the `savecontext`
        // dispatch class. Actor-driven kinds (deku_tree_intro etc.)
        // fire independently from the actor's C-side Anchor_Notify call.
        // Both paths share the same send-side dedup so double-fires are
        // absorbed. Plans/packet_family_cutscene_start_end.md.
        Anchor::Instance->TickCutsceneStartDetector();

        // Wait-for-peer coordination barrier reap (Phase 2b). Cheap
        // linear scan of pendingCoordination — usually empty or 1-3
        // entries. Releases barriers whose 15 s timeout has fired.
        // Dormant unless a consumer arms a barrier (Phase 4a).
        Anchor::Instance->TickCoordinationBarriers();

        // KB-18 (#177) Option 4 — deferred host snapshot broadcast.
        // OnSceneSpawnActors host-path armed pendingSceneActorNetIdsBroadcast;
        // we drain it on the next frame so every static actor has completed
        // Init + EnemyNetId assignment before we serialise.
        if (pendingSceneActorNetIdsBroadcast) {
            pendingSceneActorNetIdsBroadcast = false;
            SendPacket_SceneActorNetIds();
        }

        // KB-15 / issue #110 + KB-19 / issue #176 — retire vector tick.
        // Each client carries a vector of {model, framesRemaining} retirees.
        // Decrement every entry's counter; entries reaching zero are erased
        // (the unique_ptr's destructor frees the model). By that point every
        // Gfx frame that could have referenced it has been fully consumed by
        // the renderer. See AnchorClient::RetireBakedModel and kRetireFrames
        // in Anchor.h. KB-19's vector replaces the prior single-slot pattern,
        // which destroyed prior retirees during rapid re-bakes.
        for (auto& [id, client] : clients) {
            for (auto it = client.retiredBakedModels.begin();
                 it != client.retiredBakedModels.end();) {
                if (--it->framesRemaining <= 0) {
                    it = client.retiredBakedModels.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Issue #82 — sibling retire tick for local-player baked skeletons.
        // UpdateCustomSkeletonFromFolder appends to the SkeletonPatchInfo's
        // retire vector on pack switch (same reasoning as the AnchorClient
        // loop above).
        for (auto& skel : SOH::SkeletonPatcher::skeletons) {
            for (auto it = skel.retiredBakedModels.begin();
                 it != skel.retiredBakedModels.end();) {
                if (--it->framesRemaining <= 0) {
                    it = skel.retiredBakedModels.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Issue #171 fix C — within-scene room-transition detection.
        // The host's "is peer in same room" gate at
        // SendPacket_EnemyUpdate:251-264 reads `clients[peer].curRoomNum`,
        // which is only refreshed by UPDATE_CLIENT_STATE. UPDATE_CLIENT_STATE
        // fires on scene transitions (via OnSceneSpawnActors) but NOT on
        // room transitions WITHIN a scene — so when we move to a new room,
        // peers' view of our curRoomNum lags until the next scene
        // transition, and the host floods us with cross-room ENEMY_UPDATE
        // packets the whole time. Detect the change locally and fire an
        // UPDATE_CLIENT_STATE so the host's gate can suppress correctly.
        //
        // -1 sentinel: first frame after registration, no prior value to
        // compare against. Initialise without firing — the first real
        // transition (-1 → N) is a no-op; subsequent (N → M) sends.
        if (IsSaveLoaded() && gPlayState != nullptr) {
            static s8 lastObservedRoomNum = -1;
            s8 curRoom = (s8)gPlayState->roomCtx.curRoom.num;
            if (curRoom != lastObservedRoomNum) {
                bool firstObservation = (lastObservedRoomNum == -1);
                lastObservedRoomNum = curRoom;
                if (!firstObservation) {
                    SendPacket_UpdateClientState();
                }
            }
        }

        // ---------------------------------------------------------------
        // Issue #63 — TIME_SYNC periodic + cutscene-edge sends.
        //
        // Design: forward-only bidirectional reconcile (no host authority).
        // Periodic broadcasts cap the dungeon-vs-overworld desync gap.
        // Cutscene edges ensure both clients enter and leave each cutscene
        // with synchronized clocks (otherwise the off-leader's local time
        // keeps advancing during the leader's frozen cutscene). The
        // scene-transition edge below (in the SCENE_TRANSITION_HANDOFF
        // block) covers the third boundary.
        //
        // Periodic respects CVAR_REMOTE_ANCHOR("TimeSync.Enabled") (default
        // 1); edges always fire. Both gated on save loaded + normal
        // gameplay (excludes file-select / name-entry / end-credits).
        //
        // Anchor::prevCsState is reset on the OnSceneSpawnActors handler
        // above (line ~503) to prevent a false cs_end edge on every scene
        // transition: vanilla resets csCtx.state to IDLE on scene load,
        // but our cross-frame tracker would otherwise hold the prior
        // scene's last non-IDLE value.
        // See Claude/Analysis/time_of_day_sync_implementation_analysis_2026-06-16.md.
        if (IsSaveLoaded() && gPlayState != nullptr &&
            gSaveContext.gameMode == GAMEMODE_NORMAL) {
            // Periodic tick — read interval CVar each frame so menu/console
            // changes take effect immediately. Clamp defends against
            // malformed values.
            const bool periodicEnabled =
                CVarGetInteger(CVAR_REMOTE_ANCHOR("TimeSync.Enabled"), 1) != 0;
            if (periodicEnabled) {
                static int sTimeSyncTickCounter = 0;
                const int intervalSec = std::clamp(
                    CVarGetInteger(
                        CVAR_REMOTE_ANCHOR("TimeSync.IntervalSeconds"), 5),
                    1, 60);
                const int intervalTicks = MsToGameTicks(intervalSec * 1000);
                if (intervalTicks > 0 &&
                    ++sTimeSyncTickCounter >= intervalTicks) {
                    sTimeSyncTickCounter = 0;
                    SendPacket_TimeSync("periodic");
                }
            }

            // Cutscene-edge detector — fires once per cutscene boundary.
            // csCtx.state values: CS_STATE_IDLE (0), SKIPPABLE_INIT (1),
            // SKIPPABLE_EXEC (2), UNSKIPPABLE_INIT (3), UNSKIPPABLE_EXEC (4).
            // Any non-IDLE means "in cutscene". Edges bypass the .Enabled
            // CVar — they're correctness-critical for boundary alignment.
            //
            // prevCsState is a file-static that persists across scene
            // transitions; the OnSceneSpawnActors hook below resets it to
            // CS_STATE_IDLE so the first cutscene of each new scene gets
            // a clean start edge rather than a false cs_end inheriting
            // the prior scene's state.
            const u8 curCsState = gPlayState->csCtx.state;
            if (prevCsState == CS_STATE_IDLE && curCsState != CS_STATE_IDLE) {
                SendPacket_TimeSync("cs_start");
            } else if (prevCsState != CS_STATE_IDLE &&
                       curCsState == CS_STATE_IDLE) {
                SendPacket_TimeSync("cs_end");
            }
            prevCsState = curCsState;
        }

        // Phase C — SCENE_TRANSITION_HANDOFF leader-side broadcast.
        // Every client runs this: on the rising edge of transitionTrigger
        // (OFF → START), capture our current position and destination
        // entrance, broadcast to other clients. A client with follower mode
        // active will use the data to follow us through the transition
        // (SceneTransitionHandoff.cpp HandlePacket_… stashes it pending; the
        // follower state machine below consumes it once within proximity).
        //
        // Same edge also fires BOSS_EXIT_TEAM_WARP when (sourceScene,
        // destEntrance) is a synced-boss-exit pair. That packet pulls
        // teammates currently in the same boss room through the same warp
        // so post-fight exits stay grouped (post-Goma cutscene chain).
        //
        // Issue #63 piggyback — also fire TIME_SYNC("scene_transition") on
        // the same edge so the receiving client's first frame in the new
        // scene loads with synchronized time. Bypasses the .Enabled CVar
        // (edge sends always fire when connected + save loaded + NORMAL).
        if (IsSaveLoaded() && gPlayState != nullptr) {
            s32 curTrigger = gPlayState->transitionTrigger;
            if (curTrigger == TRANS_TRIGGER_START &&
                prevTransitionTrigger == TRANS_TRIGGER_OFF) {
                Player* localPlayer = GET_PLAYER(gPlayState);
                s16 fromScene  = (s16)gPlayState->sceneNum;
                s16 toEntrance = (s16)gPlayState->nextEntranceIndex;
                if (localPlayer != nullptr) {
                    Vec3f triggerPos  = localPlayer->actor.world.pos;
                    s16   triggerRotY = localPlayer->actor.shape.rot.y;
                    SendPacket_SceneTransitionHandoff(fromScene, toEntrance,
                                                      triggerPos, triggerRotY);
                }
                if (IsSyncedBossExit(fromScene, toEntrance)) {
                    SendPacket_BossExitTeamWarp(
                        fromScene, toEntrance,
                        (u16)gSaveContext.nextCutsceneIndex,
                        (s8)gPlayState->transitionType,
                        (s8)gSaveContext.nextTransitionType);
                }
                // Issue #63 — TIME_SYNC on the same rising edge. Fires
                // BEFORE the new scene loads so the receiver's first
                // frame in the new scene has the synced time. Same
                // gameMode gate as the periodic tick above.
                if (gSaveContext.gameMode == GAMEMODE_NORMAL) {
                    SendPacket_TimeSync("scene_transition");
                }
            }
            prevTransitionTrigger = curTrigger;
        } else {
            prevTransitionTrigger = TRANS_TRIGGER_OFF;
        }

        // Plan B step 5 — backstop kill for stuck modal phantoms
        // (Bug B class). Cheap walk; no-op when the adapter's phantom
        // list is empty.
        SyncedClaimableDrop::ModalPhantomAdapter::GetInstance()->Tick();

    });

    // Follower hook registration. Body moved to AIPlayerFollower/Follower.cpp's
    // Anchor::RegisterFollowerHooks per Phase 1 commit 13 of the SRP refactor
    // (#173 / #169). Both hooks (OnGameFrameUpdate state-machine driver +
    // ShouldActorUpdate input injection) are re-registered there on every
    // enable/disable of Anchor.
    RegisterFollowerHooks(isConnected);

    // AI Director hook registration. Drives AnchorDirector::Director::Tick()
    // each frame on the global-effective-host. Step 1 scaffold: Tick body
    // is a no-op until descriptors are registered (step 7+). See
    // Plans/ai_director_plan.md §9.
    RegisterDirectorHooks(isConnected);

    // #region Enemy sync hooks (Phase 1 — visibility)

    // Assign a deterministic netId to every enemy actor on spawn so both clients
    // can refer to the same enemy without any handshake. Also store the SkelAnime
    // pointer so the send/receive path can sync joint tables without re-deriving
    // the offset every frame.
    //
    // Dynamic spawn detection: gPlayState->numSetupActors > 0 while the engine is
    // iterating the scene's setup actor list (z_actor.c Actor_UpdateAll). It is
    // zeroed immediately after that loop completes, before OnSceneSpawnActors fires.
    // Any OnActorSpawn with numSetupActors == 0 is therefore a runtime (dynamic)
    // spawn (e.g. Stalchild from En_Encount1, Peahat Larva). This check is reliable
    // across scene transitions — unlike the old sceneActorsLoaded flag which stayed
    // true from the previous scene and incorrectly suppressed static actors on P2.
    //
    // Host broadcasts a dynamic spawn via ENEMY_SPAWN; non-host kills the locally-
    // spawned actor and waits to receive the host's canonical copy. Actors spawned
    // in response to HandlePacket_EnemySpawn are exempt (isSpawningNetworkActor).
    COND_HOOK(OnActorSpawn, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            // Log any actor type that might be expected to sync but has wrong category.
            // This catches e.g. Deku Baba spawning with an unexpected initial category.
            if (actor->id == ACTOR_EN_DEKUBABA) {
                SPDLOG_WARN("[EnemySpawn] OnActorSpawn: ACTOR_EN_DEKUBABA skipped — cat={} (expected ACTORCAT_ENEMY={})",
                            (int)actor->category, (int)ACTORCAT_ENEMY);
            }
            return;
        }
        if (!IsSaveLoaded()) {
            return;
        }

        // Per-variant skip guards (Dekunuts flower, Hintnut reveal-child,
        // Honotrap flame). Shared with Anchor::BackfillEnemyNetIds so
        // reconnects don't retroactively assign netIds that OnActorSpawn
        // intentionally skipped. See ShouldSkipNetIdAssignment doc-comment
        // in ActorSyncHelpers.h for the individual per-actor rationale.
        if (ShouldSkipNetIdAssignment(actor)) {
            return;
        }

        // Defense-in-depth Layer 3: per-actor expected-limbCount registry.
        // Soft warning when local actor's skelAnime->limbCount diverges from
        // the registered value. Surfaces ROM variants, tampered asset packs,
        // or sync admissions that landed without a registry entry. The wire
        // layer's kHardCap=64 clamp continues to protect runtime regardless.
        // Audit: Plans/skelanime_expected_limbcount_registry_2026-06-15.md.
        //
        // Defer-init false-positive guard (#154 follow-up, log 547):
        // For static actors whose object isn't loaded at scene-load time,
        // `actor->init` is deferred (see comment at line ~1286 below). At
        // OnActorSpawn time their skelAnime is zero-init (limbCount=0).
        // The registry check should skip this transient state. Genuine
        // mismatches surface once init runs and limbCount becomes non-zero.
        {
            auto regIt = SkelAnimeWire::kExpectedLimbCount.find(actor->id);
            if (regIt != SkelAnimeWire::kExpectedLimbCount.end()) {
                SkelAnime* skel = GetEnemySkelAnime(actor);
                if (skel != nullptr && skel->limbCount != 0 &&
                    skel->limbCount != regIt->second) {
                    SPDLOG_WARN(
                        "[LimbCountRegistry] actor id=0x{:04X} limbCount={} expected={} (scene={} room={}) "
                        "— possible ROM variant or sync admission gap",
                        (int)actor->id, (int)skel->limbCount, (int)regIt->second,
                        gPlayState ? (int)gPlayState->sceneNum : -1,
                        gPlayState ? (int)gPlayState->roomCtx.curRoom.num : -1);
                }
            }
        }

        // TEMP DIAGNOSTIC (log 195 freeze investigation): Hintnut slot-reuse
        // probe. If OnActorSpawn fires for an actor pointer that already
        // carries a stale EnemyNetId extension, the old phase persists and
        // gates state-sync forever (PhaseImpliesHasLocalDeath = true blocks
        // every receive driver path). Log whatever ext is attached BEFORE
        // we Set<> a fresh one so we can confirm the hypothesis.
        if (actor->id == ACTOR_EN_HINTNUTS) {
            const EnemyNetId* priorExt =
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (priorExt != nullptr) {
                SPDLOG_WARN(
                    "[HintnutsSlotProbe] OnActorSpawn on actor ptr={} with stale ext: "
                    "prior netId={} prior phase={} prior hasNetState={} new params=0x{:04X}",
                    (void*)actor, priorExt->netId, (int)priorExt->phase,
                    priorExt->hasNetState, (int)actor->params);
            } else {
                SPDLOG_INFO(
                    "[HintnutsSlotProbe] OnActorSpawn on actor ptr={} (clean — no prior ext) "
                    "params=0x{:04X} home=({:.0f},{:.0f},{:.0f})",
                    (void*)actor, (int)actor->params,
                    actor->home.pos.x, actor->home.pos.y, actor->home.pos.z);
            }
        }

        // Cross-scene-respawn fix (log 115 bug — Deku Babas didn't respawn on
        // host after both players left and re-entered scene 0x0):
        // OnSceneSpawnActors fires AFTER the setup-actor loop completes
        // (z_actor.c:2598). The setup actors that fire OnActorSpawn during
        // that loop see deadEnemiesByScene[currentScene] still populated from
        // the prior visit, and the host-side check below (~L2985) suppresses
        // their respawn. By the time OnSceneSpawnActors fires and clears the
        // map, the suppression has already been applied.
        //
        // Fix: clear deadEnemiesByScene[currentScene] on the FIRST setup-actor
        // spawn of a fresh **scene** entry. The original implementation gated
        // on `numSetupActors > 0`, which fires not just on fresh scene init
        // but also on **intra-scene room transitions** that load static
        // actors for the new room (assumption "Room transitions within the
        // same scene leave numSetupActors == 0" was wrong). That wiped same-
        // scene-visit kill records every time the host walked between rooms.
        //
        // Symptom: log 161 — host kills En_Sw in Inside Deku Tree Room 10,
        // walks back to Room 0 then returns to Room 10, En_Sw is alive again
        // because mSceneDeaths was wiped on the Room 0 → Room 10 transition.
        // The Karebaba-only RecordPendingKill workaround in commit
        // `e276aa74e` papered over the symptom for that one actor; this gate
        // fixes it for the entire enemy class.
        //
        // mSceneDeaths peer-aware reset.
        //
        // Vanilla OoT respawns enemies whenever a scene is unloaded and
        // reloaded. In multiplayer, the scene is "unloaded for everyone"
        // only when no client is tracking it. So: on host scene-CHANGE
        // (sceneNum != prior cleared), clear mSceneDeaths[scene] iff no
        // remote client currently reports that scene.
        //
        // This restores vanilla parity for the "P1 alone leaves and
        // returns to X" and "both leave and return to X" cases while
        // preserving the multiplayer behaviour ("P1 leaves while P2 stays
        // in X, P1 returns" → kills persist because P2 is still tracking).
        //
        // Race tolerance: if a peer is briefly in transition during the
        // gate read, we may either clear (worst case: enemies respawn for
        // both, recoverable) or not clear (worst case: kills persist one
        // extra cycle). No crash either way.
        static int16_t sLastClearedSceneNum = -1;
        if (gPlayState != nullptr && gPlayState->numSetupActors > 0 &&
            ::SceneAuthority::IsMyCurrentRoomHost() &&
            (int16_t)gPlayState->sceneNum != sLastClearedSceneNum) {
            const int16_t targetScene = (int16_t)gPlayState->sceneNum;
            if (!AnyPeerInScene(targetScene)) {
                auto& bk = EnemyStateSync::HostBookkeeping::Instance();
                bk.ClearScene(targetScene);
                // Same gate clears the per-scene-visit broadcast-dedup set:
                // without this, an enemy respawned by the clear above and
                // re-killed later would have its ENEMY_DEFEATED dedup'd
                // (peers never see the second kill).
                bk.ClearAllDefeatBroadcasts();
            }
            sLastClearedSceneNum = targetScene;
        }

        bool isDynamicSpawn = (gPlayState->numSetupActors == 0);
        if (isDynamicSpawn) {
            if (::SceneAuthority::IsMyCurrentRoomHost()) {
                // Scene host: broadcast so other clients can spawn a matching actor.
                // SendPacket_EnemySpawn runs after the netId block below so the
                // actor already has a valid extension when the send path reads it.
                // We defer the actual send to after netId assignment — see below.
            } else if (!isSpawningNetworkActor && !isSpawningDirectorActor) {
                // Non-host: kill locally-generated dynamic actors immediately.
                // The host's canonical copy arrives via ENEMY_SPAWN and is spawned
                // by HandlePacket_EnemySpawn (which sets isSpawningNetworkActor).
                //
                // Tactical 3.6.A exemption (Plans/invader_per_room_authority_handoff.md
                // §3.6.A, issue #166 / log 351): also exempt
                // isSpawningDirectorActor. The Director runs on the global
                // effective host but spawns into per-room-host territory.
                // When effective host ≠ room host (random clientId draws —
                // ~50% of sessions), the Director's spawns previously got
                // killed here, with no peer ever seeing a functional Invader.
                //
                // Caveat: this lets the Director's Invader survive on the
                // effective host even when the host isn't room host of the
                // spawn target room. ENEMY_STATE broadcasts come from the
                // per-room host (via HookHandlers.cpp:1682's IsMyCurrentRoomHost
                // gate), so the broadcast direction is reversed from typical:
                // peer (room host) broadcasts → effective host (Director)
                // receives. This is functional but architecturally awkward.
                // The full fix is Step 5 (per-room authority for the Director
                // itself); this exemption is the stopgap that makes Force
                // Spawn reliable on any clientId arrangement for field
                // testing.
                SPDLOG_INFO("[EnemySpawn] Suppressing dynamic spawn actorId={} on non-host", actor->id);
                Actor_Kill(actor);
                return;
            }
        }

        // Deterministic netId — same scene + actor id + home position +
        // timeline produce the same netId on every client. The formula
        // lives in ActorSyncHelpers::EncodeEnemyNetId so this site and the
        // OnConnected reconnect-backfill path stay in lockstep.
        //
        // KB-18 (#177) Option 4 — non-host overrides the local compute
        // with the host's snapshot value when available. Some actors
        // (En_Sw confirmed) mutate home.pos non-deterministically inside
        // Init; the local compute then disagrees with the host's. The
        // snapshot's matched entry takes precedence; falls through to
        // local compute when no entry matches (host hasn't sent yet,
        // or actor is a dynamic spawn outside the snapshot scope).
        //
        // Dynamic-spawn collision avoidance (#67-Gohma crash root cause):
        // When the host generates a dynamic spawn (numSetupActors == 0)
        // and the deterministic posHash collides with another already-
        // spawned actor's netId in the same scene, probe-bump the low 8
        // bits until unique. The host's authoritative value is broadcast
        // in ENEMY_SPAWN; non-host adopts it via HandlePacket_EnemySpawn.
        // Static spawns keep the deterministic encoding so KB-18 snapshot
        // matching continues to work.
        uint32_t netId = 0;
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
            netId = LookupHostNetIdForCurrentScene(actor);
        }
        if (netId == 0) {
            if (::SceneAuthority::IsMyCurrentRoomHost() && isDynamicSpawn &&
                !isSpawningNetworkActor) {
                netId = EncodeUniqueDynamicNetId(actor);
            } else {
                netId = EncodeEnemyNetId(actor);
            }
        }

        EnemyNetId ext;
        ext.netId = netId;
        ext.skelAnime = GetEnemySkelAnime(actor);
        ext.limbCount = ext.skelAnime ? ext.skelAnime->limbCount : 0;
        ObjectExtension::GetInstance().Set<EnemyNetId>(actor, std::move(ext));

        SPDLOG_INFO("[EnemySpawn] Extension assigned: actorId={} netId={} ptr={} home=({:.0f},{:.0f},{:.0f}) posHash=0x{:02X} limbCount={} {}",
                    actor->id, netId, (void*)actor,
                    actor->home.pos.x, actor->home.pos.y, actor->home.pos.z,
                    (int)(netId & 0xFF), (int)ext.limbCount,
                    isDynamicSpawn ? "dynamic" : "static");

        // Bug 1 (2026-04-22, log 68) — host respawn guard.
        //
        // OoT unloads / reloads room actors on every room transition, even
        // within a single scene visit. When the host briefly leaves a room
        // (e.g., Deku Tree Room 0 → Room 1 (Mad Scrub) → Room 0 again,
        // ~6 s round trip), all room-0 actors get freshly re-spawned via
        // their normal Init path. Their EnemyNetIds are recomputed
        // deterministically (same scene + actor id + posHash → same netId
        // values as before). Without this guard, the host's fresh Dekubabas
        // come back alive, host sends ENEMY_UPDATE with health > 0, and
        // non-host clients that had already killed them are forced to choose
        // between (a) overwriting their dead state (visually revives the
        // enemy) or (b) keeping it dead via hasLocalDeath (mismatch between
        // clients).
        //
        // Fix: on host, check deadEnemiesByScene[sceneNum] for the freshly-
        // computed netId. If present, the player previously killed this
        // enemy this scene-visit — kill it again on respawn. Same Karebaba /
        // non-Karebaba split as the pendingKillNetIds branch below; for
        // Karebaba the deferredDeadItemDrop flag is set so OnActorInit can
        // apply SetupDeadItemDrop AFTER actor->init() runs (otherwise
        // EnKarebaba_Init resets actionFunc back to Idle on Frame 1).
        // deadEnemiesByScene is cleared on OnSceneSpawnActors, so this
        // guard only suppresses same-scene-visit revivals — leaving and
        // re-entering the scene proper still respawns enemies as expected.
        //
        // Invader Step 5 follow-up: exempt isSpawningDirectorActor. The
        // Director may intentionally re-spawn an actor at a previously-
        // killed (scene, id, posHash) tuple — force-spawn from dev UI,
        // scene-follow continuation, #234 host-actor-missing reconcile.
        // Deterministic netId encoding collides with the prior kill's
        // SceneDeath entry (EncodeUniqueDynamicNetId only probes live
        // actors, not the dead-enemy ledger). Without this exemption the
        // Director's Actor_Spawn returns a killed-inside-OnActorSpawn
        // actor while ExecuteSpawn still records it as live; next tick
        // fires the reconcile branch and loops forever. Mirrors the
        // sibling exemption on the dynamic-spawn suppression check at
        // line 1303. Non-Director dynamic spawns still hit this guard —
        // vanilla enemies that respawn on room re-entry are exactly
        // what SceneDeath was designed to suppress.
        if (::SceneAuthority::IsMyCurrentRoomHost() && !isSpawningDirectorActor) {
            if (EnemyStateSync::HostBookkeeping::Instance().IsSceneDeath(gPlayState->sceneNum, netId)) {
                SPDLOG_INFO("[EnemySpawn] deadEnemiesByScene hit for netId={} on host — "
                            "suppressing same-scene respawn (id={})",
                            netId, actor->id);
                if (actor->id == ACTOR_EN_KAREBABA) {
                    EnemyNetId* extPtr = const_cast<EnemyNetId*>(
                        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
                    if (extPtr != nullptr) {
                        EnemyStateSync::TransitionTo(*extPtr, EnemyStateSync::LifecyclePhase::AwaitingDeadItemDrop);
                        EnemyStateSync::HostBookkeeping::Instance().ClaimDefeatBroadcast(extPtr->netId);
                        // OnActorInit applies SetupDeadItemDrop after init() runs.
                        // Phase=AwaitingDeadItemDrop is the gate (was deferredDeadItemDrop).
                    }
                } else {
                    isKillingNetworkActor = true;
                    Actor_Kill(actor);
                    isKillingNetworkActor = false;
                }
                return;
            }
        }

        // If an ENEMY_DEFEATED for this netId arrived before the scene finished
        // loading (race between scene load and packet delivery), kill it now.
        // Karebaba: use the natural death cycle so it can respawn later, same as
        // HandlePacket_EnemyDefeated. Other enemies: direct Actor_Kill is fine.
        if (EnemyStateSync::HostBookkeeping::Instance().IsPendingKill(netId)) {
            // Karebaba: do NOT erase from pendingKillNetIds yet (Fix 35).
            // The actor moves to ACTORCAT_MISC for ~420 frames at 20fps. If the
            // player exits and re-enters the room during that window, OoT destroys
            // the ACTORCAT_MISC actor (room unload) and spawns a fresh one on
            // re-entry. Without the netId still in pendingKillNetIds, the fresh
            // actor starts alive. The erase is deferred to the non-host respawn
            // detection in OnActorUpdate, which fires when the actor returns to
            // ACTORCAT_ENEMY in a living state after completing the full cycle.
            // Non-Karebaba enemies are killed instantly so their erase is immediate.
            if (actor->id == ACTOR_EN_KAREBABA) {
                SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} (Karebaba) ptr={} — deferring dead state to OnActorInit (Fix 38)",
                            netId, (void*)actor);
                EnemyNetId* extPtr = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
                if (extPtr != nullptr) {
                    EnemyStateSync::TransitionTo(*extPtr, EnemyStateSync::LifecyclePhase::AwaitingDeadItemDrop);
                    EnemyStateSync::HostBookkeeping::Instance().ClaimDefeatBroadcast(extPtr->netId);
                    // Fix 38: defer SetupDeadItemDrop to OnActorInit.
                    // OnActorSpawn fires BEFORE actor->init() is called by Actor_UpdateAll
                    // (z_actor.c:3409 vs 2638). Calling SetupDeadItemDrop here causes
                    // EnKarebaba_Init (Frame 1) to override actionFunc=DeadItemDrop back
                    // to actionFunc=Idle. The next update() (Frame 2) then runs
                    // EnKarebaba_Idle, detects the player, and calls SetupAwaken — making
                    // the Karebaba appear alive for one frame.
                    // OnActorInit (z_actor.c:2641) fires AFTER actor->init() has run,
                    // so SetupDeadItemDrop can override actionFunc without being undone.
                    // 2026-04-25: removed the previous host-only gate. The 2026-04-22
                    // OnActorInit update made the hook gate on deferredDeadItemDrop
                    // (not host/non-host), so the host-suppression rationale here is
                    // obsolete. Symmetric application also covers the cross-room kill
                    // case (Option B fix): host receives ENEMY_DEFEATED while in a
                    // different OoT room, actor not yet in actor list at receive time,
                    // pendingKill branch fires when host walks into the Karebaba's room
                    // — without this, the host's respawn-detector trips on the post-init
                    // SetupAwaken before SetupDeadItemDrop can run.
                    // Phase=AwaitingDeadItemDrop set by TransitionTo above is now the gate.
                }
            } else {
                SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} — killing actor immediately", netId);
                EnemyStateSync::HostBookkeeping::Instance().ClearPendingKill(netId); // instant kill — safe to release now
                isKillingNetworkActor = true;
                Actor_Kill(actor);
                isKillingNetworkActor = false;
            }
            return;
        }

        // Scene-host deferred broadcast: send ENEMY_SPAWN for dynamic
        // actors now that the netId extension is in place.
        //
        // `!isSpawningNetworkActor` closes the echo cascade root for
        // #186. Without it, when a peer's spawn arrives at the host,
        // HandlePacket_EnemySpawn → Actor_Spawn → OnActorSpawn fires —
        // and the host re-broadcasts the spawn back to the original
        // sender, which then re-spawns and re-broadcasts in turn. The
        // log signature is dozens of ENEMY_SPAWN packets per 100ms for
        // the same netId family, ending in `Unhandled OP code` in the
        // F3DEX interpreter (renderer chasing display lists through a
        // corrupted segment table). isSpawningNetworkActor is set by
        // HandlePacket_EnemySpawn around the Actor_Spawn call, exactly
        // for this guard.
        if (isDynamicSpawn && ::SceneAuthority::IsMyCurrentRoomHost() &&
            !isSpawningNetworkActor && !isSpawningDirectorActor) {
            SendPacket_EnemySpawn(actor);
        }
    });

    // Generic SkelAnime late-bind (log 184 Hintnut sync regression).
    //
    // OnActorSpawn fires from Actor_Spawn (z_actor.c:3409) AFTER Actor_Init
    // returns, but Actor_Init only invokes `actor->init` when the actor's
    // object is loaded (z_actor.c:1256-1268). For static actors whose
    // object isn't loaded yet at scene-load time, the init call is
    // deferred — Actor_Init returns having done only the generic Actor*
    // bookkeeping; `actor->init` runs later in the deferred-init loop at
    // z_actor.c:2633-2645 and triggers OnActorInit (line 2641).
    //
    // Result: when our OnActorSpawn hook reads `ext.skelAnime =
    // GetEnemySkelAnime(actor)`, the SkelAnime field is still zeroed
    // (limbCount=0, jointTable=nullptr). GetEnemySkelAnime's validation
    // returns nullptr. The cached extension carries `skelAnime=nullptr,
    // limbCount=0` for the rest of the session. Both send-side
    // (EnemyState.cpp:616) and receive-side (EnemyState.cpp:884) gate
    // jointTable serialization on `ext->skelAnime != nullptr &&
    // ext->limbCount > 0`, so peer animation never syncs for affected
    // actors.
    //
    // User-observed reproduction (log 184 line 2086): Inside Deku Tree
    // Compound Room Hintnut spawn logged `limbCount=0`. State-machine
    // sync (actionState) worked because that field is set in
    // EnemyState.cpp:943 unconditionally — so peer's Hintnut state
    // changed when host's did, but joints stayed at the engine-init
    // pose (Hintnut visually in nest). Another Hintnut spawned later
    // (log line 3915) showed `limbCount=10` because its object was
    // already loaded at that scene's load time.
    //
    // Fix: OnActorInit hook runs AFTER actor->init() completes. At that
    // point SkelAnime is properly initialized. If our extension exists
    // and has a stale `skelAnime=nullptr`, re-run GetEnemySkelAnime and
    // update the cache. No-op for non-deferred actors (their extension
    // already has valid skelAnime from OnActorSpawn).
    COND_HOOK(OnActorInit, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr) return;
        EnemyNetId* ext = const_cast<EnemyNetId*>(
            ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
        if (ext == nullptr) return;  // not an admitted-for-sync actor
        if (ext->skelAnime != nullptr && ext->limbCount > 0) {
            return;  // already populated by OnActorSpawn — no-op
        }
        SkelAnime* fresh = GetEnemySkelAnime(actor);
        if (fresh != nullptr) {
            ext->skelAnime = fresh;
            ext->limbCount = fresh->limbCount;
            SPDLOG_INFO("[EnemySpawn] SkelAnime late-bound on OnActorInit: actorId={} netId={} limbCount={}",
                        actor->id, ext->netId, (int)ext->limbCount);
        }
    });

    // Fix 38 — apply deferred dead state after EnKarebaba_Init has run.
    // OnActorSpawn fires before actor->init() (z_actor.c:3409 vs 2638). Setting
    // actionFunc=DeadItemDrop there is immediately overridden by EnKarebaba_Init in
    // Frame 1, which resets actionFunc=Idle. The next update() then calls SetupAwaken.
    // OnActorInit fires at z_actor.c:2641, after actor->init() and before the first
    // update(), so we can safely override here without being overwritten.
    COND_ID_HOOK(OnActorInit, ACTOR_EN_KAREBABA, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // 2026-04-22 (Bug 1): host now also sets deferredDeadItemDrop in the
        // OnActorSpawn deadEnemiesByScene-respawn-guard branch. Gate on the
        // flag itself rather than on host/non-host so both code paths reach
        // the same SetupDeadItemDrop application below.
        if (!IsSaveLoaded() || gPlayState == nullptr) {
            return;
        }
        EnemyNetId* ext = const_cast<EnemyNetId*>(
            ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
        if (ext == nullptr || !EnemyStateSync::PhaseImpliesDeferredDeadItemDrop(ext->phase)) {
            return;
        }
        EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorInit.Karebaba.deferredDeadItemDrop");
        EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
        SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} (Karebaba) ptr={} — SetupDeadItemDrop after init (Fix 38)",
                    ext->netId, (void*)actor);
        // Set the same flags EnKarebaba_SetupDying sets (natural precursor to
        // DeadItemDrop). EnKarebaba_Init (via Actor_ProcessInitChain) resets flags
        // to standard enemy flags; we must re-apply the death flags now that init
        // has run. SetupDeadItemDrop itself clears DRAW_CULLING_DISABLED.
        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
        actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
        EnKarebaba_SetupDeadItemDrop((EnKarebaba*)actor, gPlayState);
        SPDLOG_INFO("[EnemySpawn] After SetupDeadItemDrop: ptr={} category={}",
                    (void*)actor, (int)actor->category);
    });

    // Generic NPC State Sync Phase 1 — Mido (#184) declares Team scope.
    // Per Plans/generic_npc_state_sync.md §10 (locked answer Q1+Q2),
    // each NPC instance opts into syncing at Init time. Mido is team-
    // scoped: a quest-progression NPC whose state should diverge across
    // teams. Phase 0 framework reads scope on the send-side (skip emit
    // for None) and applies the receive-side team filter for "team"-
    // scoped packets.
    //
    // Note: Phase 1 does NOT yet wire host-emit / non-host-emit logic
    // for full state-machine sync. Today the actual #184 fix is the
    // VB_MOVE_MIDO_IN_KOKIRI_FOREST hook below (each client transitions
    // independently when the synced EVENTCHKINF flag is set). The
    // scope declaration here is informational for the framework so a
    // future phase can wire peer-to-peer state sync without per-actor
    // changes. EnMd_GetStateIndex / EnMd_ApplyNetState in z_en_md.{h,c}
    // are the framework primitives those future-phase wiring will use.
    COND_ID_HOOK(OnActorInit, ACTOR_EN_MD, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr) return;
        AnchorSync::SetActorSyncScope(actor, AnchorSync::ActorSyncScope::Team);
    });

    // Generic NPC State Sync Phase 2 — Inside Deku Tree B1 floating
    // platform (#185 primary concern).
    //
    // `Bg_Ydan_Hasi` covers three param-variants in one actor type:
    //   - HASI_WATER: water-plane controller (raises / lowers per the
    //     switch flag this->type, calls Flags_GetSwitch). Already
    //     visually-synced via existing SoH puzzle-completion / flag
    //     replication; this addition smooths the rising/falling
    //     animation phase between clients.
    //   - HASI_WATER_BLOCK: floating platform that rides the water
    //     plane. **Primary fix target** — without sync each client
    //     locally computed position from `play->gameplayFrames`, and
    //     small per-client tick-rate divergence put the platform in
    //     visibly different places on each screen, breaking the puzzle.
    //   - 3-blocks-2F: countdown-timer-driven reveal. Already visually-
    //     synced via existing flag replication; addition is additive
    //     smoothing.
    //
    // Per-frame world.pos + shape.rot broadcast via the standard
    // ENEMY_STATE path covers all three variants. Scope = Global —
    // puzzle state is world-canon (teams shouldn't have different
    // water levels).
    //
    // `Bg_Ydan_Maruta` (rotating spike log + falling ladder) is
    // deliberately NOT included in this phase. Spike-log damage volume
    // is at world.pos which doesn't move; rotation phase is cosmetic.
    // Falling ladder transitions on Flags_SetSwitch which already
    // syncs. Can be added later if field testing surfaces visible
    // drift — the same single-line addition to IsSyncedWorldActor +
    // an OnActorInit hook here is all it takes.
    COND_ID_HOOK(OnActorInit, ACTOR_BG_YDAN_HASI, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr) return;
        AnchorSync::SetActorSyncScope(actor, AnchorSync::ActorSyncScope::Global);
    });

    // Per-torch lit-state sync (Obj_Syokudai). Each torch instance gets
    // Global scope so partial multi-torch puzzle progress (P1 lit 2 of
    // 4 torches) is visible to all peers regardless of team.
    //
    // Vanilla `Flags_SetSwitch(switchFlag)` already replicates the
    // "puzzle complete → all torches permanently lit" terminal state
    // via WORLD_FLAG_SET. This addition fills the gap for individual
    // torch transitions before completion (Deku Stick light → s16
    // litTimer countdown → Flags_SetSwitch on completion).
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_SYOKUDAI, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr) return;
        AnchorSync::SetActorSyncScope(actor, AnchorSync::ActorSyncScope::Global);
    });

    // #193 Phase 2 — host-side ITEM_DROP_SYNC broadcast on local
    // Item_DropCollectible* spawn. Receive-side (network drop) skips
    // the broadcast and stamps the host-supplied netId/killer onto
    // the extension instead.
    //
    // Q7 transient-only allowlist: progression items (heart pieces,
    // heart containers, small keys, tunics, shields) keep per-player
    // semantics. Each client spawns its own copy via the vanilla
    // scripted path; broadcast skipped.
    //
    // Suppression of locally-spawned EN_ITEM00 on receivers (so peer's
    // own RNG doesn't double-drop) is the existing
    // `Anchor_ShouldSuppressXxxDrop` guard at SetupDyingNet sites.
    // For env-actor drops (Phase 4), Phase 2 doesn't fan-out yet —
    // env actors don't currently route through `Anchor_BeginItemDrop`.
    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr) return;
        if (!IsSaveLoaded() || gPlayState == nullptr) return;

        // Mask down to the resolved ITEM00_* type. The caller passes
        // (params | flag-bits); the actor's Init has not run yet so
        // we mask explicitly. Some types (FLEXIBLE) resolve at Init —
        // OnActorSpawn fires BEFORE Init, so a FLEXIBLE here means
        // the resolution hasn't happened. Skip; the next OnActorSpawn
        // pass after Init will see the resolved type. (Currently
        // OnActorSpawn fires once per actor lifetime — FLEXIBLE
        // drops are skipped from broadcast in this version, which
        // is acceptable for Phase 2 since FLEXIBLE drops are rare.)
        s16 resolvedType = (s16)(actor->params & 0xFF);

        // Plan B step 5 — modal-completion phantom from func_8083E4C4
        // (z_player.c). Vanilla `EnItem00_Init` strips the 0x8000 flag
        // from `actor->params` (params &= 0xFF) before this hook fires,
        // but `EnItem00::ogParams` preserves the original. Route
        // phantoms to the ModalPhantomAdapter and skip the ground-drop
        // pipeline entirely — phantoms are local-only render artifacts,
        // never broadcast.
        //
        // This replaces the modal-visual filter that lived as a
        // freestanding gate in fix 1 (051cd9801) and was removed in
        // cleanup 2/4 ahead of Plan B. The filter logic now lives
        // inside the adapter where it belongs.
        if ((((EnItem00*)actor)->ogParams & 0x8000) != 0) {
            SyncedClaimableDrop::ModalPhantomAdapter::GetInstance()
                ->OnPhantomSpawn((EnItem00*)actor);

            // MODAL_OFFER_CLAIMED — host detects modal accept here.
            // The phantom spawn IS the accept signal (vanilla
            // func_8083E4C4 in z_player.c only fires this path when
            // the player has actually picked up the modal offer).
            // Find the matching modal-offer Drop by position proximity
            // of any registered visual rep, then broadcast so peers
            // can dismiss their mirror stem at the moment of accept
            // instead of waiting for the DeadStickDrop timeout.
            //
            // Host-only: ModalOfferAdapter suppresses peers' modal
            // path entirely; a phantom on peer would be a vanilla-
            // path artifact we don't expect to broadcast.
            if (::SceneAuthority::IsMyCurrentRoomHost() &&
                Anchor::Instance != nullptr && Anchor::Instance->isConnected) {
                const Vec3f phantomPos = actor->world.pos;
                auto& reg = SyncedClaimableDrop::Registry::Instance();
                auto* modalOfferAdapter =
                    SyncedClaimableDrop::ModalOfferAdapter::GetInstance();

                uint32_t bestDropId   = 0;
                float    bestDistSq   = 200.0f * 200.0f;  // match cap

                for (const auto& [dropId, drop] : reg.GetAllDrops()) {
                    if (drop.adapter != modalOfferAdapter) continue;
                    if (drop.state ==
                        SyncedClaimableDrop::DropState::Resolved) continue;
                    for (Actor* vr : drop.visualReps) {
                        if (vr == nullptr || vr->update == nullptr) continue;
                        const float dx = vr->world.pos.x - phantomPos.x;
                        const float dy = vr->world.pos.y - phantomPos.y;
                        const float dz = vr->world.pos.z - phantomPos.z;
                        const float dSq = dx * dx + dy * dy + dz * dz;
                        if (dSq < bestDistSq) {
                            bestDistSq = dSq;
                            bestDropId = dropId;
                        }
                    }
                }

                if (bestDropId != 0) {
                    SPDLOG_INFO("[ModalOfferClaimed] host-detected modal accept: phantom "
                                "pos=({:.0f},{:.0f},{:.0f}) matched dropId={} "
                                "distSq={:.0f}",
                                phantomPos.x, phantomPos.y, phantomPos.z,
                                bestDropId, bestDistSq);
                    Anchor::Instance->SendPacket_ModalOfferClaimed(
                        bestDropId, Anchor::Instance->ownClientId);
                    // Local Resolve — relay echo will arrive shortly and
                    // be a no-op via the state!=Resolved gate. We don't
                    // dismiss visual reps locally on host either way;
                    // vanilla state machine on host's offering actor
                    // handles its own cleanup.
                    if (SyncedClaimableDrop::Drop* d = reg.Find(bestDropId)) {
                        d->claimerClientId = Anchor::Instance->ownClientId;
                        reg.TransitionTo(*d,
                            SyncedClaimableDrop::DropState::Resolved);
                    }
                } else {
                    SPDLOG_DEBUG("[ModalOfferClaimed] host modal phantom at "
                                 "({:.0f},{:.0f},{:.0f}) — no nearby modal-offer "
                                 "Drop within 200u",
                                 phantomPos.x, phantomPos.y, phantomPos.z);
                }
            }
            return;
        }

        // #193 static-actor filter — EN_ITEM00 instances placed directly
        // in scene OTRs (Inside Deku Tree has recovery hearts at
        // (-25,280,-81) / (87,744,-16); other scenes have similar
        // bare-placed pickups) spawn during the scene's setup-actor
        // loop, where `gPlayState->numSetupActors > 0`. Both clients
        // load the same scene file and spawn identical copies
        // independently — there's nothing to broadcast. Cross-client
        // pickup sync flows separately through `FLAG_SCENE_COLLECTIBLE`
        // → `SET_FLAG` (see `HandlePacket_SetFlag`'s active-despawn
        // pass, also added in #193 fix 2). Same Fix-8 trick used for
        // static enemy suppression.
        if (gPlayState->numSetupActors > 0) {
            return;
        }

        // #193 Phase 4 v3 exclusion gate — per-player drop sources
        // wrapped with Anchor_BeginItemDropLocalOnly skip both the
        // broadcast pipeline (host doesn't replicate) AND the
        // peer-local death-drop suppressor (peer keeps its own
        // local drop because peer triggered the same event
        // independently). Used by Shot_Sun's scripted Sun's Song
        // drop; the other two excluded sources (player modal-
        // completion phantoms in z_player.c, En_Ex_Ruppy dive
        // game) already filter via the 0x8000 ogParams branch
        // above.
        if (g_isLocalOnlyItemDrop) {
            return;
        }

        // Receive-side: extension stamping only. Skip broadcast.
        if (g_isSpawningNetworkItemDrop) {
            ItemDropNetId ext;
            ext.netId           = g_pendingNetworkItemDropNetId;
            ext.killerClientId  = g_pendingItemDropKillerClientId;
            ext.killerTeamId    = g_pendingItemDropKillerTeamId;
            ext.spawnTimeMs     = g_pendingItemDropSpawnTimeMs;
            ext.isFromBroadcast = true;
            ObjectExtension::GetInstance().Set<ItemDropNetId>(actor, std::move(ext));
            SPDLOG_DEBUG("[ItemDropSync] Network drop: stamped netId={} killer={} killerTeam='{}' type=0x{:02X}",
                         g_pendingNetworkItemDropNetId, g_pendingItemDropKillerClientId,
                         g_pendingItemDropKillerTeamId, (int)resolvedType);
            return;
        }

        // Peer-local death-drop suppression (log 282 follow-up).
        //
        // When peer's local synced enemy enters its death cycle —
        // DyingByLocal on a peer-killing-blow, or DyingByNetwork after
        // host's ENEMY_DEFEATED arrives — its actor code path runs the
        // natural-death drop call (e.g. Item_DropCollectible in
        // z_en_dekubaba.c:1088 ITEM00_NUTS). On peer this spawns a
        // local EN_ITEM00 with no extension that the broadcast pipe
        // skips (correct: peers don't broadcast). But the actor stays
        // alive as a non-interactive ghost drop until its 220-frame
        // countdown expires. User reported "extra non-interactive nut"
        // in field test 282.
        //
        // Detect by proximity to any synced enemy actor whose phase
        // is past Alive (Dying / Dead). Host's matching ITEM_DROP_SYNC
        // broadcast will arrive ~100ms later and spawn the real
        // synced drop alongside.
        //
        // Static drops in setup-actor phase already returned above
        // (numSetupActors > 0); modal phantoms already returned via
        // the 0x8000 branch; network-spawn arrivals already returned
        // via g_isSpawningNetworkItemDrop. So anything that reaches
        // here on peer is a peer-local Actor_Spawn — and we want to
        // suppress the subset that came from a nearby dying enemy.
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
            constexpr float kSuppressRadius   = 300.0f;
            constexpr float kSuppressRadiusSq = kSuppressRadius * kSuppressRadius;
            bool suppress = false;
            for (size_t ci = 0; ci < kSyncableActorCategoriesCount && !suppress; ++ci) {
                Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
                while (a != nullptr) {
                    const EnemyNetId* nidExt =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                    if (nidExt != nullptr &&
                        nidExt->phase != EnemyStateSync::LifecyclePhase::Alive) {
                        const float dx = a->world.pos.x - actor->world.pos.x;
                        const float dy = a->world.pos.y - actor->world.pos.y;
                        const float dz = a->world.pos.z - actor->world.pos.z;
                        if (dx * dx + dy * dy + dz * dz < kSuppressRadiusSq) {
                            suppress = true;
                            break;
                        }
                    }
                    a = a->next;
                }
            }
            if (suppress) {
                SPDLOG_INFO("[ItemDropSync] Peer-local drop suppressed at "
                            "({:.0f},{:.0f},{:.0f}) type=0x{:02X} — nearby synced "
                            "enemy past Alive phase; host's ITEM_DROP_SYNC will replace",
                            actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                            (int)resolvedType);
                Actor_Kill(actor);
            }
            return;
        }

        // Q7 transient-only allowlist. Anything else stays per-player.
        bool isTransient = false;
        switch (resolvedType) {
            case ITEM00_RUPEE_GREEN:
            case ITEM00_RUPEE_BLUE:
            case ITEM00_RUPEE_RED:
            case ITEM00_RUPEE_ORANGE:
            case ITEM00_RUPEE_PURPLE:
            case ITEM00_HEART:
            case ITEM00_MAGIC_SMALL:
            case ITEM00_MAGIC_LARGE:
            case ITEM00_BOMBS_A:
            case ITEM00_BOMBS_B:
            case ITEM00_BOMBS_SPECIAL:
            case ITEM00_ARROWS_SINGLE:
            case ITEM00_ARROWS_SMALL:
            case ITEM00_ARROWS_MEDIUM:
            case ITEM00_ARROWS_LARGE:
            case ITEM00_NUTS:
            case ITEM00_STICK:
            case ITEM00_SEEDS:
            case ITEM00_BOMBCHU:
                isTransient = true;
                break;
            default:
                isTransient = false;
                break;
        }
        if (!isTransient) {
            return;
        }

        // Compute item netId. Items are dynamic spawns by definition.
        //
        // EncodeUniqueDynamicNetId from ActorSyncHelpers.cpp probes
        // against `EnemyNetId` extensions only — it doesn't see
        // `ItemDropNetId` extensions. For grass-cluster drops where
        // `Item_DropCollectibleRandom`'s loop spawns 3+ rupees at the
        // same world position in the same frame, every Actor_Spawn
        // produces an actor with identical posHash and the probe
        // never finds a collision (because no prior actor has an
        // EnemyNetId match). All N drops broadcast with the SAME
        // netId — peer's HandlePacket_ItemDropSync idempotency check
        // then drops N-1 of them silently, leaving peer with only one
        // drop while host has N. (Field log 2026-05-06: 3 ITEM_DROP_SYNC
        // broadcasts at 03:01:18.330 with identical netId/pos/params.)
        //
        // Inline a probe that walks ACTORCAT_MISC for EnItem00 actors
        // with ItemDropNetId and bumps posHash on collision.
        uint32_t itemNetId = EncodeEnemyNetId(actor);
        {
            const uint32_t base = itemNetId & 0xFFFFFF00;
            for (int probe = 0; probe < 256; probe++) {
                bool collides = false;
                Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
                while (a != nullptr) {
                    if (a != actor && a->id == ACTOR_EN_ITEM00) {
                        const ItemDropNetId* iext =
                            ObjectExtension::GetInstance().Get<ItemDropNetId>(a);
                        if (iext != nullptr && iext->netId == itemNetId) {
                            collides = true;
                            break;
                        }
                    }
                    a = a->next;
                }
                if (!collides) break;
                itemNetId = base | (((itemNetId + 1) & 0xFF));
            }
        }

        // Killer attribution. If `Anchor_BeginItemDrop` was called by
        // the wrapping shim around `Item_DropCollectible*`, the active
        // killer is recorded. Otherwise (Actor_Spawn for EN_ITEM00
        // outside the wrapped paths — defensive fallback) fall back
        // to local clientId.
        uint32_t killerClientId = (g_pendingItemDropDepth > 0)
            ? g_pendingItemDropKillerClientId
            : Anchor::Instance->ownClientId;
        int64_t spawnTimeMs = (g_pendingItemDropDepth > 0)
            ? g_pendingItemDropSpawnTimeMs
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count();

        // Cross-client kill attribution (field test log 283/284).
        //
        // When a peer's killing-blow on a synced enemy gets re-attributed
        // to host (because host runs the natural-death cycle locally and
        // its Item_DropCollectible call resolves the killer from
        // Anchor_BeginItemDrop(NULL), defaulting to host's ownClientId),
        // Layer 1's killer-exclusive window blocks the actual killer for
        // 3 s — long enough that the drop frequently expires before they
        // can attempt pickup.
        //
        // Recover the real killer by walking synced enemy categories
        // near the EN_ITEM00 spawn position; if any has phase != Alive
        // and HostBookkeeping has a damager recorded for its netId,
        // use that as the attribution. Falls back to the
        // Anchor_BeginItemDrop value when nothing matches (heart/rupee
        // drops outside enemy deaths, etc.).
        {
            constexpr float kAttrRadius   = 200.0f;
            constexpr float kAttrRadiusSq = kAttrRadius * kAttrRadius;
            auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
            uint32_t bestDamager = 0;
            float    bestDistSq  = kAttrRadiusSq;
            for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
                Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
                while (a != nullptr) {
                    const EnemyNetId* nidExt =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                    if (nidExt != nullptr &&
                        nidExt->phase != EnemyStateSync::LifecyclePhase::Alive) {
                        const float dx = a->world.pos.x - actor->world.pos.x;
                        const float dy = a->world.pos.y - actor->world.pos.y;
                        const float dz = a->world.pos.z - actor->world.pos.z;
                        const float dSq = dx * dx + dy * dy + dz * dz;
                        if (dSq < bestDistSq) {
                            uint32_t damager = bookkeeping.LookupDamager(nidExt->netId);
                            if (damager != 0) {
                                bestDistSq  = dSq;
                                bestDamager = damager;
                            }
                        }
                    }
                    a = a->next;
                }
            }
            if (bestDamager != 0 && bestDamager != killerClientId) {
                SPDLOG_INFO("[ItemDropSync] killer attribution corrected: {} -> {} "
                            "(via nearby dying-enemy damager lookup)",
                            killerClientId, bestDamager);
                killerClientId = bestDamager;
            }
        }

        // Stamp local extension first so the pickup gate can read it
        // even on the host. isFromBroadcast=false marks "local drop".
        // killerTeamId looked up from clients map; empty when
        // killerClientId is 0 (unattributed) or not in map.
        std::string killerTeamIdStr;
        if (killerClientId != 0) {
            auto kit = Anchor::Instance->clients.find(killerClientId);
            if (kit != Anchor::Instance->clients.end()) {
                killerTeamIdStr = kit->second.teamId;
            }
        }
        ItemDropNetId ext;
        ext.netId           = itemNetId;
        ext.killerClientId  = killerClientId;
        ext.killerTeamId    = killerTeamIdStr;
        ext.spawnTimeMs     = spawnTimeMs;
        ext.isFromBroadcast = false;
        ObjectExtension::GetInstance().Set<ItemDropNetId>(actor, std::move(ext));

        // Defer the broadcast until Anchor_EndItemDrop (which runs after
        // Item_DropCollectible* sets the spawned actor's random
        // world.rot.y at z_en_item00.c:1625). Broadcasting here would
        // capture rot.y == 0 and let each receiver pick its own RNG
        // trajectory — the nut/ammo desync seen in field test 281.
        g_pendingItemDropBroadcasts.push_back({
            /*actor=*/               actor,
            /*itemNetId=*/           itemNetId,
            /*resolvedType=*/        resolvedType,
            /*killerClientId=*/      killerClientId,
            /*spawnTimeMs=*/         spawnTimeMs,
            /*invisibleDecorative=*/ g_pendingItemDropInvisibleDecorative,
        });

        // Plan B step 3 — populate the SyncedClaimableDrop registry
        // alongside the legacy broadcast path. Adapter dismissal is
        // dormant until a subsequent step wires ITEM_COLLECTED →
        // Resolved → adapter dispatch; for now this just accumulates
        // a parallel view of in-flight drops that future steps can
        // act on.
        SyncedClaimableDrop::GroundDropAdapter::GetInstance()->RegisterSpawn(
            itemNetId,
            Anchor::Instance->ownClientId,
            (int16_t)resolvedType,
            actor->world.pos,
            (int16_t)gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num,
            (uint8_t)(gSaveContext.linkAge & 0x1),
            killerClientId,
            spawnTimeMs,
            actor);
    });

    // Host sends enemy positions every frame to all clients in the same scene.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            return;
        }
        if (!IsSaveLoaded()) {
            return;
        }

        // #265 — mark the actor as having reached at least one OnActorUpdate
        // tick. The OnActorKill broadcast path reads this to suppress wire
        // traffic for actors that conditionally Actor_Kill themselves inside
        // _Init before any update could fire (En_Po_Field 8-of-10 candidate
        // cull at z_en_po_field.c:180-184, Obj_Mure2 conditional culls).
        // Cheap: write a bool every frame for every synced actor. Placed
        // BEFORE the EnGoma debris filter below so debris fragments are
        // also marked — preserves their current ENEMY_DEFEATED behaviour
        // (still emits at end of fragment lifetime).
        {
            EnemyNetId* updExt = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (updExt != nullptr) {
                updExt->hasEverUpdated = true;
            }
        }

        // EnGoma debris filter (#67 follow-up, log 299 bandwidth audit).
        //
        // Vanilla EnGoma_SpawnHatchDebris spawns ~16-20 EnGoma actors as
        // visual eggshell fragments during egg-hatch and natural-death
        // sequences. They share `actorId == ACTOR_EN_GOMA` with the
        // actual larvae but have `gomaType != ENGOMA_NORMAL` (and
        // limbCount=0 — no SkelAnime). They cannot be damaged, do not
        // affect the boss state machine, and despawn on a short timer.
        //
        // Without filtering, each fragment broadcasts ENEMY_STATE every
        // frame the host's bookkeeping detects a state delta, and there
        // are dozens of them per Goma egg-laying cycle. Log 299 showed
        // ENEMY_STATE tx peaking at 315.4 pps / 252 KB/s during the
        // boss fight — the debris was the dominant contributor.
        //
        // Skip ENEMY_STATE entirely for non-NORMAL EnGoma instances.
        // Visual debris remains local to each client (each runs its own
        // EnGoma_SpawnHatchDebris call when the host's
        // SetupHatch / SetupDying packets arrive); cross-client
        // debris position drift is invisible because the fragments are
        // small, short-lived, and decorative.
        if (actor->id == ACTOR_EN_GOMA) {
            EnGoma* lg = (EnGoma*)actor;
            if (lg->gomaType != ENGOMA_NORMAL) {
                return;
            }
        }

        // Push-block bidirectional sync (resolves limitations 1-3 of dcd2f7d47):
        // whichever client is locally pushing the block is authoritative for
        // that frame. Standard host-only broadcast leaves peer pushes
        // invisible to other clients — every client (host included) sees
        // the block stay put when a non-local player is the one pushing.
        //
        // Detection uses each actor's own "active push" state field
        // (delegated to Anchor_IsActorMidPush) rather than motion-delta so
        // we never falsely classify network-applied position writes as
        // "local motion" (which would echo). Active-push semantics for the
        // supported actors:
        //   OBJ_OSHIHIKI  — stateFlags & (PUSHBLOCK_PUSH | PUSHBLOCK_FALL)
        //   BG_SPOT15_RRBOX — unk_178 > 0 (push slide) or gravity < 0 (fall)
        //
        // Bg_Heavy_Block (Golden Gauntlets pillar) and En_Ishi (lift/throw
        // rocks) remain in IsSyncedWorldActor and ride the standard host-
        // broadcast path. Bidirectional support for them needs separate
        // motion-detection because their action funcs are static and
        // there's no public state flag — deferred until those actors
        // appear on the demo path.
        if (actor->id == ACTOR_OBJ_OSHIHIKI ||
            actor->id == ACTOR_BG_SPOT15_RRBOX) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext == nullptr) {
                return;
            }
            if (Anchor_IsActorMidPush(actor)) {
                // Local actor is the pusher — broadcast to all peers. Both
                // supported actors fire NA_SE_EV_ROCK_SLIDE-SFX_FLAG every
                // frame locally (Oshihiki at z_obj_oshihiki.c:598, Rrbox at
                // z_bg_spot15_rrbox.c:300), so the local audio is already
                // correct. Receivers play it via HandlePacket_EnemyUpdate's
                // pos-delta detector.
                SendPacket_EnemyUpdate(ext->netId, actor);
            }
            // No re-apply path here — HandlePacket_EnemyUpdate writes
            // actor->world.pos directly when packets arrive (now
            // unconditionally for push blocks, including on the host).
            return;
        }

        // Hyrule Castle child-timeline Talon — any-client state-machine
        // sync. The cucco-thrower (who could be the non-host) is the
        // only client whose local Actor_ProcessTalkRequest fires for
        // EXCH_ITEM_CHICKEN (z_en_ta.c:331-354), so the standard
        // host-only ENEMY_STATE actionState pipeline cannot drive the
        // wake transition. Whoever's local Talon transitions emits a
        // TALON_CASTLE_STATE packet; receivers forward-only-apply via
        // EnTa_NetSync_ApplyState. Mirrors MidoPostDekuLeave and the
        // ObjOshihiki any-client push pattern above.
        //
        // We DO NOT return after broadcasting — the standard
        // ENEMY_STATE pipeline below still runs for position / joints
        // / scale sync (Talon physically walks during the run-off
        // sequence and needs host-authoritative pos updates).
        //
        // See Claude/Analysis/talon_castle_wake_sync_2026-06-17.md.
        // Gate on scene only — vanilla places the castle Talon with params
        // 0xFFFF (-1 as s16), not 0. EnTa_Init discriminates by switch
        // default-fallthrough into the scene check at z_en_ta.c:161,
        // NOT by params == 0. Kakariko (params=1) and Ranch (params=2)
        // variants live in their own scenes so the scene check alone is
        // sufficient. Found via field-test log 552: gate was missing
        // every transition because params=0xFFFF != 0.
        if (actor->id == ACTOR_EN_TA &&
            gPlayState->sceneNum == SCENE_HYRULE_CASTLE) {
            EnemyNetId* extTalon = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (extTalon != nullptr) {
                uint8_t currentIdx = EnTa_NetSync_GetStateIndex((EnTa*)actor);
                // 0xFF = sentinel for "unsupported state" (transient
                // failure-wake func_80B145F8, non-castle variant
                // helpers, etc.). Skip.
                if (currentIdx != 0xFF &&
                    (s16)currentIdx != extTalon->netStateIndex) {
                    SPDLOG_INFO("[TalonCastleState] Local transition {} → {} — broadcasting",
                                (int)extTalon->netStateIndex, (int)currentIdx);
                    extTalon->netStateIndex = (s16)currentIdx;
                    SendPacket_TalonCastleState(currentIdx);
                }
            }
            // Fall through to the standard ENEMY_STATE pipeline for
            // position / joints sync.
        }

        // Per-torch lit-state sync (Obj_Syokudai) — bidirectional. Either
        // client can light a torch with their Deku Stick; both must see
        // every other client's lightings so mixed-lighting puzzles
        // (P1 lights 2, P2 lights 2) auto-complete via the receive-side
        // puzzle-complete scan in HandlePacket_EnemyState.
        //
        // Bandwidth: ExtrasDiffer for syokudai uses category buckets
        // (unlit / burning / permanently lit), not per-frame value, so
        // the steady "burning, counting down" state generates zero
        // packets. Only category transitions broadcast.
        if (actor->id == ACTOR_OBJ_SYOKUDAI) {
            const EnemyNetId* ext =
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext != nullptr) {
                SendPacket_EnemyUpdate(ext->netId, actor);
            }
            return;
        }

        if (::SceneAuthority::IsMyCurrentRoomHost()) {
            // Scene host (or every client for bidirectional actors):
            // send current state to all clients in scene.
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext == nullptr) {
                // Mirror the OnActorSpawn skip: actors that intentionally
                // don't get netIds (Hintnut reveal-child params=0xA,
                // Honotrap non-eye flame variants, etc.) also don't get
                // extensions — no point warning per-frame. Silences log 751's
                // repeated "no extension for actorId=402" spam from Hintnut
                // reveal-children (actor 0x192 = ACTOR_EN_HINTNUTS with
                // params & 0xFF == 0xA). See Plans/log751_followups_2026-07-28.md.
                if (ShouldSkipNetIdAssignment(actor)) {
                    return;
                }
                // Log once per actor pointer to avoid per-frame spam.
                static std::unordered_set<const Actor*> warnedNoExt;
                if (warnedNoExt.insert(actor).second) {
                    SPDLOG_WARN("[EnemyUpdate] Host OnActorUpdate: no extension for actorId={} cat={} — skipping update",
                                actor->id, (int)actor->category);
                }
                return;
            }
            // Karebaba respawn detection (host path, Fix 24):
            // When a Karebaba returns to Idle (stateIndex=1) after a kill cycle,
            // reset all death tracking so the next kill can be broadcast correctly.
            //
            // Two cases:
            //   defeatPacketSent=true  — host killed it locally; sentDefeatThisScene and
            //                            deadEnemiesByScene were written at kill time.
            //   pendingNaturalDeath=true — host received ENEMY_DEFEATED from a non-host
            //                             client and ran the natural death cycle itself;
            //                             HandlePacket also wrote deadEnemiesByScene.
            //                             Without this branch, pendingNaturalDeath stays
            //                             set forever and subsequent kills from non-host
            //                             are silently ignored as "already dying".
            if (actor->id == ACTOR_EN_KAREBABA) {
                EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorUpdate.host.Karebaba.respawnDetect.precond");
            }
            if (actor->id == ACTOR_EN_KAREBABA &&
                (EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(ext->netId) ||
                 EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase))) {
                EnemyNetId* extMut = const_cast<EnemyNetId*>(ext);
                s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                // Detect respawn-complete: any living state (>= 0, not a death/regrow state).
                // Death states: 5=Dying, 6=DeadItemDrop, 8=Dead, 9=Regrow.
                // We cannot rely on curState==1 (Idle) because when a player is
                // nearby, the Idle update immediately calls SetupAwaken in the same
                // frame — our OnActorUpdate hook fires AFTER update(), so by the time
                // we check, the state is already Awaken (2), never Idle.
                bool isDeathState = (curState == 5 || curState == 6 || curState == 8 || curState == 9);
                // Grow (state=0) is the initial spawn state when OoT re-creates the
                // actor after Actor_Kill. A freshly-spawned actor in Grow that inherits
                // a stale extension (pendingNaturalDeath=true) must NOT be treated as
                // a post-death respawn — the actor hasn't completed its cycle yet and
                // deadEnemiesByScene should remain set so late joiners get the replay.
                bool isGrowState  = (curState == 0);
                if (curState >= 0 && !isDeathState && !isGrowState) {
                    // Notify non-hosts to skip their remaining countdown and jump to
                    // Regrow. Send before clearing flags so the receive-side guard
                    // (pendingNaturalDeath check) still holds when the packet arrives.
                    SendPacket_EnemyRespawn(extMut->netId);
                    EnemyStateSync::AuditBooleansVsPhase(*extMut, "OnActorUpdate.host.Karebaba.respawn");
                    EnemyStateSync::TransitionTo(*extMut, EnemyStateSync::LifecyclePhase::Alive);
                    auto& bk = EnemyStateSync::HostBookkeeping::Instance();
                    bk.ReleaseDefeatBroadcast(extMut->netId);
                    bk.ClearSceneDeath(gPlayState->sceneNum, extMut->netId);
                    // Symmetric with non-host detector below: clear pendingKillNetIds
                    // so a future room reload doesn't re-trigger Fix 38 dead-state
                    // setup on the now-alive actor (T1.4 regression — see commit
                    // message for details).
                    bk.ClearPendingKill(extMut->netId);
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} respawned (state={}) (host) — defeat tracking cleared",
                                extMut->netId, curState);
                }
            }

            // #67 / KB-44 follow-up — Boss_Goma periodic larva scan.
            //
            // Defense in depth against childrenGohmaState[3] desync surfaced in
            // the 4-player log 299/300 sessions: the boss got stuck on the
            // ceiling after the second batch of larvae was defeated because
            // childrenGohmaState[] still showed live entries despite zero live
            // EnGoma actors in the scene. KB-44's wire-field sync (commit
            // 9bf853f56) closed the primary host→peer channel; this scan is a
            // secondary safety net that uses the actor list itself as ground
            // truth.
            //
            // Gate to CeilingIdle and CeilingMoveToCenter only — both run after
            // BossGoma_CeilingSpawnGohmas completes, so the scan never fires
            // mid-spawn. Tick every 60 frames (~3s at 20fps).
            //
            // Counts EnGoma actors with gomaType ∈ {ENGOMA_NORMAL, ENGOMA_EGG}.
            // Including ENGOMA_EGG covers the brief post-spawn window where
            // childrenGohmaState[i]==1 but the egg hasn't hatched into NORMAL
            // yet — without this, the scan could misfire and force-drop the
            // boss before larvae can engage.
            //
            // Trigger: liveCriticalGoma == 0 AND any childrenGohmaState[i] >= 1
            // (spawn happened; was alive; should now be dead). Forces all
            // slots to -1 so the vanilla CeilingIdle drop branch fires on the
            // next tick. KB-44's wire field carries the corrected values to
            // peers in the next ENEMY_STATE packet.
            if (actor->id == ACTOR_BOSS_GOMA) {
                BossGoma* bg = (BossGoma*)actor;
                if ((bg->actionFunc == BossGoma_CeilingIdle ||
                     bg->actionFunc == BossGoma_CeilingMoveToCenter) &&
                    (bg->frameCount % 60 == 0)) {
                    int liveCriticalGoma = 0;
                    Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
                    while (it != nullptr) {
                        if (it->id == ACTOR_EN_GOMA && it->update != nullptr) {
                            EnGoma* lg = (EnGoma*)it;
                            if (lg->gomaType == ENGOMA_NORMAL ||
                                lg->gomaType == ENGOMA_EGG) {
                                liveCriticalGoma++;
                            }
                        }
                        it = it->next;
                    }
                    bool anySpawned = (bg->childrenGohmaState[0] >= 1 ||
                                       bg->childrenGohmaState[1] >= 1 ||
                                       bg->childrenGohmaState[2] >= 1);
                    if (liveCriticalGoma == 0 && anySpawned) {
                        SPDLOG_INFO("[BossGoma] larva scan: 0 live ENGOMA_NORMAL/EGG but childrenGohmaState=[{},{},{}] — forcing all -1 to drop",
                                    bg->childrenGohmaState[0],
                                    bg->childrenGohmaState[1],
                                    bg->childrenGohmaState[2]);
                        bg->childrenGohmaState[0] = -1;
                        bg->childrenGohmaState[1] = -1;
                        bg->childrenGohmaState[2] = -1;
                    }
                }
            }

            SendPacket_EnemyUpdate(ext->netId, actor);
        } else {
            // Non-host: re-apply last received network state after AI update ran.
            // This keeps the enemy at the host-authoritative position/health while
            // still allowing the update() to register collision shapes every frame.
            EnemyNetId* ext = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext == nullptr) {
                return;
            }

            // Host-freshness gate — computed once per-actor per frame, consumed
            // at every per-actor sync site below. When host has been silent for
            // longer than kHostStalenessThresholdMs (see Common/StaleHostGate.h),
            // ShouldDeferToPeerLocalAI(actor->id, ext, gateNowMs) returns true
            // and the sync site skips the force-apply so peer's local AI drives
            // freely. Boss actors bypass the gate entirely (host-authoritative
            // always).
            const uint64_t gateNowMs = EnemyStateSync::NowMs();
            const bool hostStale = EnemyStateSync::ShouldDeferToPeerLocalAI(
                actor->id, ext, gateNowMs);

            // Detect sword hits by the local (non-host) player this frame and forward
            // them to the host for authoritative damage application.
            //
            // Timing: CollisionCheck_Damage ran BEFORE actor->update() this frame and
            // populated colChkInfo.damage. actor->update() called Actor_ApplyDamage
            // (reducing health locally) but did NOT clear colChkInfo.damage —
            // CollisionCheck_ResetDamage runs AFTER this hook. So colChkInfo.damage
            // still holds the total damage dealt this frame.
            //
            // Guard: skip on local kill (hasLocalDeath). On the killing blow,
            // actor->update() fires OnEnemyDefeat before OnActorUpdate runs, which
            // sets hasLocalDeath = true. ENEMY_DEFEATED already handles the kill;
            // sending DAMAGE_ENEMY for the final hit would be redundant.
            EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorUpdate.nonhost.DamageEnemyForward");
            // Race B mitigation (#203) — when ShouldActorUpdate clamped a
            // peer-killing-blow this frame, the original damage was stashed
            // on the extension so we broadcast the un-clamped value here.
            // Without this, the forwarder would broadcast the clamped value
            // (potentially 0 for one-shot kills at 1 HP) and host's enemy
            // would receive less damage than peer dealt — leaving host's
            // enemy unkillable from peer attacks.
            u8 forwardDamage =
                (ext->peerKillingBlowOriginalDamage > 0)
                    ? ext->peerKillingBlowOriginalDamage
                    : (u8)actor->colChkInfo.damage;

            // #90 / log 434 fix — En_St armored-hit detection.
            //
            // CollisionCheck populates colChkInfo.damage for ANY peer hit
            // on the Skulltula regardless of which cylinder was struck.
            // Vanilla's EnSt_CheckHitFrontside (z_en_st.c:440) rejects
            // front-shield hits without calling Actor_ApplyDamage AND
            // sets swayTimer to exactly 60. Without detection here, peer's
            // front-shield hits would still broadcast colChkInfo.damage
            // to host — and after the AC_HIT routing fix in a5a940261,
            // host's back-cylinder application kills the Skulltula
            // from peer's armored bounce.
            //
            // Detection: swayTimer == 59 (log 435 calibration). The
            // sequence is:
            //   CheckHitFrontside sets swayTimer = 60.
            //   Then EnSt_Update (z_en_st.c:1098) calls EnSt_Sway which
            //   immediately decrements: swayTimer = 59.
            //   THEN OnActorUpdate (this hook) fires post-update with
            //   the decremented value visible.
            //
            // Re-hit during ongoing sway: swayTimer was 30 → CheckHit
            // Frontside sets to 60 → EnSt_Sway decrements to 59. Same
            // observation point.
            //
            // Frame with no hit but ongoing sway: swayTimer decrements
            // by 1 from its prior value — anything OTHER than 59 means
            // "not just-hit this frame" (assuming the prior value
            // wasn't exactly 60, which only happens on a hit frame).
            //
            // CheckHitBackside resets swayTimer to 0 on legitimate back
            // hits — those show swayTimer == 0 at OnActorUpdate time.
            //
            // When detected, suppress the damage value but still send
            // the wire packet with `isArmoredHit=true` so host fires the
            // local sway animation. Sway anim cross-machine syncs via
            // the resulting ENEMY_STATE joint-table broadcast.
            bool isArmoredHit = false;
            if (actor->id == ACTOR_EN_ST) {
                EnSt* st = (EnSt*)actor;
                if (st->swayTimer == 59 && forwardDamage > 0) {
                    SPDLOG_INFO("[DamageEnemy] En_St armored-hit netId={} "
                                "swayTimer=59 → forwarding damage=0 isArmoredHit=true "
                                "(front-shield bounce + sway anim sync)",
                                ext->netId);
                    forwardDamage = 0;
                    isArmoredHit = true;
                }
            } else if (actor->id == ACTOR_EN_SSH) {
                // En_Ssh — same multi-collider front-shield pattern as En_St
                // but DIFFERENT sway-timer calibration. EnSsh_Sway is called
                // from EnSsh_Draw (z_en_ssh.c:904), not EnSsh_Update — so
                // the decrement happens AFTER OnActorUpdate fires. The
                // pre-decrement value (60, just set by CheckHitFront)
                // is visible here. Per-actor calibration required because
                // the two siblings have different update→draw timing for
                // the same logical mechanism.
                EnSsh* ssh = (EnSsh*)actor;
                if (ssh->swayTimer == 60 && forwardDamage > 0) {
                    SPDLOG_INFO("[DamageEnemy] En_Ssh armored-hit netId={} "
                                "swayTimer=60 → forwarding damage=0 isArmoredHit=true "
                                "(front-shield bounce + sway anim sync)",
                                ext->netId);
                    forwardDamage = 0;
                    isArmoredHit = true;
                }
            } else if (actor->id == ACTOR_EN_TEST) {
                // (#290) Stalfos front-shield block. Vanilla setup at
                // z_en_test.c:171-189: shieldCollider has AC_HARD flag;
                // z_collision_check.c:1726 CollisionCheck_SetBounce sets
                // AC_BOUNCED on shield when a Player sword AT contacts it.
                // Vanilla EnTest_UpdateDamage (z_en_test.c:1678) then
                // short-circuits — clears bodyCollider AC_HIT, no damage.
                //
                // But CollisionCheck still populates colChkInfo.damage on
                // ANY AT/AC contact — including bounces — before the
                // vanilla short-circuit clears the resulting AC_HIT. Peer's
                // OnActorUpdate reads colChkInfo.damage AFTER vanilla
                // resolution but before CollisionCheck_ResetDamage.
                // Without this gate, peer would broadcast DAMAGE_ENEMY for
                // its own blocked attack; host would apply body AC_HIT via
                // ApplySyncAcHitToActor (DamageEnemy.cpp:970-982) — no
                // shield-bounce check on host — and Stalfos would take
                // damage from peer's blocked hit.
                //
                // Detection: preUpdateShieldBounced captured at
                // ShouldActorUpdate time (pre-update). We CANNOT read the
                // live shieldCollider.acFlags here — EnTest_UpdateDamage
                // (z_en_test.c:1679) already cleared AC_BOUNCED inside
                // actor->update. Symmetric mirror of the En_St / En_Ssh
                // pattern above (which uses swayTimer's decrementing
                // signal that survives update()).
                if (ext->preUpdateShieldBounced && forwardDamage > 0) {
                    SPDLOG_INFO("[DamageEnemy] En_Test armored-hit netId={} "
                                "preUpdateShieldBounced → forwarding damage=0 isArmoredHit=true "
                                "(front-shield bounce)",
                                ext->netId);
                    forwardDamage = 0;
                    isArmoredHit = true;
                }
                // Clear the capture regardless of gate outcome, so a
                // stale flag doesn't carry into a future frame's
                // non-blocked hit.
                ext->preUpdateShieldBounced = false;
            }

            const bool shouldSend =
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) &&
                (forwardDamage > 0 || isArmoredHit);
            if (shouldSend) {
                // #174/#175: forward damageEffect (set on enemy by collision damage-table
                // lookup) and atHitEffect (set on the player by CollisionCheck_SetATvsAC
                // when the player's AT element lands a hit). Many OoT enemies branch on
                // these fields to decide whether Actor_ApplyDamage actually fires; sending
                // only `damage` left those enemies silently ignoring the synthetic hit.
                Player* localPlayer = GET_PLAYER(gPlayState);
                u8 atHitEffect = (localPlayer != nullptr) ? localPlayer->actor.colChkInfo.atHitEffect : 0;
                SendPacket_DamageEnemy(ext->netId, forwardDamage,
                                       actor->colChkInfo.damageEffect, atHitEffect, isArmoredHit);
            }
            // Clear the killing-blow stash regardless of whether we
            // forwarded (forward gate could have been blocked by phase).
            ext->peerKillingBlowOriginalDamage = 0;

            // Karebaba respawn detection (non-host path, Fix 24 + Fix 30c):
            // Must run BEFORE the hasNetState gate. When a Karebaba is killed via
            // pendingKillNetIds at scene load (host's actor is in ACTORCAT_MISC so
            // no ENEMY_UPDATE arrives), hasNetState stays false forever. The old
            // placement inside the hasNetState gate meant this detection never fired,
            // leaving pendingNaturalDeath=true permanently and blocking all future kills.
            //
            // Also guards against Grow (state=0): when OoT re-creates the actor after
            // Actor_Kill, the new instance may inherit a stale extension with
            // pendingNaturalDeath=true. Grow is the initial spawn state — it is NOT
            // a completed respawn. Skip until the actor reaches a non-death, non-Grow
            // state (Idle=1 or higher living state).
            if (actor->id == ACTOR_EN_KAREBABA) {
                EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorUpdate.nonhost.Karebaba.respawnDetect.precond");
            }
            const bool nonhostHasBroadcast =
                EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(ext->netId);
            if (actor->id == ACTOR_EN_KAREBABA &&
                (EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase) || nonhostHasBroadcast)) {
                s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                bool isDeathState = (curState == 5 || curState == 6 || curState == 8 || curState == 9);
                bool isGrowState  = (curState == 0);
                if (curState >= 0 && !isDeathState && !isGrowState) {
                    // Fix 33: non-host was the killer (not the receiver of a host kill).
                    // pendingNaturalDeath=false means we killed it locally;
                    // defeatBroadcast=true means we sent ENEMY_DEFEATED (not a dedup skip).
                    // Notify the host to skip its remaining countdown — symmetric to how
                    // the host sends ENEMY_RESPAWN to us after a host-side kill (Fix 32).
                    if (!EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase) && nonhostHasBroadcast) {
                        SendPacket_EnemyRespawn(ext->netId);
                    }

                    EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorUpdate.nonhost.Karebaba.respawn");
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::Alive);
                    ext->netStateIndex        = -1;
                    // Clear hasNetState (Fix 25): prevents stale scale/rot from the
                    // host's last packet being re-applied during the first few frames
                    // after respawn (caused "missing heads" visual). The actor runs
                    // free AI until the next ENEMY_UPDATE from the host arrives and
                    // sets hasNetState=true again; if the host is absent it stays
                    // false and the actor runs free AI permanently — correct behavior.
                    ext->hasNetState = false;

                    {
                        auto& bk = EnemyStateSync::HostBookkeeping::Instance();
                        bk.ReleaseDefeatBroadcast(ext->netId);
                        // Release the deferred pendingKillNetIds entry (Fix 35).
                        // Fix 36's "stacked kill" branch was removed 2026-04-26 — its
                        // stalledKillPending input had no remaining writers after
                        // duplicate-replay was reclassified as dedup-only in
                        // HandlePacket_EnemyDefeated. See commit message for full
                        // rationale.
                        bk.ClearPendingKill(ext->netId);
                    }
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} respawned (state={}) (non-host) — sync re-enabled",
                                ext->netId, curState);
                }
            }

            if (!ext->hasNetState) {
                return;
            }
            // Re-derive skelAnime if it was null or empty at spawn time.
            // Dormant Deku Babas (and similar actors) may not have a valid
            // SkelAnime until after their first activation (grow/wake animation).
            // Checking each frame is cheap — GetEnemySkelAnime just reads a
            // struct field and validates limbCount/jointTable.
            if (ext->skelAnime == nullptr || ext->limbCount == 0) {
                SkelAnime* ska = GetEnemySkelAnime(actor);
                if (ska != nullptr && ska->limbCount > 0 && ska->jointTable != nullptr) {
                    ext->skelAnime = ska;
                    ext->limbCount = ska->limbCount;
                }
            }
            // Skip world.pos/rot/shape.rot re-apply when the actor is in a local death
            // animation (hasLocalDeath=true). The death code drives these fields each
            // frame (e.g. BounceAround modifies world.rot for Gold Skulltula).
            // Overwriting with stale cached host values corrupts the animation.
            // Scale and health have their own hasLocalDeath guards further below.
            EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorUpdate.nonhost.reapplyGuard");
            if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                // Both En_Dekubaba and En_Karebaba compute world.pos each frame from
                // animated angles rather than using a stable model root:
                //   En_Dekubaba: head-tip position derived from home.pos + stemSectionAngles
                //   En_Karebaba: in Spin state, world.pos = f(home.pos, shape.rot) trig
                // Overriding world.pos OR shape.rot causes the stem base to drift/wobble.
                // Both are skipped here and in HandlePacket_EnemyUpdate; the state machine
                // and jointTable sync keep each actor visually correct without overrides.
                //
                // ACTOR_FLAG_ATTACHED_TO_ARROW guard (cross-cutting): when an arrow
                // pins the actor, position is driven by the parent arrow each frame.
                // Re-applying the cached host position fights the arrow physics and
                // teleports the actor back to its pre-pin location. Affects En_St,
                // En_Sw, En_Bili, En_Bb, En_Crow, En_Firefly, En_Po_Sisters. Joint/
                // rotation/scale re-apply continues normally — only world.pos and
                // shape.rot are arrow-driven and need this skip.
                const bool arrowPinned = (actor->flags & ACTOR_FLAG_ATTACHED_TO_ARROW) != 0;
                // Animation-driven actors: world.pos and shape.rot rebuilt each
                // frame from the actor's own state. Re-applying the cached
                // host values fights the actor's own update().
                //   En_Dekubaba — head-tip from home.pos + stemSectionAngles.
                //   En_Karebaba — Spin state position from home.pos + shape.rot.
                // Boss_Goma is NOT in this group: world.pos is the boss's body-
                // root location written by its actionFunc, and the host's
                // location must reach peers (Bug A from log 47 — without this,
                // host's WallClimb / CeilingMoveToCenter location stayed stuck
                // at the peer's last floor position).
                // En_Nutsball is excluded so peer's local trajectory toward
                // peer's local Link's shield isn't overridden by host's
                // (different) nut path — see EnemyState.cpp companion comment.
                //
                // En_Hintnuts is excluded for a SIBLING reason (log 439):
                // shape.rot.y drives the nutball aim at Animation_OnFrame(6.0f).
                // Each client's local EnHintnuts_ThrowNut runs
                // Math_ApproachS(shape.rot.y, yawToNearest, 2, 0xE38), where
                // yawToNearest is computed against `Anchor_GetNearestPlayerActor`
                // — which returns DIFFERENT players on each client (host's view
                // sees P1+P2-DummyPlayer; peer's view sees P2+P1-DummyPlayer).
                // Without this exclusion the ~20pps ENEMY_STATE broadcast stomps
                // peer's local Math_ApproachS progress every frame, so peer's
                // hintnut ends up facing host's chosen target every Animation
                // frame 6 → peer's nutball flies at where P1 DummyPlayer is on
                // peer's machine instead of at P2's own Link. Even with P2
                // standing still + shield up, the projectile flies past.
                // Excluding shape.rot lets each client's local aim run
                // unimpeded. world.pos exclusion is also safe — Hintnut only
                // writes world.pos = home.pos at SetupWait (z_en_hintnuts.c:141),
                // identical on both clients. Visual divergence (host sees its
                // hintnut facing host's nearest; peer sees its hintnut facing
                // peer's nearest) is the INTENDED gameplay shape — "the scrub
                // is aiming at YOU" matches the per-player single-player feel.
                // #129 — En_Bb's Green variant orbits the player via matrix-
                // rotated home.pos with continuous Math_SmoothStepToF tracking,
                // and the killer variants add a Math_CosF(bobPhase) Y bob to
                // world.pos each frame. Both shapes diverge from host's
                // per-frame state, so position sync is intentionally skipped
                // and each client runs its own actor->update() to derive pos.
                const bool isAnimationDrivenPos = (actor->id == ACTOR_EN_DEKUBABA ||
                                                   actor->id == ACTOR_EN_KAREBABA ||
                                                   actor->id == ACTOR_EN_NUTSBALL ||
                                                   actor->id == ACTOR_EN_HINTNUTS ||
                                                   actor->id == ACTOR_EN_BB);
                // #137 — En_Eiyer is split-axis (XZ host-authoritative; Y
                // is locally computed each frame as basePos.y +/- cos/sin
                // offset in states 4 Ambush / 5 Glide / 9 Hurt). basePos.y
                // itself ships in ENEMY_STATE extras and is applied
                // directly to ei->basePos.y in the receiver — see
                // EnemyState.cpp's ACTOR_EN_EIYER receive block. Outside
                // those three states the Eiyer either rotates around
                // home.pos (states 0-3 dormant) or moves via Actor_MoveXYZ
                // and standard `world.pos` overwrite is fine.
                bool eiyerSkipPosY = false;
                if (actor->id == ACTOR_EN_EIYER && ext->netStateIndex >= 0) {
                    s16 s = ext->netStateIndex;
                    eiyerSkipPosY = (s == 4 || s == 5 || s == 9);
                }
                if (!isAnimationDrivenPos && !arrowPinned) {
                    if (eiyerSkipPosY) {
                        // Sync XZ from host; preserve local Y (the
                        // actor's own update() rewrites it from the
                        // synced basePos.y on the next tick).
                        actor->world.pos.x = ext->netPos.x;
                        actor->world.pos.z = ext->netPos.z;
                    } else {
                        actor->world.pos = ext->netPos;
                    }
                    actor->shape.rot = ext->netShapeRot;
                }
                actor->world.rot = ext->netRot;
            }
            // Skip health re-apply after a local kill so the host's stale health > 0
            // packets don't revive the dying actor on this client (hasLocalDeath guard).
            //
            // For regular enemies, multi-hit guard: only re-apply if local health hasn't
            // been reduced below the network value; otherwise we'd undo locally-dealt
            // damage on multi-hit enemies.
            //
            // For boss actors, the multi-hit guard is REVERSED — bosses are strictly
            // host-authoritative. Peer's local BossGoma_UpdateHit decrements peer's
            // local HP every time peer hits the boss (the boss's own update runs
            // locally and damages itself off the synthesised AC_HIT). Without forcing
            // the network HP back, peer's local HP races to 0 ahead of the host's
            // authoritative HP, peer fires OnBossDefeat from its own SetupDefeated
            // path, and broadcasts ENEMY_DEFEATED — host then trusts peer's "boss is
            // dead" claim despite host's HP still being well above 0. Field test 273
            // showed Goma dying after only 1 ENEMY_DEFEATED-from-peer event with
            // host's `preHp=10` (full HP minus 1).
            //
            // Force-overwrite for boss actors: peer's local HP always tracks the
            // host's authoritative value, even when local damage already decremented
            // it lower. The boss's hit-reaction visuals (color flash, sound) still
            // play locally because they're driven by BUMP_HIT / state-machine sync,
            // not by the HP value.
            const bool forceNetHealth = IsSyncedBossActor(actor->id);
            if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) &&
                (forceNetHealth || actor->colChkInfo.health >= ext->netHealth)) {
                actor->colChkInfo.health = ext->netHealth;
            }
            // Karebaba: pre-compute local state and active/dormant flags here so they
            // can guard BOTH the scale re-apply below and the state-machine sync.
            // Computed even when netStateIndex < 0 or hasLocalDeath so the scale guard
            // variable has a defined value; the sync block is skipped in those cases.
            bool karebabaDormantOverride = false;
            s16  karebabaLocalState      = -1;
            if (actor->id == ACTOR_EN_KAREBABA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                karebabaLocalState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                bool kNetDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1 ||
                                     ext->netStateIndex == 2 || ext->netStateIndex == 9);
                bool kLocalActive = (karebabaLocalState == 2 || karebabaLocalState == 3 ||
                                     karebabaLocalState == 4 || karebabaLocalState == 7);
                karebabaDormantOverride = kNetDormant && kLocalActive;
            }

            // Scale sync: enemies like En_Karebaba change actor->scale throughout their
            // state machine (0 when dormant, growing to 0.01 when fully emerged). Without
            // this re-apply the non-host always sees the actor at its spawn-time scale.
            // Guards:
            //   hasLocalDeath — skip while pendingNaturalDeath/respawn cycle is active.
            //     Without this, P1's Idle scale=0 overwrites P2's Regrow animation.
            //   karebabaDormantOverride — skip when P1's Karebaba is dormant (scale=0.005)
            //     but P2's is active (Upright/Spin, scale=0.01). Without this guard,
            //     P1's dormant scale overwrites P2's active scale every frame, making
            //     the Karebaba appear tiny while P2 is standing next to it (Fix 26).
            if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) && !karebabaDormantOverride) {
                actor->scale = ext->netScale;
            }

            // Per-torch lit-state sync (Obj_Syokudai) — no post-update
            // re-apply needed. Host broadcasts on litTimer category
            // transitions only (unlit → burning → permanently lit);
            // the receive site (HandlePacket_EnemyState) writes
            // torch->litTimer directly, then peer's local
            // ObjSyokudai_Update decrements naturally each frame at
            // the same 20fps rate as host. Both clients converge to
            // the same value at the same time without per-frame
            // packets.
            //
            // Peer's local Deku-Stick lighting still sets litTimer
            // independently — peer sees its own lighting visually,
            // the puzzle's `Flags_SetSwitch` completion path syncs
            // via WORLD_FLAG_SET when sLitTorchCount reaches the
            // threshold on whichever client is actually lighting the
            // torches. For demo: typical playstyle is one player
            // drives the puzzle while the other observes; the partial
            // progress visibility is the primary fix.

            // Karebaba state machine sync: if the host's current state differs from ours,
            // drive the local actor into the matching state. Called after update() so any
            // self-transition that ran this frame is immediately corrected.
            //
            // Guards:
            //   hasLocalDeath — never override state after a local kill; the host keeps
            //     sending its pre-death state for several frames which would un-kill the
            //     actor here, causing a SetupDying↔SetupUpright oscillation loop.
            //
            //   Retract (7) blocked unconditionally — Retract is distance-driven in
            //     EnKarebaba_Upright via Anchor_GetNearestPlayerActor. Syncing P1's
            //     Retract to P2 forces P2's Karebaba to retract even when P2 is still
            //     nearby. Worse: SetupRetract doesn't reset world.pos.y, so if the
            //     actor hasn't risen from home.pos.y+14, the Retract animation completes
            //     immediately (StepTo hits target in frame 1) → SetupIdle → OnActorUpdate
            //     re-applies Retract again → rapid Retract→Idle→Retract loop every frame
            //     (~50ms per iteration, visible as oscillation in logs) (Fix 26).
            //
            //   dormant-to-active protection — if the host just entered the room its
            //     enemies start at Idle (1) while this client's were already activated.
            //     Block regression to dormant states (Grow=0/Idle=1/Awaken=2/Regrow=9)
            //     when the local enemy is already in a fully active state
            //     (Awaken=2/Upright=3/Spin=4/Retract=7). Awaken(2) is in both sets:
            //     it blocks host-sent Idle from resetting a locally-Awaken actor, AND
            //     is itself blocked from overriding already-active (Upright/Spin) actors.
            if (actor->id == ACTOR_EN_KAREBABA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                // V6 (#310) — apply Ready-phase telegraph flag on EVERY
                // ENEMY_STATE receive (not just state-change branch)
                // so peer's OnUprightTick renders the telegraph
                // continuously during Upright. Charge state changes
                // are frequent enough that a per-receive apply is
                // needed to keep peer visuals current.
                Anchor_Enhance_EnKarebaba_ApplyPeerChargedFlag(
                    (EnKarebaba*)actor,
                    ext->karebaba.netCharged ? 1 : 0);
                s16 curState = karebabaLocalState; // pre-computed above
                if (curState != ext->netStateIndex && ext->netStateIndex != 7 && !hostStale) {
                    // Intra-attack guard (Fix 29): when both the host and local Karebaba are
                    // in the active bite cycle (Upright=3 / Spin=4), let the local state-machine
                    // timers drive the Upright↔Spin transitions rather than forcing the host's
                    // exact sub-state every packet.  Syncing here causes phase-mismatch
                    // oscillation: local actor finishes Spin → SetupUpright, then ApplyNetState
                    // immediately sets Spin again because the host's last packet still said Spin
                    // (confirmed in Test 25 P2 logs: SetupUpright + SetupSpin within 50ms for
                    // the same actor).  The dormant-to-active filter below still handles the
                    // Idle→Awaken→Upright activation boundary correctly.
                    bool netIsAttacking   = (ext->netStateIndex == 3 || ext->netStateIndex == 4);
                    // Retract (7) is the natural wind-down of the attack cycle. When the local
                    // actor is retracting and the host is still in Upright/Spin, ApplyNetState
                    // would reset the actor back to Upright mid-retract — world.pos.y is already
                    // at home.pos.y+14 so the retract completes in one frame, immediately loops
                    // (confirmed in Test 27 P2 logs: rapid Upright↔Retract every 50ms).
                    bool localIsAttacking = (curState == 3 || curState == 4 || curState == 7);
                    if (!(netIsAttacking && localIsAttacking)) {
                        bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1 ||
                                              ext->netStateIndex == 2 || ext->netStateIndex == 9);
                        bool localIsActive = (curState == 2 || curState == 3 || curState == 4 || curState == 7);
                        if (!(netIsDormant && localIsActive)) {
                            if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                                SPDLOG_INFO("[EnKarebaba] rx netId={} apply {}→{}",
                                            ext->netId, (int)curState, (int)ext->netStateIndex);
                            }
                            // Pillar 5 (#310) — apply peer's enhanced-spin flag
                            // BEFORE ApplyNetState so that when ApplyNetState's
                            // case-4 branch fires EnKarebaba_SetupSpin →
                            // Anchor_Enhance_EnKarebaba_OnHostSetupSpin, the
                            // per-actor state map already carries the
                            // network-received flag. Peer's OnHostSetupSpin
                            // early-returns via SceneAuthority::IsMyCurrentRoomHost
                            // (peer isn't host for this room) so it doesn't
                            // overwrite the flag.
                            Anchor_Enhance_EnKarebaba_ApplyPeerEnhancedFlag(
                                (EnKarebaba*)actor,
                                ext->karebaba.netEnhancedSpin ? 1 : 0);
                            EnKarebaba_ApplyNetState((EnKarebaba*)actor, ext->netStateIndex, ext->karebaba.netActorParams);
                        } else if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                            SPDLOG_INFO("[EnKarebaba] rx netId={} block net={} local={} (dormant-active filter)",
                                        ext->netId, (int)ext->netStateIndex, (int)curState);
                        }
                    } else if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnKarebaba] rx netId={} block net={} local={} (intra-attack guard)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                } else if (curState != ext->netStateIndex && ext->netStateIndex == 7 &&
                           ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                    SPDLOG_INFO("[EnKarebaba] rx netId={} block net=7 local={} (retract-gate)",
                                ext->netId, (int)curState);
                }
            }

            // Goroiwa state-machine sync — issue #153.
            // The local action func runs every frame for collision registration but
            // its waypoint advance can drift across frame-rate boundaries (P2 on
            // VirtualBox at 20fps vs P1 native at 60fps). Cached waypoint state was
            // already applied directly in HandlePacket_EnemyUpdate; this block only
            // resets actionFunc when the host's state diverges from local. Position
            // is host-authoritative via ext->netPos (re-applied above).
            if (actor->id == ACTOR_EN_GOROIWA && ext->netStateIndex >= 0) {
                EnGoroiwa* boulder = (EnGoroiwa*)actor;
                s16 curState = EnGoroiwa_GetStateIndex(boulder);
                if (curState != ext->netStateIndex && !hostStale) {
                    EnGoroiwa_ApplyNetState(boulder, gPlayState, ext->netStateIndex);
                }
            }

            // Dekubaba state-machine sync — KB-08 / #7. Without this, each
            // client's free-running grow/lunge cycle drifts to a different
            // phase, and Anchor_GetNearestPlayerActor (called from Grow,
            // DecideLunge, PrepareLunge) ends up resolving to the local
            // player on each side because each Dekubaba's animation-derived
            // world.pos lands closer to its own client's player at any
            // given frame. Forcing the host's stateIndex onto the non-host
            // pins both cycles in lockstep so the targeting math converges.
            //
            // Death and post-death states (11=PrunedSomersault, 12=ShrinkDie,
            // 13=DeadStickDrop) are skipped inside ApplyNetState — the
            // ENEMY_STATE phase=DyingByLocal path drives those transitions
            // separately. PhaseImpliesHasLocalDeath also blocks the call so
            // a locally-killed Dekubaba doesn't have its death animation
            // overwritten by a stale alive-state packet.
            if (actor->id == ACTOR_EN_DEKUBABA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnDekubaba* baba = (EnDekubaba*)actor;
                s16 curState = EnDekubaba_GetStateIndex(baba);
                if (curState != ext->netStateIndex && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnDekubaba] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnDekubaba_ApplyNetState(baba, ext->netStateIndex);
                }
            }

            // boss_goma_sync_plan.md §7 / KB-26 — En_Goma (Larva)
            // state-machine sync. Resolves the egg-hatch desync where
            // each client's local hatch timer advances independently.
            // Death-class states (Hurt, Die, Dead) gated by phase.
            if (actor->id == ACTOR_EN_GOMA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnGoma* lg = (EnGoma*)actor;
                s16 curState = EnGoma_GetStateIndex(lg);
                if (curState != ext->netStateIndex && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnGoma] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnGoma_ApplyNetState(lg, gPlayState, ext->netStateIndex);
                }
            }

            // #135 / en_dekunuts_sync_plan.md §8 — Mad Scrub state-
            // machine sync. Without this each client's free Wait/
            // LookAround/Stand/ThrowNut/Burrow loop drifts; aim direction
            // and projectile-spawn frame can disagree across clients.
            // Death/stun states gated by phase + dormant-to-active filter.
            if (actor->id == ACTOR_EN_DEKUNUTS && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnDekunuts* d = (EnDekunuts*)actor;
                s16 curState = EnDekunuts_GetStateIndex(d);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 4);
                bool localIsActive = (curState == 2 || curState == 3 ||
                                      curState == 5 || curState == 6 || curState == 7);
                bool deathStateNet = (ext->netStateIndex == 8 || ext->netStateIndex == 10);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnDekunuts] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnDekunuts_ApplyNetState(d, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet                  ? "death-state-gated"
                                        : (netIsDormant && localIsActive) ? "dormant-active filter"
                                        :                                   "other";
                        SPDLOG_INFO("[EnDekunuts] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Hintnuts (Inside Deku Tree Compound Room) — host-
            // authoritative state-machine sync. Room host runs the AI
            // and broadcasts every state change via ENEMY_STATE; peers
            // receive and apply via EnHintnuts_ApplyNetState. Peer's
            // local AI still ticks (animations + ambient transitions)
            // but the rx driver below snaps peer back to host's state
            // on every divergence — host always wins.
            //
            // Projectiles (En_Nutsball) remain local-AI / local-spawn
            // — see ActorSyncHelpers.cpp's IsSyncedWorldActor comment.
            // Per-client reflect physics drives a peer-only nutball-
            // landed event, which fires PROJECTILE_HIT_ENEMY → host
            // applies HitByScrubProjectile1+2 on its local actor →
            // ENEMY_STATE round-trips the resulting transition.
            //
            // Dormant-active filter retained: blocks host's idle Wait
            // (state 0) from clobbering a peer whose local AI happens
            // to be momentarily ahead in Stand/ThrowNut/BeginRun/Run.
            // Mostly redundant under host-authoritative (peer's local
            // AI doesn't broadcast, so divergences are short-lived) but
            // kept to avoid visual blink when peer's local timer crosses
            // an idle/active boundary slightly before host's.
            if (actor->id == ACTOR_EN_HINTNUTS && ext->netStateIndex >= 0) {
                EnHintnuts* h = (EnHintnuts*)actor;
                s16 curState = EnHintnuts_GetStateIndex(h);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState == 2 || curState == 3 ||
                                      curState == 5 || curState == 6);
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive) && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnHintnuts] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnHintnuts_ApplyNetState(h, gPlayState, ext->netStateIndex);
                } else if (curState != ext->netStateIndex && hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnHintnuts] rx netId={} defer net={} local={} (host-stale gate)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnHintnuts] rx netId={} block net={} local={} (dormant-active filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // #90 / en_st_sync_plan_v2.md §3 — Skulltula state-machine
            // sync. Dormant-to-active filter: states 0/1 (init / wait
            // on ceiling) shouldn't override active ground states 2/3/4.
            // Death states 6/7/8 gated by PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_ST && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnSt* st = (EnSt*)actor;
                s16 curState = EnSt_GetStateIndex(st);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState == 2 || curState == 3 || curState == 4);
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive) && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnSt] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnSt_ApplyNetState(st, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnSt] rx netId={} block net={} local={} (dormant-active filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // en_skb_sync_plan — En_Skb (Stalchild) state-machine sync.
            // Dormant-to-active filter: state 0 (Emerge from ground)
            // shouldn't override active states 2/3/4/5/6 (Advance/Attack/
            // Recovery/Stunned/Damaged). State 1 (Burrow) is a terminal
            // hide animation — NOT dormant; host's burrow at dawn or
            // out-of-leash should propagate. Death state 7 (body break +
            // drops) gated by PhaseImpliesHasLocalDeath — peer's
            // termination is driven by ENEMY_DEFEATED + Actor_Kill;
            // drops route through ITEM_DROP_SYNC. Without the death
            // gate, peer's local body-break path would call
            // Item_DropCollectible{,Random} and double-spawn drops.
            if (actor->id == ACTOR_EN_SKB && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnSkb* skb = (EnSkb*)actor;
                s16 curState = EnSkb_GetStateIndex(skb);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState >= 2 && curState <= 6);
                bool deathStateNet = (ext->netStateIndex == 7);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnSkb] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnSkb_ApplyNetState(skb, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated"
                                                        : "dormant-active filter";
                        SPDLOG_INFO("[EnSkb] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // en_ssh_sync_plan.md §3 — En_Ssh (Cursed Skulltula people)
            // state-machine sync. Dormant-to-active filter: states 0/1
            // (Start init transient / Wait on ceiling) don't override
            // active ground states 2/3/4/6 (Drop/Land/Idle/Return).
            // State 5 (Talk) is locally-owned — peer's textbox interaction
            // drives Talk independently; never apply over wire. No death
            // gating — EnSsh has no combat-death cycle (cursed-human NPCs).
            // OQ B/C/D: hitCount / stunTimer / stateFlags are applied
            // directly in the EnemyState.cpp receiver — no extra work here.
            if (actor->id == ACTOR_EN_SSH && ext->netStateIndex >= 0) {
                EnSsh* ssh = (EnSsh*)actor;
                s16 curState = EnSsh_GetStateIndex(ssh);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState >= 2 && curState <= 6);
                bool netIsTalk     = (ext->netStateIndex == 5);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !netIsTalk && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnSsh] rx netId={} apply {}->{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnSsh_ApplyNetState(ssh, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnSsh] rx netId={} block net={} local={} (filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // en_honotrap_sync — Fake-eye fire/ice trap state-machine sync.
            // Only the EYE variant carries a netId (flame variants filtered
            // at OnActorSpawn). Dormant-to-active filter: state 0 (EyeIdle —
            // shut/sleeping) doesn't override active states 1/2/3 (Open /
            // Attack / Close). Each client's local eye independently opens
            // against its nearest local Link (per-client-local-AI), so wire
            // sync is defense-in-depth — both clients typically converge on
            // the same state via local AI within a few frames. No death
            // gating — peer destruction is driven by ENEMY_DEFEATED +
            // Actor_Kill from whichever client first hit the eye locally.
            if (actor->id == ACTOR_EN_HONOTRAP &&
                actor->params == HONOTRAP_EYE &&
                ext->netStateIndex >= 0) {
                EnHonotrap* eye = (EnHonotrap*)actor;
                s16 curState = EnHonotrap_GetStateIndex(eye);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState >= 1 && curState <= 3);
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive) && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnHonotrap] rx netId={} apply {}->{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnHonotrap_ApplyNetState(eye, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnHonotrap] rx netId={} block net={} local={} (dormant-active filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // #148 / en_sw_sync_plan.md §3 — Skullwalltula state-machine
            // sync. swType-gated dormant-to-active filter:
            //   combat (swType=0): dormant=6 (wall idle); active=7/8/9
            //                      (lunge / decel / return).
            //   gold (swType>=1):  init transients=0/1 (init / toss-flight,
            //                      brief), settled=2 (idle on web).
            // Death states gated by PhaseImpliesHasLocalDeath.
            //
            // Audit-fix for the gold variant: states 0/1 are init transients
            // that fire briefly during Init then transition to state 2.
            // The ORIGINAL filter blocked "net dormant=2 over local active=0/1"
            // — protecting the receiver while it finished its own init.
            // But it did NOT block the inverse: if local had naturally
            // settled to state 2 (the steady state) and an outdated host
            // packet still says 0 or 1 (host's init transient), we'd
            // regress local from settled-to-init. Symmetric guard: gold's
            // init transients (0/1) are NEVER applied to a settled state-2.
            if (actor->id == ACTOR_EN_SW && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnSw* sw = (EnSw*)actor;
                s16 curState = EnSw_GetStateIndex(sw);
                u8  swType   = (sw->actor.params & 0xE000) >> 13;
                bool deathStateNet;
                bool blockTransition;
                const char* blockReason = nullptr;
                if (swType == 0) {
                    bool netDormant  = (ext->netStateIndex == 6);
                    bool localActive = (curState == 7 || curState == 8 || curState == 9);
                    deathStateNet    = (ext->netStateIndex == 4 || ext->netStateIndex == 5);
                    blockTransition  = (netDormant && localActive);
                    if (blockTransition) blockReason = "dormant-active filter (combat)";
                } else {
                    // gold: 0/1 are init transients; 2 is the settled state.
                    bool netInitTransient = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                    bool localSettledIdle = (curState == 2);
                    bool netDormant  = (ext->netStateIndex == 2);
                    bool localActive = (curState == 0 || curState == 1);
                    deathStateNet    = (ext->netStateIndex == 3);
                    // Original guard + new symmetric guard against
                    // settled→init regression.
                    blockTransition  = (netDormant && localActive)
                                    || (netInitTransient && localSettledIdle);
                    if (netDormant && localActive)         blockReason = "dormant-active filter (gold)";
                    else if (netInitTransient && localSettledIdle) blockReason = "settled-init filter (gold)";
                }
                if (curState != ext->netStateIndex && !blockTransition && !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnSw] rx netId={} apply {}→{} swType={}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex, (int)swType);
                    }
                    EnSw_ApplyNetState(sw, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" :
                                          (blockReason  ? blockReason          : "other");
                        SPDLOG_INFO("[EnSw] rx netId={} block net={} local={} swType={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, (int)swType, why);
                    }
                }
            }

            // en_test_sync_plan.md §3 — En_Test (Stalfos) state-machine
            // sync. Dormant-to-active filter: states 0/1 (WaitGround /
            // WaitAbove) shouldn't override active states (>= 2 from the
            // Fall/Land/Rise init transitions through to the combat
            // decision loop). Death states 21/22/23/25 gated by
            // PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_TEST && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnTest* tst = (EnTest*)actor;
                s16 curState = EnTest_GetStateIndex(tst);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState >= 2);
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive) && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnTest] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnTest_ApplyNetState(tst, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnTest] rx netId={} block net={} local={} (dormant-active filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // Plans/en_wf_sync_plan.md §3 step 7 — En_Wf (Wolfos) state-
            // machine sync. Dormant-to-active filter: states WAIT_TO_APPEAR
            // (0) and WAIT (6) shouldn't override active combat (SLASH 8,
            // RUN_AT_PLAYER 9, SEARCH 10, RUN_AROUND 11, SIDESTEP 14). Death
            // state DIE (2) gated by PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_WF && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnWf* wf = (EnWf*)actor;
                s16 curState = EnWf_GetStateIndex(wf);
                bool netIsDormant  = (ext->netStateIndex == WOLFOS_ACTION_WAIT_TO_APPEAR ||
                                      ext->netStateIndex == WOLFOS_ACTION_WAIT);
                bool localIsActive = (curState == WOLFOS_ACTION_SLASH ||
                                      curState == WOLFOS_ACTION_RUN_AT_PLAYER ||
                                      curState == WOLFOS_ACTION_SEARCH_FOR_PLAYER ||
                                      curState == WOLFOS_ACTION_RUN_AROUND_PLAYER ||
                                      curState == WOLFOS_ACTION_SIDESTEP);
                bool deathStateNet = (ext->netStateIndex == WOLFOS_ACTION_DIE);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnWf] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnWf_ApplyNetState(wf, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated"
                                                        : "dormant-active filter";
                        SPDLOG_INFO("[EnWf] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // feature/sync-en-tite — En_Tite (Tektite) state-machine sync.
            // Dormant-to-active filter: state 0 (Idle) shouldn't override
            // active states 1-7 (Turn/Move/Attack/Recoil/Stunned/FlipOnBack/
            // FlipUpright). Death-class states 8/9 (DeathCry/FallApart) gated
            // by PhaseImpliesHasLocalDeath — routed via SetupDyingNet from
            // HandlePacket_EnemyDefeated instead.
            if (actor->id == ACTOR_EN_TITE && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnTite* tite = (EnTite*)actor;
                s16 curState = EnTite_GetStateIndex(tite);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState >= 1 && curState <= 7);
                bool deathStateNet = (ext->netStateIndex == 8 || ext->netStateIndex == 9);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnTite] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnTite_ApplyNetState(tite, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated"
                                                        : "dormant-active filter";
                        SPDLOG_INFO("[EnTite] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #47 / en_firefly_sync_plan.md §4 — En_Firefly (Keese)
            // state-machine sync. Dormant-to-active filter: state 6
            // (Perch) shouldn't override active dive states 1/2
            // (DiveAttack / DisturbDiveAttack). State 0 (FlyIdle) is
            // NOT dormant — Keese spend most of their life there and
            // we want it to propagate. Death states 7/8/9 (Fall /
            // FrozenFall / Die) gated by PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_FIREFLY && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnFirefly* ff = (EnFirefly*)actor;
                s16 curState = EnFirefly_GetStateIndex(ff);
                bool netIsDormant  = (ext->netStateIndex == 6);
                bool localIsActive = (curState == 1 || curState == 2);
                bool deathStateNet = (ext->netStateIndex >= 7);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnFirefly] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnFirefly_ApplyNetState(ff, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnFirefly] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // en_crow_sync_plan.md — En_Crow (Guay) state-machine sync.
            // Dormant-to-active filter: state 5 (Respawn — invisible, growing
            // back after death) shouldn't override active dive states. State 0
            // (FlyIdle) is NOT dormant — Guay spend most of their life there
            // and we want it to propagate (mirror of En_Firefly's choice).
            // Death states 3/4 (Damaged / Die) gated by PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_CROW && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnCrow* cw = (EnCrow*)actor;
                s16 curState = EnCrow_GetStateIndex(cw);
                bool netIsDormant  = (ext->netStateIndex == 5);
                bool localIsActive = (curState == 1 || curState == 2);
                bool deathStateNet = (ext->netStateIndex == 3 || ext->netStateIndex == 4);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnCrow] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnCrow_ApplyNetState(cw, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnCrow] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #102 / en_reeba_sync_plan.md §3 step 7 — En_Reeba (Leever)
            // state-machine sync. Dormant filter is structural: states 0/1
            // (emerge init / rising) are transient init states that
            // shouldn't override active rolling states 2-7. Death-class
            // states 8-15 (damaged / stunned / death / recovery) gated by
            // PhaseImpliesHasLocalDeath. EnReeba_ApplyNetState takes a
            // PlayState* because some setup funcs need it for sub-state
            // dispatch.
            if (actor->id == ACTOR_EN_REEBA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnReeba* rb = (EnReeba*)actor;
                s16 curState = EnReeba_GetStateIndex(rb);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState >= 2 && curState <= 7);
                bool deathStateNet = (ext->netStateIndex >= 8);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnReeba] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnReeba_ApplyNetState(rb, gPlayState, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnReeba] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // en_ik_sync_plan.md §3 step 7 — En_Ik (Iron Knuckle) state-
            // machine sync. Dormant filter: state 0 (wake-up stand-up anim)
            // is the only "dormant" transition; once past wake-up, all
            // states 1-9 are active combat. Death state 10 gated by
            // PhaseImpliesHasLocalDeath. All variants (white / red /
            // sleeping) share the combat state machine.
            if (actor->id == ACTOR_EN_IK && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnIk* ik = (EnIk*)actor;
                s16 curState = EnIk_GetStateIndex(ik);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState >= 1 && curState <= 9);
                bool deathStateNet = (ext->netStateIndex == 10);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnIk] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnIk_ApplyNetState(ik, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnIk] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #99 / en_poh_sync_plan.md §4 step 7 — En_Poh (Poe) state-
            // machine sync. Dormant filter: state 10 (Disappear) shouldn't
            // override active charge/attack states 4/5. State 11 (Appear)
            // and 6/7/8/9 (recoil/flee/hookshot spin/turn-around) are safe
            // transitions — apply directly. Death/soul states 12+ are
            // gated by PhaseImpliesHasLocalDeath at the call site AND by
            // the deathStateNet branch below (the soul-talk states 15-18
            // are per-client local interactions that should NOT be
            // apply-overridden by host's stateIndex).
            if (actor->id == ACTOR_EN_POH && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnPoh* poh = (EnPoh*)actor;
                s16 curState = EnPoh_GetStateIndex(poh);
                bool netIsDormant  = (ext->netStateIndex == 10);            // Disappear
                bool localIsActive = (curState == 4 || curState == 5);      // Charge / Attack
                bool deathStateNet = (ext->netStateIndex >= 12);            // 12+ guarded
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnPoh] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnPoh_ApplyNetState(poh, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnPoh] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #107 / en_peehat_sync_plan.md §3 - En_Peehat (Peahat)
            // state-machine sync. Dormant filter: states 0/1 (grounded
            // buried forms) shouldn't override active patrol states
            // 4/5/6/9/10 (Flying_Fly / Ground_Seek / Larva_Seek /
            // Ground_Hover / Ground_ReturnHome). Death states 13/14
            // (Adult_StateDie / StateExplode) gated by
            // PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_PEEHAT && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnPeehat* ph = (EnPeehat*)actor;
                s16 curState = EnPeehat_GetStateIndex(ph);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState == 4 || curState == 5 || curState == 6 ||
                                      curState == 9 || curState == 10);
                bool deathStateNet = (ext->netStateIndex >= 13);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnPeehat] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnPeehat_ApplyNetState(ph, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnPeehat] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #137 / en_eiyer_sync_plan — En_Eiyer (Stinger) state-machine
            // sync. Dormant filter: states 0-3 (AppearFromGround /
            // underground patrol / Inactive) shouldn't override active
            // states 4-9 (Ambush / Glide / StartAttack / DiveAttack /
            // Land / Hurt) or Stunned (12). Death states 10 (Die) /
            // 11 (Dead) gated by PhaseImpliesHasLocalDeath — peer's
            // termination is driven by ENEMY_DEFEATED + Actor_Kill;
            // ITEM_DROP_SYNC handles drops. ApplyNetState's death case
            // is also a no-op for those states so the call is defensive.
            //
            // Note: world.pos.y is skipped for states 4/5/9 above (see
            // the eiyerSkipPosY branch in the re-apply block). basePos.y
            // is applied directly in EnemyState.cpp's receive block so
            // the local Glide / Hurt cos hover math tracks host.
            if (actor->id == ACTOR_EN_EIYER && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnEiyer* ei = (EnEiyer*)actor;
                s16 curState = EnEiyer_GetStateIndex(ei);
                bool netIsDormant  = (ext->netStateIndex >= 0 && ext->netStateIndex <= 3);
                bool localIsActive = (curState >= 4 && curState <= 9) || curState == 12;
                bool deathStateNet = (ext->netStateIndex == 10 || ext->netStateIndex == 11);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnEiyer] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnEiyer_ApplyNetState(ei, gPlayState, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnEiyer] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #128 / en_bili_sync_plan.md §4 — En_Bili (Biri jellyfish)
            // state-machine sync. Dormant-to-active filter: states 0/1
            // (FloatIdle / SpawnedFlyApart) shouldn't override active
            // combat states 2/3/4 (DischargeLightning / Climb /
            // ApproachPlayer). Death states 7/8/10 (Burnt / Die / Frozen)
            // gated by PhaseImpliesHasLocalDeath.
            if (actor->id == ACTOR_EN_BILI && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnBili* bili = (EnBili*)actor;
                s16 curState = EnBili_GetStateIndex(bili);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1);
                bool localIsActive = (curState == 2 || curState == 3 || curState == 4);
                bool deathStateNet = (ext->netStateIndex == 7 || ext->netStateIndex == 8 ||
                                      ext->netStateIndex == 10);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnBili] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnBili_ApplyNetState(bili, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnBili] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // #126 — En_Vali (Bari big jellyfish) state-machine sync.
            // States: 0 Lurk / 1 DropAppear / 2 FloatIdle / 3 Attacked /
            // 4 Retaliate / 5 MoveArmsDown / 6 Burnt / 7 DivideAndDie /
            // 8 Stunned / 9 Frozen / 10 ReturnToLurk. Dormant-to-active
            // filter: 0/10 (Lurk / ReturnToLurk — ceiling-resting) must
            // not override active local combat states 3/4/8 (Attacked /
            // Retaliate / Stunned). Death states 6/7/9 gated by
            // PhaseImpliesHasLocalDeath and driven via SetupDyingNet
            // from HandlePacket_EnemyDefeated.
            if (actor->id == ACTOR_EN_VALI && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnVali* vali = (EnVali*)actor;
                s16 curState = EnVali_GetStateIndex(vali);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 10);
                bool localIsActive = (curState == 3 || curState == 4 || curState == 8);
                bool deathStateNet = (ext->netStateIndex == 6 || ext->netStateIndex == 7 ||
                                      ext->netStateIndex == 9);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnVali] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnVali_ApplyNetState(vali, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnVali] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Zf (Lizalfos / Dinolfos) — state-machine sync.
            // 18-state map:
            //   0  DropIn        — pre-spawn animation (dormant)
            //   1  Stop/Look     — idle scan
            //   2  ApproachPlayer
            //   3  Walking
            //   4  Sidestep
            //   5  Slash
            //   6  RecoilFromBlockedSlash
            //   7  JumpBack
            //   8  Stunned (post-damage / ice)
            //   9  SheatheSword  — post-victory idle (dormant)
            //   10 HopAndTaunt   — quasi-idle taunt (dormant)
            //   11 HopAway
            //   12 DrawSword (re-engage)
            //   13 Damaged hit-react
            //   14 JumpUp (hop-over wall)
            //   15 CircleAroundPlayer
            //   16 JumpForward (forward lunge)
            //   17 Die (skipped from ApplyNetState; SetupDyingNet drives)
            // Dormant-to-active filter: states 0/9/10 (DropIn / Sheathed / Taunt)
            // must not override active local combat states 5/6/8/13/16 (active
            // attack / recoil / stunned / damaged / lunge). Death state 17 is
            // gated by PhaseImpliesHasLocalDeath and driven via SetupDyingNet
            // from HandlePacket_EnemyDefeated.
            if (actor->id == ACTOR_EN_ZF && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnZf* zf = (EnZf*)actor;
                s16 curState = EnZf_GetStateIndex(zf);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 9 ||
                                      ext->netStateIndex == 10);
                bool localIsActive = (curState == 5 || curState == 6 || curState == 8 ||
                                      curState == 13 || curState == 16);
                bool deathStateNet = (ext->netStateIndex == 17);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnZf] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnZf_ApplyNetState(zf, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnZf] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Mb (Moblin) — state-machine sync. 18-state map covers
            // Club + SpearGuard + SpearPatrol variants. See agent's
            // Phase 1 report (commit 7f210411a) for full table.
            //   0  SpearGuardLookAround       — strongly dormant (idle)
            //   1  SpearPatrolTurnTowardsWP   — strongly dormant (idle)
            //   2  ClubWaitPlayerNear         — strongly dormant (idle)
            //   3  SpearGuardWalk             — dormant-ish (walking)
            //   4  SpearPatrolWalkTowardsWP   — dormant-ish (walking)
            //   5..11  Attack/charge/recovery — active
            //   12 Stunned                    — active
            //   13 SpearDamaged               — active
            //   14 ClubDamaged                — active
            //   15 ClubDamagedWhileKneeling   — active (mid-sequence; see agent note)
            //   16 SpearDead                  — skipped (SetupDyingNet drives)
            //   17 ClubDead                   — skipped (SetupDyingNet drives)
            //
            // Dormant-to-active filter: states 0/1/2/3/4 (idle + walking)
            // must not override active local combat states 5-15. Death
            // states 16/17 gated by PhaseImpliesHasLocalDeath and driven
            // via EnMb_SetupDyingNet from HandlePacket_EnemyDefeated.
            //
            // Agent's animation-quality note (Phase 2 follow-up if field
            // test reveals desync): state 12 (Stunned) interrupting an
            // in-progress club kneeling sequence (states 14/15) may
            // cause animation desync. Not currently special-cased here;
            // the dormant-to-active filter doesn't catch active→active
            // transitions. Revisit if observed.
            if (actor->id == ACTOR_EN_MB && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnMb* mb = (EnMb*)actor;
                s16 curState = EnMb_GetStateIndex(mb);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1 ||
                                      ext->netStateIndex == 2 || ext->netStateIndex == 3 ||
                                      ext->netStateIndex == 4);
                bool localIsActive = (curState >= 5 && curState <= 15);
                bool deathStateNet = (ext->netStateIndex == 16 || ext->netStateIndex == 17);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnMb] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnMb_ApplyNetState(mb, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnMb] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Bigokuta (Big Octo miniboss, #130) — state-machine sync.
            // 13-state map:
            //   0  EmergeWait (params==0 only; pre-fight voice / BGM cue)
            //   1  EmergeJump (sin-wave arc into water)
            //   2  Land+Rotate — calls Actor_ChangeCategory(PROP → ENEMY)
            //   3  Pre-fight 40-frame delay
            //   4  Main combat (rotates around home, chases player)
            //   5  Spin-block / direction-flip (player on wrong side)
            //   6  Rotate-toward-player after spin-block
            //   7  Stunned (deku-nut hit)
            //   8  Hit-stun (sinusoidal after explosive damage)
            //   9  Post-damage spin-up
            //   10 Sink (lost interest)
            //   11 Re-emerge (skipped in ApplyNetState — needs PlayState*)
            //   12 Dying (skipped in ApplyNetState; SetupDyingNet drives)
            //
            // Filter: combined emerge-boundary + dormant-to-active.
            //
            // EMERGE-BOUNDARY PROTECTION (the unusual part for this actor):
            // states 0/1/2/3 are the emerge sequence; state 2 specifically
            // calls Actor_ChangeCategory(PROP → ENEMY) at the land-and-rotate
            // completion. The LOCAL emerge sequence MUST run to completion
            // on every client; otherwise the local actor stays in PROP
            // category and exits the sync pipeline. So we BLOCK any
            // cross-boundary transition (peer mid-emerge ↔ host mid-combat
            // both directions) until the local actor has its own foot on
            // the same side of the boundary. Once both sides are past
            // emerge (>= 4), normal dormant-to-active filter applies.
            //
            // Standard dormant-active filter (post-emerge):
            //   net state 10 (Sink, post-combat) treated as dormant.
            //   Local active states 4-9 protected from regressing to Sink.
            //
            // SKIPS in ApplyNetState:
            //   11 (Re-emerge) — requires PlayState arg; let it self-
            //                    reassign via per-frame ENEMY_UPDATE.
            //   12 (Dying)     — driven via EnBigokuta_SetupDyingNet from
            //                    HandlePacket_EnemyDefeated.
            if (actor->id == ACTOR_EN_BIGOKUTA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnBigokuta* big = (EnBigokuta*)actor;
                s16 curState = EnBigokuta_GetStateIndex(big);
                bool localIsEmerging = (curState >= 0 && curState <= 3);
                bool netIsEmerging   = (ext->netStateIndex >= 0 && ext->netStateIndex <= 3);
                bool crossesEmergeBoundary = (localIsEmerging != netIsEmerging);
                bool netIsDormant  = (ext->netStateIndex == 10);
                bool localIsActive = (curState >= 4 && curState <= 9);
                bool unapplicableNet = (ext->netStateIndex == 11);
                bool deathStateNet   = (ext->netStateIndex == 12);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !crossesEmergeBoundary &&
                    !unapplicableNet &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnBigokuta] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnBigokuta_ApplyNetState(big, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = crossesEmergeBoundary ? "emerge-boundary"
                                        : deathStateNet         ? "death-state-gated"
                                        : unapplicableNet       ? "unapplicable-net"
                                        : "dormant-active filter";
                        SPDLOG_INFO("[EnBigokuta] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Fd (Flare Dancer enflamed shell, #100) — state-machine sync.
            // 7-state map:
            //   0 Reappear       — spawn/respawn entry (dormant; mirror Karebaba Idle)
            //   1 SpinAndGrow    — spin while growing scale.y
            //   2 JumpToGround   — airborne descent
            //   3 Land           — landing anim
            //   4 SpinAndSpawnFire — spin + spawn 8-cluster En_Fd_Fire (per-client
            //                       projectiles; not synced)
            //   5 Run            — run in circle around home pos
            //   6 WaitForCore    — death countdown (skipped in ApplyNetState;
            //                       driven via SetupDyingNet from
            //                       HandlePacket_EnemyDefeated, OR set locally
            //                       when En_Fw signals FLG_COREDEAD)
            //
            // Dormant-to-active filter: state 0 (Reappear) is dormant.
            // States 1-5 are active combat phases. Death state 6 gated by
            // PhaseImpliesHasLocalDeath.
            //
            // En_Fw sibling note: En_Fw (Flare Dancer core/wisp) is unsynced
            // in v1. When En_Fw is admitted, the FLG_COREDEAD/FLG_COREDONE
            // signal chain feeds back into En_Fd_WaitForCore on each client
            // independently — peer observes the signal once En_Fw is synced.
            if (actor->id == ACTOR_EN_FD && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnFd* fd = (EnFd*)actor;
                s16 curState = EnFd_GetStateIndex(fd);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState >= 1 && curState <= 5);
                bool deathStateNet = (ext->netStateIndex == 6);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnFd] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnFd_ApplyNetState(fd, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnFd] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_GeldB (Gerudo Thief) — state-machine sync. 17-state map:
            //   0  Wait     — patrol idle (dormant)
            //   1  Flee     — jump-away post-defeat (death-class; SetupDyingNet)
            //   2  Ready    — combat stance
            //   3  Advance  — Setup needs PlayState; OMITTED from ApplyNetState
            //   4  RollForward
            //   5  Pivot
            //   6  Circle
            //   7  SpinDodge — Setup needs PlayState; OMITTED from ApplyNetState
            //   8  Slash
            //   9  SpinAttack
            //   10 RollBack
            //   11 Stunned  — ice/freeze (dormant-with-effect)
            //   12 Damaged
            //   13 Jump
            //   14 Block
            //   15 Sidestep — Setup needs PlayState; OMITTED from ApplyNetState
            //   16 Defeated — death-class; SetupDyingNet drives
            //
            // Dormant-to-active filter: state 0 (Wait patrol) treated as
            // dormant. State 11 (Stunned) is dormant-with-effect — the
            // color filter is active but the actor is locally frozen, so
            // a stale net Wait/Stunned should NOT reset an active local
            // combat state. Combat-active states 2/4/5/6/8/9/10/12/13/14
            // protected from regressing to Wait. Unapplicable states
            // 3/7/15 are silently skipped by EnGeldB_ApplyNetState's
            // default branch.
            if (actor->id == ACTOR_EN_GELDB && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnGeldB* geldb = (EnGeldB*)actor;
                s16 curState = EnGeldB_GetStateIndex(geldb);
                bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 11);
                bool localIsActive = (curState == 2 || curState == 4 || curState == 5 ||
                                      curState == 6 || curState == 8 || curState == 9 ||
                                      curState == 10 || curState == 12 || curState == 13 ||
                                      curState == 14);
                bool deathStateNet = (ext->netStateIndex == 1 || ext->netStateIndex == 16);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnGeldB] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnGeldB_ApplyNetState(geldb, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnGeldB] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Po_Field (Field Poe — Hyrule Field night) — state-machine
            // sync. 12-state map:
            //   0  WaitForSpawn (dormant — autonomous; rolls Big/Small variant
            //                   via Rand_ZeroOne — see KNOWN LIMITATION below)
            //   1  Appear       — one-shot rise anim
            //   2  CirclePlayer — Setup needs PlayState; OMITTED
            //   3  Flee         — Big-Poe behavior
            //   4  Damage       — Setup signature reads acHitInfo/ac; OMITTED
            //                     (vanilla hit-reaction will fire locally
            //                     when host's ENEMY_STATE health drops)
            //   5  Death        — driven via SetupDyingNet
            //   6  Disappear    — fade-out
            //   7-11 SoulIdle / soul-talk / SoulInteract — per-client soul-
            //                    talk states; OMITTED (each player who has a
            //                    bottle captures their own Poe via per-client
            //                    Item_Give; matches En_Poh pattern)
            //
            // Dormant-to-active filter: state 0 (WaitForSpawn) treated as
            // dormant. Active states 1, 3, 6 (Appear/Flee/Disappear) are
            // the only states this driver applies. Death state 5 gated
            // by PhaseImpliesHasLocalDeath.
            //
            // KNOWN LIMITATION — Big-Poe variant divergence: actor->params
            // is rolled independently per client at WaitForSpawn via
            // Rand_ZeroOne() (z_en_po_field.c:427-432, gated on Flags_
            // GetSwitch + PLAYER_STATE1_ON_HORSE). Without explicit params
            // sync, P1 may see a Big Poe at netId X while P2 sees a Small
            // Poe at the same netId — different animations, colors, scale,
            // collider dimensions, AND bottle-capture rewards. A clean
            // fix shape exists (Karebaba's `netActorParams` per-actor
            // extension at PerActor/EnKarebabaState.h pattern) but requires
            // either ENEMY_SPAWN-time params broadcast or a sub-struct on
            // EnemyNetId. Deferred to a Phase 3 follow-up; field test will
            // reveal whether the divergence is user-visible.
            if (actor->id == ACTOR_EN_PO_FIELD && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnPoField* pof = (EnPoField*)actor;
                s16 curState = EnPoField_GetStateIndex(pof);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState == 1 || curState == 3 || curState == 6);
                bool deathStateNet = (ext->netStateIndex == 5);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnPoField] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnPoField_ApplyNetState(pof, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnPoField] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Vm (Beamos): Wait=0 / Attack=1 / Stun=2 / Die=3.
            // Dormant-to-active filter: don't roll the peer's Stun (locally
            // applied via vanilla sleep-on-deku-nut + Race-B-routed damage)
            // back to Wait when host's net value is dormant. Death state Die
            // is gated separately by PhaseImpliesHasLocalDeath; the receive
            // path routes through EnVm_SetupDyingNet in HandlePacket_-
            // EnemyDefeated.
            if (actor->id == ACTOR_EN_VM && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnVm* vm = (EnVm*)actor;
                s16 curState = EnVm_GetStateIndex(vm);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState == 1 || curState == 2);
                bool deathStateNet = (ext->netStateIndex == 3);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) &&
                    !deathStateNet && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnVm] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnVm_ApplyNetState(vm, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = deathStateNet ? "death-state-gated" : "dormant-active filter";
                        SPDLOG_INFO("[EnVm] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // En_Fw (Flare Dancer core/wisp #100): Bounce=0 / Run=1 /
            // TurnToParentInitPos=2 / JumpToParentInitPos=3.
            // No discrete death state — death happens INSIDE Run via the
            // explosionTimer countdown. EnFw_SetupDyingNet (called from
            // HandlePacket_EnemyDefeated) primes that countdown and resumes
            // Run, so a "death" appears as netStateIndex=1 (Run) while
            // phase=DyingByNetwork. The PhaseImpliesHasLocalDeath gate at
            // the top of this block prevents the regression case.
            // Dormant-to-active filter: net=Bounce (entry-dormant) must not
            // roll the peer's locally-advanced Run/Turn/Jump back to Bounce.
            if (actor->id == ACTOR_EN_FW && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnFw* fw = (EnFw*)actor;
                s16 curState = EnFw_GetStateIndex(fw);
                bool netIsDormant  = (ext->netStateIndex == 0);
                bool localIsActive = (curState == 1 || curState == 2 || curState == 3);
                if (curState != ext->netStateIndex &&
                    !(netIsDormant && localIsActive) && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnFw] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnFw_ApplyNetState(fw, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        SPDLOG_INFO("[EnFw] rx netId={} block net={} local={} (dormant-active filter)",
                                    ext->netId, (int)ext->netStateIndex, (int)curState);
                    }
                }
            }

            // #129 / en_bb_sync_plan.md — En_Bb (Bubble / flame skull)
            // state-machine sync. Only damage-class transitions are
            // synced via ApplyNetState (BB_DAMAGE=0 / BB_DOWN=3 /
            // BB_STUNNED=4). Variant-spawn states (BB_BLUE=6 / BB_RED=7
            // / BB_WHITE=8 / BB_GREEN=9) are set locally at Init from
            // params and are treated as "dormant" net values that
            // shouldn't override an active damage state on a peer who
            // already locally took a hit. Death state BB_KILL=1 is
            // gated by PhaseImpliesHasLocalDeath and driven via
            // SetupDyingNet from HandlePacket_EnemyDefeated.
            if (actor->id == ACTOR_EN_BB && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                EnBb* bb = (EnBb*)actor;
                s16 curState = EnBb_GetStateIndex(bb);
                // Variant-spawn states (6/7/8/9) — don't override an
                // active damage state on the receiver.
                bool netIsVariantSpawn = (ext->netStateIndex == 6 || ext->netStateIndex == 7 ||
                                          ext->netStateIndex == 8 || ext->netStateIndex == 9);
                bool localIsDamageActive = (curState == 0 || curState == 3 || curState == 4);
                // BB_KILL (1) routed via SetupDyingNet — never apply here.
                // BB_FLAME_TRAIL (2) is a child trail, not synced.
                // BB_UNUSED (5) is unused in vanilla.
                bool blockedState = (ext->netStateIndex == 1 || ext->netStateIndex == 2 ||
                                     ext->netStateIndex == 5);
                if (curState != ext->netStateIndex &&
                    !(netIsVariantSpawn && localIsDamageActive) &&
                    !blockedState && !hostStale) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnBb] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnBb_ApplyNetState(bb, ext->netStateIndex);
                } else if (curState != ext->netStateIndex) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, true)) {
                        const char* why = blockedState ? "blocked-state" : "variant-spawn vs damage-active";
                        SPDLOG_INFO("[EnBb] rx netId={} block net={} local={} ({})",
                                    ext->netId, (int)ext->netStateIndex, (int)curState, why);
                    }
                }
            }

            // Boss_Goma — Encounter -> combat bridge + per-actionFunc
            // dispatch. Encounter (0x00) → combat (0x01..0x10) calls the
            // cutscene-teardown bridge once, then dispatches to the
            // matching SetupX function. Any combat → any other combat
            // state calls ApplyMinimalNetState so peer plays the same
            // animation as the host (rear-back / lunge / climb / spawn-
            // eggs / fall-struck-down all transition cleanly). Death
            // state (0x20) routes via ENEMY_DEFEATED → SetupDyingNet,
            // not via this driver.
            if (actor->id == ACTOR_BOSS_GOMA && ext->netStateIndex >= 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                BossGoma* bg = (BossGoma*)actor;
                s16 curState = BossGoma_GetStateIndex(bg);
                if (curState != ext->netStateIndex && ext->netStateIndex != 0x20) {
                    if (curState == 0x00 && ext->netStateIndex >= 0x01 && ext->netStateIndex <= 0x10) {
                        if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                            SPDLOG_INFO("[BossGoma] rx netId={} bridge Encounter->combat (net=0x{:02x})",
                                        ext->netId, (int)ext->netStateIndex);
                        }
                        BossGoma_BridgeToCombat(bg, gPlayState);
                        // BridgeToCombat lands in FloorMain (0x01). If
                        // host is already past FloorMain, dispatch.
                        if (ext->netStateIndex != 0x01) {
                            BossGoma_ApplyMinimalNetState(bg, ext->netStateIndex);
                        }
                    } else if (ext->netStateIndex >= 0x01 && ext->netStateIndex <= 0x10) {
                        if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                            SPDLOG_INFO("[BossGoma] rx netId={} apply 0x{:02x}->0x{:02x}",
                                        ext->netId, (int)curState, (int)ext->netStateIndex);
                        }
                        BossGoma_ApplyMinimalNetState(bg, ext->netStateIndex);
                    }
                }
            }

        }
    });

    // Non-host clients: allow enemy update() to run so collision registration
    // (CollisionCheck_SetAC/OC) executes every frame, enabling P2 to hit enemies.
    // Position/health drift from the free-running AI is corrected in OnActorUpdate
    // below by re-applying the last state received from the host.
    // (Previously this suppressed the update entirely with *should = false.)

    // Host: before each enemy AI update runs, patch the 4 pre-computed targeting
    // fields to point at the nearest player (local or DummyPlayer). All enemy AI
    // that reads xzDistToPlayer, yDistToPlayer, xyzDistToPlayerSq, or
    // yawTowardsPlayer — including Actor_IsFacingPlayer and
    // Actor_IsFacingAndNearPlayer — will then target the correct player with no
    // per-enemy changes required.
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            return;
        }
        if (!IsSaveLoaded() || gPlayState == nullptr) {
            return;
        }

        // Race B mitigation (#203) — prevent peer-local death so kill
        // attribution + drop pipeline runs through host-authoritative
        // path uniformly.
        //
        // ShouldActorUpdate fires BEFORE actor->update. By this point,
        // collision damage for this frame has already been written to
        // colChkInfo.damage (CollisionCheck_Damage runs in the system
        // pre-update pass), but actor->update has NOT yet applied it.
        // If peer's incoming damage would drop HP to 0, clamp it so HP
        // lands at 1 instead. Peer's enemy stays alive; peer's
        // DAMAGE_ENEMY broadcast routes to host; host applies + dies +
        // broadcasts ENEMY_DEFEATED + drop fires through ITEM_DROP_SYNC.
        // Fixes the asymmetric drop on peer-killed synced enemies with
        // SetupDyingNet (En_St / En_Sw / En_Dekunuts / En_Goma) where
        // the Anchor_ShouldSuppressXxxDrop guard's PhaseImpliesPending
        // NaturalDeath check returned false for DyingByLocal.
        //
        // Gates:
        //   IsMyCurrentRoomHost — only on peer; host's local kill is
        //     authoritative and should proceed normally.
        //   IsSyncedBossActor — bosses already use forceNetHealth in
        //     OnActorUpdate (peer's HP overwritten from host's track
        //     each frame); the clamp here is redundant for them.
        //   ext exists + Alive phase — only intervene for live synced
        //     enemies. Already-dying / freshly-spawned (no extension)
        //     pass through.
        //   damage > 0 — only when peer dealt damage this frame. Non-
        //     damage HP transitions (state-machine self-decrements,
        //     environmental kills) pass through; host runs the same
        //     logic and broadcasts the resulting death naturally.
        //
        // UX cost: ~RTT/2 visible delay between peer's killing blow and
        // the death animation starting. Typical 25-75 ms — barely
        // perceptible at network latencies expected for typical home
        // connections.
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));

            // (#290) EnTest armored-hit pre-update capture. Vanilla
            // EnTest_UpdateDamage (z_en_test.c:1679) clears
            // shieldCollider.AC_BOUNCED inside actor->update BEFORE
            // OnActorUpdate fires — so the send-side gate at
            // OnActorUpdate cannot read the live flag. Snapshot here
            // (pre-update, in ShouldActorUpdate) so the OnActorUpdate
            // gate can read the captured value. Cleared to false after
            // the send-side gate reads it (see OnActorUpdate peer
            // branch, ACTOR_EN_TEST case). Restricted to EnTest to
            // minimise per-frame work on other actors — the En_St /
            // En_Ssh pattern uses swayTimer as a decrementing signal
            // that survives past actor->update, so no capture needed
            // for those.
            if (ext != nullptr && actor->id == ACTOR_EN_TEST) {
                EnTest* stalfos = (EnTest*)actor;
                ext->preUpdateShieldBounced =
                    (stalfos->shieldCollider.base.acFlags & AC_BOUNCED) != 0;
            }

            // [Diag] pending-bugs 2026-07-15 — extra-damage bug diagnostic.
            // Log peer-side damage every frame it's non-zero. Combined with
            // existing [DamageEnemy] Sent and [EnemyDefeated] logs, this
            // reveals whether the same damage is being applied multiple
            // times, or whether host's ENEMY_STATE broadcast is re-triggering
            // damage after peer has already applied locally. User report:
            // Dekubaba dying in 1 strike when it should take 2.
            // See Claude/Analysis/hintnut_heart_drop_desync_and_extra_damage_2026-07-15.md.
            if (ext != nullptr && actor->colChkInfo.damage > 0 &&
                CVarGetInteger("gEnhancements.PendingBugsDiag", 0)) {
                SPDLOG_INFO("[DamageEnemy.diag] fwd netId={} actorId={} peer.damage={} peer.hp_before={} hp_after_would_be={} phase={}",
                            ext->netId, actor->id,
                            (int)actor->colChkInfo.damage,
                            (int)actor->colChkInfo.health,
                            (int)actor->colChkInfo.health - (int)actor->colChkInfo.damage,
                            (int)ext->phase);
            }

            // Candidate B2-D (#288, 2026-06-17) — peer-side timeout
            // fallback for the Race-B killing-blow clamp. If host
            // hasn't broadcast ENEMY_DEFEATED within kFallbackTimeoutMs
            // (1 s, user-specified) of the clamp first arming, assume
            // host is incapacitated (Game Over, cutscene, network
            // stall) and kill the actor locally + broadcast a peer-
            // attributed ENEMY_DEFEATED. Commit A's hub refactor
            // means the broadcast goes directly to all scene peers
            // and that any subsequent host-side defeat is suppressed
            // by ClaimDefeatBroadcast (set in HandlePacket_EnemyDefeated
            // when the peer's broadcast arrives at host).
            //
            // Safe under jitter: false-fire on ~1100 ms lag still ends
            // with a single applied kill — the dedup ledger
            // collapses simultaneous host + peer broadcasts cleanly.
            // Cosmetic cost: one extra packet on the wire per
            // false-fire.
            // Composite fix (2026-07-13) three-layer defence for B2-D
            // false-fires. Full design at
            // Claude/Plans/b2d_composite_fix_2026-07-13.md.
            //
            // Layer 1 (allowlist): stun-not-die actors never fire B2-D.
            // Applies to both semantic (1 s) and backstop (3 s) tiers —
            // even a 3 s timeout shouldn't force-kill an actor that
            // architecturally doesn't die from damage.
            //
            // Layer 2 (semantic, 1 s): fires only when host has
            // signalled death intent via ENEMY_DEFEATED
            // (ext->phase != Alive). Catches novel stun-not-die actors
            // not in the Layer 1 allowlist.
            //
            // Layer 3 (backstop, 3 s): fires unconditionally on the
            // extended timeout regardless of phase. Prevents permanent
            // softlocks when host stalled BEFORE ext->phase transitioned
            // (Layer 2's blind spot: peer's clamp armed, damage packet
            // sent, host never processed it, phase stays Alive on
            // peer). The 3 s constant is user-tuned per Q3.
            if (ext != nullptr &&
                ext->peerKillingBlowClampedAtMs != 0 &&
                !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) &&
                !IsStunNotDieActor(actor->id)) {
                static constexpr uint64_t kSemanticFallbackMs  = 1000;
                static constexpr uint64_t kBackstopFallbackMs  = 3000;
                // #305 — Layer 3-LR "last-resort": even with host provably
                // alive, don't leave the clamp armed forever. Fires only
                // after 10s to prevent permanent softlocks from actors
                // stuck in a damage-immune state. Much rarer than the
                // regular backstop; typically a symptom of an unrelated
                // bug (e.g. actor whose state machine never releases the
                // clamp).
                static constexpr uint64_t kLastResortFallbackMs = 10000;
                const uint64_t nowMs = (uint64_t)
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                const uint64_t elapsed = nowMs - ext->peerKillingBlowClampedAtMs;

                const bool semanticFire =
                    elapsed >= kSemanticFallbackMs &&
                    ext->phase != EnemyStateSync::LifecyclePhase::Alive;
                const bool backstopFire =
                    elapsed >= kBackstopFallbackMs;
                const bool lastResortFire =
                    elapsed >= kLastResortFallbackMs;

                if (semanticFire || backstopFire) {
                    // #305 (2026-07-28) — Option A: if host is provably
                    // alive and processing this actor (ENEMY_STATE received
                    // within the freshness threshold), the sustained clamp
                    // must be from a vanilla state-dependent damage floor
                    // rather than a genuine host stall. Known instances:
                    //   - Dekubaba PullBack: z_en_dekubaba.c:1188-1190
                    //     clamps `phi_s0 = 1` when hit during retract.
                    //   - Karebaba dying-cycle invincibility.
                    //   - Boss_Goma iframes during specific attack cycles.
                    //   - Iron Knuckle armor states.
                    // In these cases peer's swing was "wasted" per vanilla
                    // single-player semantic — user must swing again. Disarm
                    // the clamp WITHOUT force-killing; peer's next hit will
                    // fresh-arm and land through the normal Race-B → host-
                    // apply → defeat-broadcast path once host exits the
                    // clamp state.
                    //
                    // Last-resort override: if elapsed >= 10s AND host is
                    // alive, something is architecturally wrong (actor
                    // stuck in a damage-immune state that never releases).
                    // Fire anyway with reason=lastResort to prevent a
                    // permanent softlock.
                    const uint64_t sinceLastState =
                        (ext->lastStateReceiveMs > 0)
                            ? (nowMs - ext->lastStateReceiveMs)
                            : UINT64_MAX;
                    const bool hostRecentlyAlive =
                        sinceLastState < EnemyStateSync::kHostStalenessThresholdMs;

                    if (hostRecentlyAlive && !lastResortFire) {
                        SPDLOG_INFO("[B2D] Skipping force-kill netId={} actorId={} "
                                    "elapsed={}ms — host alive (ENEMY_STATE {}ms ago, "
                                    "threshold {}ms). Disarming clamp; peer must re-hit. "
                                    "Vanilla state-clamp presumed (e.g. Dekubaba PullBack).",
                                    ext->netId, actor->id, elapsed,
                                    sinceLastState,
                                    EnemyStateSync::kHostStalenessThresholdMs);
                        ext->peerKillingBlowClampedAtMs    = 0;
                        ext->peerKillingBlowOriginalDamage = 0;
                        return;
                    }

                    const char* reason = lastResortFire ? "lastResort"
                                       : semanticFire   ? "semantic"
                                       :                  "backstop";
                    SPDLOG_INFO("[B2D] Timeout fired after {}ms: netId={} actorId={} "
                                "reason={} phase={} hostFreshness={}ms",
                                elapsed, ext->netId, actor->id,
                                reason,
                                (int)ext->phase,
                                sinceLastState);
                    const uint32_t netIdToBroadcast = ext->netId;
                    // Clear the timer + stashed damage BEFORE the kill so
                    // any re-entry can't double-fire.
                    ext->peerKillingBlowClampedAtMs   = 0;
                    ext->peerKillingBlowOriginalDamage = 0;
                    // Broadcast first (Commit A hub-refactor: peer self-
                    // attributes + sends directly to all scene peers).
                    // Bare call is fine — RegisterHooks() is an Anchor
                    // member fn so the lambda captures `this`.
                    SendPacket_EnemyDefeated(netIdToBroadcast);
                    // (#291) Apply the same natural death cycle locally
                    // that receiving peers will apply upon ENEMY_DEFEATED
                    // receipt. Prior code called KillNetworkActorSilently
                    // which instantly disappeared the actor without
                    // playing the death animation — asymmetric visual
                    // experience across clients (killer saw instant
                    // disappear; observers saw the full death anim).
                    // Synthesizing a payload and invoking
                    // HandlePacket_EnemyDefeated on ourselves reuses the
                    // per-actor SetupDyingNet dispatch (EnTest_SetupDyingNet
                    // etc.) so we see the same death animation as remote
                    // peers.
                    //
                    // Safe:
                    //   - Director event (EnemyState.cpp:2612) is gated on
                    //     IsEffectiveHost() — peer isn't, won't fire.
                    //   - ClaimDefeatBroadcast is idempotent.
                    //   - RecordSceneDeath (EnemyState.cpp:2668) is gated
                    //     on IsMyCurrentRoomHost() — peer isn't, won't fire.
                    //   - PacketTimeline check defaults to "same timeline"
                    //     when field missing.
                    nlohmann::json localPayload;
                    localPayload["netId"]          = netIdToBroadcast;
                    localPayload["killerClientId"] = ownClientId;
                    localPayload["killerTeamId"]   = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
                    HandlePacket_EnemyDefeated(localPayload);
                    // Pitfall 41 (session_state.md): actors WITHOUT
                    // SetupDyingNet fall through to Actor_Kill inside
                    // HandlePacket_EnemyDefeated (EnemyState.cpp:3398),
                    // which nulls actor->update. Guard against vanilla
                    // Actor_UpdateAll (z_actor.c:2729) dispatching the
                    // nulled function pointer this same frame → 0xC0000005
                    // access violation. For actors WITH SetupDyingNet
                    // the guard is defensively safe (the newly-set dying
                    // actionFunc is fine to skip this frame; anim starts
                    // next frame). Actor_Delete reaps on the NEXT frame
                    // for the Actor_Kill case.
                    if (should != nullptr) {
                        *should = false;
                    }
                    return;
                }
            }

            // Composite fix Layer 1: skip clamp arming for stun-not-die
            // actors (hint nuts, cursed Skulltula people, Mido, Talon).
            // These use damage as a state trigger, not a life-total
            // gate — arming the clamp for them causes B2-D's timeout
            // to fire spuriously ~1 s later, killing them mid-state.
            // See Claude/Analysis/hintnut_dialogue_disappear_2026-07-13.md.
            if (!IsSyncedBossActor(actor->id) && !IsStunNotDieActor(actor->id)) {
                if (ext != nullptr &&
                    !EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) &&
                    actor->colChkInfo.damage > 0 &&
                    actor->colChkInfo.damage >= actor->colChkInfo.health) {
                    s8 reducedDamage =
                        (actor->colChkInfo.health > 1)
                            ? (s8)(actor->colChkInfo.health - 1)
                            : (s8)0;
                    // Stash the original damage so the OnActorUpdate
                    // forwarder broadcasts the un-clamped value to
                    // host. Without this, the forwarder reads the
                    // clamped value and host's enemy receives less
                    // damage than peer dealt — host's enemy stays
                    // alive forever from peer's killing blows.
                    ext->peerKillingBlowOriginalDamage =
                        (u8)actor->colChkInfo.damage;
                    // Candidate B2-D (#288): arm the timeout timer on
                    // the first clamp for this actor (rising edge
                    // only — subsequent clamps within the same arm
                    // window keep the original timestamp so the
                    // timeout is measured from the FIRST observed
                    // killing-blow, not the most recent).
                    if (ext->peerKillingBlowClampedAtMs == 0) {
                        ext->peerKillingBlowClampedAtMs = (uint64_t)
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                        SPDLOG_INFO("[B2D] Clamp armed: netId={} actorId={} localHp={} dmg={} (will fallback in 1000ms if host doesn't broadcast first)",
                                    ext->netId, actor->id,
                                    (int)actor->colChkInfo.health,
                                    (int)actor->colChkInfo.damage);
                    }
                    SPDLOG_INFO("[RaceB] peer-killing-blow clamp actorId={} netId={} hp={} dmg={}→{} (deferring death to host)",
                                actor->id, ext->netId,
                                (int)actor->colChkInfo.health,
                                (int)actor->colChkInfo.damage,
                                (int)reducedDamage);
                    actor->colChkInfo.damage = reducedDamage;
                }
            }
            return;
        }

        // #190 — drain queued DAMAGE_ENEMY damage from the actor's
        // EnemyNetId pending fields onto colChkInfo + AC_HIT. ShouldActorUpdate
        // fires BEFORE actor->update each frame the world is advancing, so
        // the synthetic hit gets consumed by the actor's UpdateDamage on the
        // same frame the world resumes from any freeze (Item Get / cutscene /
        // text-box / ocarina / pause). When the world is frozen, this hook
        // doesn't fire (no actor update to gate), so pending damage stays
        // queued in the ext until resume.
        Anchor::Instance->DrainPendingSyncDamage(actor);

        Player* localPlayer = GET_PLAYER(gPlayState);
        Actor* nearest = FindNearestPlayerActor(actor, gPlayState);

        // Bug 1 fix (2026-06-17): FindNearestPlayerActor may now return nullptr
        // when no valid player candidate exists (host's local Link is dead OR
        // in cutscene, AND every in-scene DummyPlayer is also dead/cross-
        // timeline). When that happens, leave vanilla cached fields alone —
        // they still point at host's local Link (the corpse). The AI keeps
        // swinging at the corpse, but with PLAYER_STATE1_DEAD the corpse has
        // no AC, so the swings deal no damage. This is the conservative
        // variant (per analysis §9 B1-A): pushing a sentinel like
        // xzDistToPlayer=99999 is rejected because vanilla AI is not
        // designed for "no target ever" and some branches may misbehave.
        //
        // Otherwise, only overwrite when a DummyPlayer (or NPC follower) is
        // closer than local Link. If local is nearest, the vanilla
        // calculation (z_actor.c:2665-2669) is already correct.
        if (nearest != nullptr && nearest != &localPlayer->actor) {
            actor->xzDistToPlayer    = Actor_WorldDistXZToActor(actor, nearest);
            actor->yDistToPlayer     = Actor_HeightDiff(actor, nearest);
            actor->xyzDistToPlayerSq = SQ(actor->xzDistToPlayer) + SQ(actor->yDistToPlayer);
            actor->yawTowardsPlayer  = Actor_WorldYawTowardActor(actor, nearest);
        }
        // *should is not modified — enemy AI runs normally
    });

    // Any client: when an enemy dies locally, broadcast the defeat so all other
    // clients kill the matching actor. Both host and non-host send this — after
    // Fix 4 non-host clients have live colliders and can land killing blows.
    // OnEnemyDefeat fires from within each enemy's death animation code, so the
    // death animation has already played on the killing client before we notify.
    COND_HOOK(OnEnemyDefeat, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Diagnostic: log every OnEnemyDefeat invocation so we can confirm
        // which actors fire this hook and whether their extension is found.
        SPDLOG_INFO("[EnemyDefeated] OnEnemyDefeat hook: actor ptr={} id={} cat={}", (void*)actor, actor->id, actor->category);
        if (!IsSaveLoaded()) {
            SPDLOG_WARN("[EnemyDefeated] OnEnemyDefeat skipped — save not loaded (actor id={})", actor->id);
            return;
        }
        EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext == nullptr) {
            SPDLOG_WARN("[EnemyDefeated] OnEnemyDefeat: no extension for actor id={} — netId assignment missed", actor->id);
            return;
        }
        // Scene-visit dedup: skip if this netId was already broadcast during the
        // current scene visit (e.g. second Actor_Kill on a recycled actor pointer,
        // or a Karebaba being killed again before its previous respawn cycle cleared
        // the dedup set).
        //
        // ClaimDefeatBroadcast returns true on first claim; false means a prior
        // OnEnemyDefeat / OnActorKill already broadcast for this netId in this
        // scene visit. Phase still transitions to DyingByLocal in the dup case
        // so PhaseImpliesHasLocalDeath continues to block ENEMY_UPDATE revives,
        // and the next Karebaba respawn detector fires (it reads
        // HasDefeatBroadcast which stays true).
        auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
        if (!bookkeeping.ClaimDefeatBroadcast(ext->netId)) {
            SPDLOG_INFO("[EnemyDefeated] OnEnemyDefeat: netId={} already sent this scene visit — skipping duplicate",
                        ext->netId);
            EnemyStateSync::AuditBooleansVsPhase(*ext, "OnEnemyDefeat.dedup");
            // Avoid Dead→DyingByLocal phase regression — En_Hintnuts'
            // puzzle-solve flow calls Actor_Kill BEFORE
            // GameInteractor_ExecuteOnEnemyDefeat (z_en_hintnuts.c:499-500),
            // so by the time we land here the OnActorKill path has already
            // transitioned the actor to Dead. Re-running
            // TransitionTo(DyingByLocal) regresses the phase and trips
            // ValidatePhaseTransition's "Unrecognised lifecycle transition
            // Dead -> DyingByLocal" warning. Both phases evaluate
            // identically on PhaseImplies* predicates so the regression
            // is benign in practice, but the warning is log noise. Only
            // transition if the actor is still in a pre-Dead phase
            // (legitimate dup cases — Karebaba killed-again-mid-respawn
            // cycle — still benefit from the DyingByLocal write).
            if (ext->phase != EnemyStateSync::LifecyclePhase::Dead) {
                EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByLocal);
            }
            return;
        }
        EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByLocal);
        // ENEMY_UPDATE re-apply guard now derived from phase via
        // PhaseImpliesHasLocalDeath(DyingByLocal) → true. The legacy
        // hasLocalDeath boolean was deleted at end of C2 Phase 1.
        // Scene host tracks kills for join-time replay (Fix 6).
        if (::SceneAuthority::IsMyCurrentRoomHost()) {
            bookkeeping.RecordSceneDeath(gPlayState->sceneNum, ext->netId);
            // Host-local-kill room-transition survival (surfaced in log 155
            // / Test 3 of C2 Phase 4 Commit B). Karebaba's natural death
            // cycle runs ~10s in ACTORCAT_MISC; if the host leaves the room
            // mid-cycle, OoT destroys the actor and re-spawns a fresh one
            // on return. mSceneDeaths is the host respawn guard's lookup
            // (line 3059), but it is wiped by ClearScene in OnActorSpawn
            // when numSetupActors > 0 — which DOES fire on intra-scene
            // room transitions despite the comment at line 2990 claiming
            // otherwise (verified via "static" suffix on spawn logs after
            // a room change). mPendingKills survives ClearScene, so adding
            // a parallel RecordPendingKill here lets OnActorSpawn's
            // pendingKill branch (line 3085) catch the re-spawn and route
            // through Fix 38 → SetupDeadItemDrop just like the receive-
            // side path does for non-host kills. Cleared by the host
            // respawn detector (line 3245) when the cycle completes.
            //
            // Limited to Karebaba (the only actor with a long cycle that
            // can outlive a room transition); other enemies die instantly
            // via Actor_Kill and have no cycle to preserve.
            if (actor->id == ACTOR_EN_KAREBABA) {
                bookkeeping.RecordPendingKill(ext->netId);
            }
        }
        // Karebaba death-direction sync: capture shape.rot.y at
        // OnEnemyDefeat time (= host's SetupDying time) so peer can
        // apply the exact value before its own SetupDyingNet. Avoids
        // the netShapeRot cache lag (~38°/frame during Spin) that
        // made flight directions diverge in log 308.
        if (actor->id == ACTOR_EN_KAREBABA) {
            SendPacket_EnemyDefeated(ext->netId, actor->shape.rot.y, /*includeShapeRotY=*/true);
        } else {
            SendPacket_EnemyDefeated(ext->netId);
        }

        // AI Director: notify removal for director-spawned enemies. Early-
        // exits inside OnEnemyRemoved if this netId isn't in the Director's
        // registry, so the cost for non-director-spawned kills is one hash
        // lookup. Cause is always Kill from this path; descriptors that
        // need other DefeatCauses (Leash, SceneExit, etc.) trigger those
        // via their own paths before calling Actor_Kill.
        //
        // Invader Step 5 follow-up (respawn-after-kill bug): OnEnemyRemoved
        // fires on every client (not effective-host-gated). Step 5 flipped
        // Director.Tick + ForceSpawn + ExecuteDespawn to per-room authority
        // so a client that is current room host but NOT effective host can
        // spawn an Invader — its state lives locally in mNetIdToDescriptor
        // and mActiveInvaders. If OnEnemyRemoved stayed effective-host-gated
        // that client's OnEnemyDefeat would leave stale mActiveInvaders and
        // InvaderDescriptor::OnTick's #234 host-actor-missing reconcile
        // would treat the dead actor as "scene-cleanup missing" and re-
        // instantiate via bypassCooldown follow-spawn on the next tick.
        // OnEnemyRemoved's internal .find(netId) short-circuit makes firing
        // on any client safe: cleanup runs only where state actually exists.
        auto& director = AnchorDirector::Director::Instance();
        director.OnEnemyRemoved(ext->netId, AnchorDirector::DefeatCause::Kill);

        // Step 6: reactive PlayerKilledEnemy event (and BossDefeated for
        // synced-boss kills) STAYS effective-host-gated. Descriptor
        // consumers that count kills ("ambush after N kills", "revenge
        // after boss death") must see each kill exactly once — firing
        // on every client would produce duplicate events.
        if (::SceneAuthority::IsEffectiveHost()) {
            AnchorDirector::DirectorEventPayload evt{};
            evt.clientId = ownClientId;  // local kill on host
            evt.sceneNum = (gPlayState != nullptr) ? (s16)gPlayState->sceneNum : 0;
            evt.roomNum  = (gPlayState != nullptr) ? (s8)gPlayState->roomCtx.curRoom.num : 0;
            evt.data["netId"]   = ext->netId;
            evt.data["actorId"] = actor->id;

            evt.type = AnchorDirector::DirectorEvent::PlayerKilledEnemy;
            director.NotifyEvent(evt);

            if (IsSyncedBossActor(actor->id)) {
                evt.type = AnchorDirector::DirectorEvent::BossDefeated;
                director.NotifyEvent(evt);
            }
        }
    });

    // Fix 12 — Actor_Kill death path: ENEMY_DEFEATED for enemies that skip OnEnemyDefeat.
    //
    // Some enemies die by calling Actor_Kill directly inside their death animation
    // (e.g., ACTOR_EN_SKB at dawn) rather than going through the standard health-zero →
    // OnEnemyDefeat → Actor_Kill sequence. Because OnEnemyDefeat never fires for these
    // actors, no ENEMY_DEFEATED packet is sent and the actor persists on remote clients.
    //
    // This hook fires for every Actor_Kill. It sends ENEMY_DEFEATED for any actor
    // that has an EnemyNetId extension (meaning it was ACTORCAT_ENEMY/BOSS at spawn)
    // but did NOT already send a packet through OnEnemyDefeat (guarded by
    // defeatPacketSent). The isKillingNetworkActor flag prevents echo loops when
    // HandlePacket_EnemyDefeated is the one calling Actor_Kill.
    //
    // Room-unload guard: OoT calls Actor_Kill on every actor in a room that is
    // being unloaded during a room transition. These are NOT real deaths — the
    // actors respawn when the room is re-entered. After Room Init fires for the
    // new room, gPlayState->curRoom.num reflects the destination; actors in the
    // old room have actor->room != curRoom.num, so we can detect and skip them.
    COND_HOOK(OnActorKill, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Log 331 diagnostic — track EN_ITEM00 deaths to find what
        // kills drops on peer side outside the ITEM_COLLECTED path.
        if (actor->id == ACTOR_EN_ITEM00) {
            const ItemDropNetId* idExt =
                ObjectExtension::GetInstance().Get<ItemDropNetId>(actor);
            const uint32_t idNet = (idExt != nullptr) ? idExt->netId : 0u;
            const int idState = (idExt != nullptr) ? (int)idExt->pickupState : -1;
            const bool hasParent = (actor->parent != nullptr);
            SPDLOG_INFO("[ItemDrop.diag] OnActorKill EN_ITEM00 netId={} params=0x{:02X} pickupState={} hasParent={} pos=({:.0f},{:.0f},{:.0f}) isNetSpawning={} isLocalOnly={}",
                        idNet, (int)actor->params, idState, hasParent ? "true" : "false",
                        actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                        g_isSpawningNetworkItemDrop ? "true" : "false",
                        g_isLocalOnlyItemDrop ? "true" : "false");
        }
        if (isKillingNetworkActor) {
            return; // This kill originated from a received ENEMY_DEFEATED — do not echo.
        }
        if (!IsSaveLoaded()) {
            return;
        }
        // Skip room-unload kills. When OoT transitions to a different room within
        // the same scene, it calls Actor_Kill on every actor in the departing room
        // after updating curRoom to the destination. Remote clients that are still
        // in the old room must NOT receive ENEMY_DEFEATED for these actors — the
        // enemies are not dead, just unloaded for the host. Detect by comparing
        // the actor's assigned room to the room the host is now in.
        if (gPlayState != nullptr && actor->room != gPlayState->roomCtx.curRoom.num) {
            return;
        }
        // Diagnostic: log every kill for ACTORCAT_ENEMY/BOSS/MISC actors so we can
        // confirm which enemies reach OnActorKill and why some skip sending ENEMY_DEFEATED.
        // MISC is included because Deku Baba stems change category before Actor_Kill fires.
        if (actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS ||
            actor->category == ACTORCAT_MISC) {
            EnemyNetId* diagExt = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            const uint32_t diagNetId = (diagExt != nullptr) ? diagExt->netId : 0u;
            const bool diagBroadcast = (diagExt != nullptr) &&
                EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(diagNetId);
            SPDLOG_INFO("[EnemyDefeated] OnActorKill: id={} cat={} ext={} netId={} broadcast={}",
                        actor->id, (int)actor->category,
                        (diagExt != nullptr ? "found" : "NULL"),
                        diagNetId, diagBroadcast);
        }
        // Do NOT filter by actor->category here — Deku Baba stems call
        // Actor_ChangeCategory(ACTORCAT_MISC) in EnDekubaba_SetupDeadStickDrop
        // before Actor_Kill fires, so they appear as ACTORCAT_MISC at this point.
        // Instead rely on the EnemyNetId extension: it is only assigned in OnActorSpawn
        // to ACTORCAT_ENEMY/BOSS actors, so its presence is sufficient proof.
        EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext == nullptr || ext->netId == 0) {
            return;
        }
        // #276 — suppress broadcast for Obj_Mure2 distance-cull of grass-
        // cluster children. Set by Obj_Mure2_CleanupAndDie at
        // z_obj_mure2.c:132-150 immediately before its Actor_Kill loop.
        // Each peer's local Obj_Mure2 makes the same distance-based cull
        // decision INDEPENDENTLY (vanilla state machine fires on local
        // player's projected distance, not host's), so peer has nothing
        // to clean up; host's broadcasts would arrive as 12 pendingKill
        // WARN lines per cluster cull (log 524 19:17:33.187 / 19:17:49.247).
        //
        // Placed BEFORE ClaimDefeatBroadcast intentionally — this is not
        // a "death" event but a "go to sleep, will respawn on approach"
        // event. Claiming the dedup ledger would block legitimate future
        // ENEMY_DEFEATED for the same netId within the scene visit
        // (e.g., the same grass actually getting cut by the player after
        // re-spawn). TransitionTo(Dead) is also skipped because the FSM
        // dies with the actor (per-Actor extension); next-respawn assigns
        // a fresh EnemyNetId.
        //
        // En_Kusa cut path is unaffected — EnKusa_SetupCut (TYPE_1/TYPE_2)
        // fires Anchor_BroadcastEnvActorDestroy directly, and TYPE_0's
        // Actor_Kill happens outside the Obj_Mure2 cull-context, so the
        // ENEMY_DEFEATED broadcast still fires for player-driven cuts.
        if (Anchor_IsObjMure2CullingChildren()) {
            SPDLOG_DEBUG("[EnemyDefeated] Actor_Kill suppressed (Obj_Mure2 distance cull) "
                         "for actor id={} netId={}",
                         actor->id, ext->netId);
            return;
        }
        // Single dedup gate via HostBookkeeping. ClaimDefeatBroadcast is
        // false on duplicate (either a prior OnEnemyDefeat / OnActorKill
        // for this netId in this scene visit, or — pre-extraction — what
        // the ext->defeatPacketSent guard at line 3637 caught). On a
        // duplicate we still transition to Dead so the FSM matches the
        // actor's actual state, then return without emitting.
        auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
        if (!bookkeeping.ClaimDefeatBroadcast(ext->netId)) {
            EnemyStateSync::AuditBooleansVsPhase(*ext, "OnActorKill.dedup");
            EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::Dead);
            return;
        }
        // Actor died via Actor_Kill without firing OnEnemyDefeat.
        // Broadcast ENEMY_DEFEATED so remote clients remove the actor.
        // ClaimDefeatBroadcast above guarantees re-entrant or repeated
        // Actor_Kill calls (e.g. OoT calling Actor_Kill twice on the
        // same actor, or multiple actors sharing a netId via posHash
        // collision) do not emit duplicate packets.
        EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::Dead);

        // #265 — suppress broadcast for actors that conditionally Actor_Kill
        // themselves inside _Init before OnActorUpdate ever fires
        // (En_Po_Field 8-of-10 cull, Obj_Mure2 per-flag culls, ...).
        // Peers' deterministic Init makes the same decision so they have
        // no live actor to clean up. ClaimDefeatBroadcast already claimed
        // above (line 3775) so the dedup ledger still stands; RecordSceneDeath
        // and Director::OnEnemyRemoved are also skipped because peers'
        // dead-enemy replay shouldn't include actors peers never saw, and
        // mNetIdToDescriptor only populates via RecordSpawn for director-
        // spawned actors (vanilla rejected-at-Init actors are never tracked).
        if (!ext->hasEverUpdated) {
            SPDLOG_DEBUG("[EnemyDefeated] Actor_Kill suppressed (never updated) "
                         "for actor id={} netId={}",
                         actor->id, ext->netId);
            return;
        }

        SPDLOG_INFO("[EnemyDefeated] Actor_Kill path: sending defeat for actor id={} netId={}",
                    actor->id, ext->netId);
        if (::SceneAuthority::IsMyCurrentRoomHost()) {
            bookkeeping.RecordSceneDeath(gPlayState->sceneNum, ext->netId);
        }
        SendPacket_EnemyDefeated(ext->netId);

        // AI Director: notify removal for director-spawned enemies via the
        // OnActorKill path too. Many enemies (Stalfos, Skullkid, Skb, etc.)
        // die through this hook rather than OnEnemyDefeat — and when both
        // hooks fire for the same kill, OnActorKill runs first and the
        // ClaimDefeatBroadcast dedup makes the OnEnemyDefeat sibling
        // early-return BEFORE its Director::OnEnemyRemoved call.
        // OnEnemyRemoved itself is idempotent: erases the netId from
        // mNetIdToDescriptor on first call, so a duplicate fire from
        // OnEnemyDefeat (in cases where it wins the race) is a no-op.
        //
        // Invader Step 5 follow-up — no longer effective-host-gated. See
        // the sibling OnEnemyDefeat block above for the full rationale
        // (per-room-authority spawner must clean up its own Director
        // state to avoid #234 reconcile respawning the killed actor).
        AnchorDirector::Director::Instance().OnEnemyRemoved(
            ext->netId, AnchorDirector::DefeatCause::Kill);
    });

    // Issue #171 fix B — clean up all ObjectExtension entries before
    // the actor's memory is freed by Actor_Delete (z_actor.c:3492 fires
    // OnActorDestroy immediately before the cleanup). Without this, the
    // EnemyNetId.skelAnime raw pointer dangles past Actor_Delete; if
    // the same memory address is later reused for a new actor, the
    // stale extension stays attached and any subsequent deref of
    // ext->skelAnime is a use-after-free. Likely contributor to #171's
    // 0xC0000005 access violations on Deku Tree room transitions, where
    // actor memory is reused frequently as rooms unload + reload.
    //
    // ObjectExtension::Free removes every type's entry for the given
    // pointer in one call — covers EnemyNetId and any future extension
    // types attached to actors.
    COND_HOOK(OnActorDestroy, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        // Read EnemyNetId BEFORE Free() wipes it. Used to scrub the
        // host-bookkeeping damager entry once the actor's memory is
        // about to be released. Damager attribution is consumed both
        // at OnEnemyDefeat broadcast time (SendPacket_EnemyDefeated)
        // AND at OnActorSpawn(EN_ITEM00) for the drop's killer
        // attribution correction; clearing earlier (the old behavior
        // in SendPacket_EnemyDefeated) lost the attribution between
        // those two consumers and tagged peer-killed drops with the
        // host's clientId. Per-actor cleanup here lives for the full
        // lifetime; scene-transition cleanup handled separately by
        // ClearStaleDamagers.
        const EnemyNetId* nidExt =
            ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (nidExt != nullptr && nidExt->netId != 0) {
            EnemyStateSync::HostBookkeeping::Instance().ClearDamager(nidExt->netId);
        }
        ObjectExtension::GetInstance().Free(actor);
        // Plan B (#193) — scrub any stale visual-rep pointer from the
        // SyncedClaimableDrop registry. The drop entry survives the actor
        // (claim arbitration may still be in flight); only the actor's
        // entry in visualReps needs to go before its memory is freed.
        SyncedClaimableDrop::Registry::Instance().UnregisterFromAllDrops(actor);
        // Plan B step 5 — also scrub from the modal-phantom adapter's
        // local list (separate from the SyncedClaimableDrop registry).
        SyncedClaimableDrop::ModalPhantomAdapter::GetInstance()->OnActorDestroyed(actor);
    });

    // #endregion

    COND_HOOK(OnPlayerSfx, isConnected, [&](u16 sfxId) { SendPacket_PlayerSfx(sfxId); });
    COND_HOOK(OnOcarinaNote, isConnected,
              [&](uint8_t note, float modulator, int8_t bend) { SendPacket_OcarinaSfx(note, modulator, bend); });

    COND_HOOK(OnLoadGame, isConnected, [&](s16 fileNum) { justLoadedSave = true; });

    COND_HOOK(OnSaveFile, isConnected, [&](s16 fileNum, int sectionID) {
        if (sectionID == 0) {
            SendPacket_UpdateTeamState();
        }
    });

    COND_HOOK(OnFlagSet, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_SetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnFlagUnset, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_UnsetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnSceneFlagSet, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) {
                  // Skip FLAG_SCENE_COLLECTIBLE flag=0 — vanilla
                  // Flags_SetCollectible (z_actor.c:824) skips the
                  // bitmask update for flag=0 yet still fires
                  // OnSceneFlagSet, so the hook fires for every
                  // grass-drop pickup with a collectibleFlag of 0
                  // (heart, rupee, nut from grass). Sending the
                  // packet has no positive effect (no flag bit is
                  // updated on receiver) and triggers SetFlag.cpp's
                  // collateral despawn walk that kills every ground
                  // drop in the scene. Belt-and-suspenders with the
                  // receive-side flag != 0 guard in SetFlag.cpp.
                  if (flagType == FLAG_SCENE_COLLECTIBLE && flag == 0) return;
                  SendPacket_SetFlag(sceneNum, flagType, flag);
              });

    COND_HOOK(OnSceneFlagUnset, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_UnsetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnRandoSetCheckStatus, isConnected, [&](RandomizerCheck rc, RandomizerCheckStatus status) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoSetIsSkipped, isConnected, [&](RandomizerCheck rc, bool isSkipped) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoEntranceDiscovered, isConnected,
              [&](u16 entranceIndex, u8 isReversedEntrance) { SendPacket_EntranceDiscovered(entranceIndex); });

    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, isConnected, [&](void* refActor) { SendPacket_GameComplete(); });

    // Plan §4 — generalised OnBossDefeat host-side hook for synced bosses.
    // Mirrors the OnEnemyDefeat hook above but gated on IsSyncedBossActor
    // so it only fires for opted-in bosses (ACTOR_BOSS_GOMA today).
    //
    // Boss_Goma fires OnBossDefeat in BossGoma_SetupDefeated (line 421
    // of z_boss_goma.c). The host-side hook records the death in
    // bookkeeping and broadcasts ENEMY_STATE phase=DyingByLocal so peers
    // route through BossGoma_SetupDyingNet (their cutscene-playing path).
    COND_HOOK(OnBossDefeat, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (!IsSaveLoaded()) return;
        if (!IsSyncedBossActor(actor->id)) return;

        EnemyNetId* ext = const_cast<EnemyNetId*>(
            ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
        if (ext == nullptr) {
            SPDLOG_WARN("[OnBossDefeat] no EnemyNetId extension for boss id={}", actor->id);
            return;
        }

        // Host-authoritative broadcast: only the elected host announces a
        // boss defeat. Peer's local BossGoma_UpdateHit can still enter
        // SetupDefeated → fire OnBossDefeat as a side effect of state-machine
        // sync (host's actionFunc transition replicates), but peer must NOT
        // broadcast ENEMY_DEFEATED back at the host. Without this gate, peer's
        // local "I think the boss is dying" claim would route to host and
        // trigger the kill cycle prematurely (field test 273: peer broadcast
        // defeat while host's HP was still 9 — host trusted peer and the boss
        // died on a fraction of full HP).
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
            EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
            return;
        }

        auto& bk = EnemyStateSync::HostBookkeeping::Instance();
        if (!bk.ClaimDefeatBroadcast(ext->netId)) {
            // Already broadcast — duplicate; transition phase but skip send.
            EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByLocal);
            return;
        }
        EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByLocal);
        bk.RecordSceneDeath(gPlayState->sceneNum, ext->netId);
        SPDLOG_INFO("[OnBossDefeat] id={} netId={} — broadcasting defeat", actor->id, ext->netId);
        SendPacket_EnemyDefeated(ext->netId);
    });

    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        // Phase 2 (#193 spec §4 Phase 2) — re-broadcast UPDATE_CLIENT_STATE
        // whenever the local eligibility bitmap changes (e.g. just
        // acquired a bag → now eligible for that ammo type; just
        // capped → no longer eligible). Receiving clients use the
        // bitmap to decide Layer 2 deferral on future drops.
        //
        // Send only on transition to avoid flooding UPDATE_CLIENT_STATE
        // on every consumable pickup. The cached bitmap is per-process;
        // resets to 0 on connect / scene change implicitly via
        // SendPacket_UpdateClientState being called from those hooks
        // (next OnItemReceive will see the new value as different from
        // last-sent — harmless single redundant send).
        {
            static uint32_t s_lastObservedEligibilityBitmap = 0;
            const uint32_t now = ItemEligibility::ComputeLocalEligibilityBitmap();
            if (now != s_lastObservedEligibilityBitmap) {
                SPDLOG_INFO("[Eligibility] bitmap changed 0x{:08X} -> 0x{:08X} — "
                            "broadcasting UPDATE_CLIENT_STATE",
                            s_lastObservedEligibilityBitmap, now);
                s_lastObservedEligibilityBitmap = now;
                SendPacket_UpdateClientState();
            }
        }

        // Handle vanilla dungeon items a bit differently
        if (itemEntry.modIndex == MOD_NONE &&
            (itemEntry.itemId >= ITEM_KEY_BOSS && itemEntry.itemId <= ITEM_KEY_SMALL)) {
            SendPacket_UpdateDungeonItems();
            return;
        }

        // #193 Q1 — Team Shares Pickups toggle. When OFF (competitive
        // mode), transient consumables (sticks / nuts / rupees / hearts
        // / bombs / arrows / magic / bombchus — all ITEM_CATEGORY_JUNK)
        // are NOT cross-broadcast: each player keeps only what they
        // personally picked up. Progression items (keys, bag upgrades,
        // hearts pieces, etc.) still cross-broadcast so the team's
        // collective progression stays in sync regardless of mode.
        if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("TeamSharesPickups"), 1) &&
            itemEntry.getItemCategory == ITEM_CATEGORY_JUNK) {
            return;
        }

        SendPacket_GiveItem(itemEntry.tableId, itemEntry.getItemId);
    });

    COND_HOOK(OnDungeonKeyUsed, isConnected, [&](uint16_t mapIndex) {
        // Handle vanilla dungeon items a bit differently
        SendPacket_UpdateDungeonItems();
    });

    COND_VB_SHOULD(VB_APPLY_TUNIC_COLOR, isConnected, {
        Actor* myPlayer = (Actor*)GET_PLAYER(gPlayState);
        Actor* actor = va_arg(args, Actor*);
        Color_RGB8* color = va_arg(args, Color_RGB8*);

        // #182 follow-up: pause-menu rotating-Link draw passes
        // `data = &playerSwordAndShield` (a u8* stack pointer; see
        // z_player_lib.c:2081, 2183), so neither the myPlayer == actor
        // branch nor the for-loop client.player match below catches it.
        // Without an override, the GPU env color from the previous
        // DummyPlayer draw (the last remote player rendered in the
        // world that frame) leaks onto the pause-Link, painting the
        // local player's preview with the wrong client's color (visibly
        // symmetric: P1 sees P2's color in P1's pause, and vice versa).
        // The pause-Link is ALWAYS the local player's preview, so apply
        // the local own-color CVar directly when this draw is in flight.
        if (Anchor_IsDrawingPauseLink()) {
            Color_RGBA8 ownColor = CVarGetColor(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
            color->r = ownColor.r;
            color->g = ownColor.g;
            color->b = ownColor.b;
            return;
        }

        // NPC Companion (Flotilla) — same color-leak class as the
        // pause-Link bug above, same fix shape. EnFollower_Draw passes
        // the local Player* as Player_DrawImpl's `data`, so without
        // this check the hook would either:
        //   (a) match `actor == myPlayer` and apply local color to ALL
        //       NPCs (including peer replicas of remote players), OR
        //   (b) inherit the previous DummyPlayer draw's GPU env color.
        // The flag set by EnFollower_Draw lets us look up the OWNER
        // of the NPC currently being drawn and apply that owner's
        // color (own-color CVar for our local NPC; client.color for
        // a peer's replica).
        if (Actor* drawingNpc = Anchor_GetCurrentlyDrawingFollowerNpc()) {
            uint32_t ownerCid = Anchor::Instance->FindFollowerNpcOwner(drawingNpc);
            if (ownerCid == 0) {
                // Unknown NPC — shouldn't happen, fall through.
            } else if (ownerCid == Anchor::Instance->ownClientId) {
                // Local NPC → use own-color CVar (same source as the
                // pause-Link branch above).
                Color_RGBA8 ownColor = CVarGetColor(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
                color->r = ownColor.r;
                color->g = ownColor.g;
                color->b = ownColor.b;
                return;
            } else {
                // Peer replica → use the peer's color from clients map.
                auto it = Anchor::Instance->clients.find(ownerCid);
                if (it != Anchor::Instance->clients.end()) {
                    color->r = it->second.color.r;
                    color->g = it->second.color.g;
                    color->b = it->second.color.b;
                    return;
                }
                // Peer not in clients map (race? disconnect?) — fall through.
            }
        }

        // NPC Invader — same draw-context flag class as the NPC
        // Follower above. EnInvader_Draw passes the local Player* as
        // Player_DrawImpl's `data`, so without this check the hook
        // would either match `actor == myPlayer` and paint the
        // Invader with local color, or inherit the previous draw's
        // GPU env color. Hostile-black is the design tint per the
        // user's spec (plan §2.1 said "phantom red"; user requested
        // black during step 15a implementation).
        if (Anchor_GetCurrentlyDrawingInvader() != nullptr) {
            color->r = 0;
            color->g = 0;
            color->b = 0;
            return;
        }

        if (actor == myPlayer) {
            Color_RGBA8 ownColor = CVarGetColor(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
            color->r = ownColor.r;
            color->g = ownColor.g;
            color->b = ownColor.b;
            return;
        }

        // The pause-menu calls Player_DrawImpl with `data = &playerSwordAndShield`
        // (a `u8*` stack pointer) instead of an Actor*. The hook contract types
        // `data` as `Actor*` but the pause path violates that. Calling
        // GetDummyPlayerClientId on this fake pointer reads ObjectExtension by
        // raw address; collisions with stale entries can return a real DummyPlayer
        // clientId, applying that DummyPlayer's color to the LOCAL player's
        // pause-menu Link (visible bug). Defend by matching the pointer against
        // the actual DummyPlayer pointers tracked in `clients` — only override
        // the color when actor is a known DummyPlayer Player struct.
        Player* asPlayer = (Player*)actor;
        for (auto& [id, client] : Anchor::Instance->clients) {
            if (client.player == asPlayer) {
                color->r = client.color.r;
                color->g = client.color.g;
                color->b = client.color.b;
                return;
            }
        }
        // Not a recognised live player actor (e.g. pause-menu render). Fall
        // through with no override so the caller's default tunic color (or
        // local cosmetic) wins.
    });

    // #endregion

    // #region Hooks that are purely to sync actor states across the clients, not super essential

    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        EnItem00* actor = static_cast<EnItem00*>(refActor);

        if (Flags_GetCollectible(gPlayState, actor->collectibleFlag)) {
            Actor_Kill(&actor->actor);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BOMBWALL, isConnected, [&](void* refActor, bool* should) {
        BgBombwall* actor = static_cast<BgBombwall*>(refActor);

        if (actor->actionFunc == func_8086ED70 && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BREAKWALL, isConnected, [&](void* refActor, bool* should) {
        BgBreakwall* actor = static_cast<BgBreakwall*>(refActor);

        if (actor->actionFunc == BgBreakwall_Wait && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HAKA_ZOU, isConnected, [&](void* refActor, bool* should) {
        BgHakaZou* actor = static_cast<BgHakaZou*>(refActor);

        if (actor->actionFunc == func_80883000 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HAMSTEP, isConnected, [&](void* refActor, bool* should) {
        BgHidanHamstep* actor = static_cast<BgHidanHamstep*>(refActor);

        if (actor->actionFunc == func_808887C4 && Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0xFF)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HROCK, isConnected, [&](void* refActor, bool* should) {
        BgHidanHrock* actor = static_cast<BgHidanHrock*>(refActor);

        if (actor->actionFunc == func_808896B8 && Flags_GetSwitch(gPlayState, actor->unk_16A)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_ICE_SHELTER, isConnected, [&](void* refActor, bool* should) {
        BgIceShelter* actor = static_cast<BgIceShelter*>(refActor);

        if (actor->actionFunc == BgIceShelter_Idle && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            BgIceShelter_SetupMelt(actor);
            Audio_PlayActorSound2(&actor->dyna.actor, NA_SE_EV_ICE_MELT);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBCHUIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombchuiwa* actor = static_cast<BgJyaBombchuiwa*>(refActor);

        if (actor->actionFunc == BgJyaBombchuiwa_WaitForExplosion &&
            Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombiwa* actor = static_cast<BgJyaBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_MIZU_BWALL, isConnected, [&](void* refActor, bool* should) {
        BgMizuBwall* actor = static_cast<BgMizuBwall*>(refActor);

        if (actor->actionFunc == BgMizuBwall_Idle &&
            Flags_GetSwitch(gPlayState, ((u16)actor->dyna.actor.params >> 8) & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT08_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot08Bakudankabe* actor = static_cast<BgSpot08Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT11_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot11Bakudankabe* actor = static_cast<BgSpot11Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT17_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot17Bakudankabe* actor = static_cast<BgSpot17Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            func_808B6BC0(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->dyna.actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            Actor_Kill(&actor->dyna.actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_MARUTA, isConnected, [&](void* refActor, bool* should) {
        BgYdanMaruta* actor = static_cast<BgYdanMaruta*>(refActor);

        if (actor->actionFunc == func_808BF078 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_SP, isConnected, [&](void* refActor, bool* should) {
        BgYdanSp* actor = static_cast<BgYdanSp*>(refActor);

        if ((actor->actionFunc == BgYdanSp_FloorWebIdle || actor->actionFunc == BgYdanSp_WallWebIdle) &&
            Flags_GetSwitch(gPlayState, actor->isDestroyedSwitchFlag)) {
            BgYdanSp_BurnWeb(actor, gPlayState);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_SHUTTER, isConnected, [&](void* refActor, bool* should) {
        DoorShutter* actor = static_cast<DoorShutter*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            DECR(actor->unlockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_DOOR, isConnected, [&](void* refActor, bool* should) {
        EnDoor* actor = static_cast<EnDoor*>(refActor);

        if (actor->actionFunc == EnDoor_Idle && Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            DECR(actor->lockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SI, isConnected, [&](void* refActor, bool* should) {
        EnSi* actor = static_cast<EnSi*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SW, isConnected, [&](void* refActor, bool* should) {
        EnSw* actor = static_cast<EnSw*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ITEM_B_HEART, isConnected, [&](void* refActor, bool* should) {
        ItemBHeart* actor = static_cast<ItemBHeart*>(refActor);

        if (Flags_GetCollectible(gPlayState, 0x1F)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        ObjBombiwa* actor = static_cast<ObjBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjBombiwa_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 80, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_HAMISHI, isConnected, [&](void* refActor, bool* should) {
        ObjHamishi* actor = static_cast<ObjHamishi*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjHamishi_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_HAMMER_TOTEM_BREAK, isConnected, {
        BgHidanDalm* actor = va_arg(args, BgHidanDalm*);

        if (Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            *should = true;
        }
    });

    // Queue item 29 — Gold Skulltula token pickup freeze in MP.
    // Vanilla behaviour sets `player->actor.freezeTimer = 10` on pickup
    // and refreshes it per frame while the TEXT_GS_NO_FREEZE textbox is
    // open, locking controller input except A/B. Acceptable in solo;
    // disruptive in MP because a peer's cosmetic pickup interrupts a
    // moving player.
    //
    // Sources: z_en_si.c:100-102 (contact pickup), :126-128 (hookshot
    // pickup), :139-141 (per-frame refresh while textbox open). Skipping
    // VB_FREEZE_ON_SKULL_TOKEN skips both initial set and refresh; the
    // TEXT_GS_NO_FREEZE textbox variant auto-scrolls so no input is
    // needed to advance. Matches the pattern already used by the
    // gEnhancements.SkulltulaFreeze CVar handler at
    // timesaver_hook_handlers.cpp:711 — this is the MP-active variant.
    COND_VB_SHOULD(VB_FREEZE_ON_SKULL_TOKEN, isConnected, { *should = false; });

    // Generic NPC State Sync Phase 1 — Mido (#184).
    // Vanilla `EnMd_BlockPath` (z_en_md.c:735-758) gates the BlockPath ->
    // Walk transition on `talkState == NPC_TALK_STATE_ACTION` (the local
    // dialogue just concluded). In MP, only the player who actually had
    // the dialogue with Mido has their local talkState set; the OTHER
    // team member's local Mido stays in BlockPath because the dialogue
    // never ran on their machine.
    //
    // Vanilla code at z_en_md.c:746 sets `EVENTCHKINF_SHOWED_MIDO_SWORD_SHIELD`
    // inside the BlockPath -> Walk transition branch for the original
    // sword+shield encounter. That save flag syncs to all team members
    // via the existing SyncItemsAndFlags + SetFlag mechanism (Pillar 0).
    //
    // This hook returns true for `VB_MOVE_MIDO_IN_KOKIRI_FOREST` so each
    // team member's local Mido transitions to Walk independently once
    // any one of them has satisfied sword+shield. The `!has_emerald`
    // gate is critical: SHOWED stays set forever once the original
    // encounter completes, so without the gate this hook would fire
    // *should=true on the very first frame of the post-Deku-Tree
    // confrontation (Mido's Init case 2 at z_en_md.c:662-669, where
    // SHOWED is already true and the player has the emerald). That made
    // BlockPath skip the post-Deku confrontation dialog, walk the path,
    // and Actor_Kill on both clients before the player could interact —
    // setting SPOKE_TO_MIDO_AFTER_DEKU_TREES_DEATH without the
    // confrontation ever occurring.
    //
    // Post-Deku-Tree sync: after the dialog client's local Walk fires
    // Actor_Kill + SetFlag(SPOKE) (z_en_md.c:811-814), the SPOKE flag
    // syncs to remote clients. The despawn check at the top of
    // EnMd_Update (z_en_md.c) sees the synced flag and despawns the
    // remote-client Mido directly — no walk-away cinematic on remote.
    COND_VB_SHOULD(VB_MOVE_MIDO_IN_KOKIRI_FOREST, isConnected, {
        if (Flags_GetEventChkInf(EVENTCHKINF_SHOWED_MIDO_SWORD_SHIELD) &&
            !CHECK_QUEST_ITEM(QUEST_KOKIRI_EMERALD)) {
            *should = true;
        }
    });

    // #193 Phase 3 — EnItem00 pickup gate.
    //
    // Two-layer gate, applied only to drops that have an ItemDropNetId
    // extension (i.e., went through the host-broadcast path). Local-only
    // drops (no extension — scripted spawns, env actors not yet wrapped
    // in Phase 4) keep vanilla pickup behaviour.
    //
    // Layer 1: 3s killer-exclusivity. Within `kKillerExclusiveMs` of
    // spawnTimeMs, only `killerClientId` can pick up. Other players
    // walk through the drop with no effect (gate sets *should = false).
    //
    // Layer 2: per-player eligibility via ItemEligibility::CanPlayerCollectItem00
    // with `walletCapAware = true`. Wallet-capped, full-HP, ammo-capped,
    // etc. all block local pickup so the drop stays available for a
    // teammate who CAN benefit. Vanilla single-player just truncated
    // surplus silently.
    //
    // On gate-pass: broadcast ITEM_COLLECTED so peers Actor_Kill their
    // local copy. Vanilla pickup body proceeds (Item_Give credits the
    // local gSaveContext). On gate-fail: *should = false; vanilla
    // pickup is suppressed; drop persists for someone else.
    COND_VB_SHOULD(VB_GIVE_ITEM_FROM_ITEM_00, isConnected, {
        EnItem00* item00 = va_arg(args, EnItem00*);
        if (item00 == nullptr) return;

        const ItemDropNetId* ext =
            ObjectExtension::GetInstance().Get<ItemDropNetId>(&item00->actor);
        if (ext == nullptr) {
            // No extension -> local-only drop. Vanilla pickup proceeds
            // with no MP coordination (current behaviour pre-#193).
            return;
        }

        // [Diag] pending-bugs 2026-07-16 — Bug 2 rupee pickup failure.
        // Log every gate decision so we can identify which layer blocks
        // the pickup on repro. Enable with:
        //   set gEnhancements.PendingBugsDiag 1
        // Each block path already has SPDLOG_DEBUG lines, but DEBUG is
        // off by default in Release. This wrapper elevates to INFO so
        // the field-test log captures it. Zero behavior change.
        // See Claude/Analysis/playthrough_2026-07-15_session2_triage.md.
        if (CVarGetInteger("gEnhancements.PendingBugsDiag", 0)) {
            const int64_t nowMsDiag = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t ageMs = nowMsDiag - ext->spawnTimeMs;
            SPDLOG_INFO("[ItemDropPickup.diag] gate netId={} params=0x{:02X} pickupState={} "
                        "ageMs={} killer={} killerTeam='{}' localClient={} localTeam='{}'",
                        ext->netId, (unsigned)(item00->actor.params & 0xFF),
                        (int)ext->pickupState, (long long)ageMs,
                        ext->killerClientId, ext->killerTeamId,
                        Anchor::Instance->ownClientId,
                        CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default"));
        }

        // Race A mitigation: Granted state means host has arbitrated
        // this drop in our favour. Transition to Consumed (terminal)
        // and allow vanilla pickup to run.
        if (ext->pickupState == ItemPickupState::Granted) {
            ItemDropNetId* mut = const_cast<ItemDropNetId*>(ext);
            mut->pickupState = ItemPickupState::Consumed;
            SPDLOG_INFO("[ItemDrop] netId={} grant consumed — applying pickup",
                        ext->netId);
            // *should stays true; vanilla pickup runs.
            return;
        }

        // Consumed (terminal): vanilla's first-frame pickup ran. Subsequent
        // gate fires keep returning *should=true so vanilla's give-item
        // flow can complete over multiple frames (Actor_OfferGetItemNearby
        // / Actor_HasParent / Actor_Kill). Without this short-circuit,
        // the gate re-routed to race A and sent spurious ITEM_PICKUP_REQUEST
        // packets while suppressing vanilla — the actor lived to its
        // 220-frame unk_15A timeout (log 287 Bug 2).
        if (ext->pickupState == ItemPickupState::Consumed) {
            // *should stays true; vanilla pickup continues. Idempotent.
            return;
        }

        // Pending: request is in flight to host. Suppress vanilla pickup
        // until the host's ITEM_COLLECTED arbitration broadcast arrives.
        // The gate re-fires every frame the player is adjacent — when
        // ITEM_COLLECTED grants us, pickupState transitions to Granted
        // and the next gate fire (above) lets vanilla pickup run.
        if (ext->pickupState == ItemPickupState::Pending) {
            *should = false;
            return;
        }

        const int64_t kKillerExclusiveMs = 3000;
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // Spawn-grace window — suppress ALL pickup attempts for the
        // first kHostGraceMs after spawn so peers have a guaranteed
        // visible window of the drop on the ground (log 290 Bug B).
        // Without this, host's vanilla auto-pickup fires within 1-2
        // frames of the broadcast when host's player is within the
        // vanilla 30u proximity radius (which it always is after a
        // sword-kill on a Dekubaba head), and peers see the network
        // spawn appear + the ITEM_COLLECTED kill arrive on the same
        // frame — the drop flashes for 0 frames and is gone.
        //
        // 750 ms is enough for peers to register the drop visually
        // and start walking to it (race A can then arbitrate). Killer
        // gets first pickup once the grace expires, before Layer 1
        // expires for non-killer peers.
        //
        // The state-machine states above (Pending / Granted /
        // Consumed) are checked FIRST so an in-progress arbitration
        // is not re-blocked by grace.
        constexpr int64_t kHostGraceMs = 750;
        if (nowMs - ext->spawnTimeMs < kHostGraceMs) {
            *should = false;
            SPDLOG_DEBUG("[ItemDrop] netId={} grace period ({} ms remaining)",
                         ext->netId,
                         (long long)(kHostGraceMs - (nowMs - ext->spawnTimeMs)));
            return;
        }

        const bool inExclusiveWindow =
            (ext->killerClientId != 0) &&
            (nowMs - ext->spawnTimeMs < kKillerExclusiveMs);
        const bool isLocalKiller =
            (ext->killerClientId == Anchor::Instance->ownClientId);

        // Layer 1 — killer-exclusive window with team-aware bypass
        // (spec §1 Q2). During the 3 s window:
        //   - killer always bypasses
        //   - same-team players bypass iff TeamSharesPickups=true
        //   - everyone else is blocked until window expires
        const bool teamSharesPickups =
            CVarGetInteger(CVAR_REMOTE_ANCHOR("TeamSharesPickups"), 1) != 0;
        const std::string localTeamId =
            CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        const bool isSameTeamAsKiller =
            !ext->killerTeamId.empty() && (ext->killerTeamId == localTeamId);
        const bool teammateBypass =
            teamSharesPickups && isSameTeamAsKiller && !isLocalKiller;

        if (inExclusiveWindow && !isLocalKiller && !teammateBypass) {
            *should = false;
            SPDLOG_DEBUG("[ItemDrop] netId={} blocked — exclusive window cross-team "
                         "({} ms remaining; killer={} killerTeam='{}' localTeam='{}' "
                         "shares={})",
                         ext->netId,
                         (long long)(kKillerExclusiveMs - (nowMs - ext->spawnTimeMs)),
                         ext->killerClientId, ext->killerTeamId, localTeamId,
                         teamSharesPickups ? "true" : "false");
            return;
        }
        if (inExclusiveWindow && teammateBypass) {
            SPDLOG_DEBUG("[ItemDrop] netId={} teammate bypass (TeamSharesPickups=true, "
                         "team='{}')",
                         ext->netId, ext->killerTeamId);
        }

        // Layer 2 — per-player eligibility.
        //
        // Field-test 2026-05-06 (build 7f2ceab solo session, Inside
        // Deku Tree): host killed Dekubabas pre-nut-bag-acquisition.
        // ITEM00_NUTS dropped, Layer 2 blocked pickup because
        // `CUR_CAPACITY(UPG_NUTS) == 0` (no bag yet) → drops were
        // permanently unreachable until despawn. Eligibility gate's
        // purpose is "defer to teammate who CAN benefit", but in a
        // solo session there's no teammate → deferral is pointless;
        // vanilla single-player just truncates surplus silently
        // (no-op pickup).
        //
        // Skip Layer 2 when no other team member is online + save-
        // loaded. Vanilla pickup proceeds; Item_Give either credits
        // the player (if eligible) or is a silent no-op (if capped /
        // missing inventory slot).
        //
        // Field-test 2026-05-06 (build b0ea6f1 MP session, Inside
        // Deku Tree): in MP both P1 and P2 lacked the nut bag, so
        // Layer 2 blocked everyone — host's Dekubaba kill drops were
        // permanently un-collectible until despawn. Narrow fix:
        // bypass Layer 2 for the killer during their own 3 s
        // exclusive window. Rationale: the killer is owed first
        // attempt regardless of eligibility (vanilla single-player
        // would silently truncate surplus for that same player; MP
        // shouldn't be stricter than vanilla for the killer's own
        // drops). After the window expires, Layer 2 resumes for all
        // players including the original killer — the drop falls
        // back to "first eligible teammate" semantics. The wider
        // "no teammate is eligible" relaxation is left for a future
        // pass where peers broadcast bag/wallet eligibility on
        // UPDATE_CLIENT_STATE.
        //
        // Note on phantom-grant risk (Bug Report 2026-05-06,
        // BugReport_HeartPickupSoundOnDoorEntry.md): Layer 2 is the
        // last barrier against any future code path that spawns an
        // EN_ITEM00 with an ItemDropNetId extension during scene-
        // spawn. The killer-bypass here only fires when the drop is
        // a real on-the-ground EN_ITEM00 the killer is adjacent to
        // within 3 s of their own kill (VB_GIVE_ITEM_FROM_ITEM_00
        // fires from EnItem00's collide path); the WorldStateSync
        // flag-replay phantom-grant chain goes through `Item_Give`
        // directly and does NOT pass through this gate. The
        // relaxation is safe with respect to that bug.
        // Layer 2 — per-player eligibility with eligibility-bitmap bypass
        // (Phase 2 of item_drop_behavior_spec.md, Q3 resolution).
        //
        // Block ONLY when:
        //   - local player is NOT eligible (CanPlayerCollectItem00 false),
        //   - AND at least one online same-team teammate IS eligible
        //     (their broadcast eligibilityBitmap has the relevant bit),
        //   - AND we are NOT in the killer-exclusive bypass window.
        //
        // When no teammate is eligible, allow vanilla pickup
        // (silent-truncate parity with single-player). When TeamSharesPickups
        // is false (competitive), eligibility deferral makes no sense —
        // there's no shared bag to defer toward, so the gate skips.
        //
        // Earlier full-removal (commit 5e3b794b8) was a placeholder until
        // the bitmap broadcast (Phase 2) landed; this re-adds the gate
        // with the bypass logic Q3 specified.
        s16 itemType = (s16)(item00->actor.params & 0xFF);
        const bool killerExclusiveBypass = isLocalKiller && inExclusiveWindow;
        if (!killerExclusiveBypass && teamSharesPickups) {
            if (!ItemEligibility::CanPlayerCollectItem00(itemType, /*walletCapAware=*/true)) {
                bool anyTeammateEligible = false;
                const uint32_t itemBit = ItemEligibility::EligibilityBitForItem00(itemType);
                if (itemBit != 0) {
                    for (auto& [otherId, other] : Anchor::Instance->clients) {
                        if (other.self || !other.online || !other.isSaveLoaded) continue;
                        if (other.teamId != localTeamId) continue;
                        if ((other.eligibilityBitmap & itemBit) != 0) {
                            anyTeammateEligible = true;
                            break;
                        }
                    }
                }
                if (anyTeammateEligible) {
                    *should = false;
                    SPDLOG_DEBUG("[ItemDrop] netId={} blocked — local ineligible, teammate "
                                 "eligible (type=0x{:02X}); deferring",
                                 ext->netId, (int)itemType);
                    return;
                }
                SPDLOG_DEBUG("[ItemDrop] netId={} local ineligible but no teammate eligible "
                             "— allowing silent-truncate (type=0x{:02X})",
                             ext->netId, (int)itemType);
            }
        }

        // Gate passes. Diverge by host vs peer.
        //
        // Host: vanilla pickup proceeds + broadcast ITEM_COLLECTED with
        // host's own clientId as the winner. Other peers see this and
        // Actor_Kill their local copies.
        //
        // Peer: race A mitigation — request host arbitration. Set
        // pickupState=Pending, send ITEM_PICKUP_REQUEST, suppress
        // vanilla. Vanilla runs on the next gate fire after host's
        // ITEM_COLLECTED grant transitions state to Granted.
        if (::SceneAuthority::IsMyCurrentRoomHost()) {
            // Phase 3 C-hybrid: look up the decorative offering actor
            // (Dekubaba head / Karebaba) near the EN_ITEM00 to embed
            // its EnemyNetId in ITEM_COLLECTED so receivers can
            // Actor_Kill the decoration at the moment of pickup. Same
            // proximity-walk pattern as killer-attribution recovery.
            //
            // ITEM_COLLECTED broadcasts do NOT echo back to the sender
            // (verified log 290 P1 AnchorProfile: rx_pps=0 for
            // ITEM_COLLECTED after host's own grant). So host must
            // ALSO dismiss the associated actor LOCALLY here — peers
            // dismiss via HandlePacket_ItemCollected.
            uint32_t assocActorNetId = 0;
            Actor*   assocActor      = nullptr;
            {
                constexpr float kAssocRadius   = 200.0f;
                constexpr float kAssocRadiusSq = kAssocRadius * kAssocRadius;
                float bestDistSq = kAssocRadiusSq;
                for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
                    Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
                    while (a != nullptr) {
                        const EnemyNetId* nidExt =
                            ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                        if (nidExt != nullptr && nidExt->netId != 0 &&
                            nidExt->phase != EnemyStateSync::LifecyclePhase::Alive &&
                            a->update != nullptr) {
                            const float dx = a->world.pos.x - item00->actor.world.pos.x;
                            const float dy = a->world.pos.y - item00->actor.world.pos.y;
                            const float dz = a->world.pos.z - item00->actor.world.pos.z;
                            const float dSq = dx * dx + dy * dy + dz * dz;
                            if (dSq < bestDistSq) {
                                bestDistSq      = dSq;
                                assocActorNetId = nidExt->netId;
                                assocActor      = a;
                            }
                        }
                        a = a->next;
                    }
                }
            }

            SPDLOG_INFO("[ItemDrop] netId={} pickup by host — broadcasting ITEM_COLLECTED "
                        "type=0x{:02X} assocActorNetId={}",
                        ext->netId, (int)itemType, assocActorNetId);
            Anchor::Instance->SendPacket_ItemCollected(ext->netId, assocActorNetId);

            // Local dismissal (host doesn't get its own echo).
            // No isKillingNetworkActor bracket here — this code runs
            // inside a COND_VB_SHOULD lambda which has no `this`
            // capture (REGISTER_VB_SHOULD macro uses [] captures),
            // so the bare member access doesn't compile. The
            // OnActorKill ClaimDefeatBroadcast ledger dedups
            // redundant ENEMY_DEFEATED broadcasts via the actor's
            // phase state (already past Alive for offerers in
            // DeadStickDrop / DeadItemDrop), so the bracket would
            // be defensive at most.
            if (assocActor != nullptr) {
                // Skip dismissal for actors with a natural respawn
                // cycle the pickup must NOT interrupt:
                //   - EN_KUSA: cut-stub state → CutWaitRegrow →
                //     SetupRegrow → Main
                //   - EN_KAREBABA: DeadItemDrop → Dead → Regrow →
                //     Idle (z_en_karebaba.c). Field test log 304
                //     showed the dismissal Actor_Killed Karebabas
                //     mid-DeadItemDrop, removing them from the
                //     scene entirely. For EN_KAREBABA also set
                //     params=0 so the DeadItemDrop tick advances
                //     to SetupDead next frame — without that, the
                //     visible head decoration (gDekuBabaStickDropDL,
                //     z_en_karebaba.c:604) stays drawn for the
                //     remaining 200-frame countdown (log 305 user
                //     feedback: "dropped stick model isn't
                //     disappearing when picked up").
                if (assocActor->id == ACTOR_EN_KUSA) {
                    SPDLOG_INFO("[ItemDrop] skipping dismiss for assoc netId={} "
                                "(actor id=EN_KUSA — cut state regrows naturally)",
                                assocActorNetId);
                } else if (assocActor->id == ACTOR_EN_KAREBABA) {
                    assocActor->params = 0;
                    SPDLOG_INFO("[ItemDrop] fast-forwarding EN_KAREBABA assoc netId={} "
                                "to SetupDead (params=0; respawn cycle continues)",
                                assocActorNetId);
                } else {
                    SPDLOG_INFO("[ItemDrop] dismissing associated actor netId={} locally on host "
                                "(no own-echo for ITEM_COLLECTED)",
                                assocActorNetId);
                    Actor_Kill(assocActor);
                }
            }
            // Mark Consumed so subsequent gate fires (vanilla's
            // multi-frame give-item flow) keep returning *should=true
            // without re-broadcasting ITEM_COLLECTED. Same fix as the
            // peer Granted→Consumed transition.
            ItemDropNetId* mut = const_cast<ItemDropNetId*>(ext);
            mut->pickupState = ItemPickupState::Consumed;
            // *should stays true.
            return;
        }

        // Peer pickup: request host arbitration.
        ItemDropNetId* mut = const_cast<ItemDropNetId*>(ext);
        mut->pickupState = ItemPickupState::Pending;
        Anchor::Instance->SendPacket_ItemPickupRequest(ext->netId);
        SPDLOG_INFO("[ItemDrop] netId={} pickup request sent to host — suppressing vanilla pickup pending grant",
                    ext->netId);
        *should = false;
    });

    // Visual cue for non-pickable drops: REMOVED.
    //
    // The previous shrink-to-60% behaviour conflated "this drop is
    // currently un-pickable" with "this drop is small" and caused
    // user-confusion side effects (Phase 1 field test log 288: peer
    // saw drops materialise small and "pop" up to normal size on
    // window expiry). Per design discussion 2026-05-27, the cue is
    // being replaced by a material/colour swap (drop renders in
    // muted grey while un-pickable). Design captured in
    // Claude/Plans/item_drop_visual_cue_material_swap.md;
    // implementation deferred until after stick-drop testing.


    COND_VB_SHOULD(VB_FIRE_TEMPLE_BOMBABLE_WALL_BREAK, isConnected, {
        BgHidanKowarerukabe* actor = va_arg(args, BgHidanKowarerukabe*);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0x3F)) {
            *should = true;
        }
    });

    // #endregion

    // #region Hooks for visual effects that don't affect gameplay

    struct CompassIcon {
        Vec3f pos;
        Vec3s rot;
        float scale;
        Color_RGB8 color;
    };

    COND_HOOK(OnMinimapDrawCompassIcons, isConnected, [&]() {
        if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("ShowOtherPlayersOnMinimap"), 1) ||
            Anchor::Instance->roomState.showLocationsMode == 0) {
            return;
        }

        std::vector<CompassIcon> compassIcons;

        bool isInDungeon = gPlayState->sceneNum == SCENE_DEKU_TREE || gPlayState->sceneNum == SCENE_DODONGOS_CAVERN ||
                           gPlayState->sceneNum == SCENE_JABU_JABU || gPlayState->sceneNum == SCENE_FOREST_TEMPLE ||
                           gPlayState->sceneNum == SCENE_FIRE_TEMPLE || gPlayState->sceneNum == SCENE_WATER_TEMPLE ||
                           gPlayState->sceneNum == SCENE_SPIRIT_TEMPLE || gPlayState->sceneNum == SCENE_SHADOW_TEMPLE ||
                           gPlayState->sceneNum == SCENE_BOTTOM_OF_THE_WELL || gPlayState->sceneNum == SCENE_ICE_CAVERN;
        std::string teamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");

        // When transitioning to a new room via a door, curRoom.num updates immediately but the minimap still shows the
        // previous room while fading out
        s8 displayedRoomNum =
            gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

        for (auto& [clientId, client] : Anchor::Instance->clients) {
            // Show compass icons for other players in the current scene. Also require them to be in the current room
            // within dungeons. If showLocationsMode isn't all players (2), only show compass icons for players of the
            // same team
            if (!client.self && client.online && client.player && client.sceneNum == gPlayState->sceneNum &&
                (!isInDungeon || client.curRoomNum == displayedRoomNum) &&
                (Anchor::Instance->roomState.showLocationsMode == 2 || client.teamId == teamId)) {
                compassIcons.push_back(
                    CompassIcon{ client.player->actor.world.pos, client.player->actor.shape.rot, 0.3f, client.color });
            }
        }

        // The local player's compass icon is always last so it gets drawn above the others
        Player* player = GET_PLAYER(gPlayState);
        compassIcons.push_back(CompassIcon{ player->actor.world.pos, player->actor.shape.rot, 0.4f,
                                            CVarGetColor24(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 }) });

        // Adapted internals of Minimap_DrawCompassIcons()
        s16 leftMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.L"), 0);
        s16 rightMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.R"), 0);
        s16 bottomMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.B"), 0);

        s16 xMarginsMinimap;
        s16 yMarginsMinimap;
        if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ORIGINAL_LOCATION) {
                xMarginsMinimap = rightMinimapMargin;
            }
            yMarginsMinimap = bottomMinimapMargin;
        } else {
            xMarginsMinimap = 0;
            yMarginsMinimap = 0;
        }

        s16 mapWidth = isInDungeon ? R_DGN_MINIMAP_X : R_OW_MINIMAP_X;
        s16 mapStartPosX = isInDungeon ? 96 : gMapData->owMinimapWidth[R_MAP_INDEX];

        OPEN_DISPS(gPlayState->state.gfxCtx);
        Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

        for (auto& compassIcon : compassIcons) {
            gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

            // The compass offset value is a factor of 10 compared to N64 screen pixels and originates in the center of
            // the screen Compute the additional mirror offset value by normalizing the original offset position and
            // taking it's distance to the center of the map, duplicating that result and casting back to a factor of 10
            s16 mirrorOffset =
                ((mapWidth / 2) - ((R_COMPASS_OFFSET_X / 10) - (mapStartPosX - SCREEN_WIDTH / 2))) * 2 * 10;

            s16 tempX = (s16)compassIcon.pos.x;
            s16 tempZ = (s16)compassIcon.pos.z;
            tempX /= R_COMPASS_SCALE_X * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            tempZ /= R_COMPASS_SCALE_Y;

            s16 tempXOffset =
                R_COMPASS_OFFSET_X + (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? mirrorOffset : 0);
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) != ORIGINAL_LOCATION) {
                if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_LEFT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = leftMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromLeftEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                     (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                    10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_RIGHT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = rightMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                      (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                     10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_NONE) {
                    Matrix_Translate(
                        (tempXOffset + tempX + (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10) / 10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                }
            } else {
                Matrix_Translate(OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX) / 10.0f),
                                 (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ) / 10.0f, 0.0f,
                                 MTXMODE_NEW);
            }
            Matrix_Scale(compassIcon.scale, compassIcon.scale, compassIcon.scale, MTXMODE_APPLY);
            Matrix_RotateX(-1.6f, MTXMODE_APPLY);
            s16 rotation = ((0x7FFF - compassIcon.rot.y) / 0x400) *
                           (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            Matrix_RotateY(rotation / 10.0f, MTXMODE_APPLY);
            gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, compassIcon.color.r, compassIcon.color.g, compassIcon.color.b,
                            255);
            gSPDisplayList(OVERLAY_DISP++, (Gfx*)gCompassArrowDL);
        }

        CLOSE_DISPS(gPlayState->state.gfxCtx);
    });

    // Re-apply remote players' custom skeletons when the local asset-alt prefix changes
    // (e.g. the player switches their own model, which may change which .o2r is open).
    // Also re-broadcasts our own UPDATE_CLIENT_STATE so remote clients pick up our new model.
    COND_HOOK(OnAssetAltChange, isConnected, [&]() {
        if (gPlayState == nullptr) return;
        Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (actor != nullptr) {
            if (actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update) {
                uint32_t clientId = GetDummyPlayerClientId(actor);
                if (clients.contains(clientId)) {
                    AnchorClient& client = clients[clientId];
                    if (!client.customModelFilename.empty()) {
                        bool isAdult = (client.linkAge != LINK_AGE_CHILD);
                        client.customSkeleton = nullptr;
                        client.bakedModel = std::make_unique<SOH::BakedPlayerModel>();
                        SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
                            &((Player*)actor)->skelAnime, isAdult,
                            (uint8_t)client.currentTunic,
                            client.customModelFilename, client.customSkeleton,
                            *client.bakedModel);
                    }
                }
            }
            actor = actor->next;
        }
        SendPacket_UpdateClientState();
    });

    // #endregion
}

// ---------------------------------------------------------------------------
// Anchor::RegisterDirectorHooks — (re-)register the AI Director's per-frame
// tick. Step 1 scaffold per Plans/ai_director_plan.md §9 step 1.
//
// The Director itself lives at AIDirector/Director.cpp as the singleton
// AnchorDirector::Director::Instance(). Tick() body is host-gated and
// no-ops when no descriptors are registered, so this hook is safe to fire
// every frame.
//
// Re-registration on enable/disable mirrors RegisterFollowerHooks: the
// hook ID is a function-scope static, unregistered on entry and re-
// registered only when isConnected. Disconnect path is responsible for
// also clearing any descriptor-private state — currently the registry
// is set up once at construction and persists across connect cycles,
// which is intentional (cooldown ledgers / live counts persist across
// transient disconnects).
// ---------------------------------------------------------------------------
void Anchor::RegisterDirectorHooks(bool isConnected) {
    static HOOK_ID directorHookId = 0;
    GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnGameFrameUpdate>(directorHookId);
    directorHookId = 0;
    if (isConnected) {
        directorHookId = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([]() {
            AnchorDirector::Director::Instance().Tick();
        });
    }
}
