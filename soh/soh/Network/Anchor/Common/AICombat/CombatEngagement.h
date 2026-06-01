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

#include "soh/Network/Anchor/Common/AILocomotion/NavStateTransitions.h"  // AnchorAI::ThresholdPair

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
// Phase 2 — shared TickENGAGE exit-decision logic.
//
// Both NPC Follower and NPC Invader's TickENGAGE handler open with the
// same exit-check ladder: target-lost → target-fled (3D break band)
// → strike-range (3D strike band) → continue pursuit. Follower adds a
// leader-leash bail between target-lost and target-fled; Invader does
// not have that concept. Once any exit fires, the per-actor body sets
// its own state enum, optionally clears `sAttackState.target`, zeroes
// speedXZ, and logs.
//
// `EvaluateEngageExit` factors the DECISION (which exit, with the
// distance metrics ready for logging); the caller switches on the
// returned `EngageExitKind` and applies its per-actor action. The
// pursuit body itself (substrate setup, climb-cell transition,
// locomotion drive, path-empty fallback) stays in the caller — those
// portions diverge too much to share cleanly (DR-2 Phase 2 §"Risk:
// MEDIUM. Complex control flow ...").
// ----------------------------------------------------------------------------
enum class EngageExitKind {
    ContinuePursuit,  // No exit; caller proceeds to its own pursuit drive.
    TargetLost,       // target nullptr / destroyed / dead.
    LeaderTooFar,     // Follower-only — leader leash exceeded → FOLLOW.
                      //   Invader passes leaderPos=nullptr so this is
                      //   never returned to it.
    TargetFled,       // Target outside break band → ChooseCombatExitState.
    StrikeRange,      // Target in strike band → caller transitions to ATTACK.
};

struct EngageExitDecision {
    EngageExitKind kind = EngageExitKind::ContinuePursuit;
    // Populated for all decisions (computed once by EvaluateEngageExit).
    float distXZ        = 0.0f;
    float dyToTarget    = 0.0f;
    // Populated only when kind == LeaderTooFar.
    float leaderDistXZ  = 0.0f;
    float leaderDy      = 0.0f;
};

struct EngageExitContext {
    // Required. Caller's self actor (for world.pos) + target actor.
    Actor* self;
    Actor* target;

    // Whether to gate target validity on health > 0. Follower true
    // (NPC retreats from dying targets); Invader false (chases until
    // Actor_Kill).
    bool checkTargetHealth = true;

    // Optional leader-leash bail. nullptr → Invader-style "no leash"
    // (LeaderTooFar never returned). When non-null, the caller's
    // current leader position. Both `leaderLeashBand` fields are
    // ignored when leaderPos == nullptr.
    const Vec3f*           leaderPos       = nullptr;
    AnchorAI::ThresholdPair leaderLeashBand = { 0.0f, 0.0f };

    // Required. Both bands compared via AnchorAI::ShouldPursue3D /
    // IsInStrikeRange. Caller's tuning constants
    // (kEngageBreakBand / kEngageStrikeBand in each TU).
    AnchorAI::ThresholdPair breakBand;
    AnchorAI::ThresholdPair strikeBand;
};

EngageExitDecision EvaluateEngageExit(const EngageExitContext& ctx);

}  // namespace AnchorAICombat
