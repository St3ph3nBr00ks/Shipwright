/**
 * NavStateTransitions — shared state-transition predicates for AI actors.
 *
 * Three actor systems (NPC Follower, NPC Invader, AI Player Follower)
 * each had their own copy of the same three checks: "did I arrive?",
 * "should I pursue?", "did I make progress?". Each copy was XZ-only
 * until log 263 surfaced the same bug class in all three — when the
 * target is directly above/below on a ledge, XZ-only checks fire
 * false-positive arrival and never engage the next climb segment.
 *
 * The Phase 1 fix added Y-axis gates inline per-actor (3 commits).
 * Phase 2 (this file) extracts the predicates so future actors get
 * 3D-aware behavior by default — there is no XZ-only `IsArrived` to
 * copy-paste; the shared predicate forces callers to pass a Y
 * tolerance.
 *
 * Pure functions. No state, no side effects. All thresholds are
 * caller-supplied so each actor can tune its own pursuit/arrival
 * radii.
 *
 * Sibling helpers in Common/AILocomotion/:
 *   - StuckEscalation.{h,cpp} — cycle-based STUCK escalation tiers.
 *   - StuckRecovery.{h,cpp}   — TickSTUCK dispatch (consumes this header).
 *   - NavOrDirect, ScriptedFollow, LocomotionAnim, AirborneRecovery,
 *     HeadLook, StepPhase — shared locomotion mechanics.
 * Parent Common/:
 *   - DistanceMath.h — raw distance math (`Dist3D`, `DistXZ`, etc.).
 *   - JumpResolver.h — jump-feasibility checks.
 *   - LeashRespawn.h — enemy rehydration on scene re-entry.
 *   - TargetSelection.h — actor-agnostic target candidate scoring.
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md
 *   - GitHub #206 — nav refactor
 */
#pragma once

#include <cmath>

#include "../DistanceMath.h"

extern "C" {
#include "z64math.h"  // Vec3f
}

