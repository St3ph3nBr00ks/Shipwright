#ifndef NETWORK_ANCHOR_H
#define NETWORK_ANCHOR_H
#ifdef __cplusplus

#include "soh/Network/Network.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/resource/type/Skeleton.h"
#include <libultraship/libultraship.h>
#include <atomic>
#include <climits>
#include <map>
#include <queue>
#include <mutex>
#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "variables.h"
#include "z64.h"
}

// Categories walked by all receive-side netId→actor lookups.
// kSyncableActorCategories + kSyncableActorCategoriesCount moved to
// Common/ActorSyncHelpers.h in #173 Phase 1. Included below for transitive
// availability to all consumers that include Anchor.h.
#include "Common/ActorSyncHelpers.h"

// AnchorNav::ActorTrail::NavPath is held as a follower-state member.
// Phase 2 commit 2 of the SRP refactor (#173 / #169) wires HandleStateFollow
// to consume an ActorTrail-computed path snapshot. Definition needed at the
// class level because NavPath is a value-type member.
#include "Common/ActorTrail.h"

#include "EnemyStateSync/EnemyLifecycle.h"
#include "EnemyStateSync/EnemyHostBookkeeping.h"

// Forward declaration — full type lives in
// soh/Enhancements/RoomNavData/RoomNavData.h. Only Anchor.cpp /
// Follower.cpp callers that USE the type need to include the
// header; this forward decl keeps Anchor.h's transitive include
// surface narrow while letting member function signatures
// reference the type by pointer.
namespace AnchorNavRoom { struct RoomNavData; }

// Attached to enemy actors to give them a stable network id across all clients.
struct EnemyNetId {
    uint32_t netId = 0;
    SkelAnime* skelAnime = nullptr; // nullptr if this enemy type has no supported skeleton
    uint8_t limbCount = 0;          // cached from skelAnime->limbCount at spawn time

    // Pillar C2 Phase 1 — explicit lifecycle phase.
    //
    // Source of truth for the boolean flag soup that grew alongside this
    // struct: `hasLocalDeath`, `defeatPacketSent`, `pendingNaturalDeath`,
    // and `deferredDeadItemDrop` will be removed at the end of Phase 1 in
    // favour of reads off `phase`. During the migration both representations
    // are kept in lock-step by EnemyStateSync::TransitionTo().
    //
    // Default Alive — initialised at OnActorSpawn for new enemies.
    EnemyStateSync::LifecyclePhase phase = EnemyStateSync::LifecyclePhase::Alive;

    // Peer-side drop-suppression signal. Set true the instant
    // `ENEMY_STATE` from host carries `health <= 0`, regardless of
    // whether `ENEMY_DEFEATED` has arrived yet. Read by the per-actor
    // `Anchor_ShouldSuppress*Drop` predicates (Dekubaba / En_St / En_Sw
    // / En_Dekunuts / En_Goma) ORed alongside `PhaseImpliesPendingNaturalDeath`.
    //
    // Closes the race documented by 2026-05-06 field test (Inside Deku
    // Tree, b0ea6f1): peer's vanilla `EnDekubaba_Hit` reads `health == 0`
    // from the same `ENEMY_STATE` and transitions to `ShrinkDie` on the
    // same frame; peer's local `OnEnemyDefeat` fires from there with
    // `phase = DyingByLocal`. The legacy phase-only suppressor returns
    // false on `DyingByLocal` and peer spawns a duplicate local drop.
    // Driving this bool from the same packet that drives the kill itself
    // closes the race entirely — both signals land on the same frame.
    //
    // Cleared by `EnemyStateSync::TransitionTo(Alive | Regrowing)` so
    // Karebaba (and any future regrow-class enemy) doesn't carry the
    // flag across respawn.
    //
    // Host-side: never set, gated on `!IsMyCurrentRoomHost()` at the
    // write site. Host's own kill drives the broadcast normally via
    // `OnActorSpawn(EN_ITEM00)`; a stale flag here would block host's
    // legitimate drop call.
    bool networkDriveDying = false;

    // Last state received from the host via ENEMY_UPDATE (non-host clients only).
    // Re-applied each frame in OnActorUpdate so the enemy update() can run (enabling
    // collision registration) without drifting from the authoritative host position.
    bool hasNetState = false;
    Vec3f netPos = { 0.0f, 0.0f, 0.0f };
    Vec3s netRot = { 0, 0, 0 };
    Vec3s netShapeRot = { 0, 0, 0 };
    s8 netHealth = 1;
    Vec3f netScale = { 1.0f, 1.0f, 1.0f }; // actor->scale; synced for enemies that change scale during animation

    // defeatPacketSent was extracted to EnemyStateSync::HostBookkeeping at
    // end of C2 Phase 2 (consolidated with sentDefeatThisScene into
    // mDefeatBroadcasts — both signals had the same lifetime and purpose).
    // Read sites use HostBookkeeping::Instance().HasDefeatBroadcast(netId);
    // writes use ClaimDefeatBroadcast / ReleaseDefeatBroadcast.

    // hasLocalDeath was deleted at end of C2 Phase 1 (commit landing this
    // change). Read sites now use EnemyStateSync::PhaseImpliesHasLocalDeath(
    // ext->phase), which returns true for DyingByLocal / DyingByNetwork /
    // AwaitingDeadItemDrop / Dead. Write sites moved to TransitionTo()
    // calls during Phase 1 step 2; this commit drops the boolean storage.

    // Per-actor-type state machine sync fields.
    // Currently used by ACTOR_EN_KAREBABA (Withered Deku Baba) to keep its
    // Idle/Awaken/Upright/Spin/Retract state in sync with the host.
    // -1 means no state received yet (initial value).
    s16 netStateIndex = -1;
    s16 netActorParams = 0;

    // pendingNaturalDeath was deleted at end of C2 Phase 1 step 5b.
    // Read sites use EnemyStateSync::PhaseImpliesPendingNaturalDeath(
    // ext->phase), which returns true for DyingByNetwork /
    // AwaitingDeadItemDrop. Karebaba item-drop suppression and natural-
    // cycle gating now derive from phase.

    // deferredDeadItemDrop was deleted at end of C2 Phase 1 step 5c.
    // The OnActorInit Karebaba SetupDeadItemDrop gate now reads
    // EnemyStateSync::PhaseImpliesDeferredDeadItemDrop(ext->phase),
    // which returns true iff phase == AwaitingDeadItemDrop. The
    // OnActorInit handler also transitions AwaitingDeadItemDrop ->
    // DyingByNetwork after firing SetupDeadItemDrop, so the second
    // OnActorInit pass on the same actor is a no-op (the predicate
    // returns false).

    // Goroiwa-net state — issue #153.
    // First non-ACTORCAT_ENEMY actor sync (En_Goroiwa is ACTORCAT_PROP). Cached on
    // non-host from ENEMY_UPDATE; re-applied each frame in OnActorUpdate so the local
    // action function can run (collision registration) without drifting from the
    // host-authoritative path-position. -1 means no state received yet.
    s16 goroiwaCurrentWaypoint = -1;
    s16 goroiwaNextWaypoint    = -1;
    s16 goroiwaPathDirection   = 0; // ±1; 0 means uninitialized
    u8  goroiwaFlags           = 0; // ENGOROIWA_* bitmask (PLAYER_IN_THE_WAY etc.)

    // Per-torch lit-state sync (Obj_Syokudai). Host-authoritative.
    // -1 = permanently lit, 0 = unlit, >0 = remaining-burn-frames.
    // Cached at receive time and re-applied every frame in OnActorUpdate
    // so peer's local ObjSyokudai_Update can run normally (the local
    // body's writes to litTimer are overwritten post-update).
    // INT16_MIN sentinel = no host state received yet (fall through to
    // local AI, vanilla single-player parity).
    s16 syokudaiLitTimer = INT16_MIN;

    // #190 — pending DAMAGE_ENEMY queue. When peer's hit packet arrives
    // while the host is in a world-freeze (Item Get cutscene, text box,
    // ocarina playback, scene transition, vanilla pause), the host's
    // actor->update doesn't run, so the synthetic AC_HIT bit and
    // colChkInfo.damage value previously written by HandlePacket_DamageEnemy
    // got cleared by collision-system reset passes that aren't gated on
    // world time — silently dropping the damage. Now the receive path
    // queues the damage onto these fields, and the ShouldActorUpdate
    // hook drains them onto colChkInfo + AC_HIT on the first frame
    // the actor's update is about to run (i.e., when world resumes).
    // pendingSyncDamage accumulates (multi-hit during a single freeze
    // adds up); damageEffect / atHitEffect are last-write-wins.
    u8 pendingSyncDamage        = 0;
    u8 pendingSyncDamageEffect  = 0;
    u8 pendingSyncAtHitEffect   = 0;

    // Race B mitigation (#203) — peer-killing-blow stash. When peer's
    // ShouldActorUpdate hook detects a would-kill damage value, it
    // clamps colChkInfo.damage to leave 1 HP locally AND records the
    // original damage value here so the OnActorUpdate forwarder can
    // broadcast the un-clamped value to host. Without this stash, the
    // forwarder would broadcast the clamped value (e.g. 0 if peer's
    // hit was a one-shot at 1 HP), causing host's enemy to be
    // unkillable from peer hits. Cleared by the forwarder after each
    // broadcast.
    u8 peerKillingBlowOriginalDamage = 0;

    // Boss_Goma — sticky "peer is signaling encounter advance" flag (#67).
    // Set true on receipt of any BOSS_GOMA_LOOKED_AT. Stays true until
    // case 3 of BossGoma_Encounter consumes-and-clears it via
    // Anchor_BossGomaConsumePeerSignaled — at which point case 3
    // immediately calls BossGoma_SetupEncounterState4 (eye-roll
    // cinematic) instead of waiting for the local frustum-check
    // 15-frame threshold (vanilla case 3's else-branch resets
    // lookedAtFrames every frame the local check fails, so the
    // earlier per-frame `lookedAtFrames++` approach was getting
    // clobbered). Persists across host's case 1-2 cutscene window
    // (~228 frames) so peer's signal — which arrives ~11s before
    // host enters case 3 — isn't lost.
    bool bossGomaPeerSignaled = false;

    // RoomNavData Phase 2 commit 11 — slope-3 stuck-on-slope diagnostic.
    // Detection-only; intervention deferred to v2 (gEnhancements.RoomNavData
    // .ActiveSlopeRecovery) pending field-test evidence of need.
    //
    // stuckOnSlopeFrames counts up while the predicate is true (actor on
    // slope-3 surface AND velocity.y not consistently descending). Resets
    // to 0 when predicate becomes false. On rising edge above
    // kStuckFrameThreshold, RoomNavData increments stuckOnSlopeEventCount
    // and emits a rate-limited SPDLOG_WARN.
    //
    // Both reset on actor death / scene transition (via the same
    // EnemyNetId reset paths that clear netState fields).
    uint16_t stuckOnSlopeFrames     = 0;
    uint16_t stuckOnSlopeEventCount = 0;

    // ── Nav system per-navigator state (Plans/nav_system_implementation_plan.md §6 + §9)
    //
    // Despite the struct's historical name (EnemyNetId), this extension is
    // the canonical "per-actor nav state" — attached to any navigator that
    // participates, not only enemies. AI Follower (ACTOR_EN_OE2) and any
    // future Link-rigged navigator share it.
    //
    // navHeldKind discriminates the held-target representation:
    //   None     → no target held; AcquireOrHoldTarget will evaluate fresh.
    //   Player   → navTargetClientId is the held target's client ID.
    //   Enemy    → navTargetNetId is the held actor's netId.
    //   FixedPos → navHeldTargetPos is a pinned world position
    //              (HoldPositionTarget).
    enum class HeldTargetKind : uint8_t { None = 0, Player, Enemy, FixedPos };
    HeldTargetKind navHeldKind          = HeldTargetKind::None;
    uint8_t        navTargetClientId    = 0xFF;        // valid when navHeldKind == Player
    uint32_t       navTargetNetId       = 0;           // valid when navHeldKind == Enemy
    Vec3f          navHeldTargetPos     = { 0.0f, 0.0f, 0.0f }; // valid when navHeldKind == FixedPos OR cached for trails
    uint16_t       navTargetTimerFrames = 0;           // counts down from NavTraits.targetStickyFrames
    bool           navTargetIsStale     = false;       // true if returning a held target despite invalidation signals

