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

}  // namespace AnchorAICombat
