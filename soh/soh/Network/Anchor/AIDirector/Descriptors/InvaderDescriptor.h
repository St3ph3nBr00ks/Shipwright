/**
 * InvaderDescriptor — Director-side spawn-decision adapter for the
 * future AI Invader (ACTOR_EN_INVADER, ACTORCAT_NPC, hostile-NPC
 * sibling of the NPC Follower).
 *
 * **Step 11 scope**: scaffold only.
 *   - Registered alongside TestDescriptor in Director::Director().
 *   - IsEnabled reads gEnhancements.AI.Invaders.Enabled AND
 *     gEnhancements.Nav.Enabled (master gate per plan §3 / §2.3 —
 *     Nav substrate is a hard dependency for any future spawn-
 *     position selection).
 *   - ProposeSpawn returns empty vector unconditionally — no spawn
 *     logic yet. Validates the two-descriptor Director path
 *     (arbitration loop, registry iteration, debug-panel rendering).
 *
 * **Step 12 scope** (not in this file yet): eligibility predicates
 * from ai_invader_plan.md §7.1 — live-count cap, cooldown, scene-
 * flag check, boss-room-with-live-boss check, cutscene check,
 * minimum time-in-scene. Returns a proposal with placeholder
 * spawn position when all gates pass.
 *
 * **Step 13 scope**: PickSpawnPosition consumes RoomNavData to
 * pick walkable candidate nodes filtered by player-distance,
 * player-LOS, room boundaries.
 *
 * **Step 15 scope**: hand off to feature/ai-invader branch where
 * the actual ACTOR_EN_INVADER scaffolding lives. The descriptor
 * remains in the Director's registry; what changes is the actor
 * id the ExecuteSpawn call gets. Combat AI is blocked on #208
 * (follower state-machine formal design pass).
 *
 * **Translation guide reference**:
 * Claude/Plans/actor_feature_translation_guide.md catalogues the
 * NPC Follower → NPC Invader feature translation patterns. The
 * Invader actor (when built in step 15) clones NPC Follower's
 * structure with two diffs: targets players instead of leader,
 * and damage routing is host-authoritative (race B).
 *
 * Reserved descriptor id 1 per SpawnableEnemyDescriptor.h.
 */

#pragma once

#include "../SpawnableEnemyDescriptor.h"

namespace AnchorDirector {

class InvaderDescriptor : public SpawnableEnemyDescriptor {
public:
    // --- Identity ---
    uint8_t      GetDescriptorId() const override { return 1; }
    const char*  GetDebugName()    const override { return "Invader"; }

    // --- Lifecycle ---
    bool IsEnabled() const override;

    // --- Per-tick proposal (scaffold: empty) ---
    std::vector<SpawnProposal> ProposeSpawn(const Director& director,
                                            const SessionView& view) override;

    // --- Spawn-removed cleanup ---
    void OnSpawnRemoved(uint32_t netId, DefeatCause cause) override;

    // --- Debug surface ---
    std::string GetDebugSnapshotLine() const override;

private:
    // Counters for the debug panel (step 12+ will increment these).
    int      mTotalSpawned    = 0;
    int      mTotalRemoved    = 0;
    uint32_t mLastSpawnedNetId = 0;
    uint32_t mLastRemovedNetId = 0;
};

}  // namespace AnchorDirector
