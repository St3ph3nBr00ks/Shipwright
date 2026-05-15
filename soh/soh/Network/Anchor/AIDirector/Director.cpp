/**
 * Director — implementation. Plans/ai_director_plan.md §9.
 *
 * Step 1 (landed): empty registry, host-gated Tick body, ledgers declared
 * but unused, lifecycle hooks stubbed.
 * Step 2 (landed): BuildSessionView walks Anchor::clients into a per-tick
 * PlayerSnapshot vector + helper methods (AnyPlayerInScene /
 * AnyPlayerInCutscene / MostIsolatedPlayer / PlayerByClientId).
 * AllPlayersInBossRoom / currentRoomHasLiveBoss / framesInCurrentScene
 * stay stubbed until step 12+ (InvaderDescriptor needs them).
 * Step 3 (landed): ledger plumbing — RecordSpawn writes all three ledgers;
 * GetFramesSinceLastSpawn returns ticks-elapsed (was returning raw stored
 * frame); MsCooldownElapsed wired through Anchor::MsToGameTicks. ENEMY_-
 * DEFEATED routes to Director::OnEnemyRemoved on host from both the local
 * OnEnemyDefeat hook (HookHandlers.cpp) and the HandlePacket_EnemyDefeated
 * receive path (EnemyState.cpp).
 * Step 4 (landed): ENEMY_STATE schema 5 — director identity fields on
 * dynamic-spawn packets (Common/PacketSchemas.h + EnemyState.cpp).
 * Step 5 (landed): DIRECTOR_STATE_SYNC packet plus SerializeMigrationSnapshot
 * / ApplyMigrationSnapshot. Host broadcasts on every RecordSpawn / OnEnemy-
 * Removed; peers cache; OnBecameEffectiveHost fires Director::OnHostMigrated
 * so descriptors run their migration hook.
 * Step 6+: event-forwarding plumbing + proposal arbitration body.
 *
 * Hook registration lives in Anchor::RegisterDirectorHooks (HookHandlers.cpp
 * — sibling to the existing RegisterFollowerHooks call). The OnGameFrameUpdate
 * lambda there forwards to Director::Instance().Tick() when isConnected.
 */

#include "Director.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

#include "../Anchor.h"
#include "../Common/SceneAuthority.h"

extern "C" {
#include "z64.h"
#include "z64cutscene.h"  // CS_STATE_IDLE
}

namespace AnchorDirector {

namespace {
    // Cooldown-ledger key packing. Keeps the three identity bits in a
    // single uint32 so std::unordered_map<uint32_t, int> handles the
    // ledger without nested maps.
    inline uint32_t MakeCooldownKey(int16_t sceneNum, int8_t roomNum, uint8_t descId) {
        return (static_cast<uint32_t>(static_cast<uint16_t>(sceneNum)) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(roomNum))   << 8)
             |  static_cast<uint32_t>(descId);
    }
}  // namespace

Director& Director::Instance() {
    static Director sInstance;
    return sInstance;
}

SpawnableEnemyDescriptor* Director::Register(std::unique_ptr<SpawnableEnemyDescriptor> descriptor) {
    if (!descriptor) {
        return nullptr;
    }
    SpawnableEnemyDescriptor* raw = descriptor.get();
    mDescriptors.push_back(std::move(descriptor));
    // Run Initialize on the next Tick. Deferring lets callers register
    // multiple descriptors back-to-back without any descriptor seeing the
    // Director mid-registration.
    mInitializedDescriptors = false;
    return raw;
}

