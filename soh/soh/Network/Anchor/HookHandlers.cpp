#include "Anchor.h"
#include "AIDirector/Director.h"      // AnchorDirector::Director::Instance() (Director scaffold step 1)
#include "AIPlayerFollower/Follower.h"      // FollowerFrameContext for the OnGameFrameUpdate wrapper (Phase 1 commit 4)
#include "soh/cvar_prefixes.h"        // CVAR_REMOTE_ANCHOR / CVAR_ENHANCEMENT (Nav system commit 6c)
#include "Common/ActorSyncHelpers.h"  // GetEnemySkelAnime, IsSyncedWorldActor, IsSyncableActor
#include "Common/PlayerLookup.h"      // FindNearestPlayerActor
#include "Common/SceneAuthority.h"    // IsEffectiveHost (Pillar A Phase 1)
#include "Common/ItemEligibility.h"   // CanPlayerCollectItem00 (#193 Phase 0)
#include "Common/PauseLinkBuffer.h"   // Anchor_IsDrawingPauseLink (#182 follow-up)
#include "NPCFollower/FollowerNPC.h" // Anchor_GetCurrentlyDrawingFollowerNpc (NPC color fix)
#include "Common/AINavTest.h"          // Navigation Test Harness — Tick() driver
#include "NPCInvader/Invader.h"          // Anchor_GetCurrentlyDrawingInvader (black-tint color fix)
#include "Common/ActorSyncScope.h"    // ActorSyncScope (Generic NPC State Sync Phase 0/1)
#include "WorldStateSync/WorldStateSync.h"  // Pillar C v1
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
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
// Issue #153 — En_Goroiwa is ACTORCAT_PROP, the first non-ENEMY actor synced.
#include "src/overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
// Boss_Goma — minimal Encounter -> FloorMain bridge (boss-fight trigger sync).
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
// Push-block bidirectional sync — stateFlags & PUSHBLOCK_PUSH detects local push.
#include "src/overlays/actors/ovl_Obj_Oshihiki/z_obj_oshihiki.h"

extern PlayState* gPlayState;
extern MapData* gMapData;

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

// GetEnemySkelAnime, IsSyncedWorldActor, IsSyncableActor moved to
// Common/ActorSyncHelpers.h in #173 Phase 1.
// FindNearestPlayerActor moved to Common/PlayerLookup.h.

// C-callable wrapper so enemy C-code files (e.g. z_en_dekubaba.c) can query the
// nearest player actor without pulling in C++ headers. Returns the nearest
// player-type Actor* (local player or closest DummyPlayer). Safe to call any time
// gPlayState is valid; falls back to local player when Anchor is not active.
extern "C" Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play) {
    return FindNearestPlayerActor(enemy, play);
}

// Hyrule Field Stalchild spawner (En_Encount1, ACTORCAT_PROP) needs to
// round-robin spawn positions across local Link + in-timeline DummyPlayers
// so peers see Stalchildren clustered around their own Link, not just
// host's. Returns the same player list FindNearestPlayerActor walks; the
// per-player budget (2 each) is enforced by the caller.
extern "C" int Anchor_GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount) {
    return GetSyncedPlayerActors(play, outActors, maxCount);
}

// C-callable: returns true if this client is the effective host. Used by
// per-actor C-code files that need to gate AI decisions to host-authoritative
// behaviour (e.g. EnHintnuts's "burrow when any player too close" logic must
// only run on host so peers follow via state-sync rather than independently
// burrowing on local-distance checks).
//
// Phase 1 semantics (global host). For actor-context C code, prefer
// Anchor_IsCurrentRoomHost() below — it picks up Pillar A Phase 2 per-
// room authority so a peer alone in a room becomes that room's host.
extern "C" bool Anchor_IsEffectiveHost(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return true;
    return ::SceneAuthority::IsEffectiveHost();
}

// C-callable: Pillar A Phase 2 — true when this client is the room host
// for its current (sceneNum, roomNum, timeline). Used by actor C code
// that should be running its host-authoritative logic when this client
// is alone in a room (e.g. EnHintnuts's burrow-when-too-close gate),
// independent of the global effective host. Falls back to global host
// when Anchor isn't active or gPlayState is null.
extern "C" bool Anchor_IsCurrentRoomHost(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return true;
    return ::SceneAuthority::IsMyCurrentRoomHost();
}


// C-callable: returns true if any DummyPlayer (remote player) is currently
// standing on top of the given DynaPolyActor's footprint. Used by
// `DynaPolyActor_IsPlayerOnTop` callers (Obj_Lift, etc.) to make
// step-on-top triggers multiplayer-aware. Local player's IsPlayerOnTop
// flag continues to come from the engine's own dyna physics; this helper
// is a strict OR-fallthrough that fires only when the local check missed.
//
// Geometry: XZ proximity ≤ 120 units (matches Obj_Lift's 3×3 fragment
// grid at 120-unit spacing) AND Y delta in [-10, +80] (player is at or
// just above the platform top).
extern "C" bool Anchor_IsAnyPeerOnDyna(Actor* dynaActor) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr || dynaActor == nullptr) return false;

    const f32 kXZRangeSq = 120.0f * 120.0f;
    const f32 kYMin      = -10.0f;
    const f32 kYMax      =  80.0f;

    Actor* npc = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            f32 dx = npc->world.pos.x - dynaActor->world.pos.x;
            f32 dz = npc->world.pos.z - dynaActor->world.pos.z;
            f32 dy = npc->world.pos.y - dynaActor->world.pos.y;
            if (dx * dx + dz * dz < kXZRangeSq && dy > kYMin && dy < kYMax) {
                return true;
            }
        }
        npc = npc->next;
    }
    return false;
}

