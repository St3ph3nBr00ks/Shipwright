/**
 * HeadLook — see HeadLook.h for purpose.
 *
 * Logic source: FollowerNPC.cpp TickHeadLookAtLeader and
 * Invader.cpp TickHeadLookAtTarget (both identical math pre-extraction).
 */

#include "HeadLook.h"

extern "C" {
#include "functions.h"  // Math_Atan2S, Math_ScaledStepToS
}

#include <cmath>

namespace AnchorAI {

void StepHeadLookToward(const HeadLookInputs& in,
                        Vec3s* headRot,
                        Vec3s* upperRot) {
    if (headRot == nullptr || upperRot == nullptr) return;

    const s16 dirYaw = Math_Atan2S(in.targetPos.z - in.actorPos.z,
                                   in.targetPos.x - in.actorPos.x);
    const s16 yawRel = dirYaw - in.actorYaw;

    // Pitch via Math_Vec3f_Pitch idiom (z_lib.c:292-294).
    // Math_Atan2S(forward, side) takes forward axis first; correct
    // signature is Atan2S(distXZ, actor.y - target.y). Returns small
    // negative when target is above (≈ "looking up" in Player's
    // headRot.x convention); positive when below; 0 when same height.
    const float dx     = in.targetPos.x - in.actorPos.x;
    const float dz     = in.targetPos.z - in.actorPos.z;
    const float distXZ = std::sqrt(dx*dx + dz*dz);
    const s16 pitchRel = (distXZ > 1.0f)
        ? Math_Atan2S(distXZ, in.actorPos.y - in.targetPos.y)
        : 0;

    // Apportion yaw: head takes ±headYawMax, upper body twists for
    // the rest. Field-tested value 12743 (≈ 0x31C7 = 70°) for head;
    // wider caps looked unnatural.
    s16 headYawTarget  = yawRel;
    s16 upperYawTarget = 0;
    if (headYawTarget >  in.headYawMax) {
        upperYawTarget = headYawTarget - in.headYawMax;
        headYawTarget  =  in.headYawMax;
    }
    if (headYawTarget < -in.headYawMax) {
        upperYawTarget = headYawTarget + in.headYawMax;
        headYawTarget  = -in.headYawMax;
    }
    // Target mostly behind — don't snap-spin the upper body.
    if (upperYawTarget >  in.upperYawMax) upperYawTarget =  in.upperYawMax;
    if (upperYawTarget < -in.upperYawMax) upperYawTarget = -in.upperYawMax;

    // Pitch cap.
    s16 headPitchTarget = pitchRel;
    if (headPitchTarget >  in.headPitchMax) headPitchTarget =  in.headPitchMax;
    if (headPitchTarget < -in.headPitchMax) headPitchTarget = -in.headPitchMax;

    Math_ScaledStepToS(&headRot->y,  headYawTarget,   in.stepRate);
    Math_ScaledStepToS(&upperRot->y, upperYawTarget,  in.stepRate);
    Math_ScaledStepToS(&headRot->x,  headPitchTarget, in.stepRate);
}

void ResetHeadLookToNeutral(Vec3s* headRot, Vec3s* upperRot) {
    if (headRot == nullptr || upperRot == nullptr) return;
    headRot->y  = 0;
    headRot->x  = 0;
    upperRot->y = 0;
}

}  // namespace AnchorAI
