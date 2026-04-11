#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
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

        // Time-of-day sync: non-host applies the host's dayTime and nightFlag whenever
        // it receives an UpdateClientState from the host. Both clients tick time at the
        // same rate via En_Weather_Tag, but can drift when one player is in a dungeon
        // (no weather tag) while the other is in the overworld. Applying host time at
        // scene transitions corrects the drift without fighting the weather tag per-frame.
        bool weAreHost    = (roomState.ownerClientId == ownClientId);
        bool senderIsHost = (clientId == roomState.ownerClientId && roomState.ownerClientId != 0);
        if (!weAreHost && senderIsHost && IsSaveLoaded() && payload["state"].contains("dayTime")) {
            gSaveContext.dayTime   = payload["state"]["dayTime"].get<u16>();
            gSaveContext.nightFlag = payload["state"]["nightFlag"].get<s32>();
            SPDLOG_INFO("[UpdateClientState] Synced time from host: dayTime={} nightFlag={}",
                        gSaveContext.dayTime, gSaveContext.nightFlag);
        }
    }
}
