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
#include "../../Anchor.h"  // Phase 1 §7.5 — Anchor::Instance->MsToGameTicks for orphan / grace timers
                           // (TWO levels up — Descriptors/ is nested under AIDirector/ which is
                           // under Anchor/; relative path Director.h is one level up but Anchor.h
                           // is two)
#include "../Director.h"

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // step 13: PickSpawnPosition

#include <imgui.h>  // ImGui::Button for RenderDebugUI
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <cmath>    // std::sqrt for OFFERED log distance metric
#include <cstdio>   // std::snprintf for GetDebugSnapshotLine pos formatting
#include <cstdlib>  // std::rand for candidate-node sampling
#include <limits>   // std::numeric_limits<float>::infinity in PickValidTarget
#include <optional>
#include <utility>  // std::pair for OnTick's toFollowSpawn vector
#include <vector>   // OnTick's toDespawn / toFollowSpawn vectors

extern "C" {
#include "z64.h"
#include "z64actor.h"  // ACTOR_EN_TEST placeholder until step 15's ACTOR_EN_INVADER
}

// gPlayState — Phase 1 §7.5 OnTick reads sceneNum / roomCtx for the
// orphan-timer's lastKnownScene/Room population. Same trap-avoidance
// pattern as Director.cpp (Pitfall 15 territory).
extern "C" PlayState* gPlayState;

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
        case  0x44:  // SCENE_CHAMBER_OF_THE_SAGES (sage-awakening cutscene
                     // chamber; narrative-only, no gameplay)
        case  0x45:  // SCENE_CASTLE_COURTYARD_GUARDS_DAY (young-Link stealth
                     // section — Invader would break the guard mechanic)
        case  0x46:  // SCENE_CASTLE_COURTYARD_GUARDS_NIGHT (same)
        case  0x4A:  // SCENE_CASTLE_COURTYARD_ZELDA (first Zelda meeting,
                     // scripted cutscene area)
        case  0x4F:  // SCENE_GANON_BOSS
        case  0x5F:  // SCENE_HYRULE_CASTLE
        case  0x64:  // SCENE_OUTSIDE_GANONS_CASTLE (rainbow-bridge approach;
                     // log 204 — Invader followed across the bridge to the
                     // castle entrance, breaking endgame atmosphere)
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
    // live-count cap blocked all subsequent respawns.
    //
    // Tightened 100u (down from 300u) 2026-05-15 post-log-190. Inside
    // Deku Tree's main chamber has multiple stacked walkable platforms
    // within 300u — 300u was permissive enough that "same floor" still
    // accepted candidates two visible levels above the player. 100u
    // restricts to roughly "same elevation" — single-platform spawns
    // only. Tune up if a multi-level scene legitimately needs broader.
    constexpr float kMaxYDeltaFromTarget  = 100.0f;

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

void InvaderDescriptor::OnSpawnExecuted(uint32_t netId, const Vec3f& worldPos) {
    // Scalar mirror for the in-world red-marker (legacy step-8 API).
    mLastSpawnPos      = worldPos;
    mLastSpawnNetId    = netId;
    mHasLastSpawn      = true;
    mLastSpawnSceneNum = (gPlayState != nullptr) ? (int16_t)gPlayState->sceneNum : -1;

    // Phase 1 §7.5: populate runtime state for this Invader. Most
    // fields filled lazily by OnTick / OnEvent — here we just record
    // the spawn position + clear any pending-follow state from a prior
    // scene-following continuation that just landed.
    InvaderRuntimeState& state = mActiveInvaders[netId];
    state.lastSpawnPos        = worldPos;
    state.pendingFollowSpawn  = false;
    state.followGraceFrames   = 0;
    state.orphanFrames        = 0;
    // sceneNum / roomNum populated by the first OnTick after spawn
    // (reads gPlayState at that point); targetClientId by the first
    // sticky-target assignment in ProposeSpawn / OnTick.

    SPDLOG_INFO("[InvaderDescriptor] OnSpawnExecuted netId={} pos=({:.0f},{:.0f},{:.0f}) "
                "activeInvaders={}",
                netId, worldPos.x, worldPos.y, worldPos.z, mActiveInvaders.size());
}

void InvaderDescriptor::OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
    ++mTotalRemoved;
    mLastRemovedNetId = netId;

    // Phase 1 §7.5: drop runtime state for this Invader. Sticky target,
    // orphan timer, pending-follow are all gone. If another Invader
    // is still alive (MaxAlive > 1), its state in mActiveInvaders
    // continues independently.
    mActiveInvaders.erase(netId);

    // Note: do NOT clear mLastSpawnPos (scalar) here. The "where did my last
    // spawn appear" diagnostic stays useful even after kill — the
    // user might still want to know where to look for orphans on
    // subsequent rounds.
    SPDLOG_INFO("[InvaderDescriptor] OnSpawnRemoved netId={} cause={} "
                "(proposals={} removed={} remainingActive={})",
                netId, (int)cause, mProposalsOffered, mTotalRemoved,
                mActiveInvaders.size());
}

