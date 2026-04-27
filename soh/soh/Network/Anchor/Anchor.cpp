#include "Anchor.h"
#include "Common/PacketSchemas.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <algorithm>
#include <chrono>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// MARK: - AnchorClient helpers

void AnchorClient::RetireBakedModel() {
    // No live model → nothing to retire. Belt-and-braces: if a retired model is
    // already sitting in the slot with no counter, clear it on the way out (it
    // has been there at least one frame already, so safe to destroy now).
    if (bakedModel == nullptr) {
        if (retiredBakedModel != nullptr && retireFrameCounter == 0) {
            retiredBakedModel = nullptr;
        }
        return;
    }
    // If the retire slot is already full, the previous retiree has been sitting
    // there for however long it took to bake the model we're now retiring
    // (synchronous bake ≥ ~400 ms = ≥ 24 frames at 60 fps, >> kRetireFrames).
    // Destroying it here is safe; no frame can still be referencing it.
    retiredBakedModel = std::move(bakedModel);
    retireFrameCounter = kRetireFrames;
}

// MARK: - Bandwidth profiler (#62)

void Anchor::RecordProfileSample(const nlohmann::json& payload, bool tx) {
    if (CVarGetInteger("gEnhancements.AnchorProfiler", 0) == 0) return;
    if (!payload.contains("type")) return;
    std::string type = payload["type"].get<std::string>();
    size_t bytes = payload.dump().size() + 1; // +1 for null-byte delimiter the relay expects

    std::lock_guard<std::mutex> lock(profileMutex);
    auto& bucket = (tx ? profileTx : profileRx)[type];
    bucket.count += 1;
    bucket.bytes += bytes;
}

void Anchor::FlushProfileIfDue() {
    if (CVarGetInteger("gEnhancements.AnchorProfiler", 0) == 0) return;
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(profileMutex);
    if (profileWindowStartMs == 0) { profileWindowStartMs = nowMs; return; }
    uint64_t elapsed = nowMs - profileWindowStartMs;
    if (elapsed < kProfilerWindowMs) return;

    std::unordered_set<std::string> types;
    for (auto& [t, _] : profileTx) types.insert(t);
    for (auto& [t, _] : profileRx) types.insert(t);
    std::vector<std::string> sorted(types.begin(), types.end());
    std::sort(sorted.begin(), sorted.end(), [this](const std::string& a, const std::string& b) {
        return (profileTx[a].bytes + profileRx[a].bytes) >
               (profileTx[b].bytes + profileRx[b].bytes);
    });

    SPDLOG_INFO("[AnchorProfile] window={}ms   type                   tx_pps   tx_Bps   rx_pps   rx_Bps",
                elapsed);
    for (const auto& t : sorted) {
        auto& tx = profileTx[t];
        auto& rx = profileRx[t];
        SPDLOG_INFO("[AnchorProfile] {:<22} {:>8.1f} {:>8.0f} {:>8.1f} {:>8.0f}",
                    t,
                    tx.count * 1000.0 / elapsed, tx.bytes * 1000.0 / elapsed,
                    rx.count * 1000.0 / elapsed, rx.bytes * 1000.0 / elapsed);
    }
    profileTx.clear();
    profileRx.clear();
    profileWindowStartMs = nowMs;
}

// MARK: - Overrides

void Anchor::Enable() {
    // Drain the disable-time graveyard from any prior Disable(). Any
    // UI-driven re-enable takes far longer than kRetireFrames frames, so
    // every in-flight Gfx command that referenced these allocations has
    // long since been consumed by the renderer and they are safe to free.
    postDisableBakedModels.clear();
    postDisableCustomSkeletons.clear();

    Network::Enable(CVarGetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org"),
                    CVarGetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383));
    ownClientId = CVarGetInteger(CVAR_REMOTE_ANCHOR("LastClientId"), 0);
    roomState.ownerClientId = 0;
}