    // VerticalTeleport (plan §9). Counts up while |Δy(target, navigator)|
    // exceeds NavTraits.verticalTeleportYThreshold; reaches
    // verticalTeleportDelayFrames before the slow-path teleport fires.
    // Cooldown counts down after a slow-path teleport to avoid rapid
    // re-fire.
    uint16_t navVerticalMismatchFrames = 0;
    uint16_t navTeleportCooldownFrames = 0;
};

// #193 Phase 2 — attached to ACTOR_EN_ITEM00 actors so the receive-side
// pickup gate can read the host-authoritative drop metadata.
//   netId           — unique per-drop identifier (matches ITEM_DROP_SYNC).
//   killerClientId  — player who triggered the drop. Used by the 3s
//                     killer-exclusive window in the pickup gate.
//                     0 means unattributed (no exclusivity).
//   spawnTimeMs     — host's monotonic clock at drop time, in ms. Used
//                     by `now - spawnTimeMs < kKillerExclusiveMs` for
//                     the grace window. Stamped from the broadcast on
//                     receivers; stamped from `steady_clock::now()` on
//                     the host.
//   isFromBroadcast — true on receivers (drop was spawned from
//                     ITEM_DROP_SYNC). False on the host (local drop
//                     that the host then broadcasts). Used to suppress
//                     the OnActorSpawn-side broadcast on receivers so
//                     they don't echo the packet back.
// #193 race A mitigation — three-state pickup-claim machine for
// host-arbitrated pickup. Peer's local pickup gate transitions
// None → Pending when a request is sent. Host's ITEM_COLLECTED grant
// (winner == ownClientId) transitions Pending → Granted. Vanilla
// pickup runs on the next gate fire when state is Granted.
//
// Rationale: simultaneous-pickup post-window races could double-credit
// because each peer's local gate ran independently and credited
// gSaveContext before the other's broadcast arrived. Host arbitration
// serialises pickup so only one client's gSaveContext is credited.
enum class ItemPickupState : uint8_t {
    None    = 0,
    Pending = 1,
    Granted = 2,
};

struct ItemDropNetId {
    uint32_t        netId           = 0;
    uint32_t        killerClientId  = 0;
    int64_t         spawnTimeMs     = 0;
    bool            isFromBroadcast = false;
    ItemPickupState pickupState     = ItemPickupState::None;
};

void DummyPlayer_Init(Actor* actor, PlayState* play);
void DummyPlayer_Update(Actor* actor, PlayState* play);
void DummyPlayer_Draw(Actor* actor, PlayState* play);
void DummyPlayer_Destroy(Actor* actor, PlayState* play);

// Phase 5 #60 — clears the per-netId last-sent cache that SendPacket_EnemyUpdate
// consults to skip no-op packets. Called on scene-load (OnSceneSpawnActors)
// and on reconnect (OnConnected) so a fresh peer sees a send on the next
// frame instead of waiting for the keepalive timer to elapse. Cache itself
// is file-scope static in Packets/EnemyUpdate.cpp.
void Anchor_ClearEnemyUpdateCache();

// Per-netId cache eviction. Forces the next SendPacket_EnemyUpdate(netId)
// to bypass the dedup filter even when the actor's state is unchanged.
// Used by #166 mid-boss late-join snapshot — the joining peer needs the
// packet immediately, regardless of what other peers have already received.
void Anchor_ClearEnemyUpdateCacheForNetId(uint32_t netId);

typedef struct AnchorClient {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
    // Peer's per-packet-type maxSchema, populated from HANDSHAKE.
    // Pillar F: senders consult `PeerSupportsField()` to decide whether
    // to include schema-N optional fields. Empty for pre-Pillar-F peers.
    std::unordered_map<std::string, int> peerMaxSchema;
    bool online;
    bool self;
    uint32_t seed;
    bool isSaveLoaded;
    bool isGameComplete;
    s16 sceneNum;
    s8 curRoomNum;
    s32 entranceIndex;
    // Monotonic counter incremented on every OnSceneSpawnActors on the sender.
    // Host uses it to detect same-scene scene-reloads (Game Over continue, void-out,
    // Farore's Wind back to current scene) — cases where sceneNum and isSaveLoaded
    // both stay unchanged but the scene was freshly respawned, so dead-enemy replay
    // must fire. Starts at 0; first scene-spawn after connect increments to 1.
    uint32_t sceneSpawnEpoch = 0;

    // Only available in PLAYER_UPDATE packets
    s32 linkAge;
    PosRot posRot;
    Vec3s jointTable[24];
    u8 movementFlags;
    Vec3s prevTransl;
    Vec3s upperLimbRot;
    s8 currentBoots;
    s8 currentShield;
    s8 currentTunic;
    u32 stateFlags1;
    u32 stateFlags2;
    u8 buttonItem0;
    s8 itemAction;
    s8 heldItemAction;
    u8 modelGroup;
    s8 invincibilityTimer;
    f32 unk_85C;
    // Deku-Stick burning timer (Player.unk_860). 0 = unlit, >0 = burning
    // and counting down. Synced separately from unk_85C (which is a
    // visual scale field that stays at 1.0 for held-but-unlit sticks).
    // Used by DummyPlayer_Update to gate the burning-stick flame VFX.
    s16 unk_860;
    s16 unk_862;
    s8 actionVar1;
    u8 ocarinaNote;
    f32 ocarinaModulator;
    s8 ocarinaBend;

    // Multi-player dialogue redesign (#191 follow-up) — peer's
    // current `play->csCtx.state`. Used by `Anchor_ShouldAdvanceCutsceneTextLocal`
    // to detect "alone in cutscene": when no online team-member in the
    // local scene has `csCtxState != CS_STATE_IDLE`, the local player
    // is the only one with the active cutscene textbox and should
    // advance immediately on button press (vanilla parity), not
    // through the voting countdown which would block until the timer
    // elapses (no peer to vote). Defaults to CS_STATE_IDLE (0) for
    // pre-update peers — they're treated as out-of-cutscene, which
    // is the safe assumption when state is unknown.
    s8 csCtxState = 0;

    // AI follower mode (remote client is running the follower AI and should not be
    // selected as a follower's leader target). Defaults to false for pre-update peers.
    bool followerActive = false;

    // G1/G2 — set when the remote client's local Player is in a climbing state
    // (PLAYER_STATE1_CLIMBING_LADDER for vines/ladders, or HANGING_OFF_LEDGE /
    // CLIMBING_LEDGE during ledge-hoist). Broadcast on edge change so the
    // follower can teleport-and-ride along. Defaults to false for pre-update peers.
    bool isClimbing = false;

    // Test 5 (log 71) — set when the remote client's local Player has
    // PLAYER_STATE2_CRAWLING set (inside a dungeon crawlspace). Follower
    // uses this to switch its FOLLOW target from the +kFollowOffset
    // sideTarget to the leader's exact XZ — sideTarget lands off-axis
    // from the crawlspace hole and DO_ACTION_ENTER never fires on the
    // follower. Default false for pre-update peers.
    bool isCrawling = false;

    // Multiplayer cosmetic sync
    std::string customModelFilename;                    // folder name of remote client's coop pack, or ""
    std::shared_ptr<SOH::Skeleton> customSkeleton;      // keeps loaded skeleton alive; nullptr = vanilla
    std::string lastAppliedModelFilename;               // folder that was last passed to ApplyCustomSkeletonToDummyPlayer;
                                                        // suppresses per-frame retries when the lookup fails
    std::unique_ptr<SOH::BakedPlayerModel> bakedModel;  // per-DummyPlayer baked display lists;
                                                        // unique_ptr so its address (and all c_str() inside) is
                                                        // stable even if this unordered_map entry is rehashed

    // KB-15 / issue #110 — deferred destruction of a replaced BakedPlayerModel.
    //
    // When cosmetic state changes (remote pack change, local model/tunic change,
    // DummyPlayer re-spawn), the outgoing bakedModel cannot be destroyed
    // synchronously: the most recently submitted frame's Gfx buffer still holds
    // raw pointers into its pathStrings / bakedDLs / eyeTexKeys. Destroying it
    // now leaves those pointers dangling and the renderer walks freed heap memory
    // (Unhandled OP code crash flood observed in Test 17 log 113).
    //
    // Instead: append the outgoing bakedModel to retiredBakedModels with a
    // framesRemaining counter, and let OnGameFrameUpdate tick the counter
    // down. When an entry's counter hits 0 it is erased and the unique_ptr
    // destroys the model — by which point every Gfx frame that could have
    // referenced it has been fully consumed by the renderer.
    //
    // KB-19 (#176): vector replaces the prior single-slot pattern. The
    // single-slot's "≥400 ms per bake" assumption only held for pack-archive
    // bakes; vanilla revert (BuildVanillaDummyPlayerModel) is ~10 ms, so
    // rapid re-bakes from a P2 age switch could land 4 retires in 41 ms —
    // well within kRetireFrames — and the second retire's std::move
    // destroyed the first while its Gfx commands were still in-flight.
    std::vector<SOH::RetiredBake> retiredBakedModels;

    // Helper: move current bakedModel into the retire vector, armed with
    // kRetireFrames. No-op if bakedModel is already null.
    void RetireBakedModel();

    // Ptr to the dummy player
    Player* player;
} AnchorClient;

// Number of render frames a retired BakedPlayerModel must sit idle before
// destruction. See commentary on AnchorClient::retiredBakedModels for rationale.
//
// History:
//   N=4 (original) — covers one bake per transition + LUS double-buffer.
//     67 ms at 60 fps / 200 ms at 20 fps.
//   N=30 (#171 step 2, 2026-04-22) — basement-transition crash
//     reproduced on log 69 and log 74. Post-teleport scene reload into
//     Deku Tree basement packs FIVE skeleton bakes into ~250 ms (4x local
//     Malon-Heroine adult+child tunic bakes + 1x DummyPlayer 3dsLink),
//     and the 5th swap's retire slot overwrote the 1st's buffer while
//     the GPU / an in-flight ENEMY_UPDATE iteration still held a raw
//     pointer. Access-violation on freed skelAnime.
//     30 frames = 500 ms at 60 fps / 1.5 s at 20 fps — comfortably
//     covers the observed 250 ms worst case. Memory cost is ~10 KB per
//     additional slot (already-retired model sits idle until the next
//     bake evicts it), trivial.
static constexpr int kRetireFrames = 30;

typedef struct {
    uint32_t ownerClientId;
    u8 pvpMode;           // 0 = off, 1 = on, 2 = on with friendly fire
    u8 showLocationsMode; // 0 = none, 1 = team, 2 = all
    u8 teleportMode;      // 0 = off, 1 = team, 2 = all
    u8 syncItemsAndFlags; // 0 = off, 1 = on
} RoomState;

// Forward-decl for the Anchor::TickFollower parameter — defined in
// AIFollower/Follower.h (Phase 1 commit 3 of the SRP refactor).
namespace AnchorFollower {
    struct FollowerFrameContext;
    // Diagnostic recorder needs read access to private follower state.
    // Friend-declared on Anchor below so it can read followerAIState /
    // followerNavPath / G-timers without an API surface bloat.
    // Spec: Plans/follower_recorder_plan.md.
    void CaptureFrame(const FollowerFrameContext& ctx);
}

class Anchor : public Network {
    friend void AnchorFollower::CaptureFrame(const AnchorFollower::FollowerFrameContext&);

  private:
    uint32_t spawningDummyPlayerForClientId = 0;
    // Local monotonic counter sent in UPDATE_CLIENT_STATE so the host can detect
    // same-scene reloads (Game Over continue, void-out) in addition to scene
    // changes and save loads. Incremented at the top of the OnSceneSpawnActors
    // hook before SendPacket_UpdateClientState fires.
    uint32_t sceneSpawnEpoch = 0;
    bool shouldRefreshActors = false;
    bool justLoadedSave = false;
    bool isHandlingUpdateTeamState = false;
    bool isProcessingIncomingPacket = false;
    std::queue<nlohmann::json> incomingPacketQueue;
    std::mutex incomingPacketQueueMutex;
    std::queue<nlohmann::json> outgoingPacketQueue;
    std::mutex outgoingPacketQueueMutex;