std::string InvaderDescriptor::GetDebugSnapshotLine() const {
    std::string out = "proposals=" + std::to_string(mProposalsOffered) +
                      " removed=" + std::to_string(mTotalRemoved);
    if (mHasLastSpawn) {
        // Use snprintf for stable integer-precision world coords.
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      " lastSpawnPos=(%.0f,%.0f,%.0f) netId=%u",
                      mLastSpawnPos.x, mLastSpawnPos.y, mLastSpawnPos.z,
                      mLastSpawnNetId);
        out += buf;
    }
    return out;
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

// ---------------------------------------------------------------------------
// Phase 1 §7.5 — lifecycle persistence.
// ---------------------------------------------------------------------------

bool InvaderDescriptor::IsValidTarget(const PlayerSnapshot& p) const {
    // Per plan §7.5, a player is invalid when ANY of:
    //   - offline / save not loaded
    //   - in cutscene (csCtxState != CS_STATE_IDLE)
    //   - in boss room with live boss
    //   - in blacklisted scene
    //   - dead / mid-respawn (proxied by !isSaveLoaded today; a future
    //     PlayerSnapshot.isDead field would be more precise)
    //
    // online filter is implicit: SessionView::BuildSessionView only
    // includes online clients. So we only need to check the rest.
    //
    // Used for SPAWN DECISIONS (sticky-target re-eval, PickValidTarget,
    // ProposeSpawn). Cutscene exclusion is correct here — Invaders
    // should not aggro during scripted sequences. For the "should the
    // Invader STAY ALIVE through transient unavailability" check, see
    // IsPersistentTarget below.
    if (!p.isSaveLoaded) return false;
    if (p.isInCutscene)  return false;
    if (IsSceneFlaggedNoInvaders(p.sceneNum)) return false;
    if (IsBossRoomWithLiveBoss(p.sceneNum, p.roomNum)) return false;
    return true;
}

bool InvaderDescriptor::IsPersistentTarget(const PlayerSnapshot& p) const {
    // "Persistent" = the player would be a valid target if not for
    // TRANSIENT conditions (cutscene). Used by the all-unavailable
    // despawn check so Invaders aren't killed off by a brief entry
    // cutscene (Kakariko gate, Lon Lon Ranch entry, etc. — log 199
    // symptom).
    //
    // Permanent conditions still despawn:
    //   - offline (handled implicitly by empty view)
    //   - save not loaded
    //   - blacklisted scene (Ganon endgame, Hyrule Castle)
    //   - boss room with live boss (currently stubbed)
    //
    // Cutscenes are temporary; let the 60s orphan-in-scene timer
    // handle "player has been in cutscene with no resolution forever"
    // if it ever matters in practice.
    if (!p.isSaveLoaded) return false;
    if (IsSceneFlaggedNoInvaders(p.sceneNum)) return false;
    if (IsBossRoomWithLiveBoss(p.sceneNum, p.roomNum)) return false;
    return true;
}

const PlayerSnapshot* InvaderDescriptor::PickValidTarget(const SessionView& view) const {
    // "Best" valid target = most-isolated player passing IsValidTarget.
    // Iterates the view's players manually since SessionView's
    // MostIsolatedPlayer helper doesn't take a filter today; if a
    // second descriptor wants this pattern, refactor to a generic
    // filter-aware helper on SessionView.
    const PlayerSnapshot* best = nullptr;
    float bestMinDistSq = -1.0f;
    for (size_t i = 0; i < view.players.size(); ++i) {
        const PlayerSnapshot& candidate = view.players[i];
        if (!IsValidTarget(candidate)) continue;

        // Compute min-distance to any OTHER valid candidate. Isolated
        // = far from peers (Dark-Souls-flavor; lone players get hunted).
        float minDistSq = std::numeric_limits<float>::infinity();
        for (size_t j = 0; j < view.players.size(); ++j) {
            if (i == j) continue;
            const PlayerSnapshot& other = view.players[j];
            if (!IsValidTarget(other)) continue;
            if (other.sceneNum != candidate.sceneNum) continue;
            const float dx = candidate.worldPos.x - other.worldPos.x;
            const float dy = candidate.worldPos.y - other.worldPos.y;
            const float dz = candidate.worldPos.z - other.worldPos.z;
            const float d  = dx*dx + dy*dy + dz*dz;
            if (d < minDistSq) minDistSq = d;
        }
        if (minDistSq > bestMinDistSq) {
            bestMinDistSq = minDistSq;
            best          = &candidate;
        }
    }
    return best;
}

