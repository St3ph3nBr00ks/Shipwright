#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Notification/Notification.h"

extern "C" {
#include "functions.h"  // Actor_Kill — used by peer-NPC despawn on disconnect
}

/**
 * ALL_CLIENT_STATE
 *
 * Contains a list of all clients and their CLIENT_STATE currently connected to the server
 *
 * The server itself sends this packet to all clients when a client connects or disconnects
 */

void Anchor::HandlePacket_AllClientState(nlohmann::json payload) {
    std::vector<AnchorClient> newClients = payload["state"].get<std::vector<AnchorClient>>();
    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    // add new clients
    for (auto& client : newClients) {
        if (client.self) {
            ownClientId = client.clientId;
            CVarSetInteger(CVAR_REMOTE_ANCHOR("LastClientId"), ownClientId);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            clients[client.clientId].self = true;
        } else {
            clients[client.clientId].self = false;
            if (clients.contains(client.clientId)) {
                if (clients[client.clientId].online != client.online && !isGlobalRoom) {
                    Notification::Emit({
                        .prefix = client.name,
                        .message = client.online ? "Connected" : "Disconnected",
                    });
                }
            } else if (client.online && !isGlobalRoom) {
                Notification::Emit({
                    .prefix = client.name,
                    .message = "Connected",
                });
            }
        }

        clients[client.clientId].clientId = client.clientId;
        clients[client.clientId].name = client.name;
        clients[client.clientId].color = client.color;
        clients[client.clientId].clientVersion = client.clientVersion;
        clients[client.clientId].teamId = client.teamId;
        clients[client.clientId].online = client.online;
        clients[client.clientId].seed = client.seed;
        clients[client.clientId].isSaveLoaded = client.isSaveLoaded;
        clients[client.clientId].isGameComplete = client.isGameComplete;
        clients[client.clientId].sceneNum = client.sceneNum;
        clients[client.clientId].entranceIndex = client.entranceIndex;
        clients[client.clientId].customModelFilename = client.customModelFilename;
        clients[client.clientId].followerActive = client.followerActive;
        clients[client.clientId].isClimbing = client.isClimbing;
        clients[client.clientId].isCrawling = client.isCrawling;
    }

    // remove clients that are no longer in the list
    std::vector<uint32_t> clientsToRemove;
    for (auto& [clientId, client] : clients) {
        if (std::find_if(newClients.begin(), newClients.end(),
                         [clientId](AnchorClient& c) { return c.clientId == clientId; }) == newClients.end()) {
            clientsToRemove.push_back(clientId);
        }
    }
    // (separate loop to avoid iterator invalidation)
    for (auto& clientId : clientsToRemove) {
        // Despawn the peer's NPC Follower replica (Bug 5, log 67
        // 2026-05-20). Without this, when P1 disconnects (crash /
        // quit / connection drop), P2 retains P1's NPC follower
        // forever — the replica was spawned via SPAWN packet on P2's
        // side but only the matching DESPAWN packet would clean it
        // up. Disconnect doesn't send DESPAWN. Mirror of the
        // HandlePacket_FollowerNpcDespawn cleanup body.
        //
        // Future: if NPCs are ever meant to PERSIST after the owner
        // leaves, gate this on a per-NPC-class policy. v1 follower
        // NPCs are owner-anchored; disconnect = despawn.
        auto npcIt = mPeerFollowerNpcs.find(clientId);
        if (npcIt != mPeerFollowerNpcs.end()) {
            Actor* replica = npcIt->second;
            if (replica != nullptr && replica->update != nullptr) {
                Actor_Kill(replica);
            }
            mPeerFollowerNpcs.erase(npcIt);
            SPDLOG_INFO("[AllClientState] Despawned NPC replica for "
                        "disconnected client {}", clientId);
        }
        clients.erase(clientId);
    }

    // Pillar A Phase 1 — recompute effective host after client list changes.
    // Online flags, additions, and removals can all flip the election.
    RecomputeEffectiveHost();

    shouldRefreshActors = true;
}
