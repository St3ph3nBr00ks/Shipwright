/**
 * NavOrDirect implementation — see NavOrDirect.h for design.
 */

#include "NavOrDirect.h"

#include <atomic>
#include <cmath>

#include <spdlog/spdlog.h>       // SPDLOG_INFO — synthetic-flag diagnostic (Phase 2.5)

#include "../../Anchor.h"        // Anchor::Instance->gameFrameCounter, MsToGameTicks
#include "../ActorTrail.h"       // ActorTrail::GetInstance().ComputePathTo
#include "../DistanceMath.h"     // AnchorDist::DistXZSq / Dist3DSq

namespace AnchorAI {

namespace {

// File-scope diagnostic counter. Incremented on every fallback return,
// reset by callers at scene boundaries (so the ImGui debug overlay can
// show "fallbacks this scene = N").
std::atomic<uint32_t> g_fallbackInvocationCount{0};

// Choose the highest-priority applicable fallback given the policy.
// Mirrors the policy priority order documented in NavOrDirect.h.
PathEmptyFallback PickFallbackForOutOfRange(const FallbackPolicy& policy) {
    if (policy.hasRangedReady)  return PathEmptyFallback::RangedInPlace;
    if (policy.isFriendlyActor) return PathEmptyFallback::ReturnToLeader;
    if (policy.isHostileActor)  return PathEmptyFallback::RetreatHostile;
    // No applicable fallback: hold defensive (caller switches to STANDBY).
    return PathEmptyFallback::HoldDefensive;
}

// Should we refresh the path this tick? Refresh triggers:
//   - path is empty (nothing to follow)
//   - rate-limit elapsed (kPathRefreshMs since last refresh)
//   - target drifted (>kTargetDriftRefresh from last-refresh target pos)
bool NeedsRefresh(const NavState& navState, const Vec3f& targetPos,
                   uint64_t curFrame, int refreshTicks) {
    if (navState.path.Empty()) return true;
    if (refreshTicks > 0 &&
        curFrame >= navState.lastPathRefreshFrame + (uint64_t)refreshTicks) {
        return true;
    }
    if (AnchorDist::DistXZSq(targetPos, navState.lastPathTargetPos) >
        kTargetDriftRefreshSq) {
        return true;
    }
    return false;
}

}  // namespace

NavOrDirectResult ChooseSubgoal(const Actor*         navigator,
                                 const Vec3f&         targetPos,
                                 NavState&            navState,
                                 const FallbackPolicy& policy,
                                 PlayState*           play)
{
    NavOrDirectResult result;
    if (navigator == nullptr || play == nullptr) {
        // Defensive: nothing to do. Caller will hit DirectYaw with default
        // subgoal (zero); they should null-check before calling.
        result.subgoal               = targetPos;
        result.usingNavMesh          = false;
        result.fallbackEngaged       = PathEmptyFallback::DirectYaw;
        return result;
    }

    // ── Gate 1 — within direct-yaw radius? ─────────────────────────────
    // Universal 60u threshold. Inside this, substrate routing is wasted
    // BFS work; caller should yaw straight at target.
    const float dist3DSq = AnchorDist::Dist3DSq(navigator->world.pos, targetPos);
    if (dist3DSq <= kDirectYawThresholdSq) {
        result.subgoal         = targetPos;
        result.usingNavMesh    = false;
        result.fallbackEngaged = PathEmptyFallback::DirectYaw;
        // Inside-threshold isn't a "fallback engaged" event for state-machine
        // purposes (no transition needed) so leave fallbackJustEngaged=false.
        return result;
    }

    // ── Gate 2 — in water? ─────────────────────────────────────────────
    // Floor-derived nav mesh doesn't cover water surface. If the actor
    // somehow ended up in water without being in SWIMMING state, fall
    // through to direct-yaw rather than thrashing on BFS misses.
    if (navigator->yDistToWater > kWaterDepthThreshold) {
        result.subgoal         = targetPos;
        result.usingNavMesh    = false;
        result.fallbackEngaged = PickFallbackForOutOfRange(policy);
        if (result.fallbackEngaged != PathEmptyFallback::DirectYaw) {
            // Water-gate fallback counts toward the diagnostic counter
            // because nav-mesh was attempted-but-skipped.
            g_fallbackInvocationCount.fetch_add(1, std::memory_order_relaxed);
            result.fallbackJustEngaged = true;
        }
        return result;
    }

    // ── Gate 3 — caller-forced direct-yaw? ─────────────────────────────
    if (policy.forceDirectYaw) {
        result.subgoal         = targetPos;
        result.usingNavMesh    = false;
        result.fallbackEngaged = PickFallbackForOutOfRange(policy);
        g_fallbackInvocationCount.fetch_add(1, std::memory_order_relaxed);
        result.fallbackJustEngaged = true;
        return result;
    }

    // ── Refresh path if needed ─────────────────────────────────────────
    const uint64_t curFrame = Anchor::Instance != nullptr
        ? Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed)
        : 0;
    const int refreshTicks = Anchor::Instance != nullptr
        ? Anchor::Instance->MsToGameTicks(kPathRefreshMs)
        : 0;

