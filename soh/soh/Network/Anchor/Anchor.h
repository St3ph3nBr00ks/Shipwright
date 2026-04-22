#ifndef NETWORK_ANCHOR_H
#define NETWORK_ANCHOR_H
#ifdef __cplusplus

#include "soh/Network/Network.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/resource/type/Skeleton.h"
#include <libultraship/libultraship.h>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "variables.h"
#include "z64.h"
}

// Categories walked by all receive-side netId→actor lookups.
// Covers every runtime category transition surveyed in session_state.md
// (Karebaba→MISC, Armos→BG, Po Sisters→PROP, Heishi2→ITEMACTION, etc.)
// plus BOSS for full-boss sync. Adding an ID to IsSyncedWorldActor
// admits both default-category and transition-category instances.
// `inline constexpr` (C++17) deduplicates across translation units.
inline constexpr u8 kSyncableActorCategories[] = {
    ACTORCAT_ENEMY,
    ACTORCAT_BOSS,
    ACTORCAT_PROP,
    ACTORCAT_BG,
    ACTORCAT_NPC,
    ACTORCAT_SWITCH,
    ACTORCAT_ITEMACTION,
    ACTORCAT_MISC,
};
inline constexpr size_t kSyncableActorCategoriesCount =
    sizeof(kSyncableActorCategories) / sizeof(kSyncableActorCategories[0]);

// Attached to enemy actors to give them a stable network id across all clients.
struct EnemyNetId {
    uint32_t netId = 0;
    SkelAnime* skelAnime = nullptr; // nullptr if this enemy type has no supported skeleton
    uint8_t limbCount = 0;          // cached from skelAnime->limbCount at spawn time

    // Last state received from the host via ENEMY_UPDATE (non-host clients only).
    // Re-applied each frame in OnActorUpdate so the enemy update() can run (enabling
    // collision registration) without drifting from the authoritative host position.
    bool hasNetState = false;
    Vec3f netPos = { 0.0f, 0.0f, 0.0f };
    Vec3s netRot = { 0, 0, 0 };
    Vec3s netShapeRot = { 0, 0, 0 };
    s8 netHealth = 1;
    Vec3f netScale = { 1.0f, 1.0f, 1.0f }; // actor->scale; synced for enemies that change scale during animation

    // Set to true by the OnEnemyDefeat hook after it sends ENEMY_DEFEATED.
    // The OnActorKill hook checks this flag and skips sending a second packet for
    // enemies that go through the normal OnEnemyDefeat → Actor_Kill death path.
    // Only enemies that die via Actor_Kill WITHOUT firing OnEnemyDefeat (e.g.,
    // ACTOR_EN_DEKUBABA stem, ACTOR_EN_SKB at dawn) will have this false when
    // OnActorKill fires, triggering the Fix 12 broadcast path.
    bool defeatPacketSent = false;

    // Set to true on the non-host when this client kills the enemy locally
    // (OnEnemyDefeat or OnActorKill fires). Prevents ENEMY_UPDATE from re-writing
    // health > 0 after the local kill — without this, the host keeps sending
    // health > 0 for several frames while P2's kill packet travels to P1, causing
    // the dying enemy to "resurrect" visually on P2 before the Actor_Kill from
    // the host's ENEMY_DEFEATED reaches P2.
    bool hasLocalDeath = false;

    // Per-actor-type state machine sync fields.
    // Currently used by ACTOR_EN_KAREBABA (Withered Deku Baba) to keep its
    // Idle/Awaken/Upright/Spin/Retract state in sync with the host.
    // -1 means no state received yet (initial value).
    s16 netStateIndex = -1;
    s16 netActorParams = 0;

    // Set on a non-host client by HandlePacket_EnemyDefeated when the Karebaba
    // is allowed to run its natural death→respawn cycle instead of being Actor_Kill'd.
    // While true: hasLocalDeath blocks ENEMY_UPDATE overrides; item drop is suppressed.
    // Cleared in OnActorUpdate when the actor returns to Idle (respawn complete).
    bool pendingNaturalDeath = false;