// C-callable: returns true when a Karebaba's natural death cycle is running on this
// (non-host) client so that its stick drop should be suppressed (no duplicate item).
// Called from EnKarebaba_DeadItemDrop in z_en_karebaba.c.
extern "C" bool Anchor_ShouldSuppressKarebabaDrop(Actor* actor) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    // Host gate — see Anchor_ShouldSuppressDekubabaDrop for the field-
    // test rationale. Host is the canonical drop source; suppressing on
    // host eliminates the OnActorSpawn(EN_ITEM00) broadcast entirely.
    if (::SceneAuthority::IsMyCurrentRoomHost()) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    // OR with networkDriveDying — engages the moment ENEMY_STATE
    // carries health<=0 from host, before the explicit ENEMY_DEFEATED
    // packet arrives. Closes the peer-side dual-drop race documented
    // in EnemyNetId::networkDriveDying (Anchor.h).
    return ext != nullptr &&
           (ext->networkDriveDying ||
            EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase));
}

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
}  // namespace

// #135 / en_dekunuts_sync_plan.md §6 — suppresses Mad Scrub's
// Item_DropCollectibleRandom on a non-host receiver during the natural
// death cycle (after BossGoma_SetupDyingNet equivalent triggers).
// Receiver is replaying host's already-broadcast death; host's drop
// already came through the standard pipeline.
extern "C" bool Anchor_ShouldSuppressDekunutsDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    // Host is the canonical drop source — its Item_DropCollectible call
    // fires OnActorSpawn(EN_ITEM00) which broadcasts ITEM_DROP_SYNC.
    // Suppressing on host eliminates the broadcast entirely. The
    // suppressor's whole purpose is "stop peer from spawning a duplicate
    // local drop alongside the broadcast"; host has no duplicate to
    // suppress.
    //
    // Field log 2026-05-07 (Inside Deku Tree, b7025ac20): peer killed
    // dekubaba via DAMAGE_ENEMY routing; host's local OnEnemyDefeat
    // hadn't fired yet by the time peer's "Non-host route-to-host"
    // defeat packet arrived. Host took the `triggering natural death
    // cycle` branch (SetupDyingNet on host's still-alive actor), which
    // wrote phase=DyingByNetwork on host. The suppressor's phase check
    // then returned true on host, killing host's vanilla ShrinkDie drop
    // call and its OnActorSpawn(EN_ITEM00) broadcast — no nuts on
    // either client. The host gate keeps host's drop path open
    // regardless of how phase got written.
    if (::SceneAuthority::IsMyCurrentRoomHost()) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    // OR with networkDriveDying — engages the moment ENEMY_STATE
    // carries health<=0 from host, before the explicit ENEMY_DEFEATED
    // packet arrives. Closes the peer-side dual-drop race documented
    // in EnemyNetId::networkDriveDying (Anchor.h).
    return ext != nullptr &&
           (ext->networkDriveDying ||
            EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase));
}

// En_Hintnuts (Inside Deku Tree Compound Room) — suppresses the
// recovery-heart drop in EnHintnuts_SetupLeave on a non-host receiver
// when the host already broadcast the kill. Mirrors the Dekunuts drop-
// suppression pattern. Trigger condition: actor's lifecycle phase
// indicates a network-driven death-cycle is in progress.
//
// Note: Hintnuts has no health-based death (no DyingByLocal phase via
// damage). The Leave path is reached after the Talk dialog completes,
// which is locally driven on each client. The suppression here is
// defensive — if a future change routes Leave through a network-defeat
// flow, the guard prevents double-drops. Today this is effectively a
// no-op because both clients run their local Talk→Leave naturally.
extern "C" bool Anchor_ShouldSuppressHintnutsDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr) return false;
    // Two-branch decision after the 2026-05-07 host-gate fix:
    //   - Host: always run the drop. Host's Item_DropCollectible
    //     fires OnActorSpawn(EN_ITEM00) → ITEM_DROP_SYNC broadcast,
    //     and peer respawns the heart via the receive path.
    //   - Peer: always suppress. Defense-in-depth against the
    //     logs-216 actor-flood crash — any code path that reached
    //     SetupLeave on peer (DIALOG_END routing prevents the
    //     Talk→Leave path today, but future paths could) would
    //     dup the heart per call and overflow MISC actor list.
    return !::SceneAuthority::IsMyCurrentRoomHost();
}

// Hintnut state machine is host-authoritative (room host runs the AI;
// peers receive ENEMY_STATE and apply via ApplyNetState). Peers must
// not run HitByScrubProjectile1+2 locally on nutball impact — would
// cause the local actor to transition out of sync with host's
// authoritative state. Returns true on peers (non-room-hosts) so the
// local hit gets routed through PROJECTILE_HIT_ENEMY instead.
//
// Single-player and offline branches: returns false (no Anchor, no
// connection, or no scope info) so vanilla behavior is preserved.
extern "C" bool Anchor_ShouldSuppressHintnutsLocalAI(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr) return false;
    // Peer = "I'm not the room host of MY current (sceneNum, roomNum,
    // timeline)". The actor is in my actor list so its scope is mine.
    return !::SceneAuthority::IsMyCurrentRoomHost();
}

// Sender wrapper — routes a local nutball-on-hintnut collision to the
// room host. Host applies HitByScrubProjectile1+2 on its own copy of
// the actor and broadcasts the resulting state via ENEMY_STATE.
extern "C" void Anchor_NotifyProjectileHitEnemy(Actor* targetActor, s16 projectileActorId) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;  // unsynced actor — silently drop
    Anchor::Instance->SendPacket_ProjectileHitEnemy(ext->netId, projectileActorId);
}

// Sender wrapper — peer's Run actionFunc calls this when its local
// Link initiates dialog with the hintnut. Host runs the canonical
// SetupTalk on its local actor and broadcasts state=Talk so peer's
// rx-driver applies it (instead of host's stale Run state reverting
// peer back). See Packets/TalkRequest.cpp.
extern "C" void Anchor_NotifyTalkRequest(Actor* targetActor) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_TalkRequest(ext->netId);
}

// Sender wrapper — peer's Talk actionFunc calls this when its local
// Message_GetState returns TEXT_STATE_EVENT (dialog closed on peer).
// Host runs the canonical SetupLeave on its local actor and
// broadcasts state=Leave back via ENEMY_STATE so peer's rx-driver
// applies it (instead of peer's local SetupLeave running and
// spawning hearts every 50ms). See Packets/DialogEnd.cpp.
extern "C" void Anchor_NotifyDialogEnd(Actor* targetActor) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_DialogEnd(ext->netId);
}