    const bool wasEmptyAtTickStart = navState.path.Empty();
    if (NeedsRefresh(navState, targetPos, curFrame, refreshTicks)) {
        navState.path.Reset();
        AnchorNav::ActorTrail::GetInstance().ComputePathTo(
            navState.trailKey,
            navigator,
            targetPos,
            play,
            navState.path,
            /*skipLayer1LOS=*/false,
            /*preferLeaderTrail=*/true);
        navState.lastPathRefreshFrame = curFrame;
        navState.lastPathTargetPos    = targetPos;
    }

    // ── Path empty after refresh? ──────────────────────────────────────
    if (navState.path.Empty()) {
        result.subgoal         = targetPos;
        result.usingNavMesh    = false;
        result.fallbackEngaged = PickFallbackForOutOfRange(policy);
        // Rising-edge: was empty AT tick start too, OR was non-empty but
        // refresh just failed.
        result.fallbackJustEngaged = !wasEmptyAtTickStart ||
                                     result.fallbackEngaged !=
                                         PathEmptyFallback::DirectYaw;
        g_fallbackInvocationCount.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // ── Cursor advance on subgoal proximity ────────────────────────────
    Vec3f subgoal = navState.path.CurrentSubgoal();
    if (AnchorDist::DistXZSq(navigator->world.pos, subgoal) <
        kAdvanceSubgoalDistSq) {
        navState.path.Advance();
        if (!navState.path.Empty()) {
            subgoal = navState.path.CurrentSubgoal();
        }
    }

    // ── Possibly emptied during advance — re-check ─────────────────────
    if (navState.path.Empty()) {
        result.subgoal         = targetPos;
        result.usingNavMesh    = false;
        result.fallbackEngaged = PickFallbackForOutOfRange(policy);
        result.fallbackJustEngaged = true;
        g_fallbackInvocationCount.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    // ── Substrate-driven success ───────────────────────────────────────
    result.subgoal               = subgoal;
    result.usingNavMesh          = true;
    result.fallbackEngaged       = PathEmptyFallback::DirectYaw;  // sentinel; unused
    result.fallbackJustEngaged   = false;
    result.subgoalFlags          = navState.path.CurrentSubgoalFlags();

    // ── Phase 2.5 diagnostic — synthetic flag visibility ──────────────
    // Most path waypoints have flags=0 (plain walkable node from Layer 1
    // direct LOS or Layer 3 BFS without synthetic edges). Non-zero flags
    // indicate that BFS routed through a special edge:
    //   0x0400 NODE_CLIMB_LADDER       0x4000 NODE_CLIMB_BOUNDARY
    //   0x0800 NODE_CLIMB_VINE         0x8000 NODE_DROP_FROM_ABOVE
    //   0x1000 NODE_CLIMB_DESIGNATED   0x00010000 NODE_REACHED_VIA_LEDGE_GRAB
    //   0x2000 NODE_CLIMB_GENERIC      (plus possible future jump-anchor flag)
    //
    // Log emits ONLY on rising-edge into a non-zero flag. With AI Invader
    // as the only consumer today, a single static suffices; later phases
    // will key by navigator pointer or trail key.
    //
    // Purpose: determine whether jump-anchor edges actually appear in
    // Invader paths (log 240 question). If no flag log includes a
    // jump-anchor bit during a session where the Invader had to cross
    // a gap, the BFS is not emitting them — root-cause is at the
    // path-planner layer, not the consumer.
    static uint32_t s_lastLoggedFlags = 0;
    if (result.subgoalFlags != 0 && result.subgoalFlags != s_lastLoggedFlags) {
        SPDLOG_INFO("[NavOrDirect] subgoalFlags=0x{:X} subgoal=({:.0f},{:.0f},{:.0f}) "
                    "navigator.id={} (path waypoint carrying synthetic flag — "
                    "BFS routed through a special edge)",
                    result.subgoalFlags,
                    result.subgoal.x, result.subgoal.y, result.subgoal.z,
                    (int)navigator->id);
        s_lastLoggedFlags = result.subgoalFlags;
    } else if (result.subgoalFlags == 0 && s_lastLoggedFlags != 0) {
        // Path transitioned BACK to plain walkable. Reset edge tracking
        // so the next synthetic flag re-logs.
        s_lastLoggedFlags = 0;
    }

    return result;
}

uint32_t GetFallbackInvocationCount() {
    return g_fallbackInvocationCount.load(std::memory_order_relaxed);
}

void ResetFallbackInvocationCount() {
    g_fallbackInvocationCount.store(0, std::memory_order_relaxed);
}

}  // namespace AnchorAI
