/**
 * StuckRecovery — see StuckRecovery.h for purpose.
 *
 * Logic body lifted from FollowerNPC::TickSTUCK and
 * AIInvader::TickSTUCK — the two functions had converged into
 * near-duplicates aside from the teleport callback + state enum.
 */

#include "StuckRecovery.h"

#include <cmath>

#include "NavStateTransitions.h"

#include <spdlog/spdlog.h>

extern "C" {
#include "functions.h"  // Math_SinS, Math_CosS, Math_Atan2S
}

namespace AnchorAI {

namespace {

// Matches the consumer locals' YawTowardTarget = Math_Atan2S(dz, dx).
inline s16 YawTowardXZ(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

}  // namespace

bool RunStuckRecoveryStep(Actor* actor,
                          const Vec3f& targetOrLeaderPos,
                          PlayState* play,
                          StuckRecoveryConfig& cfg) {
    (void)play;
    const bool verticalDominant =
        IsVerticalDominantSeparation(actor->world.pos, targetOrLeaderPos);
    const StuckCycleAction action =
        GetStuckAction(cfg.cycle, cfg.escalationThreshold, verticalDominant);
    const uint32_t cycleCount = cfg.cycle.count;

    // Cycle 3+: teleport to next subgoal (or fallback if path empty).
    if (action == StuckCycleAction::Teleport) {
        const bool havePath = !cfg.nav.path.waypoints.empty() &&
                              cfg.nav.path.cursorIdx <
                                  cfg.nav.path.waypoints.size();
        Vec3f       dest;
        const char* reason;
        if (havePath) {
            dest   = cfg.nav.path.CurrentSubgoal();
            reason = "next subgoal";
        } else {
            dest   = targetOrLeaderPos;
            reason = "fallback (path empty)";
        }
        SPDLOG_INFO("[{}] STUCK cycle {} escalation: teleport to {} "
                    "({:.0f},{:.0f},{:.0f})",
                    cfg.logPrefix, (int)cycleCount, reason,
                    dest.x, dest.y, dest.z);
        if (cfg.teleportFn != nullptr) {
            cfg.teleportFn(cfg.teleportUser, actor, play, dest, reason);
        }
        OnStuckTeleportFired(cfg.cycle);
        return true;
    }

    // Cycle 2 (default) or cycle 1 (vertical-dominant): cursor advance
    // — skip whichever subgoal the path currently points at.
    if (action == StuckCycleAction::CursorAdvance &&
        !cfg.nav.path.waypoints.empty() &&
        cfg.nav.path.cursorIdx < cfg.nav.path.waypoints.size() - 1) {
        const size_t before = cfg.nav.path.cursorIdx;
        cfg.nav.path.Advance();
        MarkCursorAdvanced(cfg.cycle);
        SPDLOG_INFO("[{}] STUCK cycle {}: advance cursor (skip subgoal "
                    "{} → {}/{})",
                    cfg.logPrefix, (int)cycleCount,
                    (int)before, (int)cfg.nav.path.cursorIdx,
                    (int)cfg.nav.path.waypoints.size() - 1);
    }

    // Cycles 1 + 2 (and vertical-dominant cycle 1 fall-through): legacy
    // nudge. Direct world.pos write — matches AI Player Follower's
    // STUCK-FWD action.
    //
    // Path-aware nudge target (was Invader-only pre-extraction): when
    // the path is non-empty, yaw the nudge toward the current path
    // subgoal rather than the raw target/leader. Mid-corridor stalls
    // get a nudge along the corridor instead of through whichever
    // wall is between us and the leader. When the path is empty,
    // fall back to the raw target/leader pos.
    Vec3f nudgeTarget = targetOrLeaderPos;
    if (!cfg.nav.path.waypoints.empty() &&
        cfg.nav.path.cursorIdx < cfg.nav.path.waypoints.size()) {
        nudgeTarget = cfg.nav.path.CurrentSubgoal();
    }
    const s16 yaw = YawTowardXZ(actor->world.pos, nudgeTarget);
    actor->shape.rot.y = yaw;
    actor->world.rot.y = yaw;
    actor->speedXZ     = 0.0f;
    const float dx = Math_SinS(yaw) * cfg.nudgeDist;
    const float dz = Math_CosS(yaw) * cfg.nudgeDist;
    actor->world.pos.x += dx;
    actor->world.pos.z += dz;

    // Cycle 1 only: reset path so next FOLLOW tick replans from new
    // pos. Cycle 2 keeps the cursor-advanced path so next FOLLOW tick
    // steers toward the new subgoal.
    if (cycleCount <= 1) {
        cfg.nav.path.Reset();
        cfg.nav.lastPathRefreshFrame = 0;
    }

    cfg.stuckCheckPos       = actor->world.pos;
    cfg.lastStuckCheckFrame = cfg.curFrame;

    SPDLOG_INFO("[{}] STUCK→FOLLOW (cycle {} nudged {:.0f}u toward yaw={})",
                cfg.logPrefix, (int)cycleCount, cfg.nudgeDist, (int)yaw);
    return false;
}

}  // namespace AnchorAI