    // Phase 5 bandwidth profiler (#62). Gated by gEnhancements.AnchorProfiler (0=off, 1=on).
    // Per-packet-type counts+bytes flushed every kProfilerWindowMs to SPDLOG_INFO.
    struct ProfileBucket { uint64_t count = 0; uint64_t bytes = 0; };
    std::unordered_map<std::string, ProfileBucket> profileTx;
    std::unordered_map<std::string, ProfileBucket> profileRx;
    std::mutex profileMutex;
    uint64_t   profileWindowStartMs = 0;
    static constexpr uint64_t kProfilerWindowMs = 10000;

    void RecordProfileSample(const nlohmann::json& payload, bool tx);
    void FlushProfileIfDue();

    // Host-only enemy bookkeeping (deadEnemiesByScene / sentDefeatThisScene
    // / pendingKillNetIds / lastDamagerByNetId) was extracted into
    // EnemyStateSync::HostBookkeeping at end of C2 Phase 2. Access via
    // EnemyStateSync::HostBookkeeping::Instance().{RecordPendingKill,
    // RecordSceneDeath, ClaimDefeatBroadcast, RecordDamager, ...}.

    // Set to true for the duration of HandlePacket_EnemySpawn's Actor_Spawn call
    // so the resulting OnActorSpawn hook does not suppress the actor on non-host.
    bool isSpawningNetworkActor = false;

    // Set to true for the duration of HandlePacket_EnemyDefeated's Actor_Kill call
    // so the resulting OnActorKill hook (Fix 12) does not echo ENEMY_DEFEATED back
    // to the network for kills that originated from the network.
    bool isKillingNetworkActor = false;

    // Follower mode: non-host player's position is overridden to trail the host.
    // Toggled via the Anchor settings menu (AI Follower checkbox).
    // Deactivated by any controller input while active.
    bool followerActive = false;

    // NPC Follower (Flotilla — Plans/npc_follower_plan.md). Separate from
    // followerActive above. The player-rigged Follower is "AFK mode"
    // (controls Link's body); the NPC Follower is a friendly Link-skel
    // companion that walks beside the player using NPC primitives.
    //
    // mFollowerNpcLocalActor: when non-null, points to the ACTOR_EN_FOLLOWER
    //   actor we spawned for THIS client. Cleared on Actor_Kill or on
    //   scene transition. Validated at use site (update != NULL).
    // mFollowerNpcCVarLast: last-tick value of the master CVar
    //   gEnhancements.AI.FollowerNPC.Enabled; transition detection
    //   compares against this to call SetFollowerNpcActive only on
    //   0↔1 edges.
    Actor* mFollowerNpcLocalActor = nullptr;
    int    mFollowerNpcCVarLast   = 0;

    // Phase 3 receive-side bookkeeping. Map from ownerClientId →
    // local replica Actor* for NPCs owned by OTHER clients. We
    // never put our own NPC here; mFollowerNpcLocalActor handles
    // that. Cleared on Anchor::Disable / scene transitions.
    std::unordered_map<uint32_t, Actor*> mPeerFollowerNpcs;

    // STATE-broadcast throttle counter. The owner sends STATE
    // every kFollowerNpcStateMs (100ms = 10Hz nominal). Tracked as
    // game-tick counter via MsToGameTicks; mFollowerNpcStateLastTick
    // records the last tick we sent so we don't double-send within
    // one window.
    uint64_t mFollowerNpcStateLastFrame = 0;

    // AI follower state machine (runs each frame when followerActive is true).
    // IDLE     — at leader's side; scans for nearby enemies.
    // FOLLOW   — stick-driven movement toward leader's side. Used for ALL
    //            "walk toward target" pursuit, including the post-combat /
    //            post-pickup walk back to leader (formerly RETURN —
    //            removed 2026-05-12 PM, see commit message). Carries
    //            stuck-detection, item-pickup scan, and auto-CLIMBING
    //            engagement; previously these were FOLLOW-only and the
    //            return-to-leader walk had to wait 10s for G14 to fire.
    // STUCK    — stick stalled; bounded position-override nudge toward target.
    // ENGAGE   — moving toward the nearest ACTORCAT_ENEMY actor.
    // ATTACK   — within sword reach; BTN_B injection, stick continues to track enemy.
    // CLIMBING — leader is on a vine/ladder above the kMaxYDelta band; teleport
    //            follower to leader and ride along until leader stops climbing (G1/G2).
    // BLOCK    — ENGAGE target is shield-reflect class (Mad Scrub); inject BTN_R (G4).
    // RANGED_ATTACK — target is out of melee Y-band but in a known ranged class
    //                 (Gohma ceiling, En_Goma larvae, En_Sw on vines); inject BTN_Z+BTN_A
    //                 (G6/G7/G8).
    // STANDBY  — placeholder: hold position next to leader, no swings. Reserved for
    //            future use (G19 Gohma weak-point timing).
    // COLLECT_ITEM — opportunistic pickup of ACTOR_EN_ITEM00 drops after an
    //            enemy kill (rupees, filtered hearts/magic). Interrupts IDLE
    //            and FOLLOW when eligible; ATTACK→FOLLOW may divert through
    //            this state if a drop just landed. See
    //            Claude/Plans/ai_follower_item_pickup.md.
    enum class FollowerAIState {
        IDLE, FOLLOW, STUCK, ENGAGE, ATTACK,
        CLIMBING, BLOCK, RANGED_ATTACK, STANDBY,
        COLLECT_ITEM,
    };
    FollowerAIState followerAIState     = FollowerAIState::IDLE;
    int             followerStateFrames = 0;                     // frames spent in current state
    int             followerStuckFrames = 0;                     // frames spent in STUCK
    Vec3f           followerLastPos     = { 0.0f, 0.0f, 0.0f }; // position at last stuck-check
    Vec3f           followerStuckDir    = { 0.0f, 0.0f, 0.0f }; // strafe direction while STUCK (legacy; see STUCK case)
    Actor*          followerTargetEnemy = nullptr;               // current ENGAGE/ATTACK target
    Vec3f           followerMoveTarget  = { 0.0f, 0.0f, 0.0f }; // world-space position ShouldActorUpdate steers toward
    uint32_t        followerLeaderClientId = 0;                  // sticky: clientId of current leader DummyPlayer (0 = none)
    int             followerOverrunFrames = 0;                   // G10: consecutive frames distToLeader > kTeleportThreshold
    int             followerStuckCycleCount = 0;                 // G12: consecutive STUCK entries within the cycle window
    int             followerStuckCycleResetFrames = 0;           // G12: counts down each frame; resets cycle count to 0 on hit zero
    // Resume-state for STUCK recovery (Plans/follower_pursue_target_consolidation_plan.md
    // Step 2). HandleStateStuck reads this on every success-path exit
    // (SafeTerrain / JumpAcross / TrailContinues / PathAround / Retreat /
    // legacy nudge complete) instead of hardcoding FOLLOW. Set by
    // CheckStuckAndEscalate at the FOLLOW→STUCK / ENGAGE→STUCK /
    // COLLECT_ITEM→STUCK transition. Default FOLLOW preserves the
    // pre-Step-2 behavior for any code path that enters STUCK without
    // going through the helper.
    FollowerAIState followerPreStuckState = FollowerAIState::FOLLOW;
    // Edge-trigger latch for cycle-2 cursor-advance
    // (Plans/follower_subgoal_teleport_plan.md). Stores the cycle
    // count at which we last fired the cursor-advance so the same
    // cycle doesn't re-fire on every top-of-hook pass while the
    // follower remains in STUCK during cycle 2. Reset to 0 by every
    // teleport (TeleportToLeader / TeleportToNextSubgoal) and on
    // cycle-window auto-clear.
    int             followerStuckCycleAdvancedAt = 0;

    // Bug 7 / Phase B (2026-04-22) — door handoff. Shadow-track the leader's
    // last-seen position WHILE THEY WERE IN OUR ROOM, so that when the
    // leader crosses a door we know where the trigger was. On G11 room
    // divergence, instead of immediate deactivate we:
    //   (1) override followerMoveTarget = followerLeaderLastInOurRoom,
    //   (2) walk the follower toward it via FOLLOW (Phase A BTN_A handles
    //       the door open if a prompt fires),
    //   (3) clear the handoff if we end up in the leader's room,
    //   (4) on timeout, teleport to leader and clear.
    bool            followerDoorHandoff           = false;
    int             followerDoorHandoffFrames     = 0;
    Vec3f           followerLeaderLastInOurRoom   = { 0.0f, 0.0f, 0.0f };

    // Bug C (log 69) — ladder/vine dismount forward-hold. After CLIMBING→IDLE,
    // Link is standing on the rim of the destination floor. If we
    // immediately recompute the move target toward the leader, the follower
    // often turns and walks backward off the ledge (leader was standing
    // near the climb exit). Hold forward stick for kClimbDismountFrames
    // AFTER exiting CLIMBING so Link walks inward past the rim first.
    //
    // Counter is armed at CLIMBING→IDLE, decremented every frame in
    // ShouldActorUpdate, and overrides the normal stick math when > 0.
    // The held direction is a snapshot of the climb-exit forward vector
    // (yaw at the moment of dismount) so it stays stable regardless of
    // follower-state churn during the hold.
    int             followerClimbDismountFrames   = 0;
    s16             followerClimbDismountYaw      = 0;     // shape.rot.y at dismount — inject forward into this yaw

    // Test 5 (log 71, Bug: "teleport offset / stuck in wall"). After a
    // teleport (G10 leash / G11 handoff / G12 stuck), the follower lands
    // on the leader's position and IDLE immediately recomputes
    // `sideTarget = leaderPos + kFollowOffset` — which can be inside
    // geometry if the leader is standing near a wall. ShouldActorUpdate
    // then drives stick-forward toward that sideTarget and Link collides.
    // STUCK then fires, cycles reach escalation threshold, G12 re-teleports,
    // and the loop runs 26+ times (P2 log 71 Kokiri Forest 00:09:19).
    //
    // Mitigation: set a short hold counter on every teleport. While
    // followerPostTeleportFrames > 0, ShouldActorUpdate zeroes the stick
    // so Link stays where the teleport landed him. Decrement each tick.
    // Gives the physics/collision system a beat to settle before the
    // state machine moves him again; also breaks the teleport-loop
    // cadence by starving the stuck-cycle counter's active window.
    int             followerPostTeleportFrames    = 0;

    // Test 10 (log 79, Bug 1). Phase A injects BTN_A on frame N when
    // doorType != NONE. Player_Update (also frame N, after ShouldActorUpdate)
    // consumes the press to start the door-open animation and CLEARS
    // doorType. OnGameFrameUpdate's deactivate-check (end of frame N) reads
    // press.button = BTN_A, evaluates doorType — already NONE — so the
    // mask doesn't strip the bit → `state=IDLE press=0x8000` self-cancel.
    //
    // Fix: whenever Phase A injects BTN_A (door prompt OR DO_ACTION_ENTER),
    // arm this counter. While > 0, deactivate-check masks BTN_A
    // unconditionally. Covers the mid-frame race without needing to know
    // exactly which frame doorType transitions.
    int             followerDoorPressCooldown     = 0;

    // Test 10 (log 79, Bug 2). Room number where `followerLeaderLastInOurRoom`
    // was recorded. Defaults to -1 (unknown). Set every frame rooms match.
    // The G11 arm-edge teleport uses world.pos (same-room only); if the
    // follower has already crossed a room boundary before the handoff fires
    // (e.g. follower fell into Mad Scrub hole on their own), world.pos
    // teleport puts Link at the old-room coordinates while roomCtx stays
    // at the new room — broken-state loop. Guard: only teleport if the
    // follower's current room matches the room this shadow was recorded
    // in. If mismatch, skip the teleport and let G10/G12 handle via
    // scene-reload (respawn pipeline).
    s8              followerLeaderLastInOurRoomNumber = -1;

    // Test 6 (user request 2026-04-22). G14 — close-to-leader fail-timeout.
    // Complements G10 (hard XYZ leash exceeds threshold) and G12 (STUCK
    // cycle escalation). G14 catches the slow-drift case: follower keeps
    // trying to close on leader but geometry / state-machine churn
    // prevents meaningful progress, and G10's threshold (1200 u) is too
    // large to trigger. If follower is outside `kG14MinDistance` from
    // leader AND hasn't reduced distance by `kG14ProgressDelta` in
    // `kG14TimeoutFrames` (10 s), teleport.
    //
    //   followerCloseFailBaseline — distance snapshot at last "progress"
    //       moment. Reset to current distance whenever follower gets
    //       meaningfully closer, OR leaves a movement state.
    //   followerCloseFailFrames   — frames since the baseline was set.
    //       When >= kG14TimeoutFrames, fire the teleport.
    f32             followerCloseFailBaseline     = 0.0f;
    int             followerCloseFailFrames       = 0;

