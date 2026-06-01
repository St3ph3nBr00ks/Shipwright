// Refactor B.5 Phase 1+2+3 — shared combat-engagement helpers.

#include "CombatEngagement.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/DistanceMath.h"  // AnchorDist::DistXZ

#include <atomic>
#include <cmath>

namespace AnchorAICombat {

s32 ChooseCombatExitState(const CombatExitContext& ctx) {
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    // Arm the post-combat cooldown — TryEngageCombat compares `curFrame
    // < sCombatCooldownEndFrame` and suppresses re-engagement until
    // the window closes. Without this, short-anim combat exits (e.g.
    // RANGED_ATTACK at kBowShoot ~6 frames) re-fire instantly from
    // STANDBY → weapon flicker + blocked FOLLOW transitions.
    if (ctx.outCombatCooldownEndFrame != nullptr) {
        *ctx.outCombatCooldownEndFrame = curFrame +
            (uint64_t)Anchor::Instance->MsToGameTicks(ctx.postCombatCooldownMs);
    }

    // Open the sheathe-delay window. Each caller's
    // {Follower,Inv}StateToModelGroup uses sLastCombatExitFrame to
    // keep the last-combat weapon visible in non-combat states for
    // its tuned duration — mirrors Player's vanilla "stay armed for
    // N seconds after combat" behaviour.
    if (ctx.outLastCombatExitFrame != nullptr) {
        *ctx.outLastCombatExitFrame = curFrame;
    }

    Actor* nearby = ctx.findNearbyEnemy ? ctx.findNearbyEnemy() : nullptr;
    return (nearby != nullptr) ? ctx.stateIfNearbyEnemy
                               : ctx.stateIfNoEnemy;
}

EngageExitDecision EvaluateEngageExit(const EngageExitContext& ctx) {
    EngageExitDecision out;

    // Target validity. Health check is opt-in (Follower true, Invader
    // false) — see EngageExitContext::checkTargetHealth.
    if (ctx.target == nullptr || ctx.target->update == nullptr) {
        out.kind = EngageExitKind::TargetLost;
        return out;
    }
    if (ctx.checkTargetHealth && ctx.target->colChkInfo.health <= 0) {
        out.kind = EngageExitKind::TargetLost;
        return out;
    }

    const Vec3f& selfPos   = ctx.self->world.pos;
    const Vec3f& targetPos = ctx.target->world.pos;
    out.distXZ      = AnchorDist::DistXZ(selfPos, targetPos);
    out.dyToTarget  = std::fabs(targetPos.y - selfPos.y);

    // Optional leader-leash bail (Follower only). When `leaderPos` is
    // non-null, AnchorAI::ShouldPursue3D returns true once the leader
    // is far enough away in either XZ or Y to warrant yielding combat.
    if (ctx.leaderPos != nullptr &&
        AnchorAI::ShouldPursue3D(selfPos, *ctx.leaderPos, ctx.leaderLeashBand)) {
        out.kind         = EngageExitKind::LeaderTooFar;
        out.leaderDistXZ = AnchorDist::DistXZ(selfPos, *ctx.leaderPos);
        out.leaderDy     = std::fabs(ctx.leaderPos->y - selfPos.y);
        return out;
    }

    // Target fled past the break band — same predicate, applied to
    // the target instead of the leader.
    if (AnchorAI::ShouldPursue3D(selfPos, targetPos, ctx.breakBand)) {
        out.kind = EngageExitKind::TargetFled;
        return out;
    }

    // In strike range — caller transitions to ATTACK.
    if (AnchorAI::IsInStrikeRange(selfPos, targetPos, ctx.strikeBand)) {
        out.kind = EngageExitKind::StrikeRange;
        return out;
    }

    out.kind = EngageExitKind::ContinuePursuit;
    return out;
}

StandbyEvaluation EvaluateStandby(const CombatStandbyContext& ctx) {
    StandbyEvaluation out;

    // Validate existing target → re-acquire if invalid. Same shape as
    // the original TickSTANDBY bodies in FollowerNPC.cpp:3017-3024 and
    // Invader.cpp:2526-2533.
    Actor* faceTarget = ctx.existingTarget;
    bool invalid = (faceTarget == nullptr || faceTarget->update == nullptr);
    if (!invalid && ctx.checkTargetHealth) {
        invalid = (faceTarget->colChkInfo.health <= 0);
    }
    if (invalid) {
        faceTarget = ctx.findNearbyEnemy ? ctx.findNearbyEnemy() : nullptr;
    }
    out.faceTarget = faceTarget;

    // Decision precedence: handoff > drop-to-idle > stay-standby.
    // Follower's leader-far case triggers HandoffToOther regardless
    // of target presence (so "no enemy + far leader" goes FOLLOW
    // rather than IDLE). Invader's predicate returns false when
    // target is null, so the no-target branch falls through.
    const bool wantsHandoff =
        ctx.shouldHandoff ? ctx.shouldHandoff(faceTarget) : false;
    if (wantsHandoff) {
        out.decision = StandbyEvaluation::Decision::HandoffToOther;
    } else if (faceTarget == nullptr) {
        out.decision = StandbyEvaluation::Decision::DropToIdle;
    } else {
        out.decision = StandbyEvaluation::Decision::StayStandby;
    }
    return out;
}

}  // namespace AnchorAICombat