    // Set when a second ENEMY_DEFEATED arrives for this Karebaba while it is already
    // in its natural death cycle (pendingNaturalDeath=true or defeatPacketSent=true).
    // The incoming kill cannot be applied immediately — the actor must finish its
    // current cycle first. When non-host respawn detection fires, this flag causes an
    // immediate re-trigger of the death cycle (SetupDeadItemDrop) rather than clearing
    // back to live state, so the stacked kill is honoured with one cycle's delay.
    // Cleared after re-triggering (or when respawn completes with no stacked kill).
    bool stalledKillPending = false;

    // Set in OnActorSpawn when a pendingKillNetIds entry matches this Karebaba.
    // OnActorSpawn fires BEFORE actor->init() is called by Actor_UpdateAll; calling
    // SetupDeadItemDrop there causes EnKarebaba_Init (Frame 1) to override actionFunc
    // back to Idle, and the next update() then calls SetupAwaken (Fix 38).
    // OnActorInit fires AFTER actor->init() has run — safe to call SetupDeadItemDrop.
    bool deferredDeadItemDrop = false;

    // Goroiwa-net state — issue #153.
    // First non-ACTORCAT_ENEMY actor sync (En_Goroiwa is ACTORCAT_PROP). Cached on
    // non-host from ENEMY_UPDATE; re-applied each frame in OnActorUpdate so the local
    // action function can run (collision registration) without drifting from the
    // host-authoritative path-position. -1 means no state received yet.
    s16 goroiwaCurrentWaypoint = -1;
    s16 goroiwaNextWaypoint    = -1;
    s16 goroiwaPathDirection   = 0; // ±1; 0 means uninitialized
    u8  goroiwaFlags           = 0; // ENGOROIWA_* bitmask (PLAYER_IN_THE_WAY etc.)
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

typedef struct AnchorClient {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
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
    s16 unk_862;
    s8 actionVar1;
    u8 ocarinaNote;
    f32 ocarinaModulator;
    s8 ocarinaBend;

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
    // Instead: move the outgoing bakedModel into retiredBakedModel, arm
    // retireFrameCounter = kRetireFrames, and let OnGameFrameUpdate tick it down.
    // When the counter hits 0 the slot is cleared and the destructor runs — by
    // which point every Gfx frame that could have referenced the model has been
    // fully consumed by the renderer.
    //
    // Single-slot design is sufficient because the bake is synchronous and
    // blocks the main thread for ~400 ms (>> kRetireFrames worth of frames),
    // so back-to-back changes can't overlap in practice.
    std::unique_ptr<SOH::BakedPlayerModel> retiredBakedModel;
    int retireFrameCounter = 0;

    // Helper: move current bakedModel into the retire slot, arming the counter.
    // Any model already in the retire slot is destroyed immediately — acceptable
    // because it has been sitting there at least one frame already (the previous
    // bake that displaced the prior retiree took time itself).
    void RetireBakedModel();

    // Ptr to the dummy player
    Player* player;
} AnchorClient;

// Number of render frames a retired BakedPlayerModel must sit idle before
// destruction. See commentary on AnchorClient::retiredBakedModel for rationale.
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

class Anchor : public Network {
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

    // Host-only: netIds of enemies that have died in each scene.
    // Sent to newly joined (or scene-transitioning) clients so they see enemies
    // as dead immediately rather than alive until the next kill event.
    // Cleared when the host re-enters a scene (enemies respawn on re-entry).
    std::unordered_map<s16, std::unordered_set<uint32_t>> deadEnemiesByScene;

    // Set to true for the duration of HandlePacket_EnemySpawn's Actor_Spawn call
    // so the resulting OnActorSpawn hook does not suppress the actor on non-host.
    bool isSpawningNetworkActor = false;

    // Set to true for the duration of HandlePacket_EnemyDefeated's Actor_Kill call
    // so the resulting OnActorKill hook (Fix 12) does not echo ENEMY_DEFEATED back
    // to the network for kills that originated from the network.
    bool isKillingNetworkActor = false;

