/**
 * AirborneRecovery — see AirborneRecovery.h for purpose.
 *
 * Logic source: FollowerNPC.cpp:4188-4232 (the inline tracking block
 * the helper consolidates).
 */

#include "AirborneRecovery.h"

#include <cmath>

namespace AnchorAI {

void StartAirborne(AirborneState& state, const Vec3f& startPos,
                   uint64_t curFrame) {
    state.jumpInProgress       = true;
    state.jumpStartFrame       = curFrame;
    state.jumpStartPos         = startPos;
    state.jumpPeakPos          = startPos;
    state.airbornePrevPos      = startPos;
    state.airbornePrevPosFrame = curFrame;
}

void EndAirborne(AirborneState& state) {
    state.jumpInProgress = false;
}

AirborneRecoveryResult UpdateAirborneRecovery(AirborneState& state,
                                              const AirborneRecoveryInput& in) {
    AirborneRecoveryResult out{};
    out.airborneFrames = in.curFrame - state.jumpStartFrame;

    // Peak-Y tracking — used by callers for diagnostic + fall-damage.
    if (in.currentPos.y > state.jumpPeakPos.y) {
        state.jumpPeakPos = in.currentPos;
        out.peakUpdated = true;
    }

    // Position-delta resnap. Re-baseline whenever pos has moved at
    // least minPosDelta — the resulting baseline timestamp drives the
    // "no movement in posResnapTicks" frozen detection below.
    const float dx = in.currentPos.x - state.airbornePrevPos.x;
    const float dy = in.currentPos.y - state.airbornePrevPos.y;
    const float dz = in.currentPos.z - state.airbornePrevPos.z;
    const float posDelta = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (posDelta >= in.minPosDelta) {
        state.airbornePrevPos      = in.currentPos;
        state.airbornePrevPosFrame = in.curFrame;
    }

    // Branch (a): zero-velocity hover.
    out.zeroVel  = std::fabs(in.velocityY) < in.zeroVelThreshold;
    // Branch (b): pos has been baselined more than posResnapTicks
    // ticks ago AND the current delta is below minPosDelta. The
    // " > prevPosFrame + posResnapTicks" gate ensures we don't fire
    // on a freshly-baselined sample where the resnap just happened.
    out.posStuck = (in.curFrame >
                    state.airbornePrevPosFrame + (uint64_t)in.posResnapTicks) &&
                   (posDelta < in.minPosDelta);

    const bool airborneEnough =
        (int)out.airborneFrames > in.minAirborneFrames;
    out.shouldForceTeleport =
        airborneEnough && !in.isOnFloor && (out.zeroVel || out.posStuck);

    return out;
}

}  // namespace AnchorAI
