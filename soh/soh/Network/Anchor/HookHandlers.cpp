#include "Anchor.h"
#include "Common/ActorSyncHelpers.h"  // GetEnemySkelAnime, IsSyncedWorldActor, IsSyncableActor
#include "Common/PlayerLookup.h"      // FindNearestPlayerActor
#include "Common/SceneAuthority.h"    // IsEffectiveHost (Pillar A Phase 1)
#include "Common/ItemEligibility.h"   // CanPlayerCollectItem00 (#193 Phase 0)
#include "Common/PauseLinkBuffer.h"   // Anchor_IsDrawingPauseLink (#182 follow-up)
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
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
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
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
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
    // Defense-in-depth against the logs-216 actor-flood crash: peers
    // (non-room-hosts) must never spawn the recovery heart. Host's
    // local SetupLeave fires the canonical Actor_Spawn(EnItem00); the
    // resulting heart replicates to peer through standard item-spawn
    // flow. Without this gate, any code path that calls SetupLeave on
    // peer (today's DIALOG_END routing prevents the Talk→Leave path,
    // but future paths could reach SetupLeave directly) would dup the
    // heart per call and overflow MISC actor list.
    if (!::SceneAuthority::IsMyCurrentRoomHost()) return true;
    // Phase-based suppression — preserved as the original guard for
    // network-driven death cycles (not currently exercised by Hintnuts;
    // kept for when a future change routes Leave through ENEMY_DEFEATED).
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
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
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// #148 / en_sw_sync_plan.md §5 — same predicate shape, applied to
// En_Sw's combat-variant drop site (line 686). Gold-variant En_Si
// spawn deliberately NOT suppressed (cooperative collectible Design A).
extern "C" bool Anchor_ShouldSuppressEnSwDrop(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
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

void Anchor::RegisterHooks() {

    // #region Hooks that are required for basic Anchor functionality

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
        }

        SendPacket_PlayerUpdate();
    });

    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        ProcessIncomingPacketQueue();

        // Heartbeat liveness counter (#194 follow-up) — read by the
        // network thread when building the heartbeat payload. If this
        // hook stops firing (game thread frozen), the counter stops
        // advancing; peers detect a stale counter as IsClientGameFrozen
        // even though the network-thread heartbeat itself keeps flowing.
        Anchor::Instance->gameFrameCounter.fetch_add(
            1, std::memory_order_relaxed);

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

    // Follower mode (non-host only): override local player position to trail the host.
    //
    // Activation: toggled via the Anchor settings menu (AI Follower checkbox).
    // Any controller input while active immediately cancels it and returns manual control.
    //
    // Position source: the host's DummyPlayer actor (ACTORCAT_NPC, id=ACTOR_EN_OE2,
    // update=DummyPlayer_Update, clientId==roomState.ownerClientId). Its world.pos is
    // updated every frame by DummyPlayer_Update to the host's authoritative position.
    //
    // Offset: fixed units along the world +X axis from the host. P2's shape.rot.y is
    // also set to match the host so both players face the same direction.
    //
    // Note: COND_HOOK cannot be used here — the lambda body contains brace-initializer
    // lists (e.g. Vec3f sideTarget = { a, b, c }), and the C preprocessor does NOT
    // treat {} as grouping, so their commas split the macro's argument list.
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
                static constexpr int kAttackDuration     = 60;     // frames per ATTACK cycle
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

                // Yaw toward (dx, dz).  Math_Atan2S(x, y) with OoT param order.
                auto YawToward = [](f32 dx, f32 dz) -> s16 {
                    return Math_Atan2S(dz, dx); // z first, x second — OoT convention
                };

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
                                if (teleportSafe) {
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
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] BLOCK→RETURN (target gone)");
                            break;
                        }
                        // shape.rot.y points at target so the shield faces the
                        // incoming projectile. Position is held by zeroed stick
                        // (see ShouldActorUpdate isMoving exclusion).
                        f32 ex = followerTargetEnemy->world.pos.x - p2Pos.x;
                        f32 ez = followerTargetEnemy->world.pos.z - p2Pos.z;
                        if (ex * ex + ez * ez > 1.0f) {
                            player->actor.shape.rot.y = YawToward(ex, ez);
                        }
                        // Hold the shield for kAttackDuration frames per cycle,
                        // then drop to ATTACK to swing on the (now-stunned) scrub.
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::ATTACK;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] BLOCK→ATTACK (shield cycle complete)");
                        }
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
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::ENGAGE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] STANDBY→ENGAGE (window expired)");
                        }
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
                });
        }
    }

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
        if (isDynamicSpawn && ::SceneAuthority::IsMyCurrentRoomHost() && !isSpawningNetworkActor) {
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
            if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase) && actor->colChkInfo.damage > 0) {
                // #174/#175: forward damageEffect (set on enemy by collision damage-table
                // lookup) and atHitEffect (set on the player by CollisionCheck_SetATvsAC
                // when the player's AT element lands a hit). Many OoT enemies branch on
                // these fields to decide whether Actor_ApplyDamage actually fires; sending
                // only `damage` left those enemies silently ignoring the synthetic hit.
                Player* localPlayer = GET_PLAYER(gPlayState);
                u8 atHitEffect = (localPlayer != nullptr) ? localPlayer->actor.colChkInfo.atHitEffect : 0;
                SendPacket_DamageEnemy(ext->netId, (u8)actor->colChkInfo.damage,
                                       actor->colChkInfo.damageEffect, atHitEffect);
            }

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
        if (!::SceneAuthority::IsMyCurrentRoomHost()) {
            return;
        }
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            return;
        }
        if (!IsSaveLoaded() || gPlayState == nullptr) {
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