// Sender wrapper — peer's BossGoma_Encounter case 3 calls this when
// its local actor.projectedPos check passes (peer is looking up at
// Goma during the intro). Host receives and increments its local
// Goma's lookedAtFrames so the fight progresses regardless of which
// player triggered the look. See Packets/BossGomaLookedAt.cpp + #67.
extern "C" void Anchor_NotifyBossGomaLookedAt(Actor* boss) {
    if (boss == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;  // host's own check fires the local path
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(boss);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_BossGomaLookedAt(ext->netId);
}

// Sender wrapper — dialog client's EnMd_BlockPath calls this when its
// transition to Walk fires for the post-Deku-Tree confrontation
// (DEKU_TREE_DEAD + !SPOKE + KOKIRI). Broadcasts to team so peers
// can transition their local Mido through the same Walk path and play
// the walk-away cinematic instead of despawning abruptly when the SPOKE
// flag syncs. See Packets/MidoPostDekuLeave.cpp + #184 follow-up.
extern "C" void Anchor_NotifyMidoPostDekuLeave(void) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    Anchor::Instance->SendPacket_MidoPostDekuLeave();
}

// #191 — Anchor-aware override for Message_ShouldAdvance during a
// cutscene-internal textbox. C-callable from z_message_PAL.c.
//
// Returns 1 when the local message system should advance THIS frame
// (i.e., return true from Message_ShouldAdvance):
//   - The vote-completion broadcast was just received for the current
//     textId (one-shot — consumed on read).
//
// Returns 0 when the local press should NOT immediately advance the
// textbox; it has been forwarded to the host as a vote and the actual
// advance fires when host's CUTSCENE_TEXT_ADVANCED arrives.
//
// `wasLocalPressDetected` is the OR of (BTN_A | BTN_B | BTN_CUP) press
// computed by the caller — when true, we send a CUTSCENE_TEXT_ADVANCE
// vote packet to the host. When the local press is for a different
// textId than the most recent broadcast, the broadcast flag clears so
// future presses for the new textId follow the vote pattern.
//
// `currentTextId` is the textbox the caller wants to advance.
//
// Single-player or disconnected: returns wasLocalPressDetected
// unchanged — vanilla parity.
extern "C" int Anchor_ShouldAdvanceCutsceneTextLocal(int wasLocalPressDetected,
                                                     unsigned currentTextId) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        return wasLocalPressDetected ? 1 : 0;
    }

    // Consume the broadcast flag if it matches the current textbox.
    // Edge case: matched broadcast for a previous textId stays
    // consumed when we move to a new textId — the new textId's
    // vote count starts fresh.
    if (Anchor::Instance->cutsceneTextAdvanceConsumed &&
        Anchor::Instance->cutsceneTextAdvanceConsumedTextId == (uint16_t)currentTextId) {
        Anchor::Instance->cutsceneTextAdvanceConsumed = false;
        return 1;
    }
    if (Anchor::Instance->cutsceneTextAdvanceConsumed &&
        Anchor::Instance->cutsceneTextAdvanceConsumedTextId != (uint16_t)currentTextId) {
        // Stale broadcast for a different textbox — drop.
        Anchor::Instance->cutsceneTextAdvanceConsumed = false;
    }

    // Multi-player dialogue redesign (#191 follow-up) —
    // "alone in cutscene" detection.
    //
    // Vote-and-countdown semantics only make sense when MULTIPLE team
    // members share the same cutscene (e.g. the post-Goma sequence
    // both clients see). When the local player is the only client in
    // a cutscene state — typical for NPC dialog and per-player
    // scripted scenes like the Great Deku Tree opening cutscene where
    // only the player who triggered it sees the textbox — the
    // countdown becomes pure friction (the timer must elapse before
    // the local player's button press takes effect).
    //
    // Walk online team members in the same scene + timeline. If none
    // is in cutscene state with us, treat as solo and return the
    // vanilla input directly. Otherwise route through the voting
    // flow.
    //
    // The csCtxState field on AnchorClient is updated from PLAYER_UPDATE
    // (60 pps), so the detection picks up peer transitions into/out of
    // the cutscene within ~16 ms. Pre-update peers default to
    // CS_STATE_IDLE — safe (treated as not-in-cutscene).
    if (gPlayState != nullptr) {
        bool peerInCutscene = false;
        int16_t myScene    = (int16_t)gPlayState->sceneNum;
        uint8_t myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
        std::string myTeamId =
            CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        for (auto& [cid, client] : Anchor::Instance->clients) {
            if (client.self) continue;
            if (!client.online) continue;
            if (!client.isSaveLoaded) continue;
            if (client.sceneNum != myScene) continue;
            if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
            if (client.teamId != myTeamId) continue;
            if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;
            peerInCutscene = true;
            break;
        }
        if (!peerInCutscene) {
            // Solo cutscene — vanilla parity for input.
            //
            // Solo idle auto-advance (#191 follow-up): if no input
            // for `kSoloDialogIdleAdvanceMs` (default 10s, tunable
            // via gAnchor.SoloDialogIdleAutoAdvanceMs), force-advance
            // so AFK / accessibility players don't get stuck on a
            // single textbox indefinitely. Resets on textId edge
            // (new textbox starts fresh) and on input (player is
            // engaged again).
            //
            // File-scope statics suffice since this is per-Anchor
            // state on the game thread (Message_ShouldAdvance is
            // called from the game tick).
            static int64_t  s_soloIdleStartMs    = 0;
            static uint16_t s_soloIdleLastTextId = 0;

            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t idleThresholdMs = (int64_t)CVarGetInteger(
                CVAR_REMOTE_ANCHOR("SoloDialogIdleAutoAdvanceMs"), 10000);

            if (s_soloIdleLastTextId != (uint16_t)currentTextId) {
                s_soloIdleLastTextId = (uint16_t)currentTextId;
                s_soloIdleStartMs = nowMs;
            }
            if (wasLocalPressDetected) {
                s_soloIdleStartMs = nowMs;
                return 1;
            }
            if (idleThresholdMs > 0 &&
                nowMs - s_soloIdleStartMs >= idleThresholdMs) {
                SPDLOG_INFO("[CutsceneText] Solo idle auto-advance after {} ms (textId=0x{:04X})",
                            (long long)idleThresholdMs, (unsigned)currentTextId);
                s_soloIdleStartMs = nowMs;  // prevent immediate re-fire next frame
                return 1;
            }
            return 0;
        }
    }

    // Local press → forward to host as a vote.
    if (wasLocalPressDetected) {
        Anchor::Instance->SendPacket_CutsceneTextAdvance((uint16_t)currentTextId);
    }

    // Don't immediately advance — wait for the host's broadcast.
    return 0;
}