void Director::Tick() {
    // Director runs on every client but only the global-effective-host
    // actually decides spawns. Non-host clients still receive
    // DIRECTOR_STATE_SYNC (step 5) so their cached state is visible in
    // the debug panel, but Tick early-exits here.
    if (!::SceneAuthority::IsEffectiveHost()) {
        return;
    }

    // Host-side tick counter. Incremented unconditionally after the
    // IsEffectiveHost gate so the cooldown ledger reads consistent
    // values regardless of empty-registry / invalid-view early-exits.
    ++mGlobalFrameCounter;

    // No descriptors registered → nothing to do. Step 1 always lands here
    // because nothing has called Register() yet. Step 7 lands TestDescriptor;
    // step 11 lands InvaderDescriptor.
    if (mDescriptors.empty()) {
        return;
    }

    // First-tick initialization for newly-registered descriptors.
    if (!mInitializedDescriptors) {
        for (auto& d : mDescriptors) {
            d->Initialize(*this);
        }
        mInitializedDescriptors = true;
    }

    // Build the per-tick view. Empty / invalid (no players with save
    // loaded) → skip proposal work this tick.
    SessionView view = BuildSessionView();
    if (!view.IsValid()) {
        return;
    }

    // Step 3+: collect proposals from view-eligible descriptors,
    // arbitrate by priority + cooldown, ExecuteSpawn. Body lives here.
    // Step 2: view is built but no proposals collected because no
    // descriptors are registered yet (the empty-registry gate above
    // would have returned). The proposal loop is wired in step 3.
    (void)view;
}

int Director::GetFramesSinceLastSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId) const {
    uint32_t key = MakeCooldownKey(sceneNum, roomNum, descId);
    auto it = mLastSpawnFrameByKey.find(key);
    if (it == mLastSpawnFrameByKey.end()) {
        return INT_MAX;  // never spawned → cooldown trivially elapsed
    }
    const uint64_t storedFrame = static_cast<uint64_t>(it->second);
    if (mGlobalFrameCounter < storedFrame) {
        // Defensive: shouldn't happen (host counter monotonic since boot).
        // Could surface during host migration if a stale entry crossed.
        return 0;
    }
    const uint64_t delta = mGlobalFrameCounter - storedFrame;
    return (delta > static_cast<uint64_t>(INT_MAX))
               ? INT_MAX
               : static_cast<int>(delta);
}

int Director::GetLiveCount(uint8_t descId) const {
    auto it = mLiveCountByDescriptor.find(descId);
    return (it == mLiveCountByDescriptor.end()) ? 0 : it->second;
}

bool Director::MsCooldownElapsed(int16_t sceneNum, int8_t roomNum, uint8_t descId, int ms) const {
    if (ms <= 0) {
        return true;  // no cooldown requested
    }
    const int framesSince = GetFramesSinceLastSpawn(sceneNum, roomNum, descId);
    if (framesSince == INT_MAX) {
        return true;  // never spawned
    }
    if (Anchor::Instance == nullptr) {
        return true;  // host gate already caught this; defensive only
    }
    const int requiredTicks = Anchor::Instance->MsToGameTicks(ms);
    return framesSince >= requiredTicks;
}

void Director::RecordSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId, uint32_t netId) {
    const uint32_t key = MakeCooldownKey(sceneNum, roomNum, descId);
    // mGlobalFrameCounter is uint64_t but the ledger holds int. The counter
    // increments at most once per tick — overflow would take >68 years at
    // 1000 Hz. Storing as int is fine; INT_MAX cap is a far horizon.
    mLastSpawnFrameByKey[key] = (mGlobalFrameCounter > static_cast<uint64_t>(INT_MAX))
                                    ? INT_MAX
                                    : static_cast<int>(mGlobalFrameCounter);
    ++mLiveCountByDescriptor[descId];
    if (netId != 0) {
        mNetIdToDescriptor[netId] = descId;
    }

    // Step 5: broadcast updated state to peers so any migration target
    // has a fresh snapshot. Host-side only; non-host shouldn't be calling
    // RecordSpawn at all, but the SendPacket itself gates on host.
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendPacket_DirectorStateSync();
    }
}

void Director::OnEnemyRemoved(uint32_t netId, DefeatCause cause) {
    auto it = mNetIdToDescriptor.find(netId);
    if (it == mNetIdToDescriptor.end()) {
        return;  // not director-spawned
    }
    uint8_t descId = it->second;

    for (auto& d : mDescriptors) {
        if (d->GetDescriptorId() == descId) {
            d->OnSpawnRemoved(netId, cause);
            break;
        }
    }

    int& count = mLiveCountByDescriptor[descId];
    count = std::max(0, count - 1);

    mNetIdToDescriptor.erase(it);

    // Step 5: broadcast updated state to peers so any migration target
    // has a fresh snapshot. Host-side only.
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendPacket_DirectorStateSync();
    }
}

