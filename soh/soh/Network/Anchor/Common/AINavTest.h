/**
 * AINavTest — Navigation Test Harness for NPC Invader, NPC Follower,
 * and AI Player Follower.
 *
 * Diagnostic infrastructure. Lets the user set a fixed spawn point at
 * their current location, then trigger a test run that:
 *   - Spawns or relocates NPC Follower at the spawn point.
 *   - Spawns or relocates NPC Invader at the spawn point.
 *   - Broadcasts a NAV_TEST_DIRECTIVE packet to P2 instructing P2 to
 *     teleport to the spawn point and enable AI Player Follower mode.
 *   - Disables combat across all three actors (so the trace is pure
 *     locomotion, no ATTACK/RANGED_ATTACK/etc. cycles).
 *   - Starts a per-run timer.
 *
 * Each actor reports completion via 3D 60u distance to the test
 * conductor (P1). Per-run results are aggregated into a statistics
 * display (count, min, max, mean, median per actor).
 *
 * Plan: Claude/Plans/ai_nav_test_harness_plan.md.
 *
 * Authoring: 2026-05-18.
 */

#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include "z64.h"
}

namespace AINavTest {

// One run = one press of "Run Test". Reach times are wall-clock ms
// from run start. -1 = not yet reached / not applicable / DNF.
struct RunResult {
    uint64_t runStartFrame    = 0;
    int      npcFollowerMs    = -1;
    int      aiInvaderMs      = -1;
    int      aiFollowerMs     = -1;   // populated by P2 via NAV_TEST_DIRECTIVE { REACHED }
    bool     completedOrDNF   = false; // true after all reaches recorded OR 120s timeout
};

// Per-actor stats across the run history.
struct Stats {
    int count  = 0;
    int min    = 0;
    int max    = 0;
    int mean   = 0;
    int median = 0;
};

// Did the user-facing CVar gate enable the harness? Read by every
// consumer (UI gate, combat-disable gate, completion checks).
bool IsEnabled();

// Universal combat-disable gate. When IsEnabled() AND CombatDisabled
// CVar is set, all three actors' TryEngageCombat short-circuit.
// Phase 2's RangedInPlace fallback also honors this so the Invader
// doesn't fire arrows mid-test.
bool IsCombatDisabled();

// Is a run currently in progress (timer running, completion not yet
// recorded for all actors)? Used on P1 (the test conductor); reflects
// the local sRunHistory state.
bool IsRunActive();

// P2-side counterpart of IsRunActive. True for kP2TestActiveTimeoutMs
// after this client receives a NAV_TEST_DIRECTIVE RUN packet. Used by
// the AI Player Follower's reach detector — P2 doesn't have a run
// record in sRunHistory; this flag tells it "yes, I'm participating".
bool IsP2InTestMode();

// Called by HandlePacket_NavTestDirective(RUN) on the receiving peer.
// Latches the test-mode flag with a wall-clock expiry of 120s.
void NotifyP2TestStarted();

// Enable every toggle in the Anchor → "Nav Data Usage" menu section
// (master + AiFollowerConsumer + ActorTrail + RoomNavConsumer +
// RoomNavData.Enabled + RoomNavData.PathBClimbDetection +
// VerticalTeleport + EdgeAvoidance). Called from RunTest on P1's
// local AND from the NAV_TEST_DIRECTIVE RUN handler on P2, so both
// clients have the nav substrate live before navigation begins.
void EnableAllNavDataUsageFeatures();

// 3D 60u completion criterion. Returns true if `actorPos` is within
// the configured threshold of `targetPos`. Used by all three actors'
// completion checks for consistent semantics.
bool ReachedTarget(const Vec3f& actorPos, const Vec3f& targetPos);

// User-facing actions wired to UI buttons.
void SetSpawnPointAtPlayer(PlayState* play);
void RunTest();
void KillAllEnemiesInRoom(PlayState* play);
void ClearRunHistory();

// One-shot startup clear. Forces AI.NavTest.Enabled off at game launch
// so a harness session left on from a prior run cannot silently
// suppress combat across all three AI actors (Pitfall: IsCombatDisabled
// gate at Invader.cpp:2577 / FollowerNPC.cpp:2638 / Follower.cpp:4893).
// Vanilla-altering features ship default-off, permanently — same rule
// applies to this dev-tooling harness. Other harness sub-CVars are
// left alone as user preferences.
void ClearOnStartup();

// Per-actor reach reporters. Called from each actor's TickFOLLOW (or
// equivalent) when the actor reaches the target. No-op if no run is
// active or the actor already reported reach for this run.
void ReportNpcFollowerReach();
void ReportAIInvaderReach();
void ReportAIFollowerReach(int reportedMsFromP2);   // called from NAV_TEST_DIRECTIVE REACHED handler

// Statistics — computed on demand from sRunHistory.
Stats ComputeNpcFollowerStats();
Stats ComputeAIInvaderStats();
Stats ComputeAIFollowerStats();

// Run history accessor — UI reads this for the "last run" display.
const std::vector<RunResult>& GetRunHistory();

// Per-tick driver. Called from OnGameFrameUpdate. Handles the 120s
// DNF timeout — if a run has been active for > 120s without all
// actors reporting reach, mark unreported actors as DNF (leaves
// the -1 sentinel; the run record's completedOrDNF flips to true so
// it counts toward stats).
void Tick();

// OnSceneSpawnActors hook handler — clears any cached actor pointers
// used by the relocate-on-rerun logic. Without this, a scene change
// between runs would leave dangling pointers that crash Run.
void OnSceneSpawnActors();

}  // namespace AINavTest
