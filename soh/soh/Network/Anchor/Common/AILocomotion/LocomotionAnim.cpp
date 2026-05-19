/**
 * LocomotionAnim — shared climb-anim decision (Phase 6).
 *
 * Mirrors NPC Follower's FollowerNPC.cpp:4390-4435 block. The helper
 * does NOT update any caller state — the caller owns prevPos/PrevY,
 * climbNextIsRight, and the actual EnsureAnimation call. The helper
 * is a pure decision function: given a tick's pos delta + alternation
 * toggle + current-anim context, decide which climb-step (if any) to
 * fire and whether the L/R alternation should advance.
 */

#include "LocomotionAnim.h"

#include <cmath>

namespace AnchorAI {

ClimbAnimResult PickClimbAnimStep(const ClimbAnimContext& ctx) {
    ClimbAnimResult out{};
    out.step = ClimbAnimStep::kHoldCurrent;
    out.advanceLR = false;

    // |dy| for vertical magnitude; signed dx/dz combined into |dxz|.
    const float dy  = std::fabs(ctx.currentPos.y - ctx.prevPos.y);
    const float dxz_x = ctx.currentPos.x - ctx.prevPos.x;
    const float dxz_z = ctx.currentPos.z - ctx.prevPos.z;
    const float dxz = std::sqrt(dxz_x * dxz_x + dxz_z * dxz_z);

    const bool isMovingVertically = (dy  > 0.5f);
    const bool isMovingLaterally  = (dxz > 0.5f);
    // Lateral dominates only when its magnitude exceeds vertical.
    // Otherwise vertical anim drives even on slight off-axis drift.
    const bool useSideAnim = isMovingLaterally && (dxz > dy);

    if ((isMovingVertically || isMovingLaterally) && ctx.prevStepDone) {
        // Real step — alternate L/R based on caller-owned toggle.
        const bool fireRight = ctx.climbNextIsRight;
        if (useSideAnim) {
            out.step = fireRight ? ClimbAnimStep::kFireSideR
                                 : ClimbAnimStep::kFireSideL;
        } else {
            out.step = fireRight ? ClimbAnimStep::kFireUpR
                                 : ClimbAnimStep::kFireUpL;
        }
        out.advanceLR = true;
    } else if (!isMovingVertically && !isMovingLaterally) {
        // Stationary. First-tick fallback: if actor hasn't latched
        // onto a climb anim yet, fire UpL so it visibly attaches to
        // the wall. Otherwise hold current pose (matches Player's
        // PLAYER_STATE2_STATIONARY_LADDER — frozen at last frame).
        if (!ctx.currentAnimIsClimb) {
            out.step = ClimbAnimStep::kFireUpL;
            // Don't advance L/R — this isn't a real step, just a
            // pose-set. Leave the toggle so the first real step
            // can lead with whichever side the caller chose.
            out.advanceLR = false;
        } else {
            out.step = ClimbAnimStep::kHoldCurrent;
        }
    } else {
        // Moving but previous one-shot still playing — hold current.
        out.step = ClimbAnimStep::kHoldCurrent;
    }

    return out;
}

}  // namespace AnchorAI
