/**
 * InvaderDescriptor — step 12 implementation.
 *
 * Eligibility-predicate chain from ai_invader_plan.md §7.1. ProposeSpawn
 * walks the gates; if any blocks, returns empty (with a throttled
 * diagnostic SPDLOG explaining which gate). If all pass, returns a
 * proposal with a placeholder spawn position (target's worldPos) and
 * placeholder actor (ACTOR_EN_TEST + STALFOS_TYPE_1).
 *
 * Step 13 will replace the placeholder position with PickSpawnPosition
 * consuming RoomNavData. Step 15 will replace ACTOR_EN_TEST with the
 * real ACTOR_EN_INVADER.
 *
 * Predicates implemented this step:
 *   - Live-count cap (CVar AI.Invaders.MaxAlive, default 1).
 *   - Cooldown (CVar AI.Invaders.CooldownSeconds, default 90s).
 *   - Cutscene check (no team member with csCtxState != CS_STATE_IDLE).
 *   - Scene blacklist (hardcoded list of narrative-climax scenes:
 *     Hyrule Castle, Ganon endgame).
 *
 * Predicates stubbed for step 13 (return false / true respectively
 * so they never block; documented inline):
 *   - Boss-room-with-live-boss check (needs boss-room registry).
 *   - Time-in-scene check (needs per-client framesInCurrentScene
 *     plumbing on the Director, currently stubbed to 0).
 *
 * Diagnostic SPDLOGs are gated behind gEnhancements.AI.Director.Log-
 * Proposals (the shared diagnostic CVar wired in step 9). Throttled
 * to ~5s at 20fps via mTicksSinceLog.
 */

#include "InvaderDescriptor.h"
#include "../Director.h"

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // step 13: PickSpawnPosition

#include <imgui.h>  // ImGui::Button for RenderDebugUI
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <cmath>    // std::sqrt for OFFERED log distance metric
#include <cstdlib>  // std::rand for candidate-node sampling
#include <optional>

extern "C" {
#include "z64.h"
#include "z64actor.h"  // ACTOR_EN_TEST placeholder until step 15's ACTOR_EN_INVADER
}

namespace AnchorDirector {

namespace {

// Same gate as TestDescriptor — shared diagnostic CVar. Off by default
// in production; flip on when debugging "why isn't my descriptor
// spawning?"
inline bool IsLoggingProposals() {
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.LogProposals"), 0) != 0;
}

// Read MaxAlive / CooldownMs from CVars with plan-defined defaults.
inline int ReadMaxAlive() {
    int v = CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.MaxAlive"),
                           InvaderDescriptor::kDefaultMaxAlive);
    if (v < 1)  v = 1;
    if (v > 4)  v = 4;   // plan §3 range
    return v;
}

inline int ReadCooldownMs() {
    int sec = CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.CooldownSeconds"),
                             InvaderDescriptor::kDefaultCooldownMs / 1000);
    if (sec < 30)  sec = 30;
    if (sec > 600) sec = 600;  // plan §3 range
    return sec * 1000;
}

// Scenes where Invader spawns should never fire regardless of other
// gates. Narrative-climax / end-game scenes where an invader breaks
// immersion or interferes with scripted sequences.
//
// Step 12 list — minimal. Extends as field-testing surfaces more
// scenes that need exclusion. The boss-room check (currently stubbed)
// will catch dungeon-boss rooms generically; this list is for the
// non-boss-room exclusions.
//
// Values match SCENE_* enum from soh/include/tables/scene_table.h.
bool IsSceneFlaggedNoInvaders(int16_t sceneNum) {
    switch (sceneNum) {
        case  0x0A:  // SCENE_GANONS_TOWER
        case  0x0D:  // SCENE_INSIDE_GANONS_CASTLE
        case  0x0E:  // SCENE_GANONS_TOWER_COLLAPSE_INTERIOR
        case  0x0F:  // SCENE_INSIDE_GANONS_CASTLE_COLLAPSE
        case  0x19:  // SCENE_GANONDORF_BOSS
        case  0x1A:  // SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR
        case  0x4F:  // SCENE_GANON_BOSS
        case  0x5F:  // SCENE_HYRULE_CASTLE
            return true;
        default:
            return false;
    }
}

// Step 13 will implement this against a boss-room registry +
// HostBookkeeping defeat tracking. Stub returns false (= never a
// live-boss room) so it doesn't block step 12 testing. Documented
// in plan §7.1 predicate 5.
bool IsBossRoomWithLiveBoss(int16_t sceneNum, int8_t roomNum) {
    (void)sceneNum;
    (void)roomNum;
    // TODO step 13: lookup boss-room registry, check IsSyncedBossActor
    // is alive via EnemyStateSync::HostBookkeeping defeat tracking.
    return false;
}

