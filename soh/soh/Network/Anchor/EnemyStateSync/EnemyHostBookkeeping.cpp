#include "EnemyHostBookkeeping.h"

#include <nlohmann/json.hpp>

namespace EnemyStateSync {

namespace {
// Empty-set singleton for SceneDeaths()'s no-entry path so the
// reference returned to callers stays valid.
const std::unordered_set<uint32_t> kEmptyDeaths;
}  // namespace

HostBookkeeping& HostBookkeeping::Instance() {
    static HostBookkeeping instance;
    return instance;
}

// ---------------------------------------------------------------------
// Pending kills
// ---------------------------------------------------------------------

void HostBookkeeping::RecordPendingKill(uint32_t netId) {
    mPendingKills.insert(netId);
}

bool HostBookkeeping::IsPendingKill(uint32_t netId) const {
    return mPendingKills.count(netId) != 0;
}

void HostBookkeeping::ClearPendingKill(uint32_t netId) {
    mPendingKills.erase(netId);
}

void HostBookkeeping::ClearStalePendingKillsFromOtherScenes(uint16_t currentScene) {
    for (auto it = mPendingKills.begin(); it != mPendingKills.end();) {
        // netId encoding: bits 30-16 carry the scene low 15 bits.
        // Mask to the same uint16_t the historical inline loop used.
        const uint16_t entryScene = (uint16_t)(*it >> 16);
        if (entryScene != currentScene) {
            it = mPendingKills.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------
// Scene deaths
// ---------------------------------------------------------------------

void HostBookkeeping::RecordSceneDeath(int16_t sceneNum, uint32_t netId) {
    mSceneDeaths[sceneNum].insert(netId);
}

void HostBookkeeping::ClearSceneDeath(int16_t sceneNum, uint32_t netId) {
    auto it = mSceneDeaths.find(sceneNum);
    if (it != mSceneDeaths.end()) {
        it->second.erase(netId);
    }
}

void HostBookkeeping::ClearScene(int16_t sceneNum) {
    mSceneDeaths.erase(sceneNum);
}

bool HostBookkeeping::IsSceneDeath(int16_t sceneNum, uint32_t netId) const {
    auto it = mSceneDeaths.find(sceneNum);
    return it != mSceneDeaths.end() && it->second.count(netId) != 0;
}

const std::unordered_set<uint32_t>& HostBookkeeping::SceneDeaths(int16_t sceneNum) const {
    auto it = mSceneDeaths.find(sceneNum);
    return it != mSceneDeaths.end() ? it->second : kEmptyDeaths;
}

// ---------------------------------------------------------------------
// Defeat broadcasts (merged sentDefeatThisScene + defeatPacketSent)
// ---------------------------------------------------------------------

bool HostBookkeeping::ClaimDefeatBroadcast(uint32_t netId) {
    return mDefeatBroadcasts.insert(netId).second;
}

bool HostBookkeeping::HasDefeatBroadcast(uint32_t netId) const {
    return mDefeatBroadcasts.count(netId) != 0;
}

void HostBookkeeping::ReleaseDefeatBroadcast(uint32_t netId) {
    mDefeatBroadcasts.erase(netId);
}

void HostBookkeeping::ClearAllDefeatBroadcasts() {
    mDefeatBroadcasts.clear();
}

// ---------------------------------------------------------------------
// Damagers
// ---------------------------------------------------------------------

void HostBookkeeping::RecordDamager(uint32_t netId, uint32_t clientId) {
    mDamagers[netId] = clientId;
}

uint32_t HostBookkeeping::LookupDamager(uint32_t netId) const {
    auto it = mDamagers.find(netId);
    return it != mDamagers.end() ? it->second : 0u;
}

void HostBookkeeping::ClearDamager(uint32_t netId) {
    mDamagers.erase(netId);
}

void HostBookkeeping::ClearStaleDamagers(int16_t enteredScene) {
    // Bits 30-16 of netId encode sceneNum low 15 bits (Pillar B added
    // bit 31 as the timeline indicator). Mirror the existing inline
    // loop's mask + s16 cast so behaviour matches the pre-extraction
    // code byte-for-byte.
    // "Stale" = entries whose encoded scene MATCHES the just-entered
    // scene; those actors respawned with the same netId, so the pre-
    // respawn damager attribution no longer applies. Entries from other
    // scenes are kept (their actors weren't reset by this scene-load).
    for (auto it = mDamagers.begin(); it != mDamagers.end();) {
        const int16_t entryScene = static_cast<int16_t>((it->first >> 16) & 0xFFFF);
        if (entryScene == enteredScene) {
            it = mDamagers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

void HostBookkeeping::Reset() {
    mPendingKills.clear();
    mSceneDeaths.clear();
    mDefeatBroadcasts.clear();
    mDamagers.clear();
}

// ---------------------------------------------------------------------
// Pillar A Phase 2 forward-compat
// ---------------------------------------------------------------------

nlohmann::json HostBookkeeping::Serialize() const {
    nlohmann::json out;
    out["pendingKills"] = mPendingKills;
    nlohmann::json sceneDeaths = nlohmann::json::object();
    for (const auto& [sceneNum, deaths] : mSceneDeaths) {
        sceneDeaths[std::to_string(sceneNum)] = deaths;
    }
    out["sceneDeaths"]      = std::move(sceneDeaths);
    out["defeatBroadcasts"] = mDefeatBroadcasts;
    nlohmann::json damagers = nlohmann::json::object();
    for (const auto& [netId, clientId] : mDamagers) {
        damagers[std::to_string(netId)] = clientId;
    }
    out["damagers"] = std::move(damagers);
    return out;
}

void HostBookkeeping::ApplyFromHandoff(const nlohmann::json& payload) {
    Reset();
    if (payload.contains("pendingKills")) {
        for (const auto& netId : payload["pendingKills"]) {
            mPendingKills.insert(netId.get<uint32_t>());
        }
    }
    if (payload.contains("sceneDeaths") && payload["sceneDeaths"].is_object()) {
        for (auto it = payload["sceneDeaths"].begin();
             it != payload["sceneDeaths"].end();
             ++it) {
            const int16_t sceneNum = static_cast<int16_t>(std::stoi(it.key()));
            for (const auto& netId : it.value()) {
                mSceneDeaths[sceneNum].insert(netId.get<uint32_t>());
            }
        }
    }
    if (payload.contains("defeatBroadcasts")) {
        for (const auto& netId : payload["defeatBroadcasts"]) {
            mDefeatBroadcasts.insert(netId.get<uint32_t>());
        }
    }
    if (payload.contains("damagers") && payload["damagers"].is_object()) {
        for (auto it = payload["damagers"].begin();
             it != payload["damagers"].end();
             ++it) {
            const uint32_t netId = static_cast<uint32_t>(std::stoul(it.key()));
            mDamagers[netId] = it.value().get<uint32_t>();
        }
    }
}

}  // namespace EnemyStateSync
