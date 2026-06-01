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

}  // namespace AnchorAICombat
