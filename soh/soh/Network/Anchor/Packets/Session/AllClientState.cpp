#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Notification/Notification.h"

#include "soh/Network/Anchor/HorseNetId.h"  // #259 late-join re-emit

extern "C" {
#include "functions.h"  // Actor_Kill — used by peer-NPC despawn on disconnect
#include "z64.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
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

    // #259 — track which peers transitioned into a state where we should
    // re-emit HORSE_SPAWN for our owner-local horses. Two trigger
    // conditions: (a) peer flipped online=false → true (fresh join or
    // reconnect — their mPeerHorses cache is empty), (b) peer's sceneNum
    // changed to our sceneNum (they entered our scene mid-ride — their
    // OnSceneSpawnActors cleared their mPeerHorses cache). In either
    // case, the receiver wouldn't otherwise see our currently-mounted
    // Epona until our next OnActorSpawn fires (which is rare during a
    // continuous ride). Owner-side re-emit fills the gap. Cheap — one
    // HORSE_SPAWN per local horse per peer-state-change. Receiver's
    // HandlePacket_HorseSpawn already ignores duplicates safely.
    bool shouldReemitOwnerHorses = false;

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

        // #259 — detect transitions BEFORE writing through. Only peers
        // (not self) matter; only online + same-scene-as-us peers care.
        if (!client.self && gPlayState != nullptr) {
            const bool hadEntry = clients.contains(client.clientId);
            const bool wasOnline = hadEntry ? clients[client.clientId].online : false;
            const int16_t oldScene = hadEntry
                ? (int16_t)clients[client.clientId].sceneNum : (int16_t)-1;
            const bool nowSameScene =
                client.online && client.sceneNum == gPlayState->sceneNum;
            const bool wasSameScene =
                wasOnline && oldScene == gPlayState->sceneNum;
            if (nowSameScene && !wasSameScene) {
                shouldReemitOwnerHorses = true;
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

    // #259 — re-emit HORSE_SPAWN for any owner-local horses now that a
    // peer has joined our scene. Walks ACTORCAT_BG (Epona's category)
    // and re-broadcasts for each in-scope ACTOR_EN_HORSE tagged with a
    // HorseNetId owned by us. Receivers ignore duplicates; this is
    // safe to over-emit.
    if (shouldReemitOwnerHorses && isConnected && IsHorseSyncEnabled() &&
        gPlayState != nullptr) {
        int reemitCount = 0;
        Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
        while (a != nullptr) {
            if (a->id == ACTOR_EN_HORSE) {
                const HorseNetId* hext =
                    ObjectExtension::GetInstance().Get<HorseNetId>(a);
                if (hext != nullptr && hext->netId != 0 && !hext->isPeerOwned) {
                    SendPacket_HorseSpawn(hext->netId, (int16_t)a->params,
                                           (int16_t)gPlayState->sceneNum,
                                           a->world.pos, a->shape.rot.y);
                    reemitCount++;
                }
            }
            a = a->next;
        }
        if (reemitCount > 0) {
            SPDLOG_INFO("[AllClientState] #259 re-emitted {} HORSE_SPAWN(s) "
                        "for peer join/scene-transition", reemitCount);
        }
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

        // #258 — Despawn peer Epona replicas owned by the disconnected
        // client. MakeHorseNetId encodes ownerClientId in the high byte
        // of the netId, so we can match by `(netId >> 24) ==
        // (clientId & 0xFF)`. Without this cleanup, peer Eponas stay
        // alive in the receiver's actor list (mPeerHorses still points
        // at a now-orphaned horse with playerControlled=1 lock — it
        // freezes in place and never gets removed until scene
        // transition clears the cache). Mirror of the NPC follower
        // cleanup above; same disconnect-lifecycle semantics.
        int horseRemoved = 0;
        for (auto hit = mPeerHorses.begin(); hit != mPeerHorses.end(); ) {
            const uint32_t owner = (hit->first >> 24) & 0xFFu;
            if (owner == (clientId & 0xFFu)) {
                Actor* horseReplica = hit->second;
                if (horseReplica != nullptr && horseReplica->update != nullptr) {
                    KillNetworkActorSilently(horseReplica);
                }
                hit = mPeerHorses.erase(hit);
                horseRemoved++;
            } else {
                ++hit;
            }
        }
        if (horseRemoved > 0) {
            SPDLOG_INFO("[AllClientState] Despawned {} horse replica(s) for "
                        "disconnected client {}", horseRemoved, clientId);
        }

        clients.erase(clientId);
    }

    // Pillar A Phase 1 — recompute effective host after client list changes.
    // Online flags, additions, and removals can all flip the election.
    RecomputeEffectiveHost();

    shouldRefreshActors = true;

    // Title-screen peers (Plans/title_screen_peer_actors.md) — peer state
    // arrives from the relay via ALL_CLIENT_STATE aggregates, not per-peer
    // UPDATE_CLIENT_STATE forwards (verified via AnchorProfile bandwidth
    // log: rx UPDATE_CLIENT_STATE=0.0 pps for the whole session). This is
    // the correct trigger for join/disconnect/online-flag transitions.
    // MaybeRebuildTitlePeers is idempotent + reentrancy-guarded — no-op
    // when the title-screen gate fails.
    MaybeRebuildTitlePeers();
}