    // P3.3 (user 2026-05-09) — G15 hang-state safety teleport.
    // Counts continuous frames the follower has been in
    // PLAYER_STATE1_HANGING_OFF_LEDGE WITHOUT transitioning into the
    // CLIMBING_LEDGE hoist. Long persistence indicates the hang-state
    // resolution (hold-on/let-go decision) failed to fire or the
    // follower can't reach the climb threshold; teleport to leader
    // breaks the deadlock. Resets to 0 on any non-hang frame.
    int             followerHangFrames            = 0;

    // Bug 1 (user 2026-05-10 — "When P2 AI Follower attempts to climb onto
    // a ledge, it lets go/drops down immediately, and disables the AI
    // Follower system"): post-climb-exit BTN_A mask hold counter. The
    // climb-state-flags mask at Follower.cpp:560-571 exempts BTN_A while
    // stateFlags1 & (HANGING_OFF_LEDGE | CLIMBING_LEDGE | CLIMBING_LADDER)
    // — but those flags can clear one frame BEFORE OoT consumes BTN_A
    // from press.button. The deactivate-check then sees BTN_A unmasked
    // and turns the follower off. Mirror of `followerDoorPressCooldown`
    // for door-injection BTN_A: armed on the climb-state-flags
    // true→false edge in HookHandlers.cpp's existing isClimbing edge
    // detector; OR'd into the BTN_A mask condition for as long as it's
    // > 0; decremented each TickFollower tick. Default 0.
    int             followerClimbExitCooldown     = 0;

    // P3.8 part 2 / P3.6 (user 2026-05-09) — autonomous climb mode.
    // When the substrate path consumer detects the follower is at a
    // climb anchor (vine wall / ladder / climbable surface) and the
    // path goes up, CLIMBING is entered with followerAutonomousClimb
    // = true. The climb top position is captured into
    // followerClimbTopTarget so the existing CLIMBING input-injection
    // (which uses followerMoveTarget for stick_y direction) drives
    // upward autonomously. Exit when follower Y reaches the top
    // tolerance OR when the autonomous-frames safety counter elapses.
    // Cleared when CLIMBING exits.
    bool            followerAutonomousClimb       = false;
    Vec3f           followerClimbTopTarget        = { 0.0f, 0.0f, 0.0f };
    int             followerAutonomousClimbFrames = 0;

    // Climb-state stuck detection (2026-05-12 PM, log 81 fix). The
    // existing CheckStuckAndEscalate only runs in FOLLOW/ENGAGE/
    // COLLECT_ITEM — CLIMBING state had no progress-based exit and the
    // follower could loop in autonomous-climb indefinitely after
    // falling off a wall. These fields drive a 2-second 3D-progress
    // check in HandleClimbStateAutonomous: snapshot position on
    // engagement, every kClimbStuckInterval (120 frames) compare
    // current 3D pos to snapshot. If 3D displacement <
    // kClimbStuckMinProgress, exit climb. 3D metric (not Y-only) so
    // lateral climbing on vine walls / curved-wall corners counts as
    // progress and doesn't false-trigger the exit.
    Vec3f           followerClimbStuckCheckPos    = { 0.0f, 0.0f, 0.0f };

    // Mantle hold (2026-05-13, log 103 fix). Counts down frames while
    // the follower is at the top of a vine/ladder climb segment with
    // a non-climb (drop or floor) waypoint next. During the hold, the
    // follower stays in CLIMBING state with followerClimbTopTarget
    // raised above the current position so the CLIMBING-aware input
    // injection drives raw stick_y=+127 — OoT interprets this as
    // "climb up" and triggers the mantle animation when the follower
    // is at the top of a vine with a platform above. After the hold
    // expires, ExitFollowerClimbToIdle fires and the follower
    // transitions to IDLE → FOLLOW for the next path segment.
    //
    // Pre-fix the substrate-driven exit transitioned to IDLE
    // immediately, then armed kClimbDismountHoldFrames which uses
    // CAMERA-RELATIVE stick (not raw stick_y) — that doesn't
    // trigger mantle; OoT just steered Link in some world direction
    // and he released the wall.
    int             followerMantleHoldFrames      = 0;

    // Edge-prediction state (climb_surface_nav_grid_plan post-Stage-8
    // 2026-05-12; refactored to nearest-node-3D 2026-05-12 PM). When
    // the substrate engages CLIMBING, we capture which ClimbAnchor the
    // subgoal belonged to AND pre-build a flat list of every climb-
    // surface node position the follower can reach from there: the
    // active anchor's own grid cells PLUS any node positions on
    // neighbouring anchors that share an inter-anchor bridge edge with
    // the active anchor (DetectInterAnchorClimbBridges).
    //
    // Per-frame stick injection's edge-prediction gate predicts the
    // follower's next-frame position along anchor.planeAxisU/V and
    // suppresses the stick if the prediction is more than
    // kClimbNearestNodeThreshold (~30u) from any node in this list.
    //
    // Why nearest-node not planar (u, v) projection: planar projection
    // breaks when the follower drifts off the wall plane (e.g. mid-
    // bridge-crossing, where Link slides around a curved wall corner
    // and his world.pos sits between two planar facets). The 3D
    // proximity check works for any climbable topology, including
    // bridge-connected adjacent anchors, without needing per-anchor
    // bookkeeping or explicit bridge-edge lookups in the hot path.
    //
    // followerClimbAnchorIdx == UINT16_MAX means "no active anchor"
    // (legacy non-substrate engagement, or before Stage 6 fires).
    // Cleared on CLIMBING exit and on re-anchor; refilled by
    // PopulateClimbReachableNodes.
    uint16_t                       followerClimbAnchorIdx  = UINT16_MAX;
    std::vector<Vec3f>             followerClimbReachableNodes;

    // Detachment detection — tracks the follower's Y last frame while
    // CLIMBING. A sudden drop (>kClimbDetachDrop u per frame) indicates
    // OoT released the ladder/vine grab (geometry mismatch — substrate
    // says climbable, OoT's collision polys disagree). Resets to a
    // sentinel on CLIMBING exit so the first frame after entry doesn't
    // false-trigger. Initialized to a large negative number so the
    // first-frame delta is positive.
    f32                            followerClimbPrevY      = -1.0e9f;

    // Re-anchor latch state. Prevents per-frame flip-flopping between
    // touching climb anchors when the leader crosses a seam — without
    // the latch, alternating "closest anchor" decisions invert the
    // CLIMBING stick injection per frame and the follower nets zero
    // motion. Latch requires the candidate to win for kReanchorLatchFrames
    // consecutive frames before the switch fires.
    uint16_t                       followerReanchorCandidateIdx   = UINT16_MAX;
    uint16_t                       followerReanchorCandidateFrames = 0;

    // Phase 2 — held NavPath snapshot for follower pursuit. Refreshed when
    // stale (path empty / cursor exhausted), when the target's TrailKey
    // changes (leader → enemy, leader changed, target enemy changed),
    // when the destination position drifts significantly, or on scene
    // change. Shared between HandleStateFollow (leader's side-target) and
    // HandleStateEngage (enemy position with per-enemy standoff). The
    // per-state target-key disambiguates which context most recently
    // refreshed the path; switching state implicitly invalidates the
    // path via the key check. Consumed only when
    // AnchorFollower::IsAiFollowerNavSubstrateEnabled() returns true.
    AnchorNav::ActorTrail::NavPath followerNavPath;
    AnchorNav::TrailKey            followerNavPathTargetKey = 0;             // captured at last refresh
    Vec3f                          followerNavPathLastTarget = { 0, 0, 0 };  // detect significant target movement

    // Item pickup (Claude/Plans/ai_follower_item_pickup.md). An IDLE/FOLLOW
    // tick scans ACTORCAT_MISC for ACTOR_EN_ITEM00 drops. Eligible drops
    // (need-filtered; see FollowerWantsItem in HookHandlers.cpp) trigger a
    // transition to COLLECT_ITEM. The grace-period map keys on actor
    // pointer → first-seen frame; entries whose pointers disappear from
    // the scan are purged (handles pointer reuse).
    Actor*                            followerTargetItem               = nullptr;
    std::unordered_map<Actor*, int>   itemFirstSeenFrame;
    int                               followerCollectItemTimeoutFrames = 0;
    // Monotonic per-Anchor tick counter. Increments once per
    // OnGameFrameUpdate tick. Used for grace-period tracking
    // (itemFirstSeenFrame compares to this, NOT followerStateFrames
    // which resets on state transition).
    int                               followerTickCounter              = 0;

    // Item-override system (Option B). When
    // CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems") is enabled, the follower
    // can temporarily override the player's C-button item assignments to use
    // items it needs (slingshot/bow for RANGED_ATTACK; fairy/potion for the
    // future revive system). Snapshot is taken on first override and restored
    // on state exit OR on SetFollowerActive(false).
    //
    // Touches gSaveContext.equips.buttonItems[1..3] (C-left/down/right) and
    // gSaveContext.equips.cButtonSlots[0..2]. B-button (index 0) is never
    // touched — always stays as the sword.
    bool            followerItemOverrideActive  = false;
    u8              savedButtonItems[4]         = { 0xFF, 0xFF, 0xFF, 0xFF }; // [0]=B(unused), [1..3]=C-slots
    u8              savedCButtonSlots[3]        = { 0, 0, 0 };
    u8              followerActiveCSlot         = 0xFF; // 0=C-left, 1=C-down, 2=C-right; 0xFF = none

    // Phase C — pending SCENE_TRANSITION_HANDOFF replay (see #169). Set on
    // receipt of the packet; consumed when we're close enough to the trigger
    // point to fire our own transitionTrigger. Timeout clears the pending
    // state and falls back to the G11 manual-walkthrough behaviour.
    bool            hasPendingTransition        = false;
    s16             pendingTransitionFromScene  = 0;
    s16             pendingTransitionEntrance   = 0;
    Vec3f           pendingTransitionPos        = { 0.0f, 0.0f, 0.0f };
    s16             pendingTransitionRotY       = 0;
    int             pendingTransitionTimeoutFrames = 0;          // decrement each tick; 0 → clear
    // Leader-side edge detection: previous frame's transitionTrigger value.
    // Non-zero when we're already inside a transition sequence; we only fire
    // the handoff on the rising edge (OFF→START).
    s32             prevTransitionTrigger       = 0;

    nlohmann::json PrepClientState();
    nlohmann::json PrepRoomState();
    void RegisterHooks();
    // Follower hook (re-)registration. Body lives in AIFollower/Follower.cpp
    // (Phase 1 commit 13 of the SRP refactor — moved from RegisterHooks
    // body in HookHandlers.cpp). Called from RegisterHooks at the same
    // point in the enable/disable cycle. The two registrations (OnGameFrameUpdate
    // for the state machine, ShouldActorUpdate for input injection) are both
    // re-registered on every Anchor enable/disable to track isConnected.
    void RegisterFollowerHooks(bool isConnected);
    void RefreshClientActors();
    // Backfill EnemyNetId extensions on actors that are already loaded but
    // missing the extension. Recovery path for the case where a scene init
    // ran during a disconnected window (OnActorSpawn hook unregistered, no
    // netIds assigned to setup actors). Called from OnConnected after
    // reconnect so subsequent ENEMY_UPDATE packets find their targets.
    void BackfillEnemyNetIds();
    void SetDummyPlayerClientId(const Actor* actor, uint32_t clientId);