    // Scene-visit dedup: netIds of ENEMY_DEFEATED packets sent during the current
    // scene visit. Prevents duplicate sends when Actor_Kill fires more than once for
    // the same logical enemy (e.g. room-transition unload + re-load within the same
    // scene visit allocates new actor instances that share a netId via posHash).
    // Cleared in OnSceneSpawnActors (same point as deadEnemiesByScene clear).
    std::unordered_set<uint32_t> sentDefeatThisScene;

    // netIds of ENEMY_DEFEATED packets that arrived while the target actor did not
    // exist yet (e.g. the packet raced ahead of the scene load on this client).
    // OnActorSpawn checks this set and immediately kills any newly-spawned actor
    // whose netId is pending. Cleared in OnSceneSpawnActors alongside the above sets.
    std::unordered_set<uint32_t> pendingKillNetIds;

    // Follower mode: non-host player's position is overridden to trail the host.
    // Toggled via the Anchor settings menu (AI Follower checkbox).
    // Deactivated by any controller input while active.
    bool followerActive = false;

    // AI follower state machine (runs each frame when followerActive is true).
    // IDLE     — at leader's side; scans for nearby enemies.
    // FOLLOW   — stick-driven movement toward leader's side.
    // STUCK    — stick stalled; bounded position-override nudge toward target.
    // ENGAGE   — moving toward the nearest ACTORCAT_ENEMY actor.
    // ATTACK   — within sword reach; BTN_B injection, stick continues to track enemy.
    // RETURN   — returning to leader's side after ENGAGE/ATTACK.
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
    //            and FOLLOW when eligible; ATTACK→RETURN may divert through
    //            this state if a drop just landed. See
    //            Claude/Plans/ai_follower_item_pickup.md.
    enum class FollowerAIState {
        IDLE, FOLLOW, STUCK, ENGAGE, ATTACK, RETURN,
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
    void RefreshClientActors();
    void SetDummyPlayerClientId(const Actor* actor, uint32_t clientId);

    void HandlePacket_AllClientState(nlohmann::json payload);
    void HandlePacket_EnemyUpdate(nlohmann::json payload);
    void HandlePacket_EnemyDefeated(nlohmann::json payload);
    void HandlePacket_EnemySpawn(nlohmann::json payload);
    void HandlePacket_EnemyRespawn(nlohmann::json payload);
    void HandlePacket_DamageEnemy(nlohmann::json payload);
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

  public:
    uint32_t ownClientId;
    inline static const std::string clientVersion = (char*)gGitCommitHash;

    // Packet types //
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    inline static const std::string ENEMY_UPDATE = "ENEMY_UPDATE";
    inline static const std::string ENEMY_DEFEATED = "ENEMY_DEFEATED";
    inline static const std::string ENEMY_SPAWN = "ENEMY_SPAWN";
    inline static const std::string ENEMY_RESPAWN = "ENEMY_RESPAWN";
    inline static const std::string DAMAGE_ENEMY = "DAMAGE_ENEMY";
    inline static const std::string DAMAGE_PLAYER = "DAMAGE_PLAYER";
    inline static const std::string ENEMY_HIT_PLAYER = "ENEMY_HIT_PLAYER";
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

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    RoomState roomState;

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
    // G1/G2 — read local Player's stateFlags1 to detect ladder/vine/ledge climbing.
    // Returns false when there is no live PlayState or local Player. Used in
    // PrepClientState so remote clients see this client's climbing state.
    bool IsLocalPlayerClimbing() const;

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

    void SendPacket_EnemyUpdate(uint32_t netId, Actor* actor);
    void SendPacket_EnemyDefeated(uint32_t netId);
    void SendPacket_EnemySpawn(Actor* actor);
    void SendPacket_EnemyRespawn(uint32_t netId);
    void SendPacket_DamageEnemy(uint32_t netId, u8 damage);
    void SendPacket_EnemyHitPlayer(uint32_t netId);
    void HandlePacket_EnemyHitPlayer(nlohmann::json payload);
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
