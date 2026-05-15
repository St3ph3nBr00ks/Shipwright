/**
 * TestDescriptor — dev-only Director descriptor for validating spawn
 * plumbing without ACTOR_EN_INVADER existing.
 *
 * Gated by `gEnhancements.AI.Director.TestDescriptorEnabled` (default 0).
 * Permanently registered so the debug panel can show its state; production
 * builds carry one virtual-method registry entry + one IsEnabled() CVar
 * read per tick (no measurable cost).
 *
 * Spawns an ACTOR_EN_TEST (Stalfos) near the most-isolated player every
 * kCooldownSec seconds (also gated by kMaxAlive). Picks Stalfos because
 * it's a stable ACTORCAT_ENEMY that gets a netId via the existing
 * EnemyNetId pipeline and exercises ENEMY_SPAWN / ENEMY_STATE /
 * ENEMY_DEFEATED end-to-end. When the player kills it, the full Director
 * lifecycle (OnEnemyDefeat → OnEnemyRemoved → OnSpawnRemoved → live-
 * count decrement → DIRECTOR_STATE_SYNC broadcast) is exercised.
 *
 * Reserved descriptor id 255 per the SpawnableEnemyDescriptor.h reserved
 * list (1 = Invader, 255 = TestDescriptor).
 *
 * See Plans/ai_director_plan.md §6.5.
 */

#pragma once

#include "../SpawnableEnemyDescriptor.h"

namespace AnchorDirector {

class TestDescriptor : public SpawnableEnemyDescriptor {
public:
    // --- Identity ---
    uint8_t      GetDescriptorId() const override { return 255; }
    const char*  GetDebugName()    const override { return "TestDescriptor"; }

    // --- Lifecycle ---
    bool IsEnabled() const override;

    // --- Per-tick proposal ---
    std::vector<SpawnProposal> ProposeSpawn(const Director& director,
                                            const SessionView& view) override;

    // --- Spawn-removed cleanup ---
    void OnSpawnRemoved(uint32_t netId, DefeatCause cause) override;

    // --- Debug surface ---
    std::string GetDebugSnapshotLine() const override;

private:
    // Tunables (compiled-in; not CVar'd in step 7 — adjust here if
    // field-testing exposes a need).
    static constexpr int  kMaxAlive       = 1;        // one stalfos at a time
    static constexpr int  kCooldownMs     = 30 * 1000;  // 30s between spawns
    static constexpr float kSpawnOffsetXZ = 200.0f;   // distance from target player

    // Counters surfaced via the debug panel (step 8) + GetDebugSnapshotLine.
    // Note: no per-descriptor "spawn executed" hook exists yet on the
    // SpawnableEnemyDescriptor interface — descriptors learn whether
    // their proposal was honored via Director::GetLiveCount(descId).
    // mProposalsOffered counts non-empty ProposeSpawn results (offered,
    // not necessarily accepted under arbitration).
    int      mProposalsOffered = 0;
    int      mTotalRemoved     = 0;
    uint32_t mLastRemovedNetId = 0;
};

}  // namespace AnchorDirector