void Anchor::Disable() {
    Network::Disable();

    // KB-15 / issue #110 — drain BakedPlayerModel and customSkeleton off
    // every client into the disable-time graveyard before clients.clear()
    // destroys them. The previous frame's draw queued Gfx commands that
    // still reference these allocations; freeing them synchronously crashes
    // the renderer on the next frame's Gfx walk (see log 119).
    for (auto& [clientId, client] : clients) {
        if (client.bakedModel != nullptr) {
            postDisableBakedModels.push_back(std::move(client.bakedModel));
        }
        if (client.retiredBakedModel != nullptr) {
            postDisableBakedModels.push_back(std::move(client.retiredBakedModel));
        }
        if (client.customSkeleton != nullptr) {
            postDisableCustomSkeletons.push_back(std::move(client.customSkeleton));
        }
    }

    clients.clear();
    RefreshClientActors();
}

void Anchor::OnConnected() {
    SendPacket_Handshake();
    RegisterHooks();

    // Phase 5 #60 — drop any stale "last sent" snapshots so the first
    // post-reconnect send for every enemy goes out on the next frame
    // instead of waiting for the keepalive timer. Separately, the send
    // path also guards cache writes on isConnected — so if we had been
    // sending into a disconnected socket, nothing accumulated anyway.
    // Doing both ("belt-and-braces") costs one hashmap clear.
    Anchor_ClearEnemyUpdateCache();

    if (IsSaveLoaded()) {
        // Reconnect orphan-actor recovery (log 116 bug — P2 disconnected
        // during the 8-second window between connect and scene init, which
        // unregistered OnActorSpawn. Setup actors then spawned with no
        // EnemyNetId extension; ENEMY_UPDATE / ENEMY_DEFEATED couldn't
        // find them. Walk all syncable categories now and backfill any
        // actor missing the extension.
        BackfillEnemyNetIds();

        SendPacket_RequestTeamState();
        // Bug A (log 69) — on reconnect the host does NOT re-broadcast
        // ENEMY_DEFEATED packets that were sent while we were offline,
        // and RequestTeamState only covers time-of-day + item flags.
        // Bump sceneSpawnEpoch and re-send UpdateClientState; the host's
        // HandlePacket_UpdateClientState detects the epoch change and
        // replays deadEnemiesByScene for our current scene. Harmless on
        // first connect (replay is idempotent — already-dead enemies
        // just get Actor_Kill'd again).
        sceneSpawnEpoch++;
        SendPacket_UpdateClientState();
        SPDLOG_INFO("[Anchor] Reconnect epoch bump → {} (triggers host dead-enemy replay)",
                    sceneSpawnEpoch);
    }
}

void Anchor::OnDisconnected() {
    RegisterHooks();
}

void Anchor::ProcessOutgoingPackets() {
    // Copy all queued packets while holding the lock, then send them after releasing
    std::queue<nlohmann::json> packetsToSend;
    {
        std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
        packetsToSend.swap(outgoingPacketQueue);
    }

    // Send packets without holding the lock
    while (!packetsToSend.empty()) {
        nlohmann::json payload = packetsToSend.front();
        packetsToSend.pop();

        RecordProfileSample(payload, /*tx=*/true);
        if (!payload.contains("quiet")) {
            SPDLOG_DEBUG("[Anchor] Sending payload:\n{}", payload.dump());
        }
        Network::SendJsonToRemote(payload);
    }
    FlushProfileIfDue();
}

void Anchor::SendJsonToRemote(nlohmann::json payload) {
    if (!isConnected) {
        return;
    }

    payload["clientId"] = ownClientId;

    // Pillar F — auto-inject schema for all packet types. Per-packet
    // schema lives in Common/PacketSchemas.h and is bumped when fields
    // are added. Senders may override before this point if needed.
    if (!payload.contains("schema") && payload.contains("type")) {
        payload["schema"] = PacketSchemas::GetPacketSchema(payload["type"].get<std::string>());
    }

    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Queuing payload:\n{}", payload.dump());
    }

    if (payload["type"] == HANDSHAKE) {
        Network::SendJsonToRemote(payload);
        return;
    }

    // Queue the packet to be sent on the network thread
    std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
    outgoingPacketQueue.push(payload);
}

