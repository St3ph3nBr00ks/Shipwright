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

// ----------------------------------------------------------------------------
// Phase 4 — shared TickATTACK target resolution + AT-window + anim-complete.
//
// Both NPC Follower and NPC Invader's TickATTACK handler share the
// same overall structure, but with policy axes that DR-2 §"Phase 0
// animation-entry policy" identified:
//
//   1. Target validation: existing target invalidated if update==null
//      OR (Follower-only) health<=0.
//   2. Face target (per-actor — uses file-local YawTowardTarget).
//   3. AT-window: single-shot per swing while curAnimFrame is in
//      [activeStartFrame, activeEndFrame]. Caller's
//      `registerATWindow` closure wraps PositionAttackQuad +
//      CollisionCheck_SetAT against the actor's own atCollider.
//   4. Anim-complete: Follower uses plain `curFrame >= endFrame`;
//      Invader adds two guards (entry-frame hold + endFrame > 0)
//      to handle the case where the swing anim hasn't been wired
//      yet on the first tick.
//
// To preserve the original "face-then-AT" execution order (PositionAttack-
// Quad uses shape.rot.y just written by face logic), Phase 4 splits the
// shared logic into TWO helpers:
//
//   - `ResolveAttackTarget(...)` runs first; returns the resolved
//     target (nullptr if invalidated).
//   - Caller then writes the rotation locally using its own
//     YawTowardTarget.
//   - `TickAttackATAndComplete(...)` runs second; AT-window
//     registration + anim-complete decision.
//
// Per-actor still owns: speedXZ=0, on-entry animation handshake
// (Invader's explicit `InvEnsureAnimation` per DR-2 Phase 0 Option
// 2 — kept in the caller), face logic, post-exit SPDLOG + state
// transition + Follower's sLocalNav.navState.path.Reset().
// ----------------------------------------------------------------------------
struct ResolveAttackTargetContext {
    // Caller's existing target (sAttackState.target from each TU).
    Actor* existingTarget = nullptr;

    // Whether health<=0 invalidates the target. Follower true,
    // Invader false (Invader chases until Actor_Kill).
    bool checkTargetHealth = true;
};

// Returns the resolved face-target (nullptr if invalidated). Caller
// updates sAttackState.target = result before facing.
Actor* ResolveAttackTarget(const ResolveAttackTargetContext& ctx);

struct AttackTickContext {
    Actor* self;
    PlayState* play;

    // AT-window registration closure. Should call
    // PositionAttackQuad(this_) and
    // CollisionCheck_SetAT(play, &play->colChkCtx, &this_->atCollider.base).
    // Both wrapped because the actor pointer types differ (EnFollower*
    // vs EnInvader*).
    std::function<void()> registerATWindow;

    // Animation cursor + window thresholds.
    float curAnimFrame    = 0.0f;
    float endAnimFrame    = 0.0f;
    float activeStartFrame = 0.0f;
    float activeEndFrame   = 0.0f;

    // File-static swing-fired flag (sAttackState.swingFiredAT in
    // each TU). Toggled true after the active window closes so the
    // AT registers exactly once per swing.
    bool* swingFiredAT = nullptr;

    // Invader-style exit guards. Follower passes defaults
    // (curTick=0, entryFrame=0, minSwingHoldTicks=0,
    // guardEndFramePositive=false) and the guards no-op.
    uint64_t curTick               = 0;
    uint64_t entryFrame            = 0;
    int      minSwingHoldTicks     = 0;
    bool     guardEndFramePositive = false;
};

struct AttackTickResult {
    bool readyToExit = false;
};

// Runs the AT-window registration step (single-shot per swing) and
// returns whether the swing anim has completed (caller transitions
// to ChooseCombatExitState's result on true).
AttackTickResult TickAttackATAndComplete(const AttackTickContext& ctx);

// ----------------------------------------------------------------------------
// Phase 5 — shared TickBLOCK timer-expiry decision.
//
// TickBLOCK has high per-actor variance (DR-2 §"Phase 5 risk: medium-high"):
//
//   - Animation entry: Follower delegates to dispatcher (no anim wire
//     in TickBLOCK); Invader fires `InvEnsureAnimation(kBlockWait)` on
//     state-entry — DR-2 §"Policy axis #4" Option 2.
//   - Per-tick anim override: Invader swaps kBlockWait↔kBlockHit
//     based on `hitAnimFrames`; Follower has no equivalent.
//   - Frontal deflect: Invader-only AC_HIT absorption (DR-2
//     §"Policy axis #6"); Follower has none.
//   - Target validation: Follower exits via `ChooseCombatExitState`
//     when target invalidates mid-block; Invader just nulls the
//     target slot.
//   - Exit cleanup: each actor clears `sAttackState.target` under
//     different conditions.
//
// Given the divergence, Phase 5 narrow-scopes to the single shared
// block: the **timer-expiry decision** at the end of each TickBLOCK
// body. Both actors check `curFrame >= entryFrame + durationTicks`
// then branch on whether the target is in strike range. Returns
// {Continue, ExpiredInStrike, Expired}; caller switches on it and
// applies its per-actor state transition + SPDLOG.
//
// Entry-frame capture, per-tick anim override, target validation,
// face logic, frontal deflect, hitAnimFrames decrement stay
// per-actor (too divergent to share cleanly without extensive
// callback parametrization).
// ----------------------------------------------------------------------------
struct BlockTimerContext {
    Actor* self;
    Actor* target;
    uint64_t curFrame;
    uint64_t entryFrame;
    int blockDurationMs;
    AnchorAI::ThresholdPair strikeBand;  // typically kAttackEngageStrikeBand
};