// Step 13 will plumb framesInCurrentScene through Director's per-tick
// snapshot. Stub returns true (= always settled) so it doesn't block
// step 12 testing. Documented in plan §7.1 predicate 8.
bool AllPlayersSettledInScene(const SessionView& view, int16_t sceneNum) {
    (void)view;
    (void)sceneNum;
    // TODO step 13: walk view.players, require framesInCurrentScene >=
    // MsToGameTicks(5000) for the team-leader's scene.
    return true;
}

// Step 13: nav-aware spawn placement. Picks a walkable NavNode from
// RoomNavData filtered by:
//   - flags: NODE_WALKABLE set AND NODE_ORPHANED / NODE_HAZARD /
//     NODE_UNDERWATER all clear.
//   - distance: ≥ kMinPlayerDistU from any team member in the same
//     scene (plan §7.2 predicate 4 — "avoid in-face spawn").
//
// Sampling strategy: random offset into the node vector + linear scan
// up to kMaxSampledNodes. Avoids iterating the entire graph each call
// (Hyrule Field has thousands of nodes) while still touching enough
// candidates to find one passing the filters most ticks.
//
// Deferred to a follow-up if field-testing surfaces the need:
//   - Line-of-sight gate (plan §7.2 #5). Requires BgCheck raycast
//     against gPlayState; cheap to add later.
//   - Reachability gate. Candidate node already exists in the graph,
//     which implies it's reachable from at least the floodfill seeds.
//     Real reachability-from-player would need FindBestReachable-
//     SubgoalPath; add when an unreachable spawn surfaces.
//
// Returns nullopt when no candidate passes the filters or RoomNavData
// isn't loaded for the (scene, room). Caller treats nullopt as
// "abort proposal for this tick".
std::optional<Vec3f> PickSpawnPosition(int16_t sceneNum, int8_t roomNum,
                                       const SessionView& view) {
    constexpr int   kMaxSampledNodes      = 64;
    constexpr float kMinPlayerDistU       = 200.0f;
    constexpr float kMinPlayerDistSq      = kMinPlayerDistU * kMinPlayerDistU;
    // Y-delta gate — added 2026-05-15 post-field-test log 186. Inside
    // Deku Tree's room 0 nav graph spans the main chamber + the basement
    // pit (Gohma area, Y ≈ -940). Without this gate, PickSpawnPosition
    // sampled a basement node when the player was on the upper floor at
    // Y ≈ 0; the resulting Invader was unreachable, the player thought
    // they killed it but only killed a force-spawned duplicate, and the
    // live-count cap blocked all subsequent respawns. Capped to roughly
    // one OoT dungeon-floor of vertical separation.
    constexpr float kMaxYDeltaFromTarget  = 300.0f;

    const AnchorNavRoom::RoomNavData* data =
        AnchorNavRoom::GetForRoom(sceneNum, roomNum);
    if (data == nullptr || data->nodes.empty()) {
        // No nav graph for this room — RoomNavData CVar may be off,
        // scene may not yet be scanned, or it's a custom map without
        // a baked graph. Return nullopt; caller logs + aborts.
        return std::nullopt;
    }

    const size_t nodeCount = data->nodes.size();
    const size_t startIdx  = static_cast<size_t>(std::rand()) % nodeCount;
    const size_t sampled   = std::min<size_t>(kMaxSampledNodes, nodeCount);

    for (size_t i = 0; i < sampled; ++i) {
        const size_t idx = (startIdx + i) % nodeCount;
        const AnchorNavRoom::NavNode& n = data->nodes[idx];

        // Flag filters per the candidate-loop sketch in
        // ai_follower_reference_for_director_and_invader.md §3.
        if ((n.flags & AnchorNavRoom::NODE_WALKABLE) == 0) continue;
        if (n.flags & (AnchorNavRoom::NODE_ORPHANED |
                       AnchorNavRoom::NODE_HAZARD   |
                       AnchorNavRoom::NODE_UNDERWATER)) continue;

        // XZ distance gate against every team-member in this scene.
        bool tooClose = false;
        for (const PlayerSnapshot& p : view.players) {
            if (p.sceneNum != sceneNum) continue;
            const float dx = n.pos.x - p.worldPos.x;
            const float dz = n.pos.z - p.worldPos.z;
            if (dx * dx + dz * dz < kMinPlayerDistSq) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) continue;

        // Y-delta gate. Reject candidates more than kMaxYDeltaFromTarget
        // from EVERY team member in the same scene — keeps the spawn on
        // an architecturally-reachable floor instead of dropping into a
        // disconnected vertical area (basement pits, mid-air platforms
        // accessible only via teleport, etc.). Must be reachable from
        // AT LEAST one player to count as "valid floor" — multi-player
        // sessions tolerate one player being on a different floor.
        bool sameFloorAsAnyPlayer = false;
        for (const PlayerSnapshot& p : view.players) {
            if (p.sceneNum != sceneNum) continue;
            if (std::abs(n.pos.y - p.worldPos.y) <= kMaxYDeltaFromTarget) {
                sameFloorAsAnyPlayer = true;
                break;
            }
        }
        if (!sameFloorAsAnyPlayer) continue;

        return n.pos;
    }
    return std::nullopt;
}

}  // namespace