void Anchor::OnIncomingJson(nlohmann::json payload) {
    // If it doesn't contain a type, it's not a valid payload
    if (!payload.contains("type")) {
        return;
    }

    RecordProfileSample(payload, /*tx=*/false);

    // If it's not a quiet payload, log it
    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Received payload:\n{}", payload.dump());
    }

    std::string packetType = payload["type"].get<std::string>();

    // Pillar F — schema-driven compatibility. Packets carrying a "schema"
    // field opt into the schema layer and DO NOT need the clientVersion
    // check (schema versioning handles cross-version compatibility via
    // additive-only fields + maxSchema clamping).
    //
    // The hand-maintained allowlist (ALL_CLIENT_STATE, UPDATE_CLIENT_STATE,
    // PLAYER_UPDATE, ENEMY_UPDATE/DEFEATED/SPAWN/RESPAWN, DAMAGE_ENEMY,
    // ENEMY_HIT_PLAYER) was retired 2026-04-25 — those packets all carry
    // the schema field now via SendJsonToRemote auto-injection.
    //
    // Legacy fallback: pre-Pillar-F peers (without the schema field)
    // continue to be filtered by the clientVersion mismatch check.
    if (!payload.contains("schema")) {
        if (payload.contains("clientId")) {
            uint32_t clientId = payload["clientId"].get<uint32_t>();
            if (clients.contains(clientId) && clients[clientId].clientVersion != clientVersion) {
                return;
            }
        }
    }

    // Queue all packets to be processed on the game thread
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
    incomingPacketQueue.push(payload);
}

void Anchor::ProcessIncomingPacketQueue() {
    // Copy all queued packets while holding the lock, then process them after releasing
    std::queue<nlohmann::json> packetsToProcess;
    {
        std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
        packetsToProcess.swap(incomingPacketQueue);
    }

    // Process packets without holding the lock
    while (!packetsToProcess.empty()) {
        nlohmann::json payload = packetsToProcess.front();
        packetsToProcess.pop();

        std::string packetType = payload["type"].get<std::string>();

        isProcessingIncomingPacket = true;

        try {
            // packetType here is a string so we can't use a switch statement
            if (packetType == ALL_CLIENT_STATE)
                HandlePacket_AllClientState(payload);
            else if (packetType == ENEMY_UPDATE)
                HandlePacket_EnemyUpdate(payload);
            else if (packetType == ENEMY_DEFEATED)
                HandlePacket_EnemyDefeated(payload);
            else if (packetType == ENEMY_SPAWN)
                HandlePacket_EnemySpawn(payload);
            else if (packetType == ENEMY_RESPAWN)
                HandlePacket_EnemyRespawn(payload);
            else if (packetType == DAMAGE_ENEMY)
                HandlePacket_DamageEnemy(payload);
            else if (packetType == DAMAGE_PLAYER)
                HandlePacket_DamagePlayer(payload);
            else if (packetType == ENEMY_HIT_PLAYER)
                HandlePacket_EnemyHitPlayer(payload);
            else if (packetType == DISABLE_ANCHOR)
                HandlePacket_DisableAnchor(payload);
            else if (packetType == ENTRANCE_DISCOVERED)
                HandlePacket_EntranceDiscovered(payload);
            else if (packetType == GAME_COMPLETE)
                HandlePacket_GameComplete(payload);
            else if (packetType == GIVE_ITEM)
                HandlePacket_GiveItem(payload);
            else if (packetType == OCARINA_SFX)
                HandlePacket_OcarinaSfx(payload);
            else if (packetType == PLAYER_UPDATE)
                HandlePacket_PlayerUpdate(payload);
            else if (packetType == PLAYER_SFX)
                HandlePacket_PlayerSfx(payload);
            else if (packetType == UPDATE_TEAM_STATE)
                HandlePacket_UpdateTeamState(payload);
            else if (packetType == REQUEST_TEAM_STATE)
                HandlePacket_RequestTeamState(payload);
            else if (packetType == REQUEST_TELEPORT)
                HandlePacket_RequestTeleport(payload);
            else if (packetType == SERVER_MESSAGE)
                HandlePacket_ServerMessage(payload);
            else if (packetType == SET_CHECK_STATUS)
                HandlePacket_SetCheckStatus(payload);
            else if (packetType == SET_FLAG)
                HandlePacket_SetFlag(payload);
            else if (packetType == TELEPORT_TO)
                HandlePacket_TeleportTo(payload);
            else if (packetType == UNSET_FLAG)
                HandlePacket_UnsetFlag(payload);
            else if (packetType == UPDATE_BEANS_COUNT)
                HandlePacket_UpdateBeansCount(payload);
            else if (packetType == UPDATE_CLIENT_STATE)
                HandlePacket_UpdateClientState(payload);
            else if (packetType == UPDATE_ROOM_STATE)
                HandlePacket_UpdateRoomState(payload);
            else if (packetType == UPDATE_DUNGEON_ITEMS)
                HandlePacket_UpdateDungeonItems(payload);
            else if (packetType == SCENE_TRANSITION_HANDOFF)
                HandlePacket_SceneTransitionHandoff(payload);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[Anchor] Exception while processing incoming packet {}", e.what());
            SPDLOG_ERROR("[Anchor] Packet: {}", payload.dump());
        }

        isProcessingIncomingPacket = false;
    }
}

