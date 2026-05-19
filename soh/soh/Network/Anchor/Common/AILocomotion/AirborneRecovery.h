/**
 * AirborneRecovery — shared airborne-stuck detection for scripted-
 * position AI actors (NPC Follower, AI Invader).
 *
 * Mirrors NPC Follower's airborne-tracking block at
 * FollowerNPC.cpp:4188-4232. Two recovery triggers, OR'd:
 *   (a) airborneFrames > minAirborneFrames AND |velocity.y| < zeroVelThreshold
 *       (the "zero-velocity hover" scenario).
 *   (b) airborneFrames > minAirborneFrames AND no XYZ position delta
 *       ≥ minPosDelta in the last posResnapTicks ticks (the
 *       "velocity clamped at terminal but pos frozen" scenario —
 *       NPC Follower log 161, NPC frozen at -49,361,405 for 73s with
 *       velocity.y stuck at -10).
 *
 * The helper is a pure decision function. Caller owns the
 * AirborneState struct, the actual force-teleport (since each
 * caller teleports to a different target — leader for NPC,
 * combat target for Invader), and any diagnostic logging.
 *
 * Lifecycle:
 *   - Caller invokes StartAirborne() the tick it triggers a jump.
 *   - Caller invokes UpdateAirborneRecovery() every tick while
 *     state.jumpInProgress is true. Acts on result.shouldForceTeleport.
 *   - Caller invokes EndAirborne() on landing OR on force-teleport.
 */

#pragma once

extern "C" {
#include "z64.h"
}

#include <cstdint>

namespace AnchorAI {

// Caller-owned persistent state.
struct AirborneState {
    bool     jumpInProgress       = false;
    uint64_t jumpStartFrame       = 0;
    Vec3f    jumpStartPos         = { 0.0f, 0.0f, 0.0f };
    Vec3f    jumpPeakPos          = { 0.0f, 0.0f, 0.0f };  // highest Y reached
    // Position-delta resnap tracking — re-baselined every time pos
    // moves ≥ minPosDelta within the last posResnapTicks window.
    Vec3f    airbornePrevPos      = { 0.0f, 0.0f, 0.0f };
    uint64_t airbornePrevPosFrame = 0;
};

// Inputs to per-tick recovery check.
struct AirborneRecoveryInput {
    Vec3f    currentPos;
    float    velocityY;
    bool     isOnFloor;
    uint64_t curFrame;

    // Tuning — defaults match NPC Follower's tuned values.
    int   minAirborneFrames = 100;   // ~5s @ 20fps
    int   posResnapTicks    = 100;   // baseline retest window
    float minPosDelta       = 0.5f;  // smaller than this counts as frozen
    float zeroVelThreshold  = 1.0f;  // |velocity.y| treated as "zero"
};

// Outputs.
struct AirborneRecoveryResult {
    bool     shouldForceTeleport = false;  // caller should teleport + EndAirborne
    bool     posStuck            = false;  // recovery branch (b) triggered
    bool     zeroVel             = false;  // recovery branch (a) triggered
    uint64_t airborneFrames      = 0;      // for diagnostic logging
    bool     peakUpdated         = false;  // pos.y rose past prior peak this tick
};

// Call on the jump-fire tick. Initializes baselines.
void StartAirborne(AirborneState& state, const Vec3f& startPos,
                   uint64_t curFrame);

// Call once per tick while state.jumpInProgress is true.
// Updates peakPos + airbornePrev* in `state` and returns recovery decision.
// Does NOT clear jumpInProgress — caller does that on landing OR after
// acting on shouldForceTeleport (via EndAirborne).
AirborneRecoveryResult UpdateAirborneRecovery(AirborneState& state,
                                              const AirborneRecoveryInput& in);

// Reset on landing or after force-teleport.
void EndAirborne(AirborneState& state);

}  // namespace AnchorAI