struct BlockTimerDecision {
    enum class Kind {
        Continue,         // Block still ongoing — caller proceeds with current frame.
        ExpiredInStrike,  // Target in strike range — caller transitions to ATTACK.
        Expired,          // Timer up, no in-range target — caller calls ChooseCombatExitState.
    };
    Kind kind = Kind::Continue;
    // Populated when kind == ExpiredInStrike (so caller's SPDLOG can
    // render the same metrics the original body did).
    float distXZ     = 0.0f;
    float dyToTarget = 0.0f;
};

BlockTimerDecision EvaluateBlockTimer(const BlockTimerContext& ctx);

// ----------------------------------------------------------------------------
// Phase 6 — shared TryEngageCombat tier-1/2/3 evaluation.
//
// Both NPC Follower and NPC Invader's TryEngageCombat runs the same
// engagement-decision ladder once per non-combat tick:
//
//   Tier 1 (melee, `attackEngageDist`):
//     - Pick nearest reachable enemy/player target within XZ + Y bands.
//     - If HP-ratio <= blockHpThresholdRatio → BLOCK.
//     - Else → ATTACK.
//
//   Tier 2 (pursuit, `engageAcquireDist`):
//     - Pick nearest ground-level target. Tight Y filter (~60u) so
//       elevated targets fall through to Tier 3.
//     - → ENGAGE (locomotion drives pursuit to melee range).
//
//   Tier 3 (ranged, `rangedAcquireDist`):
//     - Gated on `hasRangedWeapon()`. Wide Y filter for elevated
//       targets. Additional XZ + |dy| gate so only genuinely
//       out-of-reach targets get the arrow.
//     - → RANGED_ATTACK.
//
// Per-actor differences (per DR-2 §"Policy axes"):
//   1. Target picker — Follower's `FindNearestEnemyForAttack` walks
//      ACTORCAT_ENEMY; Invader's `PickHostileTarget` walks player
//      actors (local + DummyPlayers + NPC Follower).
//   2. Max-HP source — Follower reads Link's healthCapacity; Invader
//      reads `this_->maxHealth` (intrinsic to actor).
//   3. Ranged-weapon ownership — Follower checks Link's inventory;
//      Invader v1 also checks Link's inventory (will change when
//      Invader gains independent gear).
//   4. State enum values — Follower's STATE_ATTACK / _BLOCK / _ENGAGE
//      / _RANGED_ATTACK vs Invader's matching constants.
//   5. `kRangedYFilter` value — Follower uses local 250u; Invader
//      uses `kRangedYFilter` tunable.
//   6. `kRangedElevatedYDelta` value — Follower hardcoded 60u;
//      Invader uses tunable.
//   7. Log message format — Follower logs `enemyId=0xXXXX`; Invader
//      logs `target Player at (X,Y,Z)`. Caller handles SPDLOG;
//      shared helper just returns the decision + diagnostic fields.
//   8. State transition + sAttackState bookkeeping — caller applies
//      after the decision returns (so per-actor file-static state
//      stays per-actor).
//
// The shared helper does NOT perform pre-checks (combat-disable
// gate, eligible-state gate, cooldown gate, invuln gate). Those
// stay in the caller because the gates differ across actors:
//   - Follower has `FollowerNpcInvulnerable()` (gated by CVar);
//     Invader has no equivalent.
//   - Eligible-state check uses per-actor state-enum constants.
// ----------------------------------------------------------------------------
struct EngageCombatContext {
    Actor* self;
    PlayState* play;

    // Per-actor target pickers. Each closure binds the caller's
    // `this_` + tuning constants. Tier 1 + 2 use the same 2-arg
    // signature; Tier 3 needs the Y-filter override.
    std::function<Actor*(float maxRange)>                 pickMeleeTarget;
    std::function<Actor*(float maxRange)>                 pickPursueTarget;
    std::function<Actor*(float maxRange, float maxYDelta)> pickRangedTarget;

    // Per-actor health metrics.
    std::function<int8_t()> maxHpReader;  // closure to defer the call
    int8_t                  currentHealth = 0;

    // Per-actor ranged-weapon ownership gate.
    std::function<bool()>   hasRangedWeapon;

    // Tunables — caller passes its own constants. Match the
    // existing per-actor tunable headers
    // (`InvaderTunables.h` / per-file FollowerNPC.cpp constants).
    float attackEngageDist      = 0.0f;
    float engageAcquireDist     = 0.0f;
    float rangedAcquireDist     = 0.0f;
    float rangedYFilter         = 0.0f;
    float rangedMinDist         = 0.0f;
    float rangedElevatedYDelta  = 60.0f;  // Follower's hardcoded default
    float blockHpThresholdRatio = 0.0f;
};

struct EngageDecision {
    enum class Kind {
        None,           // No tier matched — caller returns false.
        Block,          // Tier 1, low-HP branch.
        Attack,         // Tier 1, normal branch.
        Engage,         // Tier 2 pursuit.
        RangedAttack,   // Tier 3 elevated target.
    };
    Kind   kind            = Kind::None;
    Actor* target          = nullptr;
    int8_t hp              = 0;     // populated for Block diagnostic (only)
    int8_t maxHp           = 0;     // populated for Block diagnostic (only)
    float  distXZ          = 0.0f;  // populated for RangedAttack diagnostic
    float  dy              = 0.0f;  // populated for RangedAttack diagnostic
};

EngageDecision EvaluateCombatTiers(const EngageCombatContext& ctx);

}  // namespace AnchorAICombat