// Receive-side accessor — case 3 of BossGoma_Encounter calls this
// each frame. Returns 1 (and clears the flag) if a BOSS_GOMA_LOOKED_AT
// has been received during this encounter; the caller then fires
// BossGoma_SetupEncounterState4 immediately, skipping the local
// 15-frame frustum-check accumulator (which doesn't trip on host
// because host's camera isn't pointing at Goma in MP). Returns 0
// for single-player / disconnected / non-Boss_Goma callers, in
// which case case 3 falls through to its vanilla logic.
extern "C" int Anchor_BossGomaConsumePeerSignaled(Actor* boss) {
    if (boss == nullptr) return 0;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return 0;
    EnemyNetId* ext = const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(boss));
    if (ext == nullptr) return 0;
    if (!ext->bossGomaPeerSignaled) return 0;
    ext->bossGomaPeerSignaled = false;
    SPDLOG_INFO("[BossGoma] case-3 consumed peer-signal flag → firing eye-roll cinematic");
    return 1;
}

// #90 / en_st_sync_plan_v2.md §5 — same predicate shape as the
// Dekunuts suppressor, applied to En_St's drop site (line 996).
extern "C" bool Anchor_ShouldSuppressEnStDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    // Host is the canonical drop source — its Item_DropCollectible call
    // fires OnActorSpawn(EN_ITEM00) which broadcasts ITEM_DROP_SYNC.
    // Suppressing on host eliminates the broadcast entirely. The
    // suppressor's whole purpose is "stop peer from spawning a duplicate
    // local drop alongside the broadcast"; host has no duplicate to
    // suppress.
    //
    // Field log 2026-05-07 (Inside Deku Tree, b7025ac20): peer killed
    // dekubaba via DAMAGE_ENEMY routing; host's local OnEnemyDefeat
    // hadn't fired yet by the time peer's "Non-host route-to-host"
    // defeat packet arrived. Host took the `triggering natural death
    // cycle` branch (SetupDyingNet on host's still-alive actor), which
    // wrote phase=DyingByNetwork on host. The suppressor's phase check
    // then returned true on host, killing host's vanilla ShrinkDie drop
    // call and its OnActorSpawn(EN_ITEM00) broadcast — no nuts on
    // either client. The host gate keeps host's drop path open
    // regardless of how phase got written.
    if (::SceneAuthority::IsMyCurrentRoomHost()) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    // OR with networkDriveDying — engages the moment ENEMY_STATE
    // carries health<=0 from host, before the explicit ENEMY_DEFEATED
    // packet arrives. Closes the peer-side dual-drop race documented
    // in EnemyNetId::networkDriveDying (Anchor.h).
    return ext != nullptr &&
           (ext->networkDriveDying ||
            EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase));
}

// #193 field-test fix — same predicate shape as En_St / En_Sw /
// En_Dekunuts suppressors, applied to En_Dekubaba's ShrinkDie drop
// site. Field log 2026-05-06: Dekubaba killed by peer (race B routes
// kill to host → host fires drop + broadcasts) AND peer's
// SetupDyingNet → ShrinkDie path also called Item_DropCollectible
// locally, producing duplicate drops. This guard suppresses the
// peer-side natural-cycle drop call when phase indicates the actor
// is dying via a network-driven path (DyingByNetwork /
// AwaitingDeadItemDrop).
extern "C" bool Anchor_ShouldSuppressDekubabaDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    // Host is the canonical drop source — its Item_DropCollectible call
    // fires OnActorSpawn(EN_ITEM00) which broadcasts ITEM_DROP_SYNC.
    // Suppressing on host eliminates the broadcast entirely. The
    // suppressor's whole purpose is "stop peer from spawning a duplicate
    // local drop alongside the broadcast"; host has no duplicate to
    // suppress.
    //
    // Field log 2026-05-07 (Inside Deku Tree, b7025ac20): peer killed
    // dekubaba via DAMAGE_ENEMY routing; host's local OnEnemyDefeat
    // hadn't fired yet by the time peer's "Non-host route-to-host"
    // defeat packet arrived. Host took the `triggering natural death
    // cycle` branch (SetupDyingNet on host's still-alive actor), which
    // wrote phase=DyingByNetwork on host. The suppressor's phase check
    // then returned true on host, killing host's vanilla ShrinkDie drop
    // call and its OnActorSpawn(EN_ITEM00) broadcast — no nuts on
    // either client. The host gate keeps host's drop path open
    // regardless of how phase got written.
    if (::SceneAuthority::IsMyCurrentRoomHost()) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    // OR with networkDriveDying — engages the moment ENEMY_STATE
    // carries health<=0 from host, before the explicit ENEMY_DEFEATED
    // packet arrives. Closes the peer-side dual-drop race documented
    // in EnemyNetId::networkDriveDying (Anchor.h).
    return ext != nullptr &&
           (ext->networkDriveDying ||
            EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase));
}

