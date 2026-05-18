/**
 * AINavTest — implementation. See AINavTest.h for design.
 */

#include "AINavTest.h"

#include <algorithm>
#include <atomic>
#include <numeric>

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "../Anchor.h"
#include "../../../cvar_prefixes.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "z64.h"
extern PlayState* gPlayState;
extern s16        gEnInvaderId;
extern s16        gEnFollowerId;
}

namespace AINavTest {

namespace {

// ── Static state ────────────────────────────────────────────────────

// Run history. Appended to on every "Run Test" press. UI reads it for
// the statistics display. "Clear Run History" empties it.
std::vector<RunResult> sRunHistory;

// Index of the currently in-progress run (== sRunHistory.size() - 1 if
// active, -1 otherwise). When IsRunActive() returns true, this is the
// run that accepts reach reports.
int sActiveRunIndex = -1;

// P2-side test-mode flag — set when this client receives a
// NAV_TEST_DIRECTIVE RUN packet. P2 doesn't own a run record in
// sRunHistory but still needs to detect "reached" and report back to
// P1 via REACHED packet. Stores the game-tick frame at activation;
// IsP2InTestMode() checks the wall-clock elapsed against
// kP2TestActiveTimeoutMs.
uint64_t sP2TestStartFrame = 0;
constexpr int kP2TestActiveTimeoutMs = 120000;  // mirror DNF timeout

// DNF timeout — 120 seconds wall-clock from run start. If a run is
// active past this threshold, mark it as completed-or-DNF so future
// reach reports are ignored and the next "Run Test" press starts a
// fresh run record.
constexpr int kDnfTimeoutMs = 120000;

// ── CVar wrappers ───────────────────────────────────────────────────

inline int CV(const char* name, int dflt = 0) {
    return CVarGetInteger(name, dflt);
}
inline float CVf(const char* name, float dflt = 0.0f) {
    return CVarGetFloat(name, dflt);
}
inline void CVSet(const char* name, int v)   { CVarSetInteger(name, v); }
inline void CVSetf(const char* name, float v) { CVarSetFloat(name, v);   }

// ── Helpers ─────────────────────────────────────────────────────────

inline float Dist3DSqv(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

int CurrentRunElapsedMs() {
    if (sActiveRunIndex < 0 ||
        sActiveRunIndex >= (int)sRunHistory.size() ||
        Anchor::Instance == nullptr) {
        return 0;
    }
    const uint64_t now = Anchor::Instance->gameFrameCounter.load(
                             std::memory_order_relaxed);
    const uint64_t start = sRunHistory[sActiveRunIndex].runStartFrame;
    if (now <= start) return 0;
    const uint64_t ticks = now - start;
    // Convert ticks → wall-clock ms via mAvgGameTickMs (each tick ≈
    // 50ms at 20fps default; lower at unlocked framerate). Use the
    // public MsToGameTicks helper inverse: ms ≈ ticks * avgMs.
    // The Anchor class doesn't expose avgMs directly, so derive from
    // a unit conversion: 1000 ticks @ MsToGameTicks(1000) → ratio.
    const int ticksPer1000ms = Anchor::Instance->MsToGameTicks(1000);
    if (ticksPer1000ms <= 0) return (int)(ticks * 50);  // 20fps fallback
    return (int)((ticks * 1000) / (uint64_t)ticksPer1000ms);
}

}  // namespace

// ── Public predicates ──────────────────────────────────────────────

bool IsEnabled() {
    return CV(CVAR_ENHANCEMENT("AI.NavTest.Enabled"), 0) != 0;
}

bool IsCombatDisabled() {
    return IsEnabled() && CV(CVAR_ENHANCEMENT("AI.NavTest.CombatDisabled"), 1) != 0;
}

bool IsRunActive() {
    if (!IsEnabled()) return false;
    if (sActiveRunIndex < 0 ||
        sActiveRunIndex >= (int)sRunHistory.size()) return false;
    return !sRunHistory[sActiveRunIndex].completedOrDNF;
}

bool IsP2InTestMode() {
    if (!IsEnabled()) return false;
    if (sP2TestStartFrame == 0) return false;
    if (Anchor::Instance == nullptr) return false;
    const uint64_t now = Anchor::Instance->gameFrameCounter.load(
                             std::memory_order_relaxed);
    const uint64_t elapsedTicks = (now > sP2TestStartFrame)
        ? (now - sP2TestStartFrame) : 0;
    const int timeoutTicks =
        Anchor::Instance->MsToGameTicks(kP2TestActiveTimeoutMs);
    return (int)elapsedTicks < timeoutTicks;
}

void NotifyP2TestStarted() {
    if (Anchor::Instance == nullptr) return;
    sP2TestStartFrame = Anchor::Instance->gameFrameCounter.load(
                             std::memory_order_relaxed);
    SPDLOG_INFO("[NavTest] P2 test mode latched (will auto-clear after "
                "{}s without activity)", kP2TestActiveTimeoutMs / 1000);
}

bool ReachedTarget(const Vec3f& actorPos, const Vec3f& targetPos) {
    constexpr float kReachRadius = 60.0f;
    return Dist3DSqv(actorPos, targetPos) <= (kReachRadius * kReachRadius);
}

// ── SetSpawnPointAtPlayer ─────────────────────────────────────────

void SetSpawnPointAtPlayer(PlayState* play) {
    if (play == nullptr) {
        SPDLOG_WARN("[NavTest] SetSpawnPointAtPlayer: gPlayState is null");
        return;
    }
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        SPDLOG_WARN("[NavTest] SetSpawnPointAtPlayer: GET_PLAYER returned null");
        return;
    }
    const Vec3f& p = player->actor.world.pos;
    CVSetf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.X"), p.x);
    CVSetf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.Y"), p.y);
    CVSetf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.Z"), p.z);
    CVSet(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.SceneNum"), (int)play->sceneNum);
    CVSet(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.RoomNum"),
          (int)play->roomCtx.curRoom.num);
    CVSet(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.IsSet"), 1);
    SPDLOG_INFO("[NavTest] Spawn point set: ({:.0f},{:.0f},{:.0f}) "
                "scene={} room={}",
                p.x, p.y, p.z,
                (int)play->sceneNum, (int)play->roomCtx.curRoom.num);
}

// ── RunTest ────────────────────────────────────────────────────────

void RunTest() {
    if (!IsEnabled()) {
        SPDLOG_WARN("[NavTest] RunTest pressed but NavTest.Enabled=0");
        return;
    }
    if (!CV(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.IsSet"), 0)) {
        SPDLOG_WARN("[NavTest] RunTest: no spawn point set; press 'Set Spawn "
                    "Point' first");
        return;
    }
    if (gPlayState == nullptr) {
        SPDLOG_WARN("[NavTest] RunTest: gPlayState is null");
        return;
    }

    // Sceneswitch guard: if the player is in a different scene than the
    // spawn point, reject — relocating actors across scenes isn't safe.
    const int16_t spawnSceneNum =
        (int16_t)CV(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.SceneNum"), -1);
    if (gPlayState->sceneNum != spawnSceneNum) {
        SPDLOG_WARN("[NavTest] RunTest: scene mismatch (spawn point scene={} "
                    "current scene={}); re-set spawn point in the current scene",
                    (int)spawnSceneNum, (int)gPlayState->sceneNum);
        return;
    }

    const Vec3f spawnPos = {
        CVf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.X"), 0.0f),
        CVf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.Y"), 0.0f),
        CVf(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.Z"), 0.0f),
    };
    const int8_t spawnRoomNum =
        (int8_t)CV(CVAR_ENHANCEMENT("AI.NavTest.SpawnPoint.RoomNum"), 0);

    // ── 1. NPC Follower: spawn or relocate ────────────────────────
    // Two cases:
    //   (a) NPC Follower already alive (CVar was on from a prior test)
    //       → teleport it back to the spawn point.
    //   (b) NPC Follower not alive
    //       → use the existing SetFollowerNpcActive + spawn-pos override
    //         mechanism in Anchor.h so mFollowerNpcLocalActor tracking
    //         + lifecycle hooks fire correctly. We CANNOT call
    //         Actor_Spawn directly: the polling driver in
    //         TickFollowerNpcCVar would see mFollowerNpcLocalActor==null
    //         on the next frame (since our manual spawn doesn't populate
    //         that field) and would auto-respawn a SECOND NPC at the
    //         player's position. Log 245 symptom: NPC Follower appeared
    //         at P1 instead of the spawn point.
    //
    // After spawn/relocate, set the CVar so the tick runs each frame.
    // Setting CVar AFTER spawn is important: the polling driver's
    // edge-detection then sees CVarLast=0, CVar=1 → spawn branch fires
    // → SetFollowerNpcActive(true) is idempotent (returns immediately
    // because actor is already alive). No double-spawn.
    if (Anchor::Instance != nullptr) {
        Actor* npcFollower = Anchor::Instance->GetFollowerNpcLocalActor();
        if (npcFollower != nullptr && npcFollower->update != nullptr) {
            // Relocate alive NPC.
            npcFollower->world.pos  = spawnPos;
            npcFollower->velocity.x = 0.0f;
            npcFollower->velocity.y = 0.0f;
            npcFollower->velocity.z = 0.0f;
            SPDLOG_INFO("[NavTest] NPC Follower relocated to spawn point");
        } else if (gEnFollowerId != 0) {
            // Arm the spawn-pos override so SetFollowerNpcActive lands
            // the actor at the spawn point instead of player pos.
            Anchor::Instance->mFollowerNpcSpawnPosOverride    = true;
            Anchor::Instance->mFollowerNpcSpawnPosOverridePos = spawnPos;
            Anchor::Instance->mFollowerNpcSpawnPosOverrideYaw = 0;
            Anchor::Instance->SetFollowerNpcActive(true);
            SPDLOG_INFO("[NavTest] NPC Follower spawned at spawn point "
                        "via SetFollowerNpcActive override");
        }
    }
    CVSet(CVAR_ENHANCEMENT("AI.FollowerNPC.Enabled"), 1);

    // ── 2. AI Invader: spawn or relocate ──────────────────────────
    // Find an existing Invader in the scene (we don't track a pointer
    // for it; walk the actor list).
    Actor* invader = nullptr;
    if (gEnInvaderId != 0) {
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
        while (it != nullptr) {
            if (it->id == gEnInvaderId && it->update != nullptr) {
                invader = it;
                break;
            }
            it = it->next;
        }
    }
    if (invader != nullptr) {
        invader->world.pos = spawnPos;
        invader->velocity.x = invader->velocity.y = invader->velocity.z = 0.0f;
        SPDLOG_INFO("[NavTest] AI Invader relocated to spawn point");
    } else if (gEnInvaderId != 0) {
        Actor* spawned = Actor_Spawn(
            &gPlayState->actorCtx, gPlayState, gEnInvaderId,
            spawnPos.x, spawnPos.y, spawnPos.z,
            0, 0, 0, 0);
        if (spawned == nullptr) {
            SPDLOG_WARN("[NavTest] Actor_Spawn(gEnInvaderId) returned null");
        } else {
            SPDLOG_INFO("[NavTest] AI Invader spawned at spawn point");
        }
    }

    // ── 3. Broadcast NAV_TEST_DIRECTIVE to P2 ─────────────────────
    // Only when IncludeAIFollower CVar is set AND we're connected.
    if (CV(CVAR_ENHANCEMENT("AI.NavTest.IncludeAIFollower"), 1) &&
        Anchor::Instance != nullptr && Anchor::Instance->isConnected) {
        Anchor::Instance->SendPacket_NavTestDirective(
            /*directive=*/"RUN",
            spawnPos, spawnSceneNum, spawnRoomNum,
            /*runIndex=*/(int)sRunHistory.size(),
            /*reachedMs=*/-1);
    }

    // ── 4. Start run timer ────────────────────────────────────────
    RunResult run;
    run.runStartFrame = Anchor::Instance != nullptr
        ? Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed)
        : 0;
    sRunHistory.push_back(run);
    sActiveRunIndex = (int)sRunHistory.size() - 1;
    SPDLOG_INFO("[NavTest] Run {} START — spawnPos=({:.0f},{:.0f},{:.0f}) "
                "scene={} room={}",
                sActiveRunIndex, spawnPos.x, spawnPos.y, spawnPos.z,
                (int)spawnSceneNum, (int)spawnRoomNum);
}

// ── KillAllEnemiesInRoom ──────────────────────────────────────────

void KillAllEnemiesInRoom(PlayState* play) {
    if (play == nullptr) {
        SPDLOG_WARN("[NavTest] KillAllEnemies: gPlayState is null");
        return;
    }
    int killed = 0;
    int skipped = 0;
    Actor* it = play->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (it != nullptr) {
        Actor* next = it->next;
        // Skip our test agents — AI Invader is ACTORCAT_ENEMY but we
        // explicitly want it alive for the test.
        if (gEnInvaderId != 0 && it->id == gEnInvaderId) {
            skipped++;
        } else {
            Actor_Kill(it);
            killed++;
        }
        it = next;
    }
    SPDLOG_INFO("[NavTest] Killed {} enemies in current room ({} test "
                "agents preserved)", killed, skipped);
}

void ClearRunHistory() {
    sRunHistory.clear();
    sActiveRunIndex = -1;
    SPDLOG_INFO("[NavTest] Run history cleared");
}

// ── Reach reporters ────────────────────────────────────────────────

void ReportNpcFollowerReach() {
    if (!IsRunActive()) return;
    auto& run = sRunHistory[sActiveRunIndex];
    if (run.npcFollowerMs >= 0) return;  // already recorded
    run.npcFollowerMs = CurrentRunElapsedMs();
    SPDLOG_INFO("[NavTest] Run {} NPC Follower reached in {}ms",
                sActiveRunIndex, run.npcFollowerMs);
}

void ReportAIInvaderReach() {
    if (!IsRunActive()) return;
    auto& run = sRunHistory[sActiveRunIndex];
    if (run.aiInvaderMs >= 0) return;
    run.aiInvaderMs = CurrentRunElapsedMs();
    SPDLOG_INFO("[NavTest] Run {} AI Invader reached in {}ms",
                sActiveRunIndex, run.aiInvaderMs);
}

void ReportAIFollowerReach(int reportedMsFromP2) {
    if (!IsRunActive()) return;
    auto& run = sRunHistory[sActiveRunIndex];
    if (run.aiFollowerMs >= 0) return;
    // Sentinel 0 = use P1's local clock (P2 sends 0 because it can't
    // measure relative to P1's run start frame). Any positive value is
    // P2's self-measurement and is recorded verbatim.
    run.aiFollowerMs = (reportedMsFromP2 > 0)
        ? reportedMsFromP2
        : CurrentRunElapsedMs();
    SPDLOG_INFO("[NavTest] Run {} AI Follower (P2) reached in {}ms "
                "(source: {})",
                sActiveRunIndex, run.aiFollowerMs,
                reportedMsFromP2 > 0 ? "P2-measured" : "P1-measured");
}

// ── Stats computation ─────────────────────────────────────────────

namespace {
Stats ComputeStatsFor(int RunResult::*member) {
    std::vector<int> vals;
    for (const auto& r : sRunHistory) {
        const int v = r.*member;
        if (v > 0) vals.push_back(v);
    }
    Stats s;
    if (vals.empty()) return s;
    std::sort(vals.begin(), vals.end());
    s.count = (int)vals.size();
    s.min   = vals.front();
    s.max   = vals.back();
    s.mean  = (int)(std::accumulate(vals.begin(), vals.end(), 0) / s.count);
    s.median = vals[s.count / 2];
    return s;
}
}

Stats ComputeNpcFollowerStats() { return ComputeStatsFor(&RunResult::npcFollowerMs); }
Stats ComputeAIInvaderStats()   { return ComputeStatsFor(&RunResult::aiInvaderMs);   }
Stats ComputeAIFollowerStats()  { return ComputeStatsFor(&RunResult::aiFollowerMs);  }

const std::vector<RunResult>& GetRunHistory() { return sRunHistory; }

// ── Tick ───────────────────────────────────────────────────────────

void Tick() {
    if (!IsEnabled() || !IsRunActive()) return;
    auto& run = sRunHistory[sActiveRunIndex];

    // All three reaches reported → mark completed.
    const bool npcDone = (run.npcFollowerMs >= 0);
    const bool invDone = (run.aiInvaderMs   >= 0);
    const bool aiDone  = (run.aiFollowerMs  >= 0) ||
                         !CV(CVAR_ENHANCEMENT("AI.NavTest.IncludeAIFollower"), 1);
    if (npcDone && invDone && aiDone) {
        run.completedOrDNF = true;
        SPDLOG_INFO("[NavTest] Run {} COMPLETE — npc={}ms invader={}ms "
                    "aiFollower={}ms", sActiveRunIndex,
                    run.npcFollowerMs, run.aiInvaderMs, run.aiFollowerMs);
        return;
    }

    // DNF timeout — 120s wall-clock.
    const int elapsed = CurrentRunElapsedMs();
    if (elapsed >= kDnfTimeoutMs) {
        run.completedOrDNF = true;
        SPDLOG_INFO("[NavTest] Run {} DNF timeout — elapsed={}ms "
                    "npc={}ms invader={}ms aiFollower={}ms",
                    sActiveRunIndex, elapsed,
                    run.npcFollowerMs, run.aiInvaderMs, run.aiFollowerMs);
    }
}

void OnSceneSpawnActors() {
    // Currently a no-op — the only cached pointer in this module
    // (mFollowerNpcLocalActor) is already cleared by NPC Follower's
    // own OnSceneSpawnActors hook in NpcCompanionInit.cpp. AI Invader
    // is found by walking the actor list, so no pointer to clear.
    //
    // Reserved for future use (e.g. when we cache the spawned NPC
    // pointer locally for relocation).
}

}  // namespace AINavTest
