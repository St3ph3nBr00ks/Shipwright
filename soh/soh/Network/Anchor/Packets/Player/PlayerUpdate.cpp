#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

// Distance-based outbound throttle for PLAYER_UPDATE. Mirrors the
// ENEMY_UPDATE throttle landed in d0638bb5c + d043b4a89. PLAYER_UPDATE
// is broadcast every frame from every connected client; when peers are
// far away, the per-frame pos/joint/camera churn doesn't need to fire at
// the full game rate.
//
// Tiers (measured 3D from the local player's world.pos to the NEAREST
// peer's last-known posRot.pos in the same scene):
//   < kPlayerThrottleNearDist        no throttle (full rate, ~20-60 Hz)
//   < kPlayerThrottleFarDist         cap at kPlayerThrottleMidIntervalMs (~15 Hz)
//   ≥ kPlayerThrottleFarDist         cap at kPlayerThrottleFarIntervalMs (~5 Hz)
//
// Critical-edge fields (held-actor, released-actor, linkAge, equipment,
// csCtxState, sceneNum/entranceIndex/curRoomNum, item state) BYPASS the
// throttle so sync correctness is preserved — a carry-throw release at
// 1500u still arrives in the same frame it happened.
//
// CVar: gEnhancements.PlayerUpdateThrottleEnabled (default 1).
constexpr float    kPlayerThrottleNearDist      = 500.0f;
constexpr float    kPlayerThrottleFarDist       = 1000.0f;
constexpr uint64_t kPlayerThrottleMidIntervalMs = 66;   // ~15 Hz
constexpr uint64_t kPlayerThrottleFarIntervalMs = 200;  // ~5 Hz

// Snapshot of last-sent critical-edge fields. Any difference between the
// current frame's values and this snapshot forces an immediate send,
// bypassing the throttle. Persists for the process lifetime; sentinel
// values chosen so the FIRST send after launch always fires (all-zero
// snapshot won't typically match the first frame's full state).
struct PlayerUpdateCriticalSnapshot {
    bool     valid              = false;
    int32_t  sceneNum           = -1;
    int8_t   curRoomNum         = -1;
    int32_t  entranceIndex      = 0;
    int32_t  linkAge            = -1;
    uint32_t heldActorNetId     = 0;
    int8_t   currentBoots       = -1;
    int8_t   currentShield      = -1;
    int8_t   currentTunic       = -1;
    uint8_t  buttonItem0        = 0;
    int8_t   itemAction         = -1;
    int8_t   heldItemAction     = -1;
    uint8_t  modelGroup         = 0;
    int8_t   csCtxState         = -1;
    bool     isClimbing         = false;
    bool     isCrawling         = false;
    bool     isCarrying         = false;
    bool     isInvincible       = false;
};

static PlayerUpdateCriticalSnapshot sLastSentCritical;
static uint64_t                     sLastPlayerUpdateMs = 0;

static uint64_t PlayerUpdateNowMonotonicMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// File-static tracker for the local player's heldActor netId at the last
// SendPacket_PlayerUpdate. Used to detect release-edge transitions.
// Persists across reconnects — false-positive release events on stale
// netIds resolve to "actor not found" in `FindActorByNetId` and are
// silently dropped on the wire.
static uint32_t sLastLocalHeldActorNetId = 0;
// File-static tracker for the local player's last-broadcast sceneNum.
// Combined with sLastLocalHeldActorNetId to detect scene-exit-while-
// carrying: when the holder transitions scenes, the held actor is
// destroyed by scene unload (no `FindActorByNetId` hit on
// the new scene). Without intervention, the static placement re-spawns
// on scene re-entry — visible "the pot I took is back" bug (log 318).
// We record the netId in HostBookkeeping::SceneDeaths so the existing
// IsSceneDeath suppression at HookHandlers.cpp OnActorSpawn kills the
// re-spawn before any client sees it. Sentinel -1 marks pre-first-send.
static int16_t sLastLocalSceneNum = -1;

/**
 * PLAYER_UPDATE
 *
 * Contains real-time data necessary to update other clients in the same scene as the player
 *
 * Sent every frame to other clients within the same scene
 *
 * Note: This packet is sent _a lot_, so please do not include any unnecessary data in it
 */

