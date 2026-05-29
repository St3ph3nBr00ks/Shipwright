#pragma once

// Pillar C2 Phase 2 — host-only enemy bookkeeping module.
//
// Hosts the per-host state that EnemyStateSync needs but that doesn't
// belong on individual EnemyNetId extensions:
//
//   1. PendingKills  — kills received before scene load; applied at
//                      OnActorSpawn (Karebaba pendingKillNetIds path).
//   2. SceneDeaths   — per-scene set of dead netIds for join-time
//                      replay to late-joining peers.
//   3. DefeatBroadcasts — netIds for which a defeat packet has been
//                      sent or received this scene visit. Used for
//                      both OnEnemyDefeat / OnActorKill dedup AND the
//                      Karebaba "did we send the kill" attribution
//                      (consolidated from the previous twin signals
//                      `sentDefeatThisScene` + EnemyNetId::
//                      defeatPacketSent — same lifetime, same purpose).
//   4. Damagers      — netId → clientId of last damager for Q I Tier 2
//                      kill-attribution rebroadcast.
//
// All four were previously fields on the Anchor singleton; this module
// gives them a single home with a clean API. Pre-1 layout retired.
//
// Forward-compat for Pillar A Phase 2: Serialize() / ApplyFromHandoff()
// produce/consume a JSON blob the host-migration handoff packet can
// carry. v1 leaves the wiring at the host-migration call site empty.
//
// Plan: Claude/Plans/pillar_c2_enemystatesync.md.

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json_fwd.hpp>
#include "z64math.h"

namespace EnemyStateSync {

// Live-spawn snapshot record. Captured at the moment host broadcasts
// ENEMY_SPAWN for a dynamic actor; replayed verbatim from
// UpdateClientState's join-trigger to peers that missed the original
// broadcast (e.g. peer was in a different room when host's Obj_Mure2
// dynamically spawned En_Ishi children).
//
// Phase 1 of Plans/pillar_c2_live_actor_snapshot.md.
struct LiveSpawnRecord {
    int16_t  actorId;
    Vec3f    homePos;
    Vec3s    homeRot;
    int16_t  params;
    // AI Director identity (Schema 5 — mandatory).
    uint8_t  directorDescriptorId;
    uint8_t  directorVariantId;
    int      directorGroupId;
};

class HostBookkeeping {
public:
    static HostBookkeeping& Instance();

    // ----- pendingKillNetIds -----
    void RecordPendingKill(uint32_t netId);
    bool IsPendingKill(uint32_t netId) const;
    void ClearPendingKill(uint32_t netId);
    // Drops entries whose encoded scene (bits 30-16 of netId, low 15
    // bits of sceneNum) does NOT match `currentScene`. Used in
    // OnSceneSpawnActors to keep kills for the scene we just entered
    // while discarding kills for other scenes (Fix 35).
    void ClearStalePendingKillsFromOtherScenes(uint16_t currentScene);

    // Drops entries whose encoded (sceneNum, timeline) MATCHES the
    // arguments. Used by SCENE_DEATHS_CLEARED handler when the host
    // detects that a scene has become unoccupied — every client clears
    // its locally-buffered pending kills for that scene so the next
    // entrant sees the actor pool fresh (vanilla respawn parity, Test
    // 1.5 exit-gated fix). Pass `timeline = 0xFF` to match any.
    void ClearPendingKillsForScene(int16_t sceneNum, uint8_t timeline);

    // ----- deadEnemiesByScene -----
    void RecordSceneDeath(int16_t sceneNum, uint32_t netId);
    void ClearSceneDeath(int16_t sceneNum, uint32_t netId);
    void ClearScene(int16_t sceneNum);
    bool IsSceneDeath(int16_t sceneNum, uint32_t netId) const;
    // Returns the set of dead netIds for the given scene; empty set
    // when no entry exists. Reference is valid until the next mutation
    // of mSceneDeaths.
    const std::unordered_set<uint32_t>& SceneDeaths(int16_t sceneNum) const;

    // ----- liveSpawnsByScene (Phase 1 of pillar_c2_live_actor_snapshot.md) -----
    // Per-scene index of dynamic spawns currently alive on the host. Used
    // by UpdateClientState's join-trigger replay so a peer entering the
    // scene late receives matching ENEMY_SPAWN packets and spawns the
    // actors locally (mirror of SceneDeaths replay).
    void RecordLiveSpawn(int16_t sceneNum, uint32_t netId, const LiveSpawnRecord& rec);
    void RemoveLiveSpawn(uint32_t netId);
    // Returns the netId→record map for the scene; empty map when no
    // entry exists. Reference is valid until the next mutation.
    const std::unordered_map<uint32_t, LiveSpawnRecord>&
        LiveSpawnsForScene(int16_t sceneNum) const;

    // ----- sentDefeatThisScene + EnemyNetId::defeatPacketSent (merged) -----
    // ClaimDefeatBroadcast: inserts and returns true iff the netId was
    // not previously claimed. Use as a guard at OnEnemyDefeat /
    // OnActorKill: claim returns true → emit, false → already broadcast.
    bool ClaimDefeatBroadcast(uint32_t netId);
    bool HasDefeatBroadcast(uint32_t netId) const;
    // Release: per-actor-cycle clear (e.g. Karebaba respawn detector).
    // Allows the same netId to be re-killed within the same scene
    // visit after its actor instance respawns.
    void ReleaseDefeatBroadcast(uint32_t netId);
    void ClearAllDefeatBroadcasts();  // on scene re-enter (OnSceneSpawnActors)

    // ----- lastDamagerByNetId -----
    void     RecordDamager(uint32_t netId, uint32_t clientId);
    uint32_t LookupDamager(uint32_t netId) const;  // 0 if none
    void     ClearDamager(uint32_t netId);
    // Walks mDamagers, drops entries whose netId-encoded scene MATCHES
    // `enteredScene` — those actors just respawned with the same netId
    // and their old damager attribution is stale. Entries from other
    // scenes are kept. NetId encoding: bits 30-16 are sceneNum (low 15
    // bits), bit 31 is timeline (Pillar B).
    void     ClearStaleDamagers(int16_t enteredScene);

    // ----- lifecycle -----
    void Reset();  // wired into Anchor::Disable

    // ----- Pillar A Phase 2 forward-compat -----
    // Both stubs are non-empty (round-trip JSON serialise/deserialise of
    // the four containers); the host-migration packet that consumes
    // them is not yet wired. Phase 2 of the migration plan will plumb
    // these into HOST_BOOKKEEPING_HANDOFF send/receive paths.
    nlohmann::json Serialize() const;
    void           ApplyFromHandoff(const nlohmann::json& payload);

private:
    HostBookkeeping() = default;

    std::unordered_set<uint32_t>                              mPendingKills;
    std::unordered_map<int16_t, std::unordered_set<uint32_t>> mSceneDeaths;
    std::unordered_set<uint32_t>                              mDefeatBroadcasts;
    std::unordered_map<uint32_t /*netId*/, uint32_t /*clientId*/> mDamagers;
    std::unordered_map<int16_t, std::unordered_map<uint32_t, LiveSpawnRecord>>
        mLiveSpawnsByScene;
};

}  // namespace EnemyStateSync