// #148 / en_sw_sync_plan.md §5 — same predicate shape, applied to
// En_Sw's combat-variant drop site (line 686). Gold-variant En_Si
// spawn deliberately NOT suppressed (cooperative collectible Design A).
extern "C" bool Anchor_ShouldSuppressEnSwDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    // Host is the canonical drop source — its Item_DropCollectible call
    // fires OnActorSpawn(EN_ITEM00) which broadcasts ITEM_DROP_SYNC.
    // Suppressing on host eliminates the broadcast entirely. The
    // suppressor's whole purpose is "stop peer from spawning a duplicate
    // local drop alongside the broadcast"; host has no duplicate to
    // suppress.
    //
    // Field log 2026-05-07 (Inside Deku Tree, b7025ac20): peer killed
    // dekubaba via DAMAGE_ENEMY routing; host's local OnEnemyDefeat
    // hadn't fired yet by the time peer's "Non-host route-to-host"
    // defeat packet arrived. Host took the `triggering natural death
    // cycle` branch (SetupDyingNet on host's still-alive actor), which
    // wrote phase=DyingByNetwork on host. The suppressor's phase check
    // then returned true on host, killing host's vanilla ShrinkDie drop
    // call and its OnActorSpawn(EN_ITEM00) broadcast — no nuts on
    // either client. The host gate keeps host's drop path open
    // regardless of how phase got written.
    if (::SceneAuthority::IsMyCurrentRoomHost()) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    // OR with networkDriveDying — engages the moment ENEMY_STATE
    // carries health<=0 from host, before the explicit ENEMY_DEFEATED
    // packet arrives. Closes the peer-side dual-drop race documented
    // in EnemyNetId::networkDriveDying (Anchor.h).
    return ext != nullptr &&
           (ext->networkDriveDying ||
            EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase));
}

// C-callable: non-host tells host that its local Link was just hit by this enemy
// so the host can reverse/update its authoritative copy (En_Goroiwa, issue #153
// Phase 2). No-op when Anchor is disconnected, when this client is the room
// host (it would handle the hit locally), or when the actor lacks an EnemyNetId
// extension (never reached the sync pipeline). Uses Pillar A Phase 2 per-room
// authority so the gate stays correct when the original room owner is offline.
extern "C" void Anchor_NotifyEnemyHitPlayer(Actor* actor) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_EnemyHitPlayer(ext->netId);
}

// #193 Phase 2 — Item-drop killer-attribution shim.
//
// Game thread is single-threaded so file-scope statics suffice. Begin
// records the killerClientId before vanilla `Item_DropCollectible*`
// fires `Actor_Spawn(ACTOR_EN_ITEM00)`. End clears.
//
// Killer resolution:
//   - Item_DropCollectibleRandom(play, fromActor, ...): if fromActor's
//     EnemyNetId has a recorded damager (host-side bookkeeping populated
//     by ENEMY_DEFEATED arrival from a peer), use that. Else fall back
//     to the local client's id. Covers both "host kills a peer's enemy"
//     (rare; host's own kill) and "peer kills via DAMAGE_ENEMY routed
//     to host" (the dominant MP path — Damager map is populated).
//   - Item_DropCollectible / Item_DropCollectible2 (no fromActor): caller
//     uses the no-arg form, killer = local client. Phase 4 will refine
//     for env-actor drops if needed.
//
// State scope: only one `Item_DropCollectible*` invocation can be active
// at a time on the game thread, so a single static suffices. Depth
// counter handles nested calls (Phase 2 + 4): when
// `Item_DropCollectibleRandom` fires its inner-loop recursive
// `Item_DropCollectible` (z_en_item00.c:1797/1799), the outer Begin's
// killer attribution must persist through the inner Begin/End — depth
// counter ensures only the outermost Begin sets state and only the
// outermost End clears it.
static uint32_t g_pendingItemDropKillerClientId = 0;
static int64_t  g_pendingItemDropSpawnTimeMs   = 0;
static int      g_pendingItemDropDepth         = 0;

// Receive-side gate: set true while HandlePacket_ItemDropSync is calling
// Actor_Spawn so the OnActorSpawn ACTOR_EN_ITEM00 hook knows not to
// re-broadcast the drop (it's already a network drop). Mirrors the
// `isSpawningNetworkActor` pattern from ENEMY_SPAWN.
static bool     g_isSpawningNetworkItemDrop     = false;
static uint32_t g_pendingNetworkItemDropNetId   = 0;

extern "C" void Anchor_BeginItemDrop(Actor* fromActor) {
    // Inner / nested call: keep outer's killer attribution intact.
    if (g_pendingItemDropDepth++ > 0) {
        return;
    }
    g_pendingItemDropSpawnTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!Anchor::Instance) {
        g_pendingItemDropKillerClientId = 0;
        return;
    }
    // Default: local client owns the kill (host's own kill OR an env actor
    // cut by local Link).
    g_pendingItemDropKillerClientId = Anchor::Instance->ownClientId;
    if (fromActor != nullptr) {
        const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(fromActor);
        if (ext != nullptr) {
            uint32_t damager = EnemyStateSync::HostBookkeeping::Instance().LookupDamager(ext->netId);
            if (damager != 0) {
                g_pendingItemDropKillerClientId = damager;
            }
        }
    }
}

extern "C" void Anchor_EndItemDrop(void) {
    if (g_pendingItemDropDepth > 0) {
        --g_pendingItemDropDepth;
    }
    // Only clear at the outermost End.
    if (g_pendingItemDropDepth == 0) {
        g_pendingItemDropKillerClientId = 0;
        g_pendingItemDropSpawnTimeMs = 0;
    }
}

// #193 Phase 4 v2 — explicit-killer override. Used by
// `HandlePacket_EnvActorDrop` to attribute the host-side
// `Item_DropCollectible*` to the peer who cut the grass (peer's
// clientId carried in the packet's relay-injected `clientId` field).
// Pairs with the standard `Anchor_EndItemDrop`.
extern "C" void Anchor_BeginItemDropForKiller(uint32_t killerClientId) {
    if (g_pendingItemDropDepth++ > 0) {
        return;  // nested; outer scope wins
    }
    g_pendingItemDropSpawnTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_pendingItemDropKillerClientId = killerClientId;
}

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
extern "C" void Anchor_DropCollectibleEnvActor(PlayState* play, Actor* envActor,
                                                Vec3f* pos, s16 params) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        Item_DropCollectible(play, pos, params);
        return;
    }
    if (::SceneAuthority::IsMyCurrentRoomHost()) {
        Item_DropCollectible(play, pos, params);
        return;
    }
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(envActor);
    if (ext == nullptr || ext->netId == 0) {
        Item_DropCollectible(play, pos, params);
        return;
    }
    // Peer with a synced env actor: notify host, suppress local drop.
    Anchor::Instance->SendPacket_EnvActorDrop(ext->netId, params, /*forRandom=*/0, *pos);
}

