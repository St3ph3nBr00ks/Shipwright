#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/Common/PacketSchemas.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

// Returns the local player's chosen character folder name, or "" if unset.
// The folder-existence check is intentionally omitted: it fails on VirtualBox
// shared folder paths after scene entry (LocateFileAcrossAppDirs returns a
// path that does not resolve on the VM side). If the folder doesn't exist on
// the receiving client, LoadFileFromCoopFolder already fails gracefully.
static std::string GetLocalModelFilename() {
    const char* chosen = CVarGetString(CVAR_REMOTE_ANCHOR("CharacterModel"), "");
    if (chosen == nullptr || chosen[0] == '\0') {
        return "";
    }
    return std::string(chosen);
}

/**
 * UPDATE_CLIENT_STATE
 *
 * Contains a small subset of data that is cached on the server and important for the client to know for various reasons
 *
 * Sent on various events, such as changing scenes, soft resetting, finishing the game, opening file select, etc.
 *
 * Note: This packet should be cross version compatible, so if you add anything here don't assume all clients will be
 * providing it, consider doing a `contains` check before accessing any version specific data
 */

nlohmann::json Anchor::PrepClientState() {
    nlohmann::json payload;
    payload["name"] = CVarGetString(CVAR_REMOTE_ANCHOR("Name"), "");
    payload["color"] = CVarGetColor24(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
    payload["clientVersion"] = clientVersion;
    payload["teamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["online"] = true;

    std::string localModel = GetLocalModelFilename();
    SPDLOG_INFO("[CoopModel] PrepClientState: customModelFilename=\"{}\"", localModel);
    payload["customModelFilename"] = localModel;
    payload["followerActive"] = IsFollowerActive();
    payload["sceneSpawnEpoch"] = sceneSpawnEpoch;
    payload["isClimbing"] = IsLocalPlayerClimbing();
    payload["isCrawling"] = IsLocalPlayerCrawling();

    // Pillar F — broadcast our per-packet-type maxSchema so peers know
    // which optional fields they can include when sending to us.
    nlohmann::json maxSchema = nlohmann::json::object();
    for (const auto& [type, schema] : PacketSchemas::kPacketSchemas) {
        maxSchema[type] = schema;
    }
    payload["maxSchema"] = maxSchema;

    if (IsSaveLoaded()) {
        payload["seed"] = IS_RANDO ? Rando::Context::GetInstance()->GetSeed() : 0;
        payload["isSaveLoaded"] = true;
        payload["isGameComplete"] = gSaveContext.ship.stats.gameComplete;
        payload["sceneNum"] = gPlayState->sceneNum;
        payload["curRoomNum"] = gPlayState->roomCtx.curRoom.num;
        payload["entranceIndex"] = gSaveContext.entranceIndex;
        payload["dayTime"]   = (u16)gSaveContext.dayTime;
        payload["nightFlag"] = gSaveContext.nightFlag;
    } else {
        payload["seed"] = 0;
        payload["isSaveLoaded"] = false;
        payload["isGameComplete"] = false;
        payload["sceneNum"] = SCENE_ID_MAX;
        payload["curRoomNum"] = -1;
        payload["entranceIndex"] = 0x00;
    }

    return payload;
}

void Anchor::SendPacket_UpdateClientState() {
    nlohmann::json payload;
    payload["type"] = UPDATE_CLIENT_STATE;
    payload["state"] = PrepClientState();

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UpdateClientState(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();

    if (clients.contains(clientId)) {
        // Capture prior state so we can detect transitions below.
        bool     wasSaveLoaded        = clients[clientId].isSaveLoaded;
        bool     wasOnline            = clients[clientId].online;
        s16      prevSceneNum         = clients[clientId].sceneNum;
        s8       prevCurRoomNum       = clients[clientId].curRoomNum;
        uint32_t prevSceneSpawnEpoch  = clients[clientId].sceneSpawnEpoch;

        AnchorClient client = payload["state"].get<AnchorClient>();
        clients[clientId].clientId = clientId;
        clients[clientId].name = client.name;
        clients[clientId].color = client.color;
        clients[clientId].clientVersion = client.clientVersion;
        clients[clientId].teamId = client.teamId;
        clients[clientId].online = client.online;

        // Pillar A Phase 1 — if this UPDATE_CLIENT_STATE flipped online flag,
        // re-run the effective-host election. Most UCS payloads don't change
        // online (it's only set to false on disconnect/timeout), so this
        // recompute usually no-ops.
        if (wasOnline != client.online) {
            RecomputeEffectiveHost();
        }
        clients[clientId].seed = client.seed;
        clients[clientId].isSaveLoaded = client.isSaveLoaded;
        clients[clientId].isGameComplete = client.isGameComplete;
        clients[clientId].sceneNum = client.sceneNum;
        clients[clientId].curRoomNum = client.curRoomNum;
        clients[clientId].entranceIndex = client.entranceIndex;
        clients[clientId].sceneSpawnEpoch = client.sceneSpawnEpoch;
        clients[clientId].followerActive = client.followerActive;
        clients[clientId].isClimbing = client.isClimbing;
        clients[clientId].isCrawling = client.isCrawling;

        // Pillar F — record peer's maxSchema for outgoing field-clamping decisions.
        if (payload["state"].contains("maxSchema") && payload["state"]["maxSchema"].is_object()) {
            for (auto& [type, schemaVal] : payload["state"]["maxSchema"].items()) {
                if (schemaVal.is_number_integer()) {
                    clients[clientId].peerMaxSchema[type] = schemaVal.get<int>();
                }
            }
        }

        // Cosmetic sync — apply remote player's custom character model to their DummyPlayer
        if (payload["state"].contains("customModelFilename")) {
            std::string newFilename = payload["state"]["customModelFilename"].get<std::string>();
            bool modelChanged = (clients[clientId].customModelFilename != newFilename);
            SPDLOG_INFO("[CoopModel] UpdateClientState clientId={}: model \"{}\" -> \"{}\" changed={}",
                        clientId, clients[clientId].customModelFilename, newFilename, modelChanged);
            clients[clientId].customModelFilename = newFilename;

            if (modelChanged && clients[clientId].player != nullptr) {
                Player* dummy = clients[clientId].player;
                bool isAdult = (clients[clientId].linkAge != LINK_AGE_CHILD);
                auto tunic = (uint8_t)clients[clientId].currentTunic;
                SPDLOG_INFO("[CoopModel]   applying to DummyPlayer: isAdult={} tunic={}", isAdult, (int)tunic);
                clients[clientId].customSkeleton = nullptr;  // release previous (not referenced by Gfx buffers)
                // KB-15 fix (issue #110): move outgoing bakedModel to the retire slot
                // so the last-submitted frame's Gfx buffer can finish consuming it.
                // Synchronous destruction here caused P1 Unhandled-OP-code crashes when
                // P2 changed pack mid-scene (Test 17 log 113).
                clients[clientId].RetireBakedModel();
                clients[clientId].bakedModel = std::make_unique<SOH::BakedPlayerModel>();
                SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
                    &dummy->skelAnime, isAdult, tunic, newFilename,
                    clients[clientId].customSkeleton,
                    *clients[clientId].bakedModel);
                clients[clientId].lastAppliedModelFilename = newFilename;
            } else if (modelChanged) {
                SPDLOG_INFO("[CoopModel]   model changed but DummyPlayer not yet spawned — will apply at spawn");
            }
        }

        // Fix 6 — join-time dead-enemy replay.
        // When we are the host and a remote client just loaded a save or entered a
        // new scene, send ENEMY_DEFEATED for every enemy we have recorded as dead
        // in that scene. This ensures late-joining clients see the correct state.
        bool     nowLoaded           = clients[clientId].isSaveLoaded;
        s16      newScene            = clients[clientId].sceneNum;
        uint32_t newSceneSpawnEpoch  = clients[clientId].sceneSpawnEpoch;
        bool     sceneChanged        = (prevSceneNum != newScene);
        bool     justLoaded          = (!wasSaveLoaded && nowLoaded);
        // Same-scene reload signal: Game Over continue, void-out, and Farore's Wind
        // back to the current scene all leave sceneNum and isSaveLoaded unchanged,
        // but the non-host's OnSceneSpawnActors still fires and bumps its epoch.
        bool     sceneSpawned        = (prevSceneSpawnEpoch != newSceneSpawnEpoch);

        // Exit-gated vacancy detection (Test 1.5 fix follow-up).
        // PRIMARY trigger: when this client transitions OUT of a scene
        // (prevSceneNum != newScene), check whether anyone is still in
        // prevSceneNum. If not, the scene just became empty — broadcast
        // SCENE_DEATHS_CLEARED so every client clears its scene-scoped
        // buffered state in lockstep (host's mSceneDeaths and every
        // client's locally-buffered pendingKillNetIds).
        //
        // Race-tolerant: peer-presence check happens AFTER applying this
        // client's sceneNum change. A peer mid-transition will either
        // still report the prior scene (vacancy doesn't hold, we don't
        // fire) or already report a different scene (vacancy holds).
        if (::SceneAuthority::IsEffectiveHost() && wasSaveLoaded && sceneChanged &&
            prevSceneNum >= 0) {
            bool anyOtherInPrevScene =
                (gPlayState != nullptr && (s16)gPlayState->sceneNum == prevSceneNum);
            if (!anyOtherInPrevScene) {
                for (auto& [otherId, other] : clients) {
                    if (otherId == clientId) continue;
                    if (!other.online || !other.isSaveLoaded || other.self) continue;
                    if (other.sceneNum == prevSceneNum) {
                        anyOtherInPrevScene = true;
                        break;
                    }
                }
            }
            if (!anyOtherInPrevScene) {
                // 0xFF = match any timeline. We don't track per-client
                // timeline state granularly here; broadcast clears the
                // entire scene's state regardless.
                SendPacket_SceneDeathsCleared(prevSceneNum, 0xFF);
            }
        }

        // Belt-and-suspenders: keep entry-gated vacancy check as a
        // backstop for any case the exit-gated path missed (e.g.,
        // ungraceful disconnect where the OUT-transition wasn't observed).
        if (::SceneAuthority::IsEffectiveHost() && nowLoaded && sceneChanged) {
            bool anyOtherInNewScene =
                (gPlayState != nullptr && (s16)gPlayState->sceneNum == newScene);
            if (!anyOtherInNewScene) {
                for (auto& [otherId, other] : clients) {
                    if (otherId == clientId) continue;
                    if (!other.online || !other.isSaveLoaded || other.self) continue;
                    if (other.sceneNum == newScene) {
                        anyOtherInNewScene = true;
                        break;
                    }
                }
            }
            if (!anyOtherInNewScene) {
                auto& bk = EnemyStateSync::HostBookkeeping::Instance();
                bk.ClearScene(newScene);
                bk.ClearAllDefeatBroadcasts();
                SPDLOG_INFO("[UpdateClientState] Scene {} was empty before client {} entered "
                            "— entry-gated backstop cleared mSceneDeaths/mDefeatBroadcasts",
                            (int)newScene, clientId);
            }
        }

        if (::SceneAuthority::IsEffectiveHost() && nowLoaded &&
            (justLoaded || sceneChanged || sceneSpawned)) {
            const auto& deaths = EnemyStateSync::HostBookkeeping::Instance().SceneDeaths(newScene);
            if (!deaths.empty()) {
                SPDLOG_INFO("[EnemyDefeated] Replaying {} dead enemies in scene {} for client {}",
                            deaths.size(), (int)newScene, clientId);
                for (uint32_t netId : deaths) {
                    nlohmann::json killPayload;
                    // C2 Phase 4 Commit B — late-join replay also rides ENEMY_STATE.
                    killPayload["type"]           = ENEMY_STATE;
                    killPayload["phase"]          = "DyingByLocal";
                    killPayload["phaseChanged"]   = true;
                    killPayload["netId"]          = netId;
                    killPayload["targetClientId"] = clientId;
                    SendJsonToRemote(killPayload);
                }
            }
        }

        // #166 sub-item (6) — mid-boss late-join immediate snapshot.
        //
        // Without this, when a peer joins a scene+room mid-Gohma-fight, the
        // peer's local Boss_Goma_Init creates a fresh full-HP boss in the
        // intro Encounter cutscene. The standard per-frame ENEMY_STATE
        // arrives ~50ms later (one simulation frame), but during that
        // window the peer's view of the boss is full-HP / cutscene-zero —
        // visibly desynced.
        //
        // Fix: when host detects a peer's curRoomNum change AND host is
        // currently in the same scene+room, walk the synced boss actors
        // in host's actor list and fire an immediate SendPacket_EnemyUpdate
        // for each. Carries full state (actionState, cutsceneSubState,
        // visualState, eyeState, invincibility, spawnTimer, lookedAtFrames,
        // children[3], animId, curFrame, position, rotation, health,
        // scale) — peer's local boss applies and snaps to host's state.
        //
        // Trigger: host-only, room-or-scene change, peer now in host's room.
        // Cost: O(BOSS-actors-in-room) which is ~1 in normal play.
        const s8 newCurRoomNum = clients[clientId].curRoomNum;
        const bool roomChanged = (sceneChanged || prevCurRoomNum != newCurRoomNum);
        if (::SceneAuthority::IsEffectiveHost() && nowLoaded && roomChanged &&
            gPlayState != nullptr && (s16)gPlayState->sceneNum == newScene &&
            (s8)gPlayState->roomCtx.curRoom.num == newCurRoomNum) {
            int snapped = 0;
            Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head;
            while (a != nullptr) {
                if (IsSyncedBossActor(a->id)) {
                    const EnemyNetId* ext =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                    if (ext != nullptr) {
                        SendPacket_EnemyUpdate(ext->netId, a);
                        snapped++;
                    }
                }
                a = a->next;
            }
            if (snapped > 0) {
                SPDLOG_INFO("[EnemyState] Snapshot fired for {} boss(es) on client {} "
                            "scene/room change scene={} room={}",
                            snapped, clientId, (int)newScene, (int)newCurRoomNum);
            }
        }

        // Time-of-day sync: apply a received time if it is more advanced than the
        // current time. "More advanced" means the nightFlag changed (day↔night
        // transition) OR the same nightFlag with a higher dayTime value.
        //
        // This is bidirectional — both host and non-host apply each other's time.
        // Rationale: En_Weather_Tag only runs in overworld scenes. When one player
        // is in a dungeon (time stalled) while the other is in Hyrule Field (time
        // advancing), the dungeon player's time falls behind. If the dungeon player
        // is the host and enters Hyrule Field they would otherwise broadcast stale
        // time to the non-host. With forward-only bidirectional sync the more
        // advanced time always wins regardless of host assignment.
        if (IsSaveLoaded() && payload["state"].contains("dayTime")) {
            s32 receivedNightFlag = payload["state"]["nightFlag"].get<s32>();
            u16 receivedDayTime   = payload["state"]["dayTime"].get<u16>();
            bool timeIsAhead = (receivedNightFlag != gSaveContext.nightFlag) ||
                               (receivedDayTime > (u16)gSaveContext.dayTime);
            if (timeIsAhead) {
                gSaveContext.dayTime   = receivedDayTime;
                gSaveContext.nightFlag = receivedNightFlag;
                SPDLOG_INFO("[UpdateClientState] Synced time: dayTime={} nightFlag={}",
                            gSaveContext.dayTime, gSaveContext.nightFlag);
            }
        }
    }
}
