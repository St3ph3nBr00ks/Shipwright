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
// N=4 covers: synchronous CPU walks, LUS command batching, double-buffered GPU
// presentation, driver-queue stalls, gfx_texture_cache_clear flush tail, and
// VirtualBox frame-rate disparity. 67 ms at 60 fps / 200 ms at 20 fps — below
// user-visible threshold.
static constexpr int kRetireFrames = 4;

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
    // IDLE   — at P1's side; scans for nearby enemies.
    // FOLLOW — moving toward P1's side.
    // STUCK  — no progress detected; strafing to unstick, then back to FOLLOW.
    // ENGAGE — moving toward the nearest ACTORCAT_ENEMY actor.
    // ATTACK — within melee range; charge/retreat cycle, positions P2 for hitbox contact.
    // RETURN — returning to P1's side after ENGAGE/ATTACK.
    enum class FollowerAIState { IDLE, FOLLOW, STUCK, ENGAGE, ATTACK, RETURN };
    FollowerAIState followerAIState     = FollowerAIState::IDLE;
    int             followerStateFrames = 0;                     // frames spent in current state
    int             followerStuckFrames = 0;                     // frames spent in STUCK
    Vec3f           followerLastPos     = { 0.0f, 0.0f, 0.0f }; // position at last stuck-check
    Vec3f           followerStuckDir    = { 0.0f, 0.0f, 0.0f }; // strafe direction while STUCK
    Actor*          followerTargetEnemy = nullptr;               // current ENGAGE/ATTACK target
    Vec3f           followerMoveTarget  = { 0.0f, 0.0f, 0.0f }; // world-space position ShouldActorUpdate steers toward
    uint32_t        followerLeaderClientId = 0;                  // sticky: clientId of current leader DummyPlayer (0 = none)

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

    void SendPacket_EnemyUpdate(uint32_t netId, Actor* actor);
    void SendPacket_EnemyDefeated(uint32_t netId);
    void SendPacket_EnemySpawn(Actor* actor);
    void SendPacket_EnemyRespawn(uint32_t netId);
    void SendPacket_DamageEnemy(uint32_t netId, u8 damage);
    void SendPacket_EnemyHitPlayer(uint32_t netId);
    void HandlePacket_EnemyHitPlayer(nlohmann::json payload);
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