namespace AnchorAI {

// -----------------------------------------------------------------------------
// ThresholdPair — XZ + Y threshold pair for state-transition predicates.
//
// The Y-companion constant pattern (kFooXZ + kFooY) was scattered
// across the three actor files post-Phase-3:
//   FollowerNPC:   kEnterIdle/Y, kEnterFollow/Y, kEngageBreakDist/Y,
//                   kEngageLeaderLeash/Y, kEngageStrikeDist/Y...
//   NPC Invader:    kInvFollowIdleDist/Y, kInvEngageBreakDist/Y,
//                   kInvEngageStrikeDist/Y, kStandbyIdleRadius/Y...
//   AI Player Follower: kMaxLeash/Y, ...
//
// Each pair is logically a single concept ("arrival band", "leash
// radius") but spelled as two unrelated float constants. ThresholdPair
// groups them so a single named constant names the concept, and the
// predicate overloads taking a ThresholdPair read more naturally at
// call sites.
//
// Legacy two-float overloads of IsArrived3D / ShouldPursue3D /
// IsInStrikeRange remain — both pass-by-pair and pass-by-floats are
// supported, so consumers can adopt incrementally.
// -----------------------------------------------------------------------------
struct ThresholdPair {
    float xz;
    float y;
};

// -----------------------------------------------------------------------------
// IsArrived3D — "close in BOTH XZ and Y"
//
// Used by FOLLOW→IDLE arrival checks. Caller passes XZ radius (typical
// pursuit IDLE-band) and Y tolerance (typical Link body height, ~40-60u).
//
// Both gates required. The classic bug is to use XZ alone — when the
// target is directly above on a ledge (huge Y delta, small XZ), the
// XZ check false-positives "arrived" and the actor stops trying to
// climb. This predicate forces the Y check to be considered.
// -----------------------------------------------------------------------------
inline bool IsArrived3D(const Vec3f& pos, const Vec3f& target,
                         float xzRadius, float yTolerance) {
    const float distXZSq = AnchorDist::DistXZSq(pos, target);
    const float dy       = std::fabs(target.y - pos.y);
    return distXZSq <= (xzRadius * xzRadius) && dy <= yTolerance;
}
inline bool IsArrived3D(const Vec3f& pos, const Vec3f& target,
                         const ThresholdPair& band) {
    return IsArrived3D(pos, target, band.xz, band.y);
}

// -----------------------------------------------------------------------------
// ShouldPursue3D — "far enough in EITHER XZ or Y to re-engage"
//
// Hysteresis pair with IsArrived3D for IDLE→FOLLOW transitions. The
// thresholds should be LARGER than the arrival thresholds (e.g.,
// arrive at 50u/40u, re-engage at 80u/60u). Disjunctive: re-engage
// when the target has moved away in any dimension.
//
// The classic bug is to use XZ alone — when the target climbs up
// above on a ledge (huge Y delta, small XZ), XZ-only fails to
// trigger re-engage and the actor stays stuck in IDLE.
// -----------------------------------------------------------------------------
inline bool ShouldPursue3D(const Vec3f& pos, const Vec3f& target,
                            float xzThreshold, float yThreshold) {
    const float distXZSq = AnchorDist::DistXZSq(pos, target);
    const float dy       = std::fabs(target.y - pos.y);
    return distXZSq > (xzThreshold * xzThreshold) || dy > yThreshold;
}
inline bool ShouldPursue3D(const Vec3f& pos, const Vec3f& target,
                            const ThresholdPair& band) {
    return ShouldPursue3D(pos, target, band.xz, band.y);
}

// -----------------------------------------------------------------------------
// ProgressTowardTarget3D — signed 3D distance reduction between two ticks
//
// Returns positive when the actor closed the 3D distance to target,
// negative when it moved away, ~0 when stuck. Used by the stuck
// detector — XZ-only progress missed vertical motion (climbing
// counted as zero progress), triggering false-positive stuck escalation.
//
// Equivalent to: prev3D(prevPos, targetPos) - cur3D(curPos, targetPos).
// -----------------------------------------------------------------------------
inline float ProgressTowardTarget3D(const Vec3f& prevPos,
                                      const Vec3f& curPos,
                                      const Vec3f& targetPos) {
    const float prev = AnchorDist::Dist3D(prevPos, targetPos);
    const float cur  = AnchorDist::Dist3D(curPos,  targetPos);
    return prev - cur;
}

// -----------------------------------------------------------------------------
// RawDisplacement3D — magnitude of the actor's movement between two ticks
//
// Used as the "did I physically move at all?" companion to
// ProgressTowardTarget3D. Distinct from progress because raw motion
// is direction-agnostic — an actor oscillating against a wall might
// show real displacement but zero progress toward the goal.
// -----------------------------------------------------------------------------
inline float RawDisplacement3D(const Vec3f& prevPos, const Vec3f& curPos) {
    return AnchorDist::Dist3D(prevPos, curPos);
}

// -----------------------------------------------------------------------------
// IsInStrikeRange — alias for IsArrived3D with combat-tier intent
//
// Used by BLOCK / ENGAGE timer rechecks where the consumer needs to
// know "is the target STILL within my sword's effective melee reach?"
// — same math as the FOLLOW→IDLE arrival predicate, but the name
// documents the combat-state intent at the call site. Thin wrapper;
// no behavior difference vs IsArrived3D.
// -----------------------------------------------------------------------------
inline bool IsInStrikeRange(const Vec3f& attackerPos, const Vec3f& targetPos,
                             float strikeRadius, float strikeYReach) {
    return IsArrived3D(attackerPos, targetPos, strikeRadius, strikeYReach);
}
inline bool IsInStrikeRange(const Vec3f& attackerPos, const Vec3f& targetPos,
                             const ThresholdPair& reach) {
    return IsArrived3D(attackerPos, targetPos, reach.xz, reach.y);
}

// -----------------------------------------------------------------------------
// IsVerticalDominantSeparation — "the gap to target is mostly vertical"
//
// Returns true when |dy| > kRatio × distXZ. Used by stuck-recovery
// escalation: when an actor is stuck AND the separation is vertical-
// dominant, horizontal nudges can't help — the actor needs to engage
// climb/drop/hoist or teleport. Lets the recovery policy skip the
// useless horizontal-nudge cycle.
//
// kRatio defaults to 2.0 (i.e., target is more than twice as far away
// vertically as it is horizontally). Tune via the explicit parameter
// if a specific actor needs different sensitivity.
// -----------------------------------------------------------------------------
inline bool IsVerticalDominantSeparation(const Vec3f& pos, const Vec3f& target,
                                           float ratio = 2.0f) {
    const float distXZ = AnchorDist::DistXZ(pos, target);
    const float dy     = std::fabs(target.y - pos.y);
    return dy > ratio * std::max(distXZ, 1.0f);  // guard against /0 when XZ-aligned
}

}  // namespace AnchorAI