    void HandlePacket_AllClientState(nlohmann::json payload);
    // Pillar C2 Phase 4 — unified ENEMY_STATE handler. Sole entry point
    // for incoming enemy lifecycle packets; dispatches by phase to the
    // per-phase helpers below. All bodies live in
    // EnemyStateSync/Packets/EnemyState.cpp.
    void HandlePacket_EnemyState(nlohmann::json payload);
    // Per-phase helpers — invoked only by HandlePacket_EnemyState's
    // dispatcher; not registered as packet types in OnIncomingJson.
    void HandlePacket_EnemyUpdate(nlohmann::json payload);    // phase=Alive       phaseChanged=false
    void HandlePacket_EnemyDefeated(nlohmann::json payload);  // phase=DyingByLocal phaseChanged=true
    void HandlePacket_EnemySpawn(nlohmann::json payload);     // phase=Alive       phaseChanged=true
    void HandlePacket_EnemyRespawn(nlohmann::json payload);   // phase=Regrowing   phaseChanged=true
    // KB-18 (#177) Option 4 — host-authoritative netId snapshot.
    void HandlePacket_SceneActorNetIds(nlohmann::json payload);
    void HandlePacket_DamageEnemy(nlohmann::json payload);

    // #190 — drain queued DAMAGE_ENEMY damage from EnemyNetId pending
    // fields onto the actor's colChkInfo + AC_HIT. Called from
    // ShouldActorUpdate (host-side) before the actor's own update runs,
    // so UpdateDamage consumes the synthetic hit on the same frame
    // the world resumes. No-op when ext->pendingSyncDamage == 0.
    void DrainPendingSyncDamage(Actor* actor);
    void HandlePacket_ConsumeAdultTradeItem(nlohmann::json payload);
    void HandlePacket_DamagePlayer(nlohmann::json payload);
    void HandlePacket_DisableAnchor(nlohmann::json payload);
    void HandlePacket_EntranceDiscovered(nlohmann::json payload);
    void HandlePacket_GameComplete(nlohmann::json payload);
    void HandlePacket_GiveItem(nlohmann::json payload);
    void HandlePacket_OcarinaSfx(nlohmann::json payload);
    void HandlePacket_PlayerSfx(nlohmann::json payload);
    void HandlePacket_PlayerUpdate(nlohmann::json payload);
    void HandlePacket_RequestTeamState(nlohmann::json payload);
    void HandlePacket_RequestTeleport(nlohmann::json payload);
    void HandlePacket_ServerMessage(nlohmann::json payload);
    void HandlePacket_SetCheckStatus(nlohmann::json payload);
    void HandlePacket_SetFlag(nlohmann::json payload);
    void HandlePacket_TeleportTo(nlohmann::json payload);
    void HandlePacket_UnsetFlag(nlohmann::json payload);
    void HandlePacket_UpdateBeansCount(nlohmann::json payload);
    void HandlePacket_UpdateClientState(nlohmann::json payload);
    void HandlePacket_UpdateDungeonItems(nlohmann::json payload);
    void HandlePacket_UpdateRoomState(nlohmann::json payload);
    void HandlePacket_UpdateTeamState(nlohmann::json payload);
    void HandlePacket_SceneTransitionHandoff(nlohmann::json payload);
    // Pillar C v1
    void HandlePacket_WorldFlagSet(nlohmann::json payload);
    void HandlePacket_WorldFlagUnset(nlohmann::json payload);
    void HandlePacket_WorldStateRequest(nlohmann::json payload);
    void HandlePacket_WorldStateSnapshot(nlohmann::json payload);

    // Test 1.5 exit-gated fix — broadcast on host when a scene becomes
    // unoccupied. Clears mSceneDeaths/mDefeatBroadcasts on host and
    // pendingKillNetIds for that scene on every client.
    void SendPacket_SceneDeathsCleared(int16_t sceneNum, uint8_t timeline);
    void HandlePacket_SceneDeathsCleared(nlohmann::json payload);

    // Team co-warp on synced-boss-room exit. When any team member walks
    // into the dungeon-clear blue warp, every teammate currently in the
    // same boss room is force-transitioned to the same destination so
    // their post-fight exit stays grouped. Receivers apply silently
    // (no fade VFX) so the transition feels seamless. Fired from the
    // existing transitionTrigger OFF→START edge in OnGameFrameUpdate
    // and gated by IsSyncedBossExit(sceneNum, entranceIndex).
    void SendPacket_BossExitTeamWarp(s16 sourceSceneNum, s16 entranceIndex,
                                     u16 nextCutsceneIndex, s8 transitionType,
                                     s8 nextTransitionType);
    void HandlePacket_BossExitTeamWarp(nlohmann::json payload);

  public:
    uint32_t ownClientId;

    // Pillar A Phase 1 — global effective-host migration (pure-(a)).
    //
    // Cached effective host client id. Recomputed by RecomputeEffectiveHost()
    // on every ALL_CLIENT_STATE / UPDATE_CLIENT_STATE apply that could change
    // online state. All "is this client the host?" checks across the codebase
    // read this via SceneAuthority::IsEffectiveHost() (defined in
    // Common/SceneAuthority.cpp), which compares this field to ownClientId.
    //
    // Election rule:
    //   1. If roomState.ownerClientId is in clients[] AND that client is
    //      online → effectiveHostClientId = roomState.ownerClientId.
    //   2. Else: lowest-numbered online clientId in clients[].
    //   3. Fallback (no clients online — shouldn't happen): ownClientId.
    //
    // Pure (a): once migration fires, the new host stays host even if the
    // original returns within the relay's 5-min INACTIVITY_TIMEOUT window.
    // The "pendingMigrateBack" hybrid (b) semantics from the design doc are
    // deferred to a future Phase 1.5 session.
    //
    // First migration is via the relay's HEARTBEAT/INACTIVITY detection
    // (~30-90s real time after the original host's actual TCP disconnect).
    // No explicit observation timer in v1 — adding one only affects the
    // upper end of that window.
    uint32_t effectiveHostClientId = 0;

    void RecomputeEffectiveHost();
    void OnBecameEffectiveHost();

    inline static const std::string clientVersion = (char*)gGitCommitHash;

    // Packet types //
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    // Pillar C2 Phase 4 — unified enemy lifecycle packet. Consolidates
    // the four legacy enemy packet types (ENEMY_UPDATE, ENEMY_DEFEATED,
    // ENEMY_SPAWN, ENEMY_RESPAWN) under one wire identifier with phase +
    // phaseChanged tags. All emit/receive logic lives in
    // EnemyStateSync/Packets/EnemyState.cpp.
    inline static const std::string ENEMY_STATE = "ENEMY_STATE";
    inline static const std::string DAMAGE_ENEMY = "DAMAGE_ENEMY";
    inline static const std::string DAMAGE_PLAYER = "DAMAGE_PLAYER";
    inline static const std::string ENEMY_HIT_PLAYER = "ENEMY_HIT_PLAYER";
    inline static const std::string PROJECTILE_HIT_ENEMY = "PROJECTILE_HIT_ENEMY";
    inline static const std::string TALK_REQUEST = "TALK_REQUEST";
    inline static const std::string DIALOG_END = "DIALOG_END";
    inline static const std::string BOSS_GOMA_LOOKED_AT = "BOSS_GOMA_LOOKED_AT";
    inline static const std::string MIDO_POST_DEKU_LEAVE = "MIDO_POST_DEKU_LEAVE";
    inline static const std::string CUTSCENE_TEXT_ADVANCE = "CUTSCENE_TEXT_ADVANCE";
    inline static const std::string CUTSCENE_TEXT_ADVANCED = "CUTSCENE_TEXT_ADVANCED";
    // NPC Follower (Flotilla — Plans/npc_follower_plan.md §2.9).
    // Single-owner authority: the client that toggled the CVar owns
    // its local NPC; SPAWN announces, STATE periodically broadcasts
    // pos/rot at ~10Hz, DESPAWN tears down. Peers spawn/apply/kill
    // read-only replicas keyed by ownerClientId.
    inline static const std::string FOLLOWER_NPC_SPAWN   = "FOLLOWER_NPC_SPAWN";
    inline static const std::string FOLLOWER_NPC_STATE   = "FOLLOWER_NPC_STATE";
    inline static const std::string FOLLOWER_NPC_DESPAWN = "FOLLOWER_NPC_DESPAWN";
    inline static const std::string DISABLE_ANCHOR = "DISABLE_ANCHOR";
    inline static const std::string ENTRANCE_DISCOVERED = "ENTRANCE_DISCOVERED";
    inline static const std::string GAME_COMPLETE = "GAME_COMPLETE";
    inline static const std::string GIVE_ITEM = "GIVE_ITEM";
    inline static const std::string HANDSHAKE = "HANDSHAKE";
    inline static const std::string OCARINA_SFX = "OCARINA_SFX";
    inline static const std::string PLAYER_SFX = "PLAYER_SFX";
    inline static const std::string PLAYER_UPDATE = "PLAYER_UPDATE";
    inline static const std::string REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
    inline static const std::string REQUEST_TELEPORT = "REQUEST_TELEPORT";
    inline static const std::string SERVER_MESSAGE = "SERVER_MESSAGE";
    inline static const std::string SET_CHECK_STATUS = "SET_CHECK_STATUS";
    inline static const std::string SET_FLAG = "SET_FLAG";
    inline static const std::string TELEPORT_TO = "TELEPORT_TO";
    inline static const std::string UNSET_FLAG = "UNSET_FLAG";
    inline static const std::string UPDATE_BEANS_COUNT = "UPDATE_BEANS_COUNT";
    inline static const std::string UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
    inline static const std::string UPDATE_DUNGEON_ITEMS = "UPDATE_DUNGEON_ITEMS";
    inline static const std::string UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
    inline static const std::string UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";
    // Leader→follower scene-transition handoff. Sent by the leader on edge-
    // detect of `gPlayState->transitionTrigger` OFF→START. Payload carries the
    // source sceneNum, destination nextEntranceIndex, and the world-space
    // position (+rotation) of the trigger in the source scene so the follower
    // can navigate there and fire its own transition. Supersedes the G13
    // boss-room deactivate path and unblocks cross-scene doors / grotto /
    // crawlspace entry. See #169.
    inline static const std::string SCENE_TRANSITION_HANDOFF = "SCENE_TRANSITION_HANDOFF";

    // BOSS_EXIT_TEAM_WARP — team-routed scene transition for synced boss
    // exits. When a team member enters the dungeon-clear blue warp, all
    // teammates in the same boss room transition to the same destination
    // entrance / next-cutscene, in lockstep. Routed via `targetTeamId`.
    inline static const std::string BOSS_EXIT_TEAM_WARP = "BOSS_EXIT_TEAM_WARP";

    // Pillar C v1 — globally-replicated world state. Plan in
    // Claude/Plans/pillar_c_worldstatesync.md.
    inline static const std::string WORLD_FLAG_SET        = "WORLD_FLAG_SET";
    inline static const std::string WORLD_FLAG_UNSET      = "WORLD_FLAG_UNSET";
    inline static const std::string WORLD_STATE_REQUEST   = "WORLD_STATE_REQUEST";
    inline static const std::string WORLD_STATE_SNAPSHOT  = "WORLD_STATE_SNAPSHOT";

    // Test 1.5 exit-gated fix — host broadcasts when a scene becomes
    // unoccupied (last player + host all transitioned out). Receivers
    // clear scene-scoped buffered state for that scene so the next
    // entrant sees fresh actor pool (vanilla respawn parity).
    inline static const std::string SCENE_DEATHS_CLEARED  = "SCENE_DEATHS_CLEARED";

    // KB-18 (#177) Option 4 — host-authoritative netId snapshot. Host
    // broadcasts the per-static-actor netId table on scene-spawn so non-
    // hosts override their locally-computed (potentially divergent) netIds
    // with the host's value. Solves the entire class of post-init
    // home.pos non-determinism (En_Sw raycast + future actors).
    inline static const std::string SCENE_ACTOR_NETIDS = "SCENE_ACTOR_NETIDS";

    // ITEM_DROP_SYNC — host → all clients (team-broadcast). Sent on
    // each host-side `OnActorSpawn(ACTOR_EN_ITEM00)` for transient
    // drop types (rupees, hearts, ammo, magic, sticks, nuts, seeds).
    // Carries the drop's authoritative netId, ITEM00_* params, world
    // position, killerClientId (the player who triggered the drop —
    // captured at the `Item_DropCollectible*` call site via the
    // Anchor_BeginItemDrop / EndItemDrop shim), and spawnTimeMs (host
    // monotonic clock at drop time, used for the killer-exclusive
    // grace window). Receivers spawn a matching local EnItem00 with
    // an `ItemDropNetId` extension stamped from the payload. See #193.
    inline static const std::string ITEM_DROP_SYNC = "ITEM_DROP_SYNC";