// #193 Phase 4 v2 — sibling for `Item_DropCollectibleRandom` from
// env-actor sites (En_Kusa TYPE_0/TYPE_2 path passes a "drop group"
// param shifted up by 4, NOT a specific ITEM00_*). Same behavioural
// matrix as Anchor_DropCollectibleEnvActor; routes the random param
// through `dropParamForRandom` so the host re-dispatches via
// `Item_DropCollectibleRandom`.
extern "C" void Anchor_DropCollectibleRandomEnvActor(PlayState* play, Actor* envActor,
                                                     Vec3f* pos, s16 dropGroupParams) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    if (::SceneAuthority::IsMyCurrentRoomHost()) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(envActor);
    if (ext == nullptr || ext->netId == 0) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    Anchor::Instance->SendPacket_EnvActorDrop(ext->netId, /*dropParam=*/0,
                                               dropGroupParams, *pos);
}

// Receive-side helper called from HandlePacket_ItemDropSync /
// HandlePacket_ItemDropSnapshot to bracket the local Actor_Spawn so
// the OnActorSpawn EN_ITEM00 hook stamps the extension with the
// host's broadcast netId/killer/spawnTimeMs and suppresses
// re-broadcast.
//
// Increments the shim's depth counter so any inner
// `Anchor_BeginItemDrop` call (e.g., from a vanilla
// `Item_DropCollectible` invocation made during the receive-side
// re-spawn for animation-replication) sees depth>0 and bails out
// without overwriting the broadcast-supplied killer/spawnTime state.
// Pairs symmetrically with `Anchor_EndNetworkItemDropSpawn`.
void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                       int64_t spawnTimeMs) {
    g_isSpawningNetworkItemDrop      = true;
    g_pendingNetworkItemDropNetId    = netId;
    g_pendingItemDropKillerClientId  = killerClientId;
    g_pendingItemDropSpawnTimeMs     = spawnTimeMs;
    g_pendingItemDropDepth++;
}