void Director::NotifyEvent(const DirectorEventPayload& evt) {
    for (auto& d : mDescriptors) {
        if (!d->IsEnabled()) {
            continue;
        }
        d->OnEvent(evt);
    }
}

void Director::OnHostMigrated(bool isNewHost) {
    for (auto& d : mDescriptors) {
        d->OnDirectorHostMigrated(isNewHost);
    }
}

SessionView Director::BuildSessionView() const {
    SessionView view;
    view.globalFrameCounter = mGlobalFrameCounter;

    // Defensive: Tick already early-exits on non-host via SceneAuthority,
    // and RegisterDirectorHooks only registers the hook when connected,
    // but we hit this path from the debug panel too (step 8) where Anchor
    // state may still be torn down.
    if (Anchor::Instance == nullptr) {
        return view;
    }
    view.currentTickMs = Anchor::Instance->mAvgGameTickMs;

    view.players.reserve(Anchor::Instance->clients.size());
    for (const auto& [clientId, client] : Anchor::Instance->clients) {
        if (!client.online) continue;

        PlayerSnapshot snap{};
        snap.clientId        = clientId;
        snap.teamId          = client.teamId;
        snap.sceneNum        = client.sceneNum;
        snap.roomNum         = client.curRoomNum;
        snap.worldPos        = client.posRot.pos;
        snap.isLocal         = (clientId == Anchor::Instance->ownClientId);
        snap.isSaveLoaded    = client.isSaveLoaded;
        snap.isInCutscene    = (client.csCtxState != CS_STATE_IDLE);
        snap.isInvulnerable  = (client.invincibilityTimer != 0);
        snap.followerActive  = client.followerActive;
        snap.isClimbing      = client.isClimbing;

        // step 12+ fields left at struct-default values until the
        // boss-room / scene-residency plumbing lands. Documented in
        // SessionView::AllPlayersInBossRoom + the descriptor that
        // first needs framesInCurrentScene.

        view.players.push_back(std::move(snap));
    }

    return view;
}

// ---------------------------------------------------------------------------
// SessionView helpers.
// ---------------------------------------------------------------------------

bool SessionView::AnyPlayerInScene(int16_t sceneNum) const {
    for (const auto& p : players) {
        if (p.sceneNum == sceneNum) return true;
    }
    return false;
}

bool SessionView::AnyPlayerInCutscene() const {
    for (const auto& p : players) {
        if (p.isInCutscene) return true;
    }
    return false;
}

bool SessionView::AllPlayersInBossRoom(int16_t sceneNum) const {
    // step 12+: needs the boss-room registry + per-room boss-alive
    // lookup. Stubbed to false (= "not committed") so descriptors that
    // gate spawns on this predicate behave conservatively (i.e. they
    // never assume team is committed when we can't yet prove it).
    (void)sceneNum;
    return false;
}

const PlayerSnapshot* SessionView::MostIsolatedPlayer() const {
    if (players.size() < 2) {
        return players.empty() ? nullptr : &players.front();
    }

    const PlayerSnapshot* best = nullptr;
    float bestMinDistSq = -1.0f;

    for (size_t i = 0; i < players.size(); ++i) {
        float minDistSq = std::numeric_limits<float>::infinity();
        for (size_t j = 0; j < players.size(); ++j) {
            if (i == j) continue;
            // Cross-scene players are infinitely far apart for isolation
            // purposes — a player alone in their own scene counts as
            // "most isolated" by default.
            if (players[i].sceneNum != players[j].sceneNum) continue;

            const float dx = players[i].worldPos.x - players[j].worldPos.x;
            const float dy = players[i].worldPos.y - players[j].worldPos.y;
            const float dz = players[i].worldPos.z - players[j].worldPos.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < minDistSq) minDistSq = distSq;
        }
        if (minDistSq > bestMinDistSq) {
            bestMinDistSq = minDistSq;
            best = &players[i];
        }
    }
    return best;
}