void Anchor::SendPacket_PlayerUpdate() {
    if (!IsSaveLoaded()) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);

    // Carry-exit detection runs BEFORE the no-peers-in-scene early-return
    // below. Otherwise a holder walking alone into a different scene skips
    // the carry-exit notification entirely (their PLAYER_UPDATE is gated
    // off because no peer is in the new scene). We send via
    // SendPacket_EnemyRemovedFromScene which broadcasts independent of
    // scene, so the SceneDeath record propagates regardless of where peers
    // currently are. Log 319 root cause: this block previously lived below
    // the early-return and never fired on the holder's lone visit.
    uint32_t currentHeldActorNetId = 0;
    if (player->heldActor != nullptr) {
        const EnemyNetId* ext =
            ObjectExtension::GetInstance().Get<EnemyNetId>(player->heldActor);
        if (ext != nullptr) currentHeldActorNetId = ext->netId;
    }
    const int16_t currentScene = (int16_t)gPlayState->sceneNum;
    const bool sceneJustChanged =
        (sLastLocalSceneNum != -1) && (sLastLocalSceneNum != currentScene);

    bool didSendCarryExit = false;
    if (sLastLocalHeldActorNetId != 0 &&
        currentHeldActorNetId != sLastLocalHeldActorNetId &&
        sceneJustChanged &&
        FindActorByNetId(gPlayState, sLastLocalHeldActorNetId) == nullptr) {
        SendPacket_EnemyRemovedFromScene(sLastLocalHeldActorNetId, sLastLocalSceneNum);
        didSendCarryExit = true;
    }
    sLastLocalSceneNum = currentScene;

    uint32_t currentPlayerCount = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            currentPlayerCount++;
        }
    }
    if (currentPlayerCount == 0) {
        // Keep sLastLocalHeldActorNetId in sync so a future frame (when
        // peers re-join our scene) doesn't re-trigger the carry-exit
        // detection on a stale comparison.
        sLastLocalHeldActorNetId = currentHeldActorNetId;
        return;
    }

    // ---- Distance-based outbound throttle (#61 follow-up) ----
    // Skip this frame's send when peers are far away AND no critical-edge
    // field changed since the last send. Mirrors the ENEMY_UPDATE throttle.
    // See file-top constants + PlayerUpdateCriticalSnapshot doc comment.
    const bool throttleEnabled =
        CVarGetInteger("gEnhancements.PlayerUpdateThrottleEnabled", 1) != 0;

    // Build the current critical-edge snapshot. Cheap (just reads); we use
    // it both for the throttle decision and to update sLastSentCritical
    // after a send actually fires.
    PlayerUpdateCriticalSnapshot curCritical;
    curCritical.valid          = true;
    curCritical.sceneNum       = (int32_t)gPlayState->sceneNum;
    curCritical.curRoomNum     = (int8_t)gPlayState->roomCtx.curRoom.num;
    curCritical.entranceIndex  = (int32_t)gSaveContext.entranceIndex;
    curCritical.linkAge        = (int32_t)gSaveContext.linkAge;
    curCritical.heldActorNetId = currentHeldActorNetId;
    curCritical.currentBoots   = (int8_t)player->currentBoots;
    curCritical.currentShield  = (int8_t)player->currentShield;
    curCritical.currentTunic   = (int8_t)player->currentTunic;
    curCritical.buttonItem0    = (uint8_t)gSaveContext.equips.buttonItems[0];
    curCritical.itemAction     = (int8_t)player->itemAction;
    curCritical.heldItemAction = (int8_t)player->heldItemAction;
    curCritical.modelGroup     = (uint8_t)player->modelGroup;
    curCritical.csCtxState     = (int8_t)gPlayState->csCtx.state;
    curCritical.isClimbing     =
        (player->stateFlags1 &
         (PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_CLIMBING_LADDER)) != 0;
    curCritical.isCrawling     = (player->stateFlags2 & PLAYER_STATE2_CRAWLING) != 0;
    curCritical.isCarrying     = (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) != 0;
    curCritical.isInvincible   = (player->invincibilityTimer != 0);

    auto criticalChanged = [&]() -> bool {
        const PlayerUpdateCriticalSnapshot& a = sLastSentCritical;
        const PlayerUpdateCriticalSnapshot& b = curCritical;
        if (!a.valid)                                       return true;
        if (a.sceneNum       != b.sceneNum)                 return true;
        if (a.curRoomNum     != b.curRoomNum)               return true;
        if (a.entranceIndex  != b.entranceIndex)            return true;
        if (a.linkAge        != b.linkAge)                  return true;
        if (a.heldActorNetId != b.heldActorNetId)           return true;
        if (a.currentBoots   != b.currentBoots)             return true;
        if (a.currentShield  != b.currentShield)            return true;
        if (a.currentTunic   != b.currentTunic)             return true;
        if (a.buttonItem0    != b.buttonItem0)              return true;
        if (a.itemAction     != b.itemAction)               return true;
        if (a.heldItemAction != b.heldItemAction)           return true;
        if (a.modelGroup     != b.modelGroup)               return true;
        if (a.csCtxState     != b.csCtxState)               return true;
        if (a.isClimbing     != b.isClimbing)               return true;
        if (a.isCrawling     != b.isCrawling)               return true;
        if (a.isCarrying     != b.isCarrying)               return true;
        if (a.isInvincible   != b.isInvincible)             return true;
        return false;
    };

    const uint64_t nowMs = PlayerUpdateNowMonotonicMs();

    if (throttleEnabled && !criticalChanged()) {
        // Compute distance to NEAREST same-scene peer. Tier the cap by it.
        float bestDistSq = std::numeric_limits<float>::infinity();
        for (auto& [cid, c] : clients) {
            if (!c.online || !c.isSaveLoaded || c.self) continue;
            if (c.sceneNum != gPlayState->sceneNum) continue;
            float dx = c.posRot.pos.x - player->actor.world.pos.x;
            float dy = c.posRot.pos.y - player->actor.world.pos.y;
            float dz = c.posRot.pos.z - player->actor.world.pos.z;
            float ds = dx * dx + dy * dy + dz * dz;
            if (ds < bestDistSq) bestDistSq = ds;
        }
        uint64_t minIntervalMs = 0;
        if (std::isfinite(bestDistSq)) {
            const float bestDist = sqrtf(bestDistSq);
            if (bestDist > kPlayerThrottleFarDist) {
                minIntervalMs = kPlayerThrottleFarIntervalMs;
            } else if (bestDist > kPlayerThrottleNearDist) {
                minIntervalMs = kPlayerThrottleMidIntervalMs;
            }
        }
        if (minIntervalMs > 0 && sLastPlayerUpdateMs != 0 &&
            (nowMs - sLastPlayerUpdateMs) < minIntervalMs) {
            // Throttled this frame. We do NOT update sLastLocalHeldActorNetId
            // here — the next un-throttled send will compare against the
            // same baseline, so a release-edge that happens mid-throttle
            // still fires correctly when the throttle releases (because
            // currentHeldActorNetId will then differ from the still-stale
            // sLastLocalHeldActorNetId baseline). Same for sLastSentCritical.
            return;
        }
    }
    // ---- End throttle gate ----

    nlohmann::json payload;

    payload["type"] = PLAYER_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    // curRoomNum on PLAYER_UPDATE (~20 Hz) so the host-side ENEMY_UPDATE filter
    // (EnemyState.cpp ~705) sees mid-scene room transitions. UPDATE_CLIENT_STATE
    // alone (scene-boundary only) left peer room state stale for the full
    // duration peers spent in a sibling room — host kept broadcasting actor
    // updates that the peer couldn't match locally, generating cross-room
    // "No actor found for netId=… sceneNum=N" warning noise.
    payload["curRoomNum"] = gPlayState->roomCtx.curRoom.num;
    payload["entranceIndex"] = gSaveContext.entranceIndex;
    payload["linkAge"] = gSaveContext.linkAge;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;

    // Active camera state — read by host's AIDirector PickSpawnPosition
    // to gate spawns against every peer's actual view, not just Link's
    // position. Camera eye = world-space camera origin; at = look-at
    // point. Forward direction = normalize(at - eye).
    Camera* activeCam = GET_ACTIVE_CAM(gPlayState);
    if (activeCam != nullptr) {
        payload["cameraEye"] = activeCam->eye;
        payload["cameraAt"]  = activeCam->at;
    }
    std::vector<int> jointArray;
    for (size_t i = 0; i < 24; i++) {
        Vec3s joint = player->skelAnime.jointTable[i];
        jointArray.push_back(joint.x);
        jointArray.push_back(joint.y);
        jointArray.push_back(joint.z);
    }
    payload["prevTransl"] = player->skelAnime.prevTransl;
    payload["movementFlags"] = player->skelAnime.movementFlags;
    payload["jointTable"] = jointArray;
    // Upper-body anim joint table — carry pose only. Sent only when the
    // owner is actively carrying an actor; the field's absence in the
    // packet tells the observer "do not merge upper limbs this frame",
    // letting the main jointTable render unmodified for walk / run /
    // attack / etc. Vanilla's merge gate is "upperActionFunc returned
    // non-zero" (z_player.c:3617); we use PLAYER_STATE1_CARRYING_ACTOR
    // as the conservative approximation.
    // See Plans/carry_held_actor_sync.md §3.1.
    if (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) {
        std::vector<int> upperJointArray;
        for (size_t i = 0; i < 24; i++) {
            Vec3s joint = player->upperSkelAnime.jointTable[i];
            upperJointArray.push_back(joint.x);
            upperJointArray.push_back(joint.y);
            upperJointArray.push_back(joint.z);
        }
        payload["upperJointTable"] = upperJointArray;
    }

    // Held-actor sync (§3.2). currentHeldActorNetId was computed at the
    // top of the function (above the early-return) for carry-exit
    // detection; broadcast it here too so observers' DummyPlayer attach
    // edge-trigger has the value.
    payload["heldActorNetId"] = currentHeldActorNetId;

    // Throw release-edge: actor still alive in current scene; ship throw
    // velocity so observer's local copy starts the same trajectory.
    // Carry-exit was handled above the early-return — skip if it already
    // fired so we don't re-broadcast something that's already gone.
    if (!didSendCarryExit && sLastLocalHeldActorNetId != 0 &&
        currentHeldActorNetId != sLastLocalHeldActorNetId) {
        Actor* released = FindActorByNetId(gPlayState, sLastLocalHeldActorNetId);
        if (released != nullptr) {
            payload["releasedActorNetId"] = sLastLocalHeldActorNetId;
            payload["releaseSpeedXZ"]     = released->speedXZ;
            payload["releaseVelocityY"]   = released->velocity.y;
            payload["releaseYaw"]         = (int)released->world.rot.y;
        }
    }
    sLastLocalHeldActorNetId = currentHeldActorNetId;
    payload["upperLimbRot"] = player->upperLimbRot;
    payload["currentBoots"] = player->currentBoots;
    payload["currentShield"] = player->currentShield;
    payload["currentTunic"] = player->currentTunic;
    payload["stateFlags1"] = player->stateFlags1;
    payload["stateFlags2"] = player->stateFlags2 & ~PLAYER_STATE2_DISABLE_DRAW;
    payload["buttonItem0"] = gSaveContext.equips.buttonItems[0];
    payload["itemAction"] = player->itemAction;
    payload["heldItemAction"] = player->heldItemAction;
    payload["modelGroup"] = player->modelGroup;
    payload["invincibilityTimer"] = player->invincibilityTimer;
    payload["unk_862"] = player->unk_862;
    payload["unk_85C"] = player->unk_85C;
    payload["unk_860"] = player->unk_860;  // Deku-Stick burning timer
    payload["actionVar1"] = player->av1.actionVar1;
    // Multi-player dialogue redesign (#191 follow-up) — peer's csCtx.state
    // for "alone in cutscene" detection in Anchor_ShouldAdvanceCutsceneTextLocal.
    payload["csCtxState"] = (int)gPlayState->csCtx.state;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }

    // Update throttle bookkeeping AFTER a successful send. Critical-edge
    // snapshot and last-send timestamp captured here only; throttled frames
    // leave these untouched so a delayed critical-edge change is detected
    // against the last-actually-sent baseline.
    sLastSentCritical   = curCritical;
    sLastPlayerUpdateMs = nowMs;
}

