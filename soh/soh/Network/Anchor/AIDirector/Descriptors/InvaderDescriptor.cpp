/**
 * InvaderDescriptor — step 11 scaffold implementation.
 *
 * No spawn logic yet. ProposeSpawn returns empty unconditionally.
 * IsEnabled is a chained CVar gate — both Invaders.Enabled AND
 * Nav.Enabled must be on. Per plan §2.3, the Nav substrate is a
 * hard dependency: without it, future PickSpawnPosition (step 13)
 * has no candidate-node graph to consume.
 *
 * See header for the full step roadmap.
 */

#include "InvaderDescriptor.h"
#include "../Director.h"

#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

namespace AnchorDirector {

bool InvaderDescriptor::IsEnabled() const {
    // Chained gate — both must be on. Plan §3:
    //   "Master gate is chained with gEnhancements.Nav.Enabled
    //    (both must be on for any invader behaviour)."
    //
    // The Nav dependency is structural: PickSpawnPosition (step 13)
    // consumes RoomNavData. With Nav off, the Invader can't pick a
    // walkable spawn position; gate prevents partial functionality.
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.Enabled"), 0) != 0
        && CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0;
}

std::vector<SpawnProposal> InvaderDescriptor::ProposeSpawn(const Director& director,
                                                          const SessionView& view) {
    // Step 11: no spawn logic. Step 12 wires eligibility predicates
    // (§7.1: live-count cap, cooldown, scene-flag, boss-room-alive,
    // cutscene, time-in-scene). Step 13 adds PickSpawnPosition.
    //
    // Note: this method is still called every tick when IsEnabled
    // returns true — Director's per-tick proposal loop iterates
    // enabled descriptors. Returning empty here is the correct
    // scaffold behaviour and exercises the multi-descriptor
    // arbitration path without spawning anything.
    (void)director;
    (void)view;
    return {};
}

void InvaderDescriptor::OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
    // Step 11: stub. Step 12+ will track per-invader cleanup
    // (e.g. reset cooldown faster on Kill than Leash per plan §7.4).
    ++mTotalRemoved;
    mLastRemovedNetId = netId;
    SPDLOG_INFO("[InvaderDescriptor] OnSpawnRemoved netId={} cause={} "
                "(total spawned={} removed={}) — scaffold no-op",
                netId, (int)cause, mTotalSpawned, mTotalRemoved);
}

std::string InvaderDescriptor::GetDebugSnapshotLine() const {
    // Surfaced in the AI Director debug panel's per-descriptor block.
    // Step 11: trivial state report. Step 12+ adds current target,
    // sticky-target expires-at, recent attackers (per plan §2.11
    // weighted-scoring with revenge factor), etc.
    return "scaffold (step 11) — spawned=" + std::to_string(mTotalSpawned) +
           " removed=" + std::to_string(mTotalRemoved);
}

}  // namespace AnchorDirector
