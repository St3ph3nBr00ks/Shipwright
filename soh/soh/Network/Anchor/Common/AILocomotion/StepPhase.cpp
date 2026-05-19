/**
 * StepPhase — see StepPhase.h for purpose.
 *
 * Logic source: FollowerNPC.cpp StepPhaseCrossed + TickStepPhaseAndSfx
 * and Invader.cpp InvStepPhaseCrossed + InvTickStepPhaseAndSfx (both
 * identical pre-extraction).
 */

#include "StepPhase.h"

extern "C" {
#include "z64.h"
#include "functions.h"  // func_800F4010 (SFX), R_UPDATE_RATE
#include "variables.h"  // R_UPDATE_RATE (regs.h via variables.h)
#include "macros.h"
#include "sfx.h"        // NA_SE_PL_WALK_GROUND
}

namespace AnchorAI {

bool StepPhaseCrossed(float prevPhase, float curPhase, float footDown,
                      float cycle) {
    // No advance, no fire.
    if (curPhase == prevPhase) return false;
    // Wrap case: prev was near the end, cur wrapped to near the start.
    if (curPhase < prevPhase) {
        return (prevPhase < footDown && footDown <= cycle) ||
               (curPhase  >= footDown && footDown >= 0.0f);
    }
    // Normal forward case.
    return (prevPhase < footDown && footDown <= curPhase);
}

bool TickStepPhase(float& phase, Actor* actor,
                   float playSpeed,
                   float cycle,
                   float footDownL,
                   float footDownR) {
    if (actor == nullptr) return false;

    // Phase advance — playSpeed × half-update-rate. Matches Player's
    // step-phase advance at z_player.c:8087 (`unk_868 += playSpeed *
    // R_UPDATE_RATE * 0.5`). The 0.5 factor compensates for OoT's
    // 30Hz internal update being applied to a 60Hz anim cycle.
    const float updateRate = R_UPDATE_RATE * 0.5f;
    const float advance    = playSpeed * updateRate;
    const float prevPhase  = phase;
    float       newPhase   = prevPhase + advance;
    while (newPhase >= cycle) newPhase -= cycle;
    while (newPhase < 0.0f)   newPhase += cycle;
    phase = newPhase;

    if (StepPhaseCrossed(prevPhase, newPhase, footDownL, cycle) ||
        StepPhaseCrossed(prevPhase, newPhase, footDownR, cycle)) {
        // NA_SE_PL_WALK_GROUND is the base walk-on-ground SFX
        // (Player_ApplyFloorAndAgeSfxOffsets in vanilla picks per-
        // surface variants from the base; we use base for v1).
        // Pitch shifts with speed via the speedXZ arg.
        func_800F4010(&actor->projectedPos, NA_SE_PL_WALK_GROUND,
                      actor->speedXZ);
        return true;
    }
    return false;
}

}  // namespace AnchorAI
