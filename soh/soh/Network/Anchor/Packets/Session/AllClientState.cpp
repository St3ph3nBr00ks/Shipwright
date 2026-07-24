#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Notification/Notification.h"
#include "soh/Enhancements/audio/VoicePack.h"  // OnPeerAudioModChanged — peer pack load on snapshot apply

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
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            clients[client.clientId].self = true;
        } else {
            clients[client.clientId].self = false;
            if (clients.contains(client.clientId)) {
                const bool wasOnline = clients[client.clientId].online;
                const bool isNowOnline = client.online;
                if (wasOnline != isNowOnline && !isGlobalRoom) {
                    Notification::Emit({
                        .prefix = client.name,
                        .message = isNowOnline ? "Connected" : "Disconnected",
                    });
                }

                // #263 — peer-state-driven cleanup. The relay marks
                // disconnected clients online=false but keeps them in the
                // newClients list for reconnect continuity (verified at
                // Anchor/anchor_git/client.go:166-172). The pre-existing
                // clientsToRemove loop below only fires for clients
                // ACTUALLY removed (rare admin-kick path). The genuine
                // disconnect path is the online=true → false transition
                // detected here. Move both NPC Follower cleanup (was
                // previously buried in clientsToRemove; same architectural
                // bug — never fired) and horse cleanup (#258 → fixed at
                // #263) here.
                if (wasOnline && !isNowOnline) {
                    // NPC Follower cleanup (was incorrectly in
                    // clientsToRemove loop; moved here per #263).
                    auto npcIt = mPeerFollowerNpcs.find(client.clientId);
                    if (npcIt != mPeerFollowerNpcs.end()) {
                        Actor* replica = npcIt->second;
                        if (replica != nullptr && replica->update != nullptr) {
                            Actor_Kill(replica);
                        }
                        mPeerFollowerNpcs.erase(npcIt);
                        SPDLOG_INFO("[AllClientState] #263 Despawned NPC replica "
                                    "for disconnected client {}", client.clientId);
                    }

                    // Horse cleanup (#258 → fixed at #263). Use
                    // HorseNetId::ownerClientId field directly — full
                    // uint32 compare. The original cleanup used
                    // `(netId >> 24) & 0xFF` which truncated to 8 bits,
                    // self-consistent for current session sizes but
                    // fragile (Anchor's nextClientId is atomic.Uint64).
                    int horseRemoved = 0;
                    for (auto hit = mPeerHorses.begin();
                         hit != mPeerHorses.end(); ) {
                        const HorseNetId* hext =
                            ObjectExtension::GetInstance().Get<HorseNetId>(hit->second);
                        if (hext != nullptr &&
                            hext->ownerClientId == client.clientId &&
                            hit->second != nullptr &&
                            hit->second->update != nullptr) {
                            KillNetworkActorSilently(hit->second);
                            hit = mPeerHorses.erase(hit);
                            horseRemoved++;
                        } else {
                            ++hit;
                        }
                    }
                    if (horseRemoved > 0) {
                        SPDLOG_INFO("[AllClientState] #263 Despawned {} horse "
                                    "replica(s) for disconnected client {}",
                                    horseRemoved, client.clientId);
                    }

                    // Wait-for-peer coordination barrier cleanup
                    // (Phase 2b). Peer disconnect drops them from every
                    // pending barrier's remaining set; barriers whose
                    // remaining set empties as a result release
                    // immediately.
                    for (auto bit = pendingCoordination.begin();
                         bit != pendingCoordination.end(); ) {
                        bit->second.peersRemaining.erase(client.clientId);
                        if (bit->second.peersRemaining.empty()) {
                            SPDLOG_INFO("[CoordBarrier] Released key='{}' — "
                                        "last remaining peer {} disconnected",
                                        bit->first, client.clientId);
                            bit = pendingCoordination.erase(bit);
                        } else {
                            ++bit;
                        }
                    }
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

        // Capture peer's prior audio pack before write-through so we can
        // detect a peer-side change delivered via ALL_CLIENT_STATE
        // (e.g. reconnect snapshot). The UPDATE_CLIENT_STATE receive path
        // already fires OnPeerAudioModChanged on delta; ALL_CLIENT_STATE
        // must do the same or peer's `gPeerPacks[clientId]` stays empty
        // and P1 hears default voice for P2 (playtest 2026-07-15,
        // log 723 P2 reconnect at 19:29:10 → voice never loaded on P1).
        const std::string prevAudio =
            clients.contains(client.clientId)
                ? clients[client.clientId].audioModFilename
                : std::string();

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
        clients[client.clientId].audioModFilename    = client.audioModFilename;
        clients[client.clientId].followerActive = client.followerActive;
        clients[client.clientId].isClimbing = client.isClimbing;
        clients[client.clientId].isCrawling = client.isCrawling;

        // Trigger peer-pack load on transition. Mirrors
        // UpdateClientState.cpp:224-226. Skip self (local pack managed
        // by dropdown handler) and no-op when unchanged.
        if (!client.self &&
            client.audioModFilename != prevAudio) {
            SPDLOG_INFO("[CoopVoice] AllClientState clientId={}: audio \"{}\" -> \"{}\"",
                        client.clientId, prevAudio, client.audioModFilename);
            SOH::VoicePack::OnPeerAudioModChanged(client.clientId,
                                                   client.audioModFilename);
        }
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
    //
    // NOTE (#263): the normal disconnect path now handles NPC + horse
    // cleanup at the online=true→false transition detection above
    // (lines ~52-90). This block fires only for the rare case where a
    // client is genuinely REMOVED from the relay's newClients list (e.g.
    // admin kick of an online client). When that's the only path that
    // triggered cleanup (the client was online, never went through
    // online=false transition first), this block needs the same logic.
    // Idempotent when the new path already fired — find returns nothing.
    for (auto& clientId : clientsToRemove) {
        auto npcIt = mPeerFollowerNpcs.find(clientId);
        if (npcIt != mPeerFollowerNpcs.end()) {
            Actor* replica = npcIt->second;
            if (replica != nullptr && replica->update != nullptr) {
                Actor_Kill(replica);
            }
            mPeerFollowerNpcs.erase(npcIt);
            SPDLOG_INFO("[AllClientState] Despawned NPC replica for "
                        "removed client {} (admin-kick path)", clientId);
        }

        // Horse cleanup (#263 fix). Use HorseNetId::ownerClientId field
        // directly for full uint32 compare (the previous bit-shift
        // truncation truncated to 8 bits — see #263 for details).
        int horseRemoved = 0;
        for (auto hit = mPeerHorses.begin(); hit != mPeerHorses.end(); ) {
            const HorseNetId* hext =
                ObjectExtension::GetInstance().Get<HorseNetId>(hit->second);
            if (hext != nullptr && hext->ownerClientId == clientId &&
                hit->second != nullptr && hit->second->update != nullptr) {
                KillNetworkActorSilently(hit->second);
                hit = mPeerHorses.erase(hit);
                horseRemoved++;
            } else {
                ++hit;
            }
        }
        if (horseRemoved > 0) {
            SPDLOG_INFO("[AllClientState] Despawned {} horse replica(s) for "
                        "removed client {} (admin-kick path)",
                        horseRemoved, clientId);
        }

        clients.erase(clientId);
    }

    // Pillar A Phase 1 — recompute effective host after client list changes.
    // Online flags, additions, and removals can all flip the election.
    RecomputeEffectiveHost();

    // Pillar A Phase 1.5 A2.3 — banner when returning original host has
    // been offline past the relay's 5-min INACTIVITY_TIMEOUT. The relay
    // deletes their old clientId; they reconnect with a new one, so
    // roomState.ownerClientId is no longer resolvable in clients[]. Only
    // fires once per session (guarded by sBannerFired) and only on the
    // local client that IS the returning original owner (based on the
    // relay's re-assigned local ownClientId not matching roomState.ownerClientId
    // AND the old ownerClientId being absent from clients[]).
    static bool sSessionExpiredBannerFired = false;
    if (!sSessionExpiredBannerFired && !isGlobalRoom) {
        const uint32_t origOwner = roomState.ownerClientId;
        if (origOwner != 0 && origOwner != ownClientId &&
            clients.find(origOwner) == clients.end()) {
            // origOwner is set (session had a real host at some point),
            // isn't us, and isn't in the roster anymore. No way to know
            // from HERE alone whether WE are the returning original — the
            // banner text is written neutrally ("Session host role has
            // transferred") so it makes sense on any client that observes
            // this state.
            Notification::Emit({
                .prefix = "Session host expired",
                .message = "Original host's session timed out; new host retains authority.",
            });
            sSessionExpiredBannerFired = true;
            SPDLOG_INFO("[Anchor] A2.3 session-expired banner emitted: origOwner={} "
                        "no longer in clients (relay INACTIVITY_TIMEOUT expired)",
                        origOwner);
        }
    }

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
