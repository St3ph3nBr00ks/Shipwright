// Refactor B.5 Phase 1 — shared combat-engagement helpers.

#include "CombatEngagement.h"

#include "soh/Network/Anchor/Anchor.h"

#include <atomic>

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
