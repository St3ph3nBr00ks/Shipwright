/**
 * GenericSpawnDescriptor — data-driven scene/room-appropriate random
 * enemy spawns for the AI Director.
 *
 * Substrate for #311 (`Plans/game_director_scene_room_spawns.md`).
 *
 * v1 scope (this file): scaffold + data-driven registry. The registry
 * itself (`kRoomSpawnRegistry`) is EMPTY at scaffold — future consumers
 * (Werewolf #316, Goma egg swarms #323, per-dungeon ambient spawns)
 * populate entries as they land. This keeps the substrate scaffold
 * fully behavior-neutral until a consumer opts in.
 *
 * Descriptor id: 2 (reserved per SpawnableEnemyDescriptor.h — Invader=1,
 * TestDescriptor=255).
 *
 * Priority: Ambient (20) — always preempted by Invader (Standard=50)
 * when both would fire in the same tick. Ambient background flavor
 * yields to hostile-pursuer gameplay.
 *
 * Sync: host-authoritative RNG per Rule 1 of
 * `Analysis/en_sw_mp_sync_audit_2026-07-31.md`. Spawn broadcasts via
 * existing ENEMY_SPAWN + Director substrate (schema-5 descriptorId
 * field). Zero new packet families.
 */

#pragma once

#include "../SpawnableEnemyDescriptor.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"  // Vec3f
}

namespace AnchorDirector {

// Which surface a spawn candidate prefers. Directs
// FindSpawnPositionForSurface (defined in GenericSpawnDescriptor.cpp)
// to sample the appropriate geometry.
enum class SpawnSurface : uint8_t {
    Floor   = 0,
    Ceiling = 1,   // v2 — deferred until first consumer needs it
    Water   = 2,   // v2
    Air     = 3,   // v2
};

// One candidate actor within a room's spawn pool. Multiple candidates
// per room use `weight` for weighted-random pick; higher weights are
// more likely.
struct SpawnCandidate {
    int16_t     actorId       = 0;
    uint16_t    actorParams   = 0;
    uint8_t     weight        = 1;   // 1-100 typical
    // Optional gates. 0 = no restriction; otherwise LinkAge/day-time
    // check against gSaveContext.linkAge / IS_DAY.
    uint8_t     requireLinkAge = 0;  // 0=any, 1=child, 2=adult
    uint8_t     requireDay     = 0;  // 0=any, 1=day-only, 2=night-only
    SpawnSurface surface       = SpawnSurface::Floor;
};

// Room-level spawn configuration. One entry per (sceneNum, roomNum)
// tuple. sceneNum + roomNum are the lookup key. -1 as roomNum is
// reserved for "any room in this scene" but NOT wired at scaffold —
// consumers use explicit rooms until the wildcard is needed.
struct RoomSpawnConfig {
    int16_t                       sceneNum             = -1;
    int8_t                        roomNum              = -1;
    uint16_t                      cooldownSeconds      = 60;
    uint8_t                       maxConcurrentSpawns  = 2;   // per-room live cap
    uint16_t                      minTimeInRoomSeconds = 5;   // wait N seconds after player enters room before proposing
    float                         minDistFromPlayer    = 300.0f;
    std::vector<SpawnCandidate>   candidates;
};

// Registry accessor. Returns a reference to the singleton vector so
// consumers can append entries at init time (e.g. from an actor's
// ShipInit or the Director constructor). Empty at scaffold.
std::vector<RoomSpawnConfig>& RoomSpawnRegistry();

// CVar name — host-authoritative sync via EnforcedCVarRegistry.
constexpr const char* GenericSpawnsCVarName() {
    return "gEnhancements.Director.GenericSpawnsEnabled";
}

class GenericSpawnDescriptor : public SpawnableEnemyDescriptor {
public:
    // --- Identity ---
    uint8_t     GetDescriptorId() const override { return 2; }
    const char* GetDebugName()    const override { return "GenericSpawn"; }

    // --- Lifecycle ---
    bool IsEnabled() const override;

    // --- Per-tick proposal ---
    std::vector<SpawnProposal> ProposeSpawn(const Director& director,
                                            const SessionView& view) override;

    // --- Spawn-executed / removed callbacks (track per-room live count) ---
    void OnSpawnExecuted(uint32_t netId, const Vec3f& worldPos) override;
    void OnSpawnRemoved(uint32_t netId, DefeatCause cause) override;

    // --- Debug surface ---
    std::string GetDebugSnapshotLine() const override;

private:
    // Per-room live-spawn tracking. Keyed on packed (sceneNum, roomNum).
    // Bumped in OnSpawnExecuted with the last-proposed (scene, room);
    // decremented in OnSpawnRemoved using the netId → (scene, room) map.
    std::unordered_map<uint32_t, int>     mLiveCountByRoom;
    std::unordered_map<uint32_t, uint32_t> mNetIdToRoomKey;

    // Per-descriptor counters for the debug panel.
    int mProposalsOffered = 0;
    int mSpawnsExecuted   = 0;

    // Cache of the last proposal's (scene, room) so OnSpawnExecuted
    // knows which room bucket to bump. Written at ProposeSpawn's
    // successful return; consumed by OnSpawnExecuted (fires 1-2 ticks
    // later after arbitration + Actor_Spawn).
    int16_t mLastProposedScene = -1;
    int8_t  mLastProposedRoom  = -1;

    // Cooldown ledger key packer — matches Director's internal packer
    // but scoped to (scene, room) only (no descId dimension since
    // GenericSpawnDescriptor owns its own map).
    static uint32_t MakeRoomKey(int16_t sceneNum, int8_t roomNum) {
        return (static_cast<uint32_t>(static_cast<uint16_t>(sceneNum)) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(roomNum))   << 8);
    }
};

}  // namespace AnchorDirector