// MARK: - Misc/Helpers

// Kills all existing anchor actors and respawns them with the new client data

struct DummyPlayerClientId {
    uint32_t clientId = 0;
};
static ObjectExtension::Register<DummyPlayerClientId> DummyPlayerClientIdRegister;
ObjectExtension::Register<EnemyNetId> EnemyNetIdRegister;

uint32_t Anchor::GetDummyPlayerClientId(const Actor* actor) {
    const DummyPlayerClientId* clientId = ObjectExtension::GetInstance().Get<DummyPlayerClientId>(actor);
    return clientId != nullptr ? clientId->clientId : 0;
}

void Anchor::SetDummyPlayerClientId(const Actor* actor, uint32_t clientId) {
    ObjectExtension::GetInstance().Set<DummyPlayerClientId>(actor, DummyPlayerClientId{ clientId });
}

void Anchor::RefreshClientActors() {
    if (!IsSaveLoaded()) {
        return;
    }

    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;

    while (actor != NULL) {
        if (actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update) {
            NameTag_RemoveAllForActor(actor);
            Actor_Kill(actor);
        }
        actor = actor->next;
    }

    for (auto& [clientId, client] : clients) {
        if (!client.online || client.self) {
            continue;
        }

        spawningDummyPlayerForClientId = clientId;
        // We are using a hook `ShouldActorInit` to override the init/update/draw/destroy functions of the Player we
        // spawn We quickly store a mapping of "index" to clientId, then within the init function we use this to get the
        // clientId and store it on player->zTargetActiveTimer (unused s32 for the dummy) for convenience
        auto dummy =
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, client.posRot.pos.x, client.posRot.pos.y,
                        client.posRot.pos.z, client.posRot.rot.x, client.posRot.rot.y, client.posRot.rot.z, 0);
        client.player = (Player*)dummy;
    }
    spawningDummyPlayerForClientId = 0;
}