const PlayerSnapshot* SessionView::PlayerByClientId(uint32_t clientId) const {
    for (const auto& p : players) {
        if (p.clientId == clientId) return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Host-migration snapshot (step 5).
// ---------------------------------------------------------------------------
//
// Wire format (compact arrays-of-pairs keep the JSON small):
//
//   {
//     "globalFrame":  <uint64>,
//     "lastSpawnFrames": [[<uint32 key>, <int frame>], ...],
//     "liveCounts":     [[<uint8 descId>, <int count>], ...],
//     "netIdToDesc":    [[<uint32 netId>, <uint8 descId>], ...],
//     "descState":      {"<descId>": <opaque blob>, ...}
//   }
//
// The packet envelope (type, schema, etc.) is added by
// SendPacket_DirectorStateSync; this method returns just the data payload.

nlohmann::json Director::SerializeMigrationSnapshot() const {
    nlohmann::json snapshot;
    snapshot["globalFrame"] = mGlobalFrameCounter;

    nlohmann::json lastSpawnFrames = nlohmann::json::array();
    for (const auto& [key, frame] : mLastSpawnFrameByKey) {
        lastSpawnFrames.push_back(nlohmann::json::array({key, frame}));
    }
    snapshot["lastSpawnFrames"] = lastSpawnFrames;

    nlohmann::json liveCounts = nlohmann::json::array();
    for (const auto& [descId, count] : mLiveCountByDescriptor) {
        liveCounts.push_back(nlohmann::json::array({descId, count}));
    }
    snapshot["liveCounts"] = liveCounts;

    nlohmann::json netIdToDesc = nlohmann::json::array();
    for (const auto& [netId, descId] : mNetIdToDescriptor) {
        netIdToDesc.push_back(nlohmann::json::array({netId, descId}));
    }
    snapshot["netIdToDesc"] = netIdToDesc;

    // Per-descriptor opaque state. Key as string for stable JSON-object
    // representation; descriptor ids are small uint8 → string conversion
    // is unambiguous.
    nlohmann::json descState = nlohmann::json::object();
    for (const auto& d : mDescriptors) {
        nlohmann::json blob = d->SerializeMigrationState();
        if (!blob.is_null() && !blob.empty()) {
            descState[std::to_string(d->GetDescriptorId())] = std::move(blob);
        }
    }
    snapshot["descState"] = descState;

    return snapshot;
}

void Director::ApplyMigrationSnapshot(const nlohmann::json& snapshot) {
    if (!snapshot.is_object()) {
        return;
    }

    // Replace, don't merge. The host is authoritative; whatever local
    // ledger entries existed are stale by definition.
    mLastSpawnFrameByKey.clear();
    mLiveCountByDescriptor.clear();
    mNetIdToDescriptor.clear();

    if (snapshot.contains("globalFrame")) {
        mGlobalFrameCounter = snapshot["globalFrame"].get<uint64_t>();
    }

    if (snapshot.contains("lastSpawnFrames") && snapshot["lastSpawnFrames"].is_array()) {
        for (const auto& pair : snapshot["lastSpawnFrames"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mLastSpawnFrameByKey[pair[0].get<uint32_t>()] = pair[1].get<int>();
        }
    }

    if (snapshot.contains("liveCounts") && snapshot["liveCounts"].is_array()) {
        for (const auto& pair : snapshot["liveCounts"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mLiveCountByDescriptor[pair[0].get<uint8_t>()] = pair[1].get<int>();
        }
    }

    if (snapshot.contains("netIdToDesc") && snapshot["netIdToDesc"].is_array()) {
        for (const auto& pair : snapshot["netIdToDesc"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mNetIdToDescriptor[pair[0].get<uint32_t>()] = pair[1].get<uint8_t>();
        }
    }

    if (snapshot.contains("descState") && snapshot["descState"].is_object()) {
        const auto& descState = snapshot["descState"];
        for (auto& d : mDescriptors) {
            const std::string key = std::to_string(d->GetDescriptorId());
            if (descState.contains(key)) {
                d->RestoreMigrationState(descState[key]);
            }
        }
    }
}

}  // namespace AnchorDirector