void Anchor_EndNetworkItemDropSpawn(void) {
    if (g_pendingItemDropDepth > 0) {
        --g_pendingItemDropDepth;
    }
    g_isSpawningNetworkItemDrop      = false;
    g_pendingNetworkItemDropNetId    = 0;
    if (g_pendingItemDropDepth == 0) {
        g_pendingItemDropKillerClientId  = 0;
        g_pendingItemDropSpawnTimeMs     = 0;
    }
}

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
            (void)sceneNum;
            Anchor_LocalPlayerFaceSwapResetOnSceneTransition();
            Anchor_FollowerNpcDrawStateResetOnSceneTransition();
            Anchor_InvaderDrawStateResetOnSceneTransition();
        });

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
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
                    bool anyPeerInPrevScene = false;
                    if (Anchor::Instance != nullptr) {
                        for (auto& [otherId, other] : Anchor::Instance->clients) {
                            if (!other.online || !other.isSaveLoaded || other.self) continue;
                            if (other.sceneNum == sLastHostSceneEntered) {
                                anyPeerInPrevScene = true;
                                break;
                            }
                        }
                    }
                    if (!anyPeerInPrevScene && Anchor::Instance != nullptr) {
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

        }
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
            }
            prevTransitionTrigger = curTrigger;
        } else {
            prevTransitionTrigger = TRANS_TRIGGER_OFF;
        }

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

        // #135 / en_dekunuts_sync_plan.md §3 step 1 — DEKUNUTS_FLOWER child
        // shares its parent Mad Scrub's home.pos and actor->id, so the
        // deterministic netId scheme would collide with the parent. The
        // flower has no actionFunc (Update early-returns) and no collider
        // — nothing to sync. Skip netId assignment entirely so the parent
        // owns the netId unambiguously.
        if (actor->id == ACTOR_EN_DEKUNUTS && actor->params == /*DEKUNUTS_FLOWER*/ 10) {
            return;
        }

        // En_Hintnuts (Inside Deku Tree Compound Room) — same flower
        // child pattern as Dekunuts. Parent (params 1-3 or 0) spawns a
        // child with params=0xA (line 100 of z_en_hintnuts.c). The child
        // is a static decorative flower with no actionFunc, no collider,
        // and identical home.pos/id to the parent. Skip netId assignment
        // for the flower so parent owns the netId. Without this skip,
        // logs show the same netId assigned twice — collision.
        if (actor->id == ACTOR_EN_HINTNUTS && (actor->params & 0xFF) == 0xA) {
            return;
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
            bool anyPeerInScene = false;
            for (auto& [clientId, client] : Anchor::Instance->clients) {
                if (!client.self && client.online && client.isSaveLoaded &&
                    client.sceneNum == targetScene) {
                    anyPeerInScene = true;
                    break;
                }
            }
            if (!anyPeerInScene) {
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
            } else if (!isSpawningNetworkActor) {
                // Non-host: kill locally-generated dynamic actors immediately.
                // The host's canonical copy arrives via ENEMY_SPAWN and is spawned
                // by HandlePacket_EnemySpawn (which sets isSpawningNetworkActor).
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
        if (::SceneAuthority::IsMyCurrentRoomHost()) {
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

        // #193 instrumentation 2026-05-07 — diagnose mystery STICK
        // spawn origin. Log every EN_ITEM00 spawn with resolved type +
        // raw params + the depth/sequence flags so it can be matched
        // against the [ItemDropTrace] entries from the three vanilla
        // drop functions (z_en_item00.c). For unknown-source spawns
        // (no matching trace line in the same frame), the spawn came
        // from an Actor_Spawn(ACTOR_EN_ITEM00) call outside the
        // wrapped paths — that's the path we're hunting. Drop after
        // diagnosis.
        SPDLOG_INFO("[ItemDropTrace] OnActorSpawn EN_ITEM00 params=0x{:04X} resolvedType=0x{:02X} "
                    "pos=({:.0f},{:.0f},{:.0f}) g_isSpawningNetworkItemDrop={} g_pendingItemDropDepth={}",
                    (uint16_t)actor->params, (int)resolvedType,
                    actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                    (int)g_isSpawningNetworkItemDrop,
                    g_pendingItemDropDepth);

        // Receive-side: extension stamping only. Skip broadcast.
        if (g_isSpawningNetworkItemDrop) {
            ItemDropNetId ext;
            ext.netId           = g_pendingNetworkItemDropNetId;
            ext.killerClientId  = g_pendingItemDropKillerClientId;
            ext.spawnTimeMs     = g_pendingItemDropSpawnTimeMs;
            ext.isFromBroadcast = true;
            ObjectExtension::GetInstance().Set<ItemDropNetId>(actor, std::move(ext));
            SPDLOG_DEBUG("[ItemDropSync] Network drop: stamped netId={} killer={} type=0x{:02X}",
                         g_pendingNetworkItemDropNetId, g_pendingItemDropKillerClientId,
                         (int)resolvedType);
            return;
        }

        // Host-side: broadcast iff this is the room host and the type
        // is on the transient allowlist (Q7).
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
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

        // Stamp local extension first so the pickup gate can read it
        // even on the host. isFromBroadcast=false marks "local drop".
        ItemDropNetId ext;
        ext.netId           = itemNetId;
        ext.killerClientId  = killerClientId;
        ext.spawnTimeMs     = spawnTimeMs;
        ext.isFromBroadcast = false;
        ObjectExtension::GetInstance().Set<ItemDropNetId>(actor, std::move(ext));

        // Broadcast.
        Anchor::Instance->SendPacket_ItemDropSync(itemNetId, (u8)resolvedType,
                                                   actor->world.pos,
                                                   killerClientId, spawnTimeMs);
        SPDLOG_INFO("[ItemDropSync] Host broadcast netId={} type=0x{:02X} pos=({:.0f},{:.0f},{:.0f}) "
                    "killer={} spawnTimeMs={}",
                    itemNetId, (int)resolvedType,
                    actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                    killerClientId, (long long)spawnTimeMs);
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
        // Detection uses ObjOshihiki's stateFlags rather than motion-delta
        // so we never falsely classify network-applied position writes as
        // "local motion" (which would echo). PUSHBLOCK_PUSH is set every
        // frame in ObjOshihiki_Push (z_obj_oshihiki.c:564); PUSHBLOCK_FALL
        // covers the post-edge fall case where the block tips off a ledge.
        //
        // Bg_Heavy_Block (Golden Gauntlets pillar) and En_Ishi (lift/throw
        // rocks) remain in IsSyncedWorldActor and ride the standard host-
        // broadcast path. Bidirectional support for them needs separate
        // motion-detection because their action funcs are static and
        // there's no public state flag — deferred until those actors
        // appear on the demo path.
        if (actor->id == ACTOR_OBJ_OSHIHIKI) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext == nullptr) {
                return;
            }
            ObjOshihiki* block = (ObjOshihiki*)actor;
            const bool isLocallyMoving =
                (block->stateFlags & (PUSHBLOCK_PUSH | PUSHBLOCK_FALL)) != 0;
            if (isLocallyMoving) {
                // Local actor is the pusher — broadcast to all peers.
                // ObjOshihiki_Push fires NA_SE_EV_ROCK_SLIDE-SFX_FLAG every
                // frame locally (z_obj_oshihiki.c:598), so the local audio
                // is already correct. Receivers play it via
                // HandlePacket_EnemyUpdate's pos-delta detector.
                SendPacket_EnemyUpdate(ext->netId, actor);
            }
            // No re-apply path here — HandlePacket_EnemyUpdate writes
            // actor->world.pos directly when packets arrive (now
            // unconditionally for push blocks, including on the host).
            return;
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
            if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) && forwardDamage > 0) {
                // #174/#175: forward damageEffect (set on enemy by collision damage-table
                // lookup) and atHitEffect (set on the player by CollisionCheck_SetATvsAC
                // when the player's AT element lands a hit). Many OoT enemies branch on
                // these fields to decide whether Actor_ApplyDamage actually fires; sending
                // only `damage` left those enemies silently ignoring the synthetic hit.
                Player* localPlayer = GET_PLAYER(gPlayState);
                u8 atHitEffect = (localPlayer != nullptr) ? localPlayer->actor.colChkInfo.atHitEffect : 0;
                SendPacket_DamageEnemy(ext->netId, forwardDamage,
                                       actor->colChkInfo.damageEffect, atHitEffect);
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
                const bool isAnimationDrivenPos = (actor->id == ACTOR_EN_DEKUBABA ||
                                                   actor->id == ACTOR_EN_KAREBABA ||
                                                   actor->id == ACTOR_EN_NUTSBALL);
                if (!isAnimationDrivenPos && !arrowPinned) {
                    actor->world.pos = ext->netPos;
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
                s16 curState = karebabaLocalState; // pre-computed above
                if (curState != ext->netStateIndex && ext->netStateIndex != 7) {
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
                            EnKarebaba_ApplyNetState((EnKarebaba*)actor, ext->netStateIndex, ext->netActorParams);
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
                if (curState != ext->netStateIndex) {
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
                if (curState != ext->netStateIndex) {
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
                if (curState != ext->netStateIndex) {
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
                    !deathStateNet) {
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
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive)) {
                    if (ShouldLogStateChange(ext->netId, curState, ext->netStateIndex, false)) {
                        SPDLOG_INFO("[EnHintnuts] rx netId={} apply {}→{}",
                                    ext->netId, (int)curState, (int)ext->netStateIndex);
                    }
                    EnHintnuts_ApplyNetState(h, gPlayState, ext->netStateIndex);
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
                if (curState != ext->netStateIndex && !(netIsDormant && localIsActive)) {
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
                if (curState != ext->netStateIndex && !blockTransition && !deathStateNet) {
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
            if (!IsSyncedBossActor(actor->id)) {
                EnemyNetId* ext = const_cast<EnemyNetId*>(
                    ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
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

        // Only overwrite when a DummyPlayer is closer. If the local player is
        // nearest, the automatic calculation (z_actor.c:2665-2669) is already
        // correct and we leave the fields untouched.
        if (nearest != &localPlayer->actor) {
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
            EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByLocal);
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
        SendPacket_EnemyDefeated(ext->netId);

        // AI Director: notify removal for director-spawned enemies. Early-
        // exits inside OnEnemyRemoved if this netId isn't in the Director's
        // registry, so the cost for non-director-spawned kills is one hash
        // lookup. Cause is always Kill from this path; descriptors that
        // need other DefeatCauses (Leash, SceneExit, etc.) trigger those
        // via their own paths before calling Actor_Kill.
        //
        // Step 6: also fire reactive PlayerKilledEnemy event (and Boss-
        // Defeated for synced-boss kills) for ANY kill — descriptor
        // hooks like "ambush after N kills" / "revenge after boss
        // death" can consume these regardless of whether the killed
        // actor was director-spawned.
        if (::SceneAuthority::IsEffectiveHost()) {
            auto& director = AnchorDirector::Director::Instance();
            director.OnEnemyRemoved(ext->netId, AnchorDirector::DefeatCause::Kill);

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
        if (isKillingNetworkActor) {
            return; // This kill originated from a received ENEMY_DEFEATED — do not echo.
        }
        Actor* actor = static_cast<Actor*>(refActor);
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
        if (::SceneAuthority::IsEffectiveHost()) {
            AnchorDirector::Director::Instance().OnEnemyRemoved(
                ext->netId, AnchorDirector::DefeatCause::Kill);
        }
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
        ObjectExtension::GetInstance().Free(actor);
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
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_SetFlag(sceneNum, flagType, flag); });

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
        // Handle vanilla dungeon items a bit differently
        if (itemEntry.modIndex == MOD_NONE &&
            (itemEntry.itemId >= ITEM_KEY_BOSS && itemEntry.itemId <= ITEM_KEY_SMALL)) {
            SendPacket_UpdateDungeonItems();
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

        // Race A mitigation: Granted state means host has arbitrated
        // this drop in our favour. Clear the flag (back to None for
        // safety / re-entry) and allow vanilla pickup to run.
        if (ext->pickupState == ItemPickupState::Granted) {
            ItemDropNetId* mut = const_cast<ItemDropNetId*>(ext);
            mut->pickupState = ItemPickupState::None;
            SPDLOG_INFO("[ItemDrop] netId={} grant consumed — applying pickup",
                        ext->netId);
            // *should stays true; vanilla pickup runs.
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
        const bool inExclusiveWindow =
            (ext->killerClientId != 0) &&
            (nowMs - ext->spawnTimeMs < kKillerExclusiveMs);
        const bool isLocalKiller =
            (ext->killerClientId == Anchor::Instance->ownClientId);

        // Layer 1.
        if (inExclusiveWindow && !isLocalKiller) {
            *should = false;
            SPDLOG_DEBUG("[ItemDrop] netId={} blocked — exclusive window ({} ms remaining) for killer={}",
                         ext->netId,
                         (long long)(kKillerExclusiveMs - (nowMs - ext->spawnTimeMs)),
                         ext->killerClientId);
            return;
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
        s16 itemType = (s16)(item00->actor.params & 0xFF);
        const bool killerExclusiveBypass = isLocalKiller && inExclusiveWindow;
        if (killerExclusiveBypass) {
            SPDLOG_DEBUG("[ItemDrop] netId={} killer-exclusive bypass — skipping Layer 2 "
                         "(type=0x{:02X})",
                         ext->netId, (int)itemType);
        } else {
            bool anyTeammateOnline = false;
            for (auto& [otherId, other] : Anchor::Instance->clients) {
                if (other.self) continue;
                if (other.online && other.isSaveLoaded) {
                    anyTeammateOnline = true;
                    break;
                }
            }
            if (anyTeammateOnline) {
                if (!ItemEligibility::CanPlayerCollectItem00(itemType, /*walletCapAware=*/true)) {
                    *should = false;
                    SPDLOG_DEBUG("[ItemDrop] netId={} blocked — local player ineligible (type=0x{:02X}); "
                                 "deferring to teammate",
                                 ext->netId, (int)itemType);
                    return;
                }
            } else {
                SPDLOG_DEBUG("[ItemDrop] netId={} solo session — skipping Layer 2 eligibility "
                             "(type=0x{:02X})",
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
            SPDLOG_INFO("[ItemDrop] netId={} pickup by host — broadcasting ITEM_COLLECTED type=0x{:02X}",
                        ext->netId, (int)itemType);
            Anchor::Instance->SendPacket_ItemCollected(ext->netId);
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

    // #193 Phase 3 — visual cue for non-killer during exclusive window.
    //
    // Apply a scale-down to drops the local player CANNOT pick up yet
    // (still inside the killer's 3s window AND not the local player's
    // kill). Reverts to vanilla scale once the window expires. Cheap +
    // visible: peer sees a smaller drop that "pops" to full size at the
    // 3s mark, signaling "now anyone can collect".
    //
    // Hook fires post-update each frame; the EnItem00 update itself
    // smooth-steps `actor.scale` toward `this->scale` (the actor's
    // intended target scale). Overwriting scale.x/y/z directly takes
    // immediate visual effect.
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        if (actor == nullptr || actor->update == nullptr) return;

        const ItemDropNetId* ext =
            ObjectExtension::GetInstance().Get<ItemDropNetId>(actor);
        if (ext == nullptr) return;
        if (ext->killerClientId == 0) return;
        if (ext->killerClientId == Anchor::Instance->ownClientId) return;

        const int64_t kKillerExclusiveMs = 3000;
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - ext->spawnTimeMs >= kKillerExclusiveMs) {
            // Window expired. Don't keep clamping scale — vanilla
            // smooth-step will restore it on the next update tick.
            return;
        }

        // Inside the window: shrink to 60% so non-killer drops are
        // visually distinct.
        actor->scale.x *= 0.6f;
        actor->scale.y *= 0.6f;
        actor->scale.z *= 0.6f;
    });

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