void Anchor::RecomputeEffectiveHost() {
    // Pillar A Phase 1 — pure (a) election rule. See Anchor.h doc on the
    // effectiveHostClientId field for the full semantics.
    uint32_t orig = roomState.ownerClientId;
    uint32_t newEffective = ownClientId;  // safety fallback

    auto origIt = clients.find(orig);
    if (origIt != clients.end() && origIt->second.online) {
        newEffective = orig;
    } else {
        // Original owner is offline (or not in clients). Pick the lowest-
        // numbered online clientId. All clients converge on the same answer
        // because they all see the same ALL_CLIENT_STATE.
        uint32_t lowest = UINT32_MAX;
        for (auto& [clientId, client] : clients) {
            if (client.online && clientId < lowest) {
                lowest = clientId;
            }
        }
        if (lowest != UINT32_MAX) {
            newEffective = lowest;
        }
    }

    if (newEffective != effectiveHostClientId) {
        SPDLOG_INFO("[Anchor] Effective host changed: {} -> {} (orig owner={} {})",
                    effectiveHostClientId, newEffective, orig,
                    (origIt != clients.end() && origIt->second.online) ? "online" : "offline");
        bool localBecameHost = (newEffective == ownClientId &&
                                effectiveHostClientId != ownClientId);
        effectiveHostClientId = newEffective;
        if (localBecameHost) {
            OnBecameEffectiveHost();
        }
    }
}

void Anchor::OnBecameEffectiveHost() {
    SPDLOG_INFO("[Anchor] This client is now effective host (clientId={})", ownClientId);
    // Per migration plan A1: accept empty deadEnemiesByScene. Original host
    // already broadcast ENEMY_DEFEATED for every kill up to the disconnect,
    // so other clients' world-state is already correct. Only future late-
    // joiners are affected — they'll see corpses for post-migration kills
    // only. Acceptable for First Dungeon Demo scope.
    sentDefeatThisScene.clear();
}

void Anchor::BackfillEnemyNetIds() {
    // Recovery for the reconnect-during-scene-init scenario (log 116):
    // if the OnActorSpawn hook was unregistered when the scene set up its
    // initial actors, those actors stayed in the actor lists with no
    // EnemyNetId extension. ENEMY_UPDATE / ENEMY_DEFEATED then can't find
    // them by netId and the scene's enemy sync is silently broken until
    // the player leaves the scene.
    //
    // This function walks every syncable actor category and assigns the
    // same deterministic netId that OnActorSpawn would have set. It uses
    // the identical hash/encoding as OnActorSpawn so a future hit through
    // the normal hook produces the same netId — idempotent.
    //
    // Skip actors that already have an extension; only fill in missing.
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    int filled = 0;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (actor != nullptr) {
            // Use the same admission predicate OnActorSpawn uses.
            if (IsSyncableActor(actor) &&
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor) == nullptr) {
                // Single source of truth for the netId encoding lives in
                // ActorSyncHelpers::EncodeEnemyNetId. Both this backfill path
                // and OnActorSpawn route through it so a future hit through
                // the normal hook produces the same netId — idempotent.
                uint32_t netId = EncodeEnemyNetId(actor);

                EnemyNetId ext;
                ext.netId = netId;
                ext.skelAnime = GetEnemySkelAnime(actor);
                ext.limbCount = ext.skelAnime ? ext.skelAnime->limbCount : 0;
                ObjectExtension::GetInstance().Set<EnemyNetId>(actor, std::move(ext));

                SPDLOG_INFO("[BackfillNetId] Assigned netId={} to actorId={} ptr={} "
                            "(scene init missed by hook during disconnect)",
                            netId, actor->id, (void*)actor);
                filled++;
            }
            actor = actor->next;
        }
    }
    if (filled > 0) {
        SPDLOG_INFO("[BackfillNetId] Backfilled {} actor(s) on reconnect", filled);
    }
}

bool Anchor::IsSaveLoaded() {
    if (gPlayState == nullptr) {
        return false;
    }

    if (GET_PLAYER(gPlayState) == nullptr) {
        return false;
    }

    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }

    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }

    return true;
}