void Anchor::HandlePacket_PlayerUpdate(nlohmann::json payload) {
    uint32_t clientId = payload["clientId"].get<uint32_t>();

    if (clients.contains(clientId)) {
        auto& client = clients[clientId];

        if (client.linkAge != payload.value("linkAge", (s32)LINK_AGE_ADULT)) {
            shouldRefreshActors = true;
        }

        client.sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
        // curRoomNum: default -1 matches UpdateClientState.cpp's "no save loaded"
        // sentinel; the ENEMY_UPDATE filter's `client.curRoomNum == hostRoom`
        // suppresses sends when the field is sentinel, which is the safe default.
        client.curRoomNum = payload.value("curRoomNum", (s8)-1);
        client.entranceIndex = payload.value("entranceIndex", (s32)0);
        client.linkAge = payload.value("linkAge", (s32)LINK_AGE_ADULT);
        client.posRot = payload.value("posRot", PosRot{ 0 });
        client.cameraEye = payload.value("cameraEye", Vec3f{ 0.0f, 0.0f, 0.0f });
        client.cameraAt  = payload.value("cameraAt",  Vec3f{ 0.0f, 0.0f, 0.0f });
        std::vector<int> jointArray = payload.value("jointTable", std::vector<int>{});
        jointArray.resize(24 * 3); // Ensure it has enough elements, in case of missing data
        for (int i = 0; i < 24; i++) {
            client.jointTable[i].x = jointArray[i * 3];
            client.jointTable[i].y = jointArray[i * 3 + 1];
            client.jointTable[i].z = jointArray[i * 3 + 2];
        }
        // Upper-body anim joint table — per-frame gate. The owner only
        // includes the field when actively carrying an actor; otherwise
        // we skip the observer-side merge so the main jointTable renders
        // unmodified for walk / run / attack / etc.
        if (payload.contains("upperJointTable")) {
            std::vector<int> upperJointArray = payload["upperJointTable"].get<std::vector<int>>();
            upperJointArray.resize(24 * 3);
            for (int i = 0; i < 24; i++) {
                client.upperJointTable[i].x = upperJointArray[i * 3];
                client.upperJointTable[i].y = upperJointArray[i * 3 + 1];
                client.upperJointTable[i].z = upperJointArray[i * 3 + 2];
            }
            client.upperMergeActiveThisFrame = true;
        } else {
            client.upperMergeActiveThisFrame = false;
        }
        client.movementFlags = payload.value("movementFlags", (u8)0);
        client.prevTransl = payload.value("prevTransl", Vec3s{ 0 });
        client.upperLimbRot = payload.value("upperLimbRot", Vec3s{ 0 });
        client.currentBoots = payload.value("currentBoots", (s8)0);
        client.currentShield = payload.value("currentShield", (s8)0);
        client.currentTunic = payload.value("currentTunic", (s8)0);
        client.stateFlags1 = payload.value("stateFlags1", (u32)0);
        client.stateFlags2 = payload.value("stateFlags2", (u32)0);
        client.buttonItem0 = payload.value("buttonItem0", (u8)0);
        client.itemAction = payload.value("itemAction", (s8)0);
        client.heldItemAction = payload.value("heldItemAction", (s8)0);
        client.modelGroup = payload.value("modelGroup", (u8)0);
        client.invincibilityTimer = payload.value("invincibilityTimer", (s8)0);
        client.unk_862 = payload.value("unk_862", (s16)0);
        client.unk_85C = payload.value("unk_85C", (f32)0);
        client.unk_860 = payload.value("unk_860", (s16)0);
        client.actionVar1 = payload.value("actionVar1", (s8)0);
        // Pre-update peers default to CS_STATE_IDLE (0) — treated as
        // out-of-cutscene for the multi-player dialogue detection.
        client.csCtxState = (s8)payload.value("csCtxState", 0);

        // Held-actor sync (§3.2). Update the netId; DummyPlayer_Update
        // performs the actual attach / detach edge-trigger using this
        // value (it has the Player* + can write player->heldActor).
        client.heldActorNetId = payload.value("heldActorNetId", (uint32_t)0);

        // Release-edge throw velocity. When the owner just released a
        // held actor, the payload carries the speedXZ / velocity.y / yaw
        // the vanilla throw code wrote to it. Apply to our local copy so
        // its actionFunc (already transitioning to a Thrown state) reads
        // the matching trajectory inputs and produces a symmetric arc.
        if (payload.contains("releasedActorNetId")) {
            uint32_t releasedNetId = payload["releasedActorNetId"].get<uint32_t>();
            Actor* released = FindActorByNetId(gPlayState, releasedNetId);
            if (released != nullptr) {
                released->speedXZ    = payload["releaseSpeedXZ"].get<f32>();
                released->velocity.y = payload["releaseVelocityY"].get<f32>();
                released->world.rot.y = (s16)payload["releaseYaw"].get<int>();
            }
        }

    }
}