    // ITEM_COLLECTED — collector client → all (team-broadcast). Sent
    // when a local pickup gate passes and the player's gSaveContext
    // has been credited. Receivers walk their actor list for the
    // matching itemNetId and Actor_Kill the local copy. Per-player
    // pickup attribution: only one client claims the drop. See #193.
    inline static const std::string ITEM_COLLECTED = "ITEM_COLLECTED";

    // ITEM_DROP_SNAPSHOT — host → joining peer (targeted). Late-join
    // replay of in-flight EN_ITEM00 drops in the joining peer's scene.
    // Sent when a peer transitions into the host's scene (UPDATE_CLIENT_STATE
    // edge): host walks ACTORCAT_MISC for live EN_ITEM00 with
    // ItemDropNetId, packs the list, sends to the named client. Mirrors
    // the SCENE_ACTOR_NETIDS pattern. See #193 Phase 5.
    inline static const std::string ITEM_DROP_SNAPSHOT = "ITEM_DROP_SNAPSHOT";

    // ITEM_PICKUP_REQUEST — peer → room host (targeted). Race A
    // mitigation. Sent when peer's local pickup gate passes Layers 1+2
    // (3s exclusivity + per-player eligibility). Peer's vanilla pickup
    // is suppressed; peer waits for host's ITEM_COLLECTED arbitration
    // broadcast. Host walks for the matching netId; if alive, kills
    // the drop locally (claim) and broadcasts ITEM_COLLECTED with the
    // sender's clientId; if dead (already collected by host or another
    // peer), drops the request silently. Eliminates the post-window
    // simultaneous-pickup double-credit.
    inline static const std::string ITEM_PICKUP_REQUEST = "ITEM_PICKUP_REQUEST";

    // ENV_ACTOR_DROP — peer → host (targeted). Phase 4 v2. When peer's
    // local env actor (En_Kusa grass, Obj_Mure cluster child, etc.) is
    // cut/destroyed and would have dropped an item, peer suppresses
    // its local `Item_DropCollectible*` call and broadcasts this packet
    // with the env actor's netId + the drop param peer chose. Host
    // walks its actor list for the matching netId; if the actor is
    // still alive on host (peer's cut beat host's own potential cut
    // of the same shrub), host fires `Item_DropCollectible` with the
    // peer-supplied param, attributed to peer. The OnActorSpawn
    // EN_ITEM00 hook then broadcasts ITEM_DROP_SYNC. Net result: one
    // drop per cut event, regardless of which client triggered it.
    // See #193 Phase 4 v2.
    inline static const std::string ENV_ACTOR_DROP = "ENV_ACTOR_DROP";

    // HEARTBEAT — every client → all clients. Sent every ~2s from the
    // network thread (NOT the game thread) so it survives game-thread
    // freezes (textbox stuck, cutscene gate, pause). Carries:
    //   - sendTimeMs: monotonic-clock timestamp at network-thread send,
    //     used by receivers to detect connection liveness.
    //   - gameFrameCounter: monotonic counter incremented from
    //     OnGameFrameUpdate, used by receivers to detect game-thread
    //     liveness (counter stale → game frozen, but heartbeats
    //     arriving → connection alive).
    // The two-axis liveness model lets peers distinguish "client offline"
    // from "client connected but frozen" — surfaced via
    // IsClientLikelyFrozen() and IsClientGameFrozen() respectively.
    // See #194 for the originating bug class (host's send loop stalled
    // during a hintnut dialog freeze, relay timed out, peers had no
    // signal until reconnect).
    inline static const std::string HEARTBEAT = "HEARTBEAT";

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    RoomState roomState;

    // Disable-time graveyard for BakedPlayerModel and customSkeleton.
    // KB-15 / issue #110: clients.clear() in Disable() destroys baked Gfx
    // allocations while the renderer's pipeline still has commands in flight
    // referencing them — the GPU walks freed memory next frame and emits a
    // flood of "Unhandled OP code" errors before crashing on a bad pointer
    // (log 119: 0xC9 0x78 0x64 ucode bytes → 0xC0000005). The per-AnchorClient
    // retire slot can't help because clearing the map destroys both slots
    // simultaneously. Disable() moves these allocations here before clear()
    // so they outlive the in-flight Gfx; Enable() drains them (UI-driven
    // toggle always takes >> kRetireFrames frames, so safe to free by then).
    std::vector<std::unique_ptr<SOH::BakedPlayerModel>> postDisableBakedModels;
    std::vector<std::shared_ptr<SOH::Skeleton>> postDisableCustomSkeletons;

    void Enable();
    void Disable();
    void OnIncomingJson(nlohmann::json payload);
    void OnConnected();
    void OnDisconnected();
    void ProcessOutgoingPackets();
    void DrawMenu();
    void ProcessIncomingPacketQueue();
    void SendJsonToRemote(nlohmann::json packet);
    bool IsSaveLoaded();
    bool CanTeleportTo(uint32_t clientId);
    uint32_t GetDummyPlayerClientId(const Actor* actor);
    bool IsFollowerActive() const { return followerActive; }
    void SetFollowerActive(bool active);

    // NPC Follower (Flotilla — Plans/npc_follower_plan.md). Phase 2 wiring:
    // spawn an ACTOR_EN_FOLLOWER at the local player's position on
    // active=true; Actor_Kill the tracked instance on active=false.
    // Idempotent: passing the same state twice is a no-op.
    void SetFollowerNpcActive(bool active);
    // Public accessor — returns the local NPC's actor pointer (or
    // nullptr if no local NPC is alive). Used by the per-tick state-
    // machine driver to distinguish owner-tick from peer-replica-tick.
    Actor* GetFollowerNpcLocalActor() const { return mFollowerNpcLocalActor; }
    // Stage 3 — true when the local NPC follower exists, is alive,
    // and is not flagged invulnerable. Used by FindNearestPlayerActor
    // to include the NPC as a target candidate for enemy AI. When
    // false (no NPC, dead, or invulnerable) the NPC is invisible to
    // targeting and enemies behave as before. Implementation in
    // AIFollowerNPC/FollowerNPC.cpp (needs the CVar read helper).
    bool IsFollowerNpcTargetable() const;
    // Color-fix lookup: given an NPC Actor*, return the ownerClientId
    // (== ownClientId for our local NPC; otherwise the peer
    // ownerClientId from mPeerFollowerNpcs). Returns 0 if the actor
    // isn't a tracked NPC. Used by the VB_APPLY_TUNIC_COLOR hook to
    // pick the right color (own-color CVar for local; client.color
    // for peer).
    uint32_t FindFollowerNpcOwner(Actor* npc) const;
    // Scene-transition pointer cleanup. OoT scene reload destroys the
    // actor list; our cached Actor* pointers become dangling. This
    // explicit clear (called from a OnSceneSpawnActors hook) is the
    // safe alternative to checking actor->update on a possibly-freed
    // pointer.
    void ClearFollowerNpcSceneCache();
    // CVar-transition polling helper, called from OnGameFrameUpdate.
    // Compares the current CVar value against mFollowerNpcCVarLast and
    // calls SetFollowerNpcActive on edges. Standalone so future spawn
    // mechanisms (save-state recruit flag, per-scene anchors per plan
    // §2.7) can swap out this driver without touching the spawn impl.
    void TickFollowerNpcCVar();
    // G1/G2 — read local Player's stateFlags1 to detect ladder/vine/ledge climbing.
    // Returns false when there is no live PlayState or local Player. Used in
    // PrepClientState so remote clients see this client's climbing state.
    bool IsLocalPlayerClimbing() const;
    // Nav system (plan §9 / commit 6a) — read the broadcast `isClimbing` flag
    // for a remote client by clientId. Returns false when the clientId is
    // not present in the clients map (e.g. caller passed 0 / unknown id) or
    // the field hasn't been set since the last UPDATE_CLIENT_STATE.
    bool GetClientIsClimbing(uint32_t clientId) const;

    // Test 5 (log 71) — whether the local Player is in a crawlspace.
    // Returns false when there is no live PlayState or local Player.
    bool IsLocalPlayerCrawling() const;

    // Option B — follower item override (see #169 polish list).
    // FollowerTryEquipRangedWeapon: if the CVar is enabled and Link has a
    // slingshot (child form) or bow (adult form) in inventory, snapshot the
    // current C-button loadout and assign the ranged weapon to C-left
    // (cSlot=0). Returns the C-slot used (0), or 0xFF if not available or
    // the CVar is off. Safe to call repeatedly — no-op if override already
    // active.
    u8   FollowerTryEquipRangedWeapon();
    // Restore the player's original C-button loadout. No-op if no override
    // is active. Called on every RANGED_ATTACK exit path and unconditionally
    // on SetFollowerActive(false) as a safety net.
    void FollowerRestoreItems();

    // Per-frame follower state-machine entry point. Body lives in
    // AIFollower/Follower.cpp (Phase 1 commit 4 of the SRP refactor —
    // moved verbatim from the OnGameFrameUpdate lambda body in
    // HookHandlers.cpp). Method on Anchor:: rather than a free function
    // so the body's accesses to private members (followerActive, clients,
    // followerAIState, etc.) work unchanged.
    //
    // Caller (HookHandlers.cpp's OnGameFrameUpdate hook) does the early-
    // out checks (effective-host, save-loaded, gPlayState non-null,
    // player non-null), populates `ctx`, then calls TickFollower(ctx).
    // ctx is currently used minimally; future per-state-extraction
    // commits expand its use.
    void TickFollower(AnchorFollower::FollowerFrameContext& ctx);

    // Per-frame follower input-injection entry point. Body lives in
    // AIFollower/Follower.cpp (Phase 1 commit 5 of the SRP refactor —
    // moved verbatim from the ShouldActorUpdate lambda body in
    // HookHandlers.cpp). Caller (HookHandlers.cpp's ShouldActorUpdate
    // hook) does the early-outs (followerActive, gPlayState non-null,
    // actor->id == ACTOR_PLAYER) and passes the player Actor*.
    //
    // ShouldActorUpdate fires immediately BEFORE the player's update()
    // each frame, so input written here is consumed by Player_Update on
    // the same frame. Driving locomotion / sword swings / climb-ups all
    // happen via stick + button injection on input[0].
    void TickFollowerInput(Actor* actor);

    // Per-state handlers — peeled off TickFollower's switch (Phase 1
    // commit 6+ of the SRP refactor). Each handler reads/writes
    // Anchor:: state directly through `this` and takes explicit
    // parameters for parent-function locals it needs (e.g., player,
    // p2Pos). Future commits in Phase 1 add more handlers; eventually
    // TickFollower's switch becomes a thin dispatcher.
    void HandleStateStandby();
    void HandleStateBlock(Player* player, const Vec3f& p2Pos);
    void HandleStateClimbing(Player* player, const Vec3f& leaderPos, Actor* leaderActor);
    // CLIMBING sub-phases (R1 of follower_nav_refactor). Same-file
    // helpers, used by HandleStateClimbing as the thin dispatcher.
    void HandleClimbStateAutonomous(Player* player, const Vec3f& leaderPos);
    void HandleClimbStateLeaderFollowing(Player* player, const Vec3f& leaderPos, Actor* leaderActor);
    // Compute the dismount yaw from the active climb anchor's topPos
    // toward the nearest floor node. Falls back to the player's current
    // shape.rot.y when no active anchor is set or no nearby floor
    // exists. Encapsulates the geometry the two CLIMBING exit sites
    // previously duplicated.
    s16  ComputeFollowerDismountYaw(Player* player) const;
    // Apply the state-machine cleanup common to every CLIMBING→IDLE
    // exit. Writes followerClimbDismountYaw + arms the dismount
    // forward-hold counter, transitions to IDLE, clears the anchor
    // index + reachable-nodes list, and (when `clearAutonomous`)
    // zeroes the autonomous-climb tracking fields too.
    void ExitFollowerClimbToIdle(s16 dismountYaw, bool clearAutonomous);
    // Repopulate followerClimbReachableNodes from the active anchor's
    // grid PLUS any cell positions on bridge-connected neighbour
    // anchors. Called from every CLIMBING engagement site AND from
    // the in-flight re-anchor switch (so the gate stays seeded after
    // the active anchor flips). No-op when navData is null or
    // anchorIdx is out of range — caller should clear the list before
    // dispatching when the failure semantics matter.
    void PopulateClimbReachableNodes(
        const ::AnchorNavRoom::RoomNavData* navData, uint16_t anchorIdx);
    void HandleStateStuck(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos);
    void HandleStateRangedAttack(Player* player, const Vec3f& p2Pos);
    void HandleStateCollectItem(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos);
    void HandleStateIdle(Player* player, Actor* dummyActor, const Vec3f& sideTarget, const Vec3f& p2Pos);
    void HandleStateFollow(Player* player, const Vec3f& sideTarget, const Vec3f& p2Pos);
    void HandleStateEngage(Player* player, const Vec3f& leaderPos, const Vec3f& p2Pos);
    void HandleStateAttack(Player* player, const Vec3f& p2Pos);