bool InvaderDescriptor::IsEnabled() const {
    // Chained gate — both must be on. See header for rationale.
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.Enabled"), 0) != 0
        && CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0;
}

std::vector<SpawnProposal> InvaderDescriptor::ProposeSpawn(const Director& director,
                                                          const SessionView& view) {
    // Throttled diagnostic logging — gated by LogProposals CVar AND
    // throttle counter so a stuck descriptor logs once per ~5s.
    ++mTicksSinceLog;
    const bool shouldLog = (mTicksSinceLog >= 100) && IsLoggingProposals();
    auto markLog = [&]() { mTicksSinceLog = 0; };

    // Predicate 1: live-count cap (plan §7.1 #2).
    const int liveCount = director.GetLiveCount(GetDescriptorId());
    const int maxAlive = ReadMaxAlive();
    if (liveCount >= maxAlive) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: liveCount={} cap={}",
                        liveCount, maxAlive);
            markLog();
        }
        return {};
    }

    // Pick the target. Plan §7.2 says "team member with longest trail"
    // — that's PlayerTrail consumption in step 13. Step 12 placeholder:
    // most-isolated player (matches what TestDescriptor uses; serves
    // the same role for proposal-position purposes).
    const PlayerSnapshot* target = view.MostIsolatedPlayer();
    if (target == nullptr) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: no target player "
                        "(view.players={})", view.players.size());
            markLog();
        }
        return {};
    }

    if (!target->isSaveLoaded) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: target save not loaded "
                        "(clientId={})", target->clientId);
            markLog();
        }
        return {};
    }

    // Predicate 6: no team member in a cutscene (plan §7.1 #7).
    // Invaders should not aggro during scripted sequences.
    if (view.AnyPlayerInCutscene()) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: a player is in cutscene");
            markLog();
        }
        return {};
    }

    // Predicate 4: scene blacklist (plan §7.1 #4).
    if (IsSceneFlaggedNoInvaders(target->sceneNum)) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: scene={} flagged noInvaders",
                        (int)target->sceneNum);
            markLog();
        }
        return {};
    }

    // Predicate 5: boss-room-with-live-boss (plan §7.1 #5).
    // Stub returns false at step 12 — never blocks. Step 13 wires it.
    if (IsBossRoomWithLiveBoss(target->sceneNum, target->roomNum)) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: boss room with live boss "
                        "(scene={} room={})",
                        (int)target->sceneNum, (int)target->roomNum);
            markLog();
        }
        return {};
    }

    // Predicate 8: team has been in scene long enough (plan §7.1 #8).
    // Stub returns true at step 12 — never blocks. Step 13 wires it.
    if (!AllPlayersSettledInScene(view, target->sceneNum)) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: not yet 5s in scene "
                        "(scene={})", (int)target->sceneNum);
            markLog();
        }
        return {};
    }

    // Predicate 3: cooldown (plan §7.1 #3). MsCooldownElapsed wraps
    // Anchor::MsToGameTicks so the threshold holds at any tick rate.
    const int cooldownMs = ReadCooldownMs();
    if (!director.MsCooldownElapsed(target->sceneNum, target->roomNum,
                                    GetDescriptorId(), cooldownMs)) {
        if (shouldLog) {
            const int framesSince = director.GetFramesSinceLastSpawn(
                target->sceneNum, target->roomNum, GetDescriptorId());
            SPDLOG_INFO("[InvaderDescriptor] no proposal: cooldown "
                        "(scene={} room={} framesSinceLast={} required={}ms)",
                        (int)target->sceneNum, (int)target->roomNum,
                        framesSince, cooldownMs);
            markLog();
        }
        return {};
    }

    // Step 13: nav-aware spawn placement. Picks a walkable graph
    // node ≥200u from any team member. Returns nullopt if RoomNav-
    // Data isn't loaded for this room, or if no candidate passed the
    // filters after sampling.
    auto pickedPos = PickSpawnPosition(target->sceneNum, target->roomNum, view);
    if (!pickedPos.has_value()) {
        if (shouldLog) {
            SPDLOG_INFO("[InvaderDescriptor] no proposal: PickSpawnPosition "
                        "found no candidate (scene={} room={}) — nav graph "
                        "absent or all sampled nodes failed filters",
                        (int)target->sceneNum, (int)target->roomNum);
            markLog();
        }
        return {};
    }

    // All gates passed. Build proposal.
    //
    // Step 12-13 placeholders:
    //   - actorId = ACTOR_EN_TEST (Stalfos). Real ACTOR_EN_INVADER lands
    //     in step 15 once #208 unblocks combat AI.
    //   - actorParams = 1 (STALFOS_TYPE_1, visible variant — type 0 is
    //     Lens-of-Truth-invisible).
    //   - worldPos: step-13 nav-aware placement via PickSpawnPosition.
    SpawnProposal p;
    p.source       = this;
    p.sceneNum     = target->sceneNum;
    p.roomNum      = target->roomNum;
    p.worldPos     = *pickedPos;
    p.yawTowards   = 0;
    p.actorId      = ACTOR_EN_TEST;
    p.actorParams  = 1;
    p.variantId    = 0;
    p.priority     = DescriptorPriority::Standard;
    p.groupId      = 0;
    ++mProposalsOffered;
    if (IsLoggingProposals()) {
        const float dx = p.worldPos.x - target->worldPos.x;
        const float dz = p.worldPos.z - target->worldPos.z;
        const float distFromTarget = std::sqrt(dx * dx + dz * dz);
        SPDLOG_INFO("[InvaderDescriptor] OFFERED proposal: target=client{} "
                    "scene={} room={} playerPos=({:.0f},{:.0f},{:.0f}) "
                    "spawnPos=({:.0f},{:.0f},{:.0f}) distFromTarget={:.0f}u "
                    "liveCount={}/{} cooldown={}ms",
                    target->clientId, (int)target->sceneNum, (int)target->roomNum,
                    target->worldPos.x, target->worldPos.y, target->worldPos.z,
                    p.worldPos.x, p.worldPos.y, p.worldPos.z,
                    distFromTarget,
                    liveCount, maxAlive, cooldownMs);
    }
    markLog();
    return { p };
}