void InvaderDescriptor::OnEvent(const DirectorEventPayload& evt) {
    // Phase 1 §7.5 scene-following — only PlayerEnteredRoom matters for
    // the v1 lifecycle. Other DirectorEvents are ignored.
    if (evt.type != DirectorEvent::PlayerEnteredRoom) return;

    // Find any active Invader currently tracking the transitioning
    // player as its sticky target. If found, arm the follow-spawn:
    // captures the player's entrance position (= their worldPos right
    // now, since OnSceneSpawnActors just placed them there) + grace
    // counter. OnTick despawns the old actor + schedules the new
    // spawn at the entrance.
    for (auto& [netId, state] : mActiveInvaders) {
        if (state.targetClientId != evt.clientId) continue;
        // Look up the transitioning player's current pos (= entrance
        // position) from SessionView. We don't have view here, so
        // capture sceneNum/roomNum + flag — OnTick reads the live
        // worldPos at follow-spawn time.
        state.pendingFollowSpawn   = true;
        state.pendingFollowScene   = evt.sceneNum;
        state.pendingFollowRoom    = evt.roomNum;
        state.followGraceFrames    = 0;  // OnTick initializes from MsToGameTicks
        // entrance position captured at OnTick time when grace is set.
        SPDLOG_INFO("[InvaderDescriptor] OnEvent PlayerEnteredRoom: client={} "
                    "scene={} room={} -> arming follow-spawn for netId={}",
                    evt.clientId, (int)evt.sceneNum, (int)evt.roomNum, netId);
    }
}