    // ─────────────────────────────────────────────────────────────────
    // Shared pursuit helpers (Plans/follower_pursue_target_consolidation_plan.md).
    // FOLLOW / ENGAGE / COLLECT_ITEM all "walk toward a target"; these
    // helpers extract the substrate pathfinding, stuck escalation, and
    // auto-CLIMBING engagement that previously lived in FOLLOW only.
    // ─────────────────────────────────────────────────────────────────

    // Step 1 — substrate-driven subgoal resolution. Computes / refreshes /
    // advances followerNavPath against the caller's finalGoal, returns
    // the world position the caller should set as followerMoveTarget.
    // May transition state to CLIMBING when the current subgoal carries
    // a NODE_CLIMB_* flag. Caller must check followerAIState after the
    // call and bail if it changed.
    //
    //   finalGoal     — destination position (leader / enemy / item)
    //   trailKey      — Layer 2 trail to walk (leader's TrailKey, or
    //                   per-enemy NetId-derived key, or 0 for static
    //                   targets like items / door positions)
    //   useDoorTarget — true only for FOLLOW with active door handoff
    //                   (skipLayer1LOS=true + preferLeaderTrail=true)
    Vec3f ComputePursuitSubgoal(Player* player,
                                 const Vec3f& p2Pos,
                                 const Vec3f& finalGoal,
                                 AnchorNav::TrailKey trailKey,
                                 bool useDoorTarget);

    // Step 2 — stuck-detection helper. Runs the FOLLOW-style progress
    // check every kStuckCheckInterval frames; on no-progress sets
    // followerAIState=STUCK + increments the cycle counter + arms the
    // post-stuck resume state. Returns true when a STUCK transition
    // fired (caller should `return` from its handler).
    //
    //   resumeState — which state to return to after STUCK recovery
    //                 completes (FOLLOW for normal pursuit; ENGAGE /
    //                 COLLECT_ITEM for combat / pickup pursuit).
    //                 Stored in followerPreStuckState; consumed by
    //                 HandleStateStuck on every recovery exit path.
    bool CheckStuckAndEscalate(const Vec3f& p2Pos,
                                FollowerAIState resumeState);

    // Step 3 — legacy 2-point auto-CLIMBING engagement. Throttled to
    // every 10 frames internally. Returns true if CLIMBING engaged
    // (caller should bail). Final goal must be meaningfully ABOVE the
    // follower (Δy > kAutonomousClimbMinHeight). Skipped for v7
    // climb-surface rooms where the substrate path's climb-flag check
    // (inside ComputePursuitSubgoal) handles engagement instead.
    bool TryEngageAutoClimb(Player* player,
                             const Vec3f& p2Pos,
                             const Vec3f& finalGoal);

    // Helper for IDLE / FOLLOW state — scans ACTORCAT_MISC for an eligible
    // EN_ITEM00 drop the follower should engage. Maintains the
    // itemFirstSeenFrame grace-period map. Returns the nearest in-range
    // item whose grace window has elapsed, or nullptr. Was a parent-
    // function lambda inside TickFollower; promoted in Phase 1 commit 10
    // so HandleStateIdle / HandleStateFollow can reference it.
    Actor* ScanForItemCandidate(Player* player);

    // Pillar C2 Phase 4 — phase-specific senders for the unified
    // ENEMY_STATE wire packet. All four emit type=ENEMY_STATE with the
    // matching phase tag; bodies live in EnemyStateSync/Packets/EnemyState.cpp.
    void SendPacket_EnemyUpdate(uint32_t netId, Actor* actor);   // phase=Alive       phaseChanged=false
    void SendPacket_EnemyDefeated(uint32_t netId);               // phase=DyingByLocal phaseChanged=true
    void SendPacket_EnemySpawn(Actor* actor);                    // phase=Alive       phaseChanged=true
    void SendPacket_EnemyRespawn(uint32_t netId);                // phase=Regrowing   phaseChanged=true
    void SendPacket_DamageEnemy(uint32_t netId, u8 damage, u8 damageEffect, u8 atHitEffect);
    void SendPacket_EnemyHitPlayer(uint32_t netId);
    void HandlePacket_EnemyHitPlayer(nlohmann::json payload);

    // PROJECTILE_HIT_ENEMY — peer → room host. Sent when a peer's local
    // projectile (currently En_Nutsball only) lands on a peer's local
    // synced enemy. Lets the host's authoritative state machine apply
    // the hit-response transition without the peer running redundant /
    // conflicting local logic. Receiver dispatches by `actor->id` to a
    // per-actor helper (matches the ENEMY_HIT_PLAYER pattern).
    void SendPacket_ProjectileHitEnemy(uint32_t targetNetId, s16 projectileActorId);
    void HandlePacket_ProjectileHitEnemy(nlohmann::json payload);

    // TALK_REQUEST — peer → room host. Sent when a peer's local Link
    // initiates dialog with a synced NPC-style actor (currently
    // En_Hintnuts only). Lets the host's authoritative state machine
    // run the natural Talk→Leave dialog cycle so peer's view doesn't
    // get reverted back to a pre-talk state by host's stale broadcasts.
    // Receiver dispatches by `actor->id` to a per-actor helper.
    void SendPacket_TalkRequest(uint32_t targetNetId);
    void HandlePacket_TalkRequest(nlohmann::json payload);

    // DIALOG_END — peer → room host. Sent when a peer's local dialog
    // with a synced NPC-style actor closes (Message_GetState returns
    // TEXT_STATE_EVENT on peer; host's local msgCtx never opened the
    // dialog). Lets the host's authoritative state machine advance the
    // actor through its post-talk transition (typically SetupLeave) so
    // peer's actor doesn't fire SetupLeave locally — which would spawn
    // a duplicate recovery heart and oscillate against host's stale
    // Talk broadcasts (logs 216 actor-flood crash).
    void SendPacket_DialogEnd(uint32_t targetNetId);
    void HandlePacket_DialogEnd(nlohmann::json payload);

    // BOSS_GOMA_LOOKED_AT — peer → room host. Sent every frame peer's
    // local `BossGoma_Encounter` case 3 sees Goma in its camera frustum
    // during the intro. Host increments its local `lookedAtFrames` so
    // either player's look triggers the boss-fall transition. See #67.
    void SendPacket_BossGomaLookedAt(uint32_t bossNetId);
    void HandlePacket_BossGomaLookedAt(nlohmann::json payload);

    // MIDO_POST_DEKU_LEAVE — team broadcast. Sent by the dialog client
    // when its local Mido transitions BlockPath → Walk for the post-
    // Deku-Tree confrontation (z_en_md.c BlockPath transition gated on
    // DEKU_TREE_DEAD + !SPOKE + KOKIRI). Peers force their local Mido
    // through the same transition so the walk-away cinematic plays on
    // every client instead of remote despawning abruptly when the
    // SetFlag(SPOKE) sync arrives. See #184 follow-up.
    void SendPacket_MidoPostDekuLeave();
    void HandlePacket_MidoPostDekuLeave(nlohmann::json payload);

    // ITEM_DROP_SYNC (#193 Phase 1) — host fans out the authoritative
    // drop after its local Item_DropCollectible* call site spawns the
    // EnItem00 actor. Receivers spawn a matching local EnItem00 with
    // an `ItemDropNetId` extension stamped from the payload. Caller
    // (host's `OnActorSpawn(ACTOR_EN_ITEM00)`) is responsible for the
    // transient-only allowlist filter (Q7) — progression items
    // (heart pieces, small keys, tunics, shields) are NOT broadcast.
    void SendPacket_ItemDropSync(uint32_t itemNetId, u8 itemParams,
                                 Vec3f pos, uint32_t killerClientId,
                                 int64_t spawnTimeMs);
    void HandlePacket_ItemDropSync(nlohmann::json payload);

    // ITEM_COLLECTED (#193 Phase 1) — local pickup gate has fired and
    // the local player's gSaveContext has been credited. Receivers
    // walk their actor list for the matching itemNetId and Actor_Kill
    // the local copy. Per-player attribution: only one client wins.
    void SendPacket_ItemCollected(uint32_t itemNetId);
    void HandlePacket_ItemCollected(nlohmann::json payload);

    // ITEM_DROP_SNAPSHOT (#193 Phase 5) — late-join replay. Host
    // enumerates live EN_ITEM00 drops in the joining peer's scene+
    // timeline and sends them targeted at `targetClientId`. Receiver
    // spawns matching local drops with extension stamped (idempotent
    // per drop netId — drops the peer already spawned locally aren't
    // duplicated).
    void SendPacket_ItemDropSnapshot(uint32_t targetClientId);
    void HandlePacket_ItemDropSnapshot(nlohmann::json payload);

    // ITEM_PICKUP_REQUEST (#193 race A mitigation) — peer → room host.
    // Sent when peer's local pickup gate passes; peer suppresses
    // vanilla pickup, transitions extension's pickupState to Pending,
    // and awaits host's ITEM_COLLECTED arbitration response.
    void SendPacket_ItemPickupRequest(uint32_t itemNetId);
    void HandlePacket_ItemPickupRequest(nlohmann::json payload);

    // ENV_ACTOR_DROP (#193 Phase 4 v2) — peer → host. Sent when peer's
    // local env actor would have dropped an item locally; peer
    // suppresses the local drop and asks host to do it instead.
    // `dropParamForRandom` distinguishes the two `Item_DropCollectible*`
    // entry points: when nonzero, host dispatches to
    // `Item_DropCollectibleRandom(play, fromActor, pos, dropParamForRandom)`
    // (vanilla En_Kusa TYPE_0/TYPE_2 path); when zero, host dispatches
    // to `Item_DropCollectible(play, pos, dropParam)` (TYPE_1 path).
    void SendPacket_EnvActorDrop(uint32_t netId, s16 dropParam,
                                 s16 dropParamForRandom, Vec3f pos);
    void HandlePacket_EnvActorDrop(nlohmann::json payload);

    // CUTSCENE_TEXT_ADVANCE — peer → effective scene host. Sent when a
    // local A/B/CUP press fires Message_ShouldAdvance during a cutscene-
    // internal textbox. Host accumulates the press into per-textbox
    // state; when all team members have pressed OR the countdown timer
    // elapses, host broadcasts CUTSCENE_TEXT_ADVANCED to all clients.
    // See #191.
    void SendPacket_CutsceneTextAdvance(uint16_t textId);
    void HandlePacket_CutsceneTextAdvance(nlohmann::json payload);

    // CUTSCENE_TEXT_ADVANCED — host → all clients. Broadcast when a
    // textbox-advance vote completes (all-pressed or timer elapsed).
    // Each receiver sets a one-shot flag that the next
    // Message_ShouldAdvance call returns true for. See #191.
    void SendPacket_CutsceneTextAdvanced(uint16_t textId, const char* reason);
    void HandlePacket_CutsceneTextAdvanced(nlohmann::json payload);

    // FOLLOWER_NPC_* — Flotilla NPC Follower companion (Plans/
    // npc_follower_plan.md §2.6 / §2.9). Single-owner authority:
    // owner sends, peers apply read-only. Phase 3 wiring.
    void SendPacket_FollowerNpcSpawn(uint32_t netId, const Vec3f& pos,
                                      const Vec3s& rot, int16_t sceneNum,
                                      int8_t roomNum, uint8_t linkAge);
    void HandlePacket_FollowerNpcSpawn(nlohmann::json payload);
    void SendPacket_FollowerNpcState();   // throttle + own-state-fetch internal
    void HandlePacket_FollowerNpcState(nlohmann::json payload);
    void SendPacket_FollowerNpcDespawn(uint32_t netId, uint8_t reason);
    void HandlePacket_FollowerNpcDespawn(nlohmann::json payload);