std::vector<SpawnProposal> InvaderDescriptor::BuildForcedProposal(const Director& director,
                                                                 const SessionView& view) {
    // Bypasses cooldown / cap / cutscene / scene-blacklist gates — the
    // user explicitly asked for a spawn via the dev button.
    //
    // Position selection: still tries PickSpawnPosition first (so the
    // spawn lands on a real walkable node), but if no candidate passes
    // we fall back to the target player's worldPos. Force-spawn must
    // produce a visible spawn for testing; "force button did nothing"
    // would defeat the purpose.
    (void)director;
    const PlayerSnapshot* target = view.MostIsolatedPlayer();
    if (target == nullptr) {
        SPDLOG_INFO("[InvaderDescriptor] BuildForcedProposal: no target");
        return {};
    }

    Vec3f spawnPos = target->worldPos;
    auto pickedPos = PickSpawnPosition(target->sceneNum, target->roomNum, view);
    if (pickedPos.has_value()) {
        spawnPos = *pickedPos;
        SPDLOG_INFO("[InvaderDescriptor] ForceSpawn: PickSpawnPosition succeeded "
                    "at ({:.0f},{:.0f},{:.0f})",
                    spawnPos.x, spawnPos.y, spawnPos.z);
    } else {
        SPDLOG_INFO("[InvaderDescriptor] ForceSpawn: PickSpawnPosition failed; "
                    "falling back to player.worldPos");
    }

    SpawnProposal p;
    p.source       = this;
    p.sceneNum     = target->sceneNum;
    p.roomNum      = target->roomNum;
    p.worldPos     = spawnPos;
    p.yawTowards   = 0;
    p.actorId      = ACTOR_EN_TEST;
    p.actorParams  = 1;
    p.variantId    = 0;
    p.priority     = DescriptorPriority::Standard;
    p.groupId      = 0;
    ++mProposalsOffered;
    return { p };
}

void InvaderDescriptor::OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
    ++mTotalRemoved;
    mLastRemovedNetId = netId;
    SPDLOG_INFO("[InvaderDescriptor] OnSpawnRemoved netId={} cause={} "
                "(proposals={} removed={})",
                netId, (int)cause, mProposalsOffered, mTotalRemoved);
}

std::string InvaderDescriptor::GetDebugSnapshotLine() const {
    return "proposals=" + std::to_string(mProposalsOffered) +
           " removed=" + std::to_string(mTotalRemoved) +
           " lastRemove=" + std::to_string(mLastRemovedNetId);
}

void InvaderDescriptor::RenderDebugUI(const Director& director) {
    (void)director;
    if (ImGui::Button("Force Spawn Invader")) {
        // Bypasses cooldown / cap / cutscene / scene-blacklist gates.
        // RecordSpawn still increments the live count + broadcasts
        // state. Use sparingly during step 14 testing.
        Director::Instance().ForceSpawn(GetDescriptorId());
    }
}

}  // namespace AnchorDirector