void InvaderDescriptor::OnTick(Director& director, const SessionView& view) {
    // No active Invaders → nothing to manage.
    if (mActiveInvaders.empty()) return;

    // Collect netIds to mutate-after-iteration so we don't invalidate
    // the map during the loop.
    std::vector<uint32_t> toDespawn;
    std::vector<std::pair<uint32_t, Vec3f>> toFollowSpawn;  // (netId, entrancePos)

    // Phase 1 §7.5 "all-unavailable" precheck — if NO PERSISTENT target
    // exists (everyone in blacklist / boss-room / not-save-loaded),
    // despawn all active Invaders. Transient unavailability (cutscene)
    // does NOT trigger this — the orphan-in-scene 60s timer handles
    // pathological "player in cutscene forever" cases.
    //
    // anyValid (for sticky-target re-eval) still uses IsValidTarget
    // which includes cutscene exclusion — we don't want to switch
    // sticky to a cutscene player or have ProposeSpawn target them.
    const PlayerSnapshot* anyValid = PickValidTarget(view);
    bool anyPersistent = false;
    for (const auto& p : view.players) {
        if (IsPersistentTarget(p)) { anyPersistent = true; break; }
    }
    const bool allUnavailable = !anyPersistent;

    for (auto& [netId, state] : mActiveInvaders) {
        // Sticky-target validity check. If the current target is no
        // longer valid, try to switch to another. If no valid target
        // exists at all, queue this Invader for despawn.
        const PlayerSnapshot* currentTarget = view.PlayerByClientId(state.targetClientId);
        const bool targetInvalid =
            (currentTarget == nullptr) || !IsValidTarget(*currentTarget);

        if (allUnavailable) {
            // No persistent target anywhere — terminal condition.
            // Cutscene is excluded from this branch (see IsPersistent-
            // Target) so the Kakariko entry-cutscene window doesn't
            // kill the follow-spawn (log 199 symptom).
            toDespawn.push_back(netId);
            continue;
        }

        if (targetInvalid) {
            // Switch sticky target to whoever is best valid right now.
            if (anyValid != nullptr) {
                if (state.targetClientId != anyValid->clientId) {
                    SPDLOG_INFO("[InvaderDescriptor] OnTick: sticky target "
                                "for netId={} switched ({} -> {})",
                                netId, state.targetClientId, anyValid->clientId);
                    state.targetClientId = anyValid->clientId;
                }
            }
        } else if (state.targetClientId == 0) {
            // Initial target assignment (first OnTick after OnSpawnExecuted).
            state.targetClientId = anyValid->clientId;
            SPDLOG_INFO("[InvaderDescriptor] OnTick: initial sticky target "
                        "for netId={} -> client {}",
                        netId, state.targetClientId);
        }

        // Update last-known scene/room — used by the orphan-timer
        // check below. Use the spawn-time data from OnSpawnExecuted
        // (we don't continuously chase the actor's wandering position
        // — the orphan check is about "is anyone in the room I spawned
        // into"). Lazily populated on first OnTick when scene/room is 0.
        if (state.lastKnownSceneNum == 0 && gPlayState != nullptr) {
            state.lastKnownSceneNum = (int16_t)gPlayState->sceneNum;
            state.lastKnownRoomNum  = (int8_t)gPlayState->roomCtx.curRoom.num;
        }

        // Orphan-in-scene timer: increment if no team player is in
        // (lastKnownScene, lastKnownRoom). Reset to 0 if any is.
        bool playerInScene = false;
        for (const PlayerSnapshot& p : view.players) {
            if (p.sceneNum == state.lastKnownSceneNum &&
                p.roomNum  == state.lastKnownRoomNum) {
                playerInScene = true;
                break;
            }
        }
        if (playerInScene) {
            state.orphanFrames = 0;
        } else {
            ++state.orphanFrames;
            const int kOrphanMs = 60 * 1000;
            if (state.orphanFrames >= Anchor::Instance->MsToGameTicks(kOrphanMs)) {
                SPDLOG_INFO("[InvaderDescriptor] OnTick: netId={} orphan-in-scene "
                            "timeout ({}s, scene={} room={}) — despawning",
                            netId, kOrphanMs / 1000,
                            (int)state.lastKnownSceneNum,
                            (int)state.lastKnownRoomNum);
                toDespawn.push_back(netId);
                continue;
            }
        }

        // Scene-follow grace counter. OnEvent set pendingFollowSpawn=true
        // when the target player transitioned; we initialize the grace
        // counter here on the first OnTick after the event so MsToGameTicks
        // applies the right tick rate.
        if (state.pendingFollowSpawn) {
            if (state.followGraceFrames == 0) {
                // First tick after OnEvent — initialize grace counter +
                // capture the target's CURRENT worldPos (= entrance pos
                // since they JUST spawned in via OnSceneSpawnActors).
                const PlayerSnapshot* tgt = view.PlayerByClientId(state.targetClientId);
                if (tgt != nullptr) {
                    state.pendingFollowPos = tgt->worldPos;
                    const int kGraceMs = 1500;
                    state.followGraceFrames = Anchor::Instance->MsToGameTicks(kGraceMs);
                    SPDLOG_INFO("[InvaderDescriptor] OnTick: follow-spawn armed "
                                "netId={} entrancePos=({:.0f},{:.0f},{:.0f}) grace={}ms",
                                netId,
                                state.pendingFollowPos.x,
                                state.pendingFollowPos.y,
                                state.pendingFollowPos.z,
                                kGraceMs);
                }
            } else {
                // Tick down grace counter; fire follow-spawn when it
                // hits 0. Captures the entrancePos at arm-time, not now
                // (player may have walked away during the 1.5s grace).
                --state.followGraceFrames;
                if (state.followGraceFrames <= 0) {
                    toFollowSpawn.push_back({netId, state.pendingFollowPos});
                    state.pendingFollowSpawn = false;
                }
            }
        }
    }

    // Execute despawns. ExecuteDespawn handles missing-actor case
    // (e.g. scene-unload already destroyed it) by cleaning up
    // bookkeeping without calling Actor_Kill.
    for (uint32_t netId : toDespawn) {
        director.ExecuteDespawn(netId, DefeatCause::Leash);
    }

    // Execute scene-following follow-spawns. Despawn the old Invader,
    // then spawn a fresh one at the captured entrance position with
    // bypassCooldown=true so the new scene's cooldown ledger doesn't
    // stamp this as a "fresh natural spawn".
    for (const auto& [oldNetId, entrancePos] : toFollowSpawn) {
        SPDLOG_INFO("[InvaderDescriptor] OnTick: executing follow-spawn "
                    "(old netId={} -> entrancePos=({:.0f},{:.0f},{:.0f}))",
                    oldNetId, entrancePos.x, entrancePos.y, entrancePos.z);

        director.ExecuteDespawn(oldNetId, DefeatCause::SceneExit);

        SpawnProposal p;
        p.source         = this;
        p.sceneNum       = (gPlayState != nullptr) ? (int16_t)gPlayState->sceneNum : 0;
        p.roomNum        = (gPlayState != nullptr) ? (int8_t)gPlayState->roomCtx.curRoom.num : 0;
        p.worldPos       = entrancePos;
        p.yawTowards     = 0;
        p.actorId        = ACTOR_EN_TEST;
        p.actorParams    = 1;
        p.variantId      = 0;
        p.priority       = DescriptorPriority::Standard;
        p.groupId        = 0;
        p.bypassCooldown = true;  // CRITICAL — continuation, not a new encounter
        ++mProposalsOffered;
        // ExecuteSpawn directly — bypass arbitration. The new spawn
        // doesn't compete with other descriptors; it's a continuation
        // of an already-decided spawn.
        director.ExecuteSpawn(p);
    }
}

}  // namespace AnchorDirector