    // Per-tick driver for Phase 3 STATE broadcasts. Called from
    // OnGameFrameUpdate after TickFollowerNpcCVar. Cheap no-op when
    // no local NPC exists.
    void TickFollowerNpcStateBroadcast();

    // #191 — per-active-textbox vote-skip state lives on the host.
    // Reset on each new textbox (detected by textId edge or no active
    // textbox state). Cleared when CUTSCENE_TEXT_ADVANCED is broadcast.
    struct CutsceneTextAdvanceState {
        bool                   active = false;
        uint16_t               textId = 0;
        int16_t                sceneNum = -1;
        std::set<uint32_t>     pressedClientIds;
        std::chrono::steady_clock::time_point countdownEndsAt;
        bool                   countdownStarted = false;
    };
    CutsceneTextAdvanceState cutsceneTextAdvanceState;

    // Tick called from OnGameFrameUpdate (host only). Decrements
    // countdown; broadcasts CUTSCENE_TEXT_ADVANCED on timer-0.
    // Also resets state on textId-edge (new textbox).
    void TickCutsceneTextAdvance();

    // Receive-side flag — set by HandlePacket_CutsceneTextAdvanced,
    // consumed by Anchor_ShouldAdvanceCutsceneTextLocal in z_message_PAL.c
    // hook. Refreshes per textId so multi-page cutscenes don't double-
    // consume the same advance signal.
    bool     cutsceneTextAdvanceConsumed = false;
    uint16_t cutsceneTextAdvanceConsumedTextId = 0;

    // Heartbeat (#194 follow-up) — two-axis liveness signal.
    //
    // gameFrameCounter is incremented from OnGameFrameUpdate on the game
    // thread. Read from the network thread when building the heartbeat
    // payload. atomic so the cross-thread read is well-defined without
    // touching the heartbeat mutex.
    std::atomic<uint64_t> gameFrameCounter{0};

    // Stage 2 (npc_follower_health_and_respawn_plan) — respawn cooldown.
    // Public because the file-scope TickDEAD/TickFollowerNpcCVar helpers
    // in FollowerNPC.cpp need to write these. 0 = no pending respawn.
    uint64_t mFollowerNpcRespawnAtFrame = 0;
    // Optional override for the next NPC spawn position. When set,
    // SetFollowerNpcActive(true) spawns at this pose instead of the
    // local player's. Single-shot — the spawn helper clears the flag.
    bool  mFollowerNpcSpawnPosOverride = false;
    Vec3f mFollowerNpcSpawnPosOverridePos = { 0.0f, 0.0f, 0.0f };
    s16   mFollowerNpcSpawnPosOverrideYaw = 0;

    // ----- Game-tick timing infrastructure (2026-05-15 log 118 followup) -----
    //
    // SoH game logic ticks at variable rates: 20 Hz vanilla (50ms/tick),
    // up to display refresh when "Match refresh rate" is on, plus
    // VirtualBox slowdowns that can drag it lower. Hard-coded
    // "@60fps" frame counts (kStuckCheckInterval=120 etc.) ran 3-6×
    // long on the 20 Hz default.
    //
    // Solution: measure the wall-clock interval between consecutive
    // game ticks, EWMA-smooth it, and convert millisecond thresholds
    // to tick counts at the consumer site via MsToGameTicks(ms).
    //
    // Frame counters in consumers stay as game-tick integers — they
    // increment correctly per tick regardless of rate. Only the
    // threshold comparison needs the helper:
    //
    //   if (counter >= MsToGameTicks(kThresholdMs)) { ... }
    //
    // Properties:
    //   - 20 fps default → MsToGameTicks(2000) = 40 → 2 s actual.
    //   - 60 fps unlocked → MsToGameTicks(2000) = 120 → 2 s actual.
    //   - Game paused → no game tick fires → no counter increment →
    //     timers freeze. Correct behaviour.
    //   - Long hitches (>200ms / scene transitions) capped out of the
    //     EWMA so the rolling average stays stable.
    //
    // Updated at the top of OnGameFrameUpdate (HookHandlers.cpp).
    // Default 50ms (= 20 fps) so MsToGameTicks works correctly on the
    // first tick before any sample arrives.
    uint32_t mAvgGameTickMs      = 50;
    uint64_t mLastGameTickWallMs = 0;

    // Returns the number of game ticks (frame increments) that
    // approximate the given wall-clock millisecond duration based on
    // the current measured tick interval. Rounds UP so a 2000ms
    // threshold never fires earlier than 2 seconds.
    inline int MsToGameTicks(int ms) const {
        if (ms <= 0) return 0;
        const uint32_t tickMs = (mAvgGameTickMs == 0) ? 50 : mAvgGameTickMs;
        return ((int)ms + (int)tickMs - 1) / (int)tickMs;
    }

    // Network-thread-side bookkeeping. Written from the network thread
    // (TickHeartbeat sender; HandlePacket_Heartbeat receiver) and read
    // from the game thread via the IsClientLikely{Frozen,GameFrozen}
    // getters. Mutex is held only for short map operations.
    std::mutex heartbeatMutex;
    std::chrono::steady_clock::time_point lastHeartbeatTx{};
    // Per-peer last network-thread receive time. Stale → connection
    // dropped or relay-blocked.
    std::map<uint32_t, std::chrono::steady_clock::time_point> lastHeartbeatRxByClientId;
    // Per-peer last gameFrameCounter we received in their heartbeat
    // and the local time we received it. If the counter stops advancing
    // across multiple heartbeats, the peer's game thread is frozen
    // even though their network thread is fine.
    std::map<uint32_t, uint64_t> lastHeartbeatGameFrameByClientId;
    std::map<uint32_t, std::chrono::steady_clock::time_point>
        lastHeartbeatGameFrameAtByClientId;
    // Per-peer "currently flagged frozen" so transitions log once
    // instead of per-tick. Toggled inside the getters.
    std::map<uint32_t, bool> heartbeatFrozenFlaggedByClientId;
    std::map<uint32_t, bool> heartbeatGameFrozenFlaggedByClientId;

    // Heartbeat tunables. Defaults match #194 description: 2s tx
    // cadence, 5s connection-frozen threshold, 3s game-frozen
    // threshold. Future: surface as CVars if a user-tunable shape is
    // wanted (low priority — defaults should suit every realistic
    // network).
    static constexpr float kHeartbeatTxIntervalSec        = 2.0f;
    static constexpr float kClientFrozenThresholdSec      = 5.0f;
    static constexpr float kClientGameFrozenThresholdSec  = 3.0f;

    // Network-thread tick: called from ProcessOutgoingPackets after
    // the queue drain. No-op when not connected or before
    // kHeartbeatTxIntervalSec has elapsed since last send.
    void TickHeartbeat();

    // Network-thread fast-path: called from OnIncomingJson when
    // type==HEARTBEAT, BEFORE the packet is queued for the game thread.
    // Updates lastHeartbeatRx + lastHeartbeatGameFrame maps under
    // heartbeatMutex. Doesn't enqueue (no game-thread work needed —
    // detection runs on whichever thread queries the getters).
    void HandlePacket_Heartbeat(nlohmann::json payload);

    void SendPacket_Heartbeat();

    // Game-thread-callable getters. Return true when the named peer's
    // last heartbeat is older than the threshold, indicating either
    // a dead connection (Likely) or a frozen game thread (Game).
    // Returns false for the local client (we can't observe our own
    // freeze) and for unknown clientIds.
    bool IsClientLikelyFrozen(uint32_t clientId);
    bool IsClientGameFrozen(uint32_t clientId);

    // KB-18 (#177) Option 4 — host-authoritative netId snapshot.
    //
    // Some actors (En_Sw / Skullwalltula confirmed; others suspected)
    // mutate `actor->home.pos` non-deterministically inside `Init` (raycast
    // results vary by collision-pipeline state and float truncation). When
    // both clients independently compute their netId from the post-init
    // home.pos, they get DIFFERENT netIds for what is conceptually the
    // same scene-table entry — leading to the "No actor found for netId"
    // warning flood and broken sync.
    //
    // Fix: at scene-spawn time, the host serialises every static actor's
    // {actorId, room, params, homePos} → netId mapping and broadcasts it
    // team-scoped. Non-host caches the snapshot keyed by (sceneNum,
    // timeline). At OnActorSpawn time, non-host looks up the matching
    // entry by approximate homePos (±50 unit tolerance to absorb the
    // very divergence that motivates the fix) and uses the host's netId
    // verbatim instead of computing locally. Snapshot also retroactively
    // fixes already-spawned actors when the snapshot arrives late
    // (typical race: non-host loads scene faster than the snapshot
    // transit). Falls back to local compute when no snapshot exists
    // (host hasn't sent yet) or no entry matches (dynamic spawn — host
    // sends ENEMY_STATE phase=Alive phaseChanged=true for those, with
    // an authoritative spawnInfo that includes the host's netId via
    // `EncodeEnemyNetId` on the host side; non-host's local compute
    // matches by deterministic-input convention).
    struct SceneActorNetIdEntry {
        int16_t  actorId;
        int8_t   room;
        int16_t  params;
        Vec3f    homePos;
        uint32_t netId;
    };
    // Keyed by (sceneNum<<8 | timeline). Cleared on Disable.
    std::unordered_map<uint32_t, std::vector<SceneActorNetIdEntry>> sceneActorNetIdSnapshots;
    // Set true by OnSceneSpawnActors host-path. Drained on next
    // OnGameFrameUpdate so the broadcast fires AFTER all static actors
    // have spawned + had their netIds assigned (OnSceneSpawnActors
    // itself runs before the static-actor batch completes).
    bool pendingSceneActorNetIdsBroadcast = false;
    // Sender — walks current scene's syncable-actor categories and
    // builds the snapshot from each actor's EnemyNetId extension.
    void SendPacket_SceneActorNetIds();
    // Helper — apply a just-cached snapshot to currently-loaded actors,
    // overriding their (potentially divergent) netIds. Called from
    // HandlePacket_SceneActorNetIds when the receiving client is in the
    // matching scene+timeline.
    void ApplyHostNetIdsToCurrentScene(int16_t sceneNum, uint8_t timeline);
    // Helper — find the matching cache entry for a given actor.
    // Returns 0 on miss or ambiguous (multi-match within tolerance).
    uint32_t LookupHostNetIdForActor(struct Actor* actor,
                                     const std::vector<SceneActorNetIdEntry>& cache);
    // Convenience — cache lookup + LookupHostNetIdForActor for the
    // current scene+timeline. Used by OnActorSpawn non-host path.
    uint32_t LookupHostNetIdForCurrentScene(struct Actor* actor);
    // Scene-transition handoff — see #169 Phase C. Leader sends on
    // transitionTrigger OFF→START edge. Follower stores pending state and
    // fires its own transition once in proximity of the trigger point.
    void SendPacket_SceneTransitionHandoff(s16 fromSceneNum, s16 toEntranceIndex,
                                           Vec3f triggerPos, s16 triggerRotY);

    void SendPacket_ClearTeamState(std::string teamId);
    void SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage);
    void SendPacket_EntranceDiscovered(u16 entranceIndex);
    void SendPacket_GameComplete();
    void SendPacket_GiveItem(u16 modId, s16 getItemId);
    void SendPacket_Handshake();
    void SendPacket_OcarinaSfx(uint8_t note, float modulator, int8_t bend);
    void SendPacket_PlayerSfx(u16 sfxId);
    void SendPacket_PlayerUpdate();
    void SendPacket_RequestTeamState();
    void SendPacket_RequestTeleport(u32 clientId);
    void SendPacket_SetCheckStatus(RandomizerCheck rc);
    void SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_TeleportTo(u32 clientId);
    void SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_UpdateBeansCount();
    void SendPacket_UpdateClientState();
    void SendPacket_UpdateDungeonItems();
    void SendPacket_UpdateRoomState();
    void SendPacket_UpdateTeamState();
};

typedef enum {
    // Starting at 5 to continue from the last value in the PlayerDamageResponseType enum
    DUMMY_PLAYER_HIT_RESPONSE_STUN = 5,
    DUMMY_PLAYER_HIT_RESPONSE_FIRE,
    DUMMY_PLAYER_HIT_RESPONSE_NORMAL,
} DummyPlayerDamageResponseType;

class AnchorRoomWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override;
    void Draw() override;
    void UpdateElement() override{};
};

#endif // __cplusplus
#endif // NETWORK_ANCHOR_H
