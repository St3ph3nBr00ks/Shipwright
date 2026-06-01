#pragma once

// Refactor B.5 Phase 1 — shared combat-engagement helpers.
//
// First extract from the duplicated NPC Follower / NPC Invader
// combat handlers (Plans/B.5_design_review.md). Phase 1 is the MVP:
// only `ChooseCombatExitState` lands here. Later phases (TickENGAGE,
// TickSTANDBY, TickATTACK, TickBLOCK, TickRANGED_ATTACK,
// TryEngageCombat) will append to this header.
//
// Design pattern (DR-2 §"Recommended extraction sequence"): per-
// caller factory function builds a `CombatExitContext`, then the
// shared `ChooseCombatExitState` reads it. Differences between
// FollowerNPC.cpp and Invader.cpp surface as struct-field values
// (cooldown duration, exit state enum values) or as callbacks
// (target picker — closure captures `this_` + tuning).
//
// Why not delete the per-actor `ChooseCombatExitState` wrappers:
// they're called from many TickXxx sites inside each .cpp. Keeping
// the wrappers as thin `ctx-build + shared-call` delegators lets
// the call sites stay untouched; the wrapper itself shrinks from
// ~14 LoC to ~10 LoC of per-actor binding.

#include <cstdint>
#include <functional>

#include <libultraship/libultraship.h>  // pre-load C++ bridge headers

extern "C" {
#include "z64.h"  // Actor, PlayState, s32
}

namespace AnchorAICombat {

// Inputs to the shared combat-exit chooser. Each caller (FollowerNPC,
// Invader) builds one of these in its per-actor `ChooseCombatExitState`
// wrapper using its local `this_` + file-static state.
struct CombatExitContext {
    // Closure capturing the caller's `this_` + `play` + tuning. Returns
    // nullptr when no enemy is in standby-detect range. Both callers'
    // existing target pickers (FollowerNPC's
    // `FindNearestEnemyForAttack` and Invader's `PickHostileTarget`)
    // already take maxRange/maxYDelta — bind them via lambda.
    std::function<Actor*()> findNearbyEnemy;

    // Post-combat re-engagement cooldown (ms). Drives
    // `*outCombatCooldownEndFrame`. Differs per caller: Follower
    // 1500 ms, Invader 2500 ms.
    int postCombatCooldownMs;

    // Per-caller file-static state. Both callers have identically-
    // named statics (`sCombatCooldownEndFrame`, `sLastCombatExitFrame`)
    // in their own TU; the wrapper passes pointers to its locals.
    uint64_t* outCombatCooldownEndFrame;
    uint64_t* outLastCombatExitFrame;

    // Caller's state-machine enum values. The shared impl treats them
    // as opaque s32. Follower returns STANDBY/FOLLOW; Invader returns
    // STANDBY/IDLE.
    s32 stateIfNearbyEnemy;
    s32 stateIfNoEnemy;
};

// Pick the post-combat exit state (STANDBY if any enemy still in
// detect range, else the caller's idle/follow state) and arm the
// re-engagement cooldown + sheathe-delay window.
//
// Behaviour mirror of the original ChooseCombatExitState body in
// FollowerNPC.cpp:3083–3097 and Invader.cpp:2672–2685.
s32 ChooseCombatExitState(const CombatExitContext& ctx);

// ----------------------------------------------------------------------------
// Phase 3 — shared TickSTANDBY target resolution + handoff decision.
//
// Both NPC Follower and NPC Invader's TickSTANDBY zero speedXZ, refresh
// the face-target (validate existing then re-acquire via per-actor
// picker), then pick one of three exits:
//   - HandoffToOther (→ FOLLOW): the locomotion layer should resume
//     pursuit. Follower's predicate is leader-based; Invader's is
//     target-based — see `shouldHandoff` doc below.
//   - DropToIdle (→ IDLE): no target in detect range AND no handoff.
//     Follower drops to IDLE only when leader is close; Invader drops
//     to IDLE unconditionally (no leader concept).
//   - StayStandby: target acquired AND no handoff. TryEngageCombat
//     will swap to ATTACK / BLOCK / ENGAGE / RANGED_ATTACK once a
//     tier matches.
//
// `EvaluateStandby` returns the resolved face-target + decision; the
// caller updates `sAttackState.target`, applies per-actor facing
// (YawTowardTarget is file-local in each TU; Follower falls back to
// facing the leader, Invader skips facing when no target), and
// runs the per-actor state transition + SPDLOG.
// ----------------------------------------------------------------------------
struct StandbyEvaluation {
    enum class Decision {
        StayStandby,
        HandoffToOther,  // caller transitions to its FOLLOW-equivalent.
        DropToIdle,      // caller transitions to IDLE.
    };
    Decision decision      = Decision::StayStandby;
    Actor*   faceTarget    = nullptr;  // resolved target (null if none in range).
};

struct CombatStandbyContext {
    Actor* self;
    PlayState* play;

    // Existing target from caller's combat-state file-static
    // (sAttackState.target). May be null, stale, or freshly valid.
    Actor* existingTarget = nullptr;

    // Re-acquisition closure. Called when existingTarget is invalid
    // (null / update==nullptr / health<=0 if checkTargetHealth).
    // Follower passes FindNearestEnemyForAttack; Invader passes
    // PickHostileTarget.
    std::function<Actor*()> findNearbyEnemy;

    // Whether to invalidate the existing target on health <= 0.
    // Follower true (NPC retreats from dying targets); Invader false.
    bool checkTargetHealth = true;

    // Per-actor handoff predicate. Returns true when STANDBY should
    // transition to the caller's FOLLOW-equivalent. Receives the
    // resolved face-target (nullptr if no target acquired) so the
    // predicate can branch on target presence.
    //
    //   Follower: `[a, &leaderPos](Actor*) {
    //                return ShouldPursue3D(a->world.pos, leaderPos,
    //                                       kEnterFollowBand); }`
    //     — leader-based. Returns true whether or not a target was
    //     acquired (no-enemy + far-leader → FOLLOW; target + far-leader
    //     → FOLLOW).
    //
    //   Invader: `[a](Actor* t) {
    //                if (t == nullptr) return false;
    //                return ShouldPursue3D(a->world.pos, t->world.pos,
    //                                       kStandbyIdleBand); }`
    //     — target-based. Returns false when target is null (so the
    //     no-target branch falls through to DropToIdle).
    std::function<bool(Actor* faceTarget)> shouldHandoff;
};

StandbyEvaluation EvaluateStandby(const CombatStandbyContext& ctx);

}  // namespace AnchorAICombat
