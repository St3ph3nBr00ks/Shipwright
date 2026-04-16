#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
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
        bool wasSaveLoaded = clients[clientId].isSaveLoaded;
        s16  prevSceneNum  = clients[clientId].sceneNum;

        AnchorClient client = payload["state"].get<AnchorClient>();
        clients[clientId].clientId = clientId;
        clients[clientId].name = client.name;
        clients[clientId].color = client.color;
        clients[clientId].clientVersion = client.clientVersion;
        clients[clientId].teamId = client.teamId;
        clients[clientId].online = client.online;
        clients[clientId].seed = client.seed;
        clients[clientId].isSaveLoaded = client.isSaveLoaded;
        clients[clientId].isGameComplete = client.isGameComplete;
        clients[clientId].sceneNum = client.sceneNum;
        clients[clientId].curRoomNum = client.curRoomNum;
        clients[clientId].entranceIndex = client.entranceIndex;

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
                clients[clientId].customSkeleton = nullptr; // release previous
                SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
                    &dummy->skelAnime, isAdult, tunic, newFilename,
                    clients[clientId].customSkeleton);
                clients[clientId].lastAppliedModelFilename = newFilename;
            } else if (modelChanged) {
                SPDLOG_INFO("[CoopModel]   model changed but DummyPlayer not yet spawned — will apply at spawn");
            }
        }

        // Fix 6 — join-time dead-enemy replay.
        // When we are the host and a remote client just loaded a save or entered a
        // new scene, send ENEMY_DEFEATED for every enemy we have recorded as dead
        // in that scene. This ensures late-joining clients see the correct state.
        bool nowLoaded = clients[clientId].isSaveLoaded;
        s16  newScene  = clients[clientId].sceneNum;
        bool sceneChanged = (prevSceneNum != newScene);
        bool justLoaded   = (!wasSaveLoaded && nowLoaded);

        if (roomState.ownerClientId == ownClientId && nowLoaded && (justLoaded || sceneChanged)) {
            auto it = deadEnemiesByScene.find(newScene);
            if (it != deadEnemiesByScene.end()) {
                SPDLOG_INFO("[EnemyDefeated] Replaying {} dead enemies in scene {} for client {}",
                            it->second.size(), (int)newScene, clientId);
                for (uint32_t netId : it->second) {
                    nlohmann::json killPayload;
                    killPayload["type"]           = ENEMY_DEFEATED;
                    killPayload["netId"]          = netId;
                    killPayload["targetClientId"] = clientId;
                    SendJsonToRemote(killPayload);
                }
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
