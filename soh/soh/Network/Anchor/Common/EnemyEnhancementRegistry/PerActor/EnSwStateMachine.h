/**
 * EnSwStateMachine — Option B pilot for En_Sw combat enhancement.
 *
 * Per Plans/en_sw_enhanced_state_machine_pilot.md — replaces the
 * splice-into-vanilla NavConsume approach (proven architecturally
 * dead by log 761) with a self-contained state machine that owns
 * motion + rotation for the enhancement lifecycle.
 *
 * M4 scope (this file, initial version): enum + state block + dispatch
 * + TickUninitialized + TickWallIdle. Skeleton only — the state
 * machine tick is NOT yet wired to actor updates (M5 installs it via
 * actionFunc override at OnInit).
 *
 * Future milestones expand this file:
 *   M5 — TickWallPursue + tangent-basis math + actionFunc install
 *   M6 — TickWallEdgeDrop + TickGroundPursue + TickGroundToWallReattach
 *   M7 — TickLungeYield + vanilla state 7 address detection
 *   M8 — extended [EEDiag] output for state-machine transitions
 *
 * Threading + entity-agnosticism notes:
 *   - Runs on the game thread (state machine tick fires from
 *     Anchor_Enhance_EnSw_Tick / eventually EnSw_EnhancedActionFunc).
 *   - Per-actor state map is file-static in EnSwStateMachine.cpp; no
 *     per-thread issues since En_Sw updates are single-threaded.
 *   - Entity-specific by design (contra Common/AILocomotion/ helpers
 *     which are actor-agnostic). This state machine assumes En_Sw
 *     struct layout + vanilla En_Sw actionFunc chain awareness. Other
 *     wall-crawler descriptors that adopt this pattern should clone
 *     the file to PerActor/EnXxxStateMachine.{h,cpp} rather than
 *     generalising prematurely (per parent plan §4.9 "first pilot
 *     lives fully in the descriptor").
 */

#pragma once

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before the extern "C" block below opens.
#include "soh/Network/Anchor/Anchor.h"

#include <cstdint>

// Overlay header pulls in global.h → z64.h transitively (Pitfall 40: safe
// because Anchor.h above already seeded libultraship + nlohmann templates
// in C++ linkage; the guarded transitive z64.h is a no-op inside this
// extern "C" block).
extern "C" {
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
}

namespace AnchorEnemyEnhancement {

// -------------------------------------------------------------------
// State enum
// -------------------------------------------------------------------
// Values are stable — used in [EEDiag] output + potentially in wire
// format later if peer replicas need to disambiguate state. Additions
// go at the end; do not renumber.
enum class EnSwState : uint8_t {
    Uninitialized        = 0,  // pre-attachment; TickUninitialized attempts basis raycast
    WallIdle             = 1,  // on wall, no target in detect range; hold position
    WallPursue           = 2,  // on wall, target in range; tangent-plane advance      (M5)
    WallEdgeDrop         = 3,  // was WallPursue, walked off wall's edge; falling      (M6)
    GroundPursue         = 4,  // airborne resolved to floor; ground-XZ pursuit         (M6)
    GroundToWallReattach = 5,  // GroundPursue hit a wall; establishing new basis      (M6)
    LungeYield           = 6,  // vanilla state 7 wind-up firing; delegate to vanilla  (M7)
    PermanentlyDisabled  = 7,  // basis raycast failed at init; yield to vanilla forever
    JumpLunge            = 8,  // ballistic wind-up + arc + landing (Tektite-style)    (M10)
    WalkLunge            = 9,  // custom ground wind-up + straight-line dash (M11 —
                                // replaces vanilla func_80B0E728 walk-lunge for
                                // enhanced spiders, eliminates actionFunc oscillation)
};

// -------------------------------------------------------------------
// Per-actor state block
// -------------------------------------------------------------------
// Held in a file-static std::unordered_map<Actor*, EnSwEnhancedState>
// inside EnSwStateMachine.cpp. Not exposed publicly — callers go
// through the tick function only.
struct EnSwEnhancedState {
    EnSwState state         = EnSwState::Uninitialized;
    int       framesInState = 0;

    // Wall basis established via TickUninitialized's raycast. Reused
    // by TickWallIdle / TickWallPursue for rotation rebuild + tangent-
    // plane motion. Recomputed on GroundToWallReattach for new walls.
    bool           hasWallBasis = false;
    Vec3f          wallNormal   = {0.0f, 1.0f, 0.0f};  // outward from wall (unit)
    Vec3f          wallTangentU = {1.0f, 0.0f, 0.0f};  // forward walk direction (unit)
    Vec3f          wallTangentV = {0.0f, 0.0f, 1.0f};  // orthogonal in-plane (unit)
    CollisionPoly* wallPoly     = nullptr;             // reference for per-frame re-attach

    // Vanilla ambient actionFunc pointer, captured at OnInit. Used as
    // the "we're safe to override" signal — when self->actionFunc still
    // equals this snapshot, vanilla is in its ambient wait state (which
    // doesn't write world.pos) so our motion is uncontested. When
    // self->actionFunc has advanced to something else (lunge cycle,
    // etc.), we yield the frame so vanilla lunge runs uninterrupted.
    //
    // Design pivot from the pilot plan §5.1 "install actionFunc override"
    // approach: intercepting actionFunc entirely would freeze vanilla's
    // internal state advancement, breaking the lunge cycle. Running as
    // a post-hook alongside vanilla with an ambient-actionFunc check is
    // simpler and preserves vanilla lunge for free.
    EnSwActionFunc initialAmbientActionFunc = nullptr;
    bool           haveAmbientSnapshot      = false;

    // Snapshot of vanilla actionFunc at the moment we entered LungeYield.
    // Used to detect vanilla's post-lunge transition (func_80B0E728 →
    // func_80B0E9BC walk-home OR func_80B0E90C post-lunge-stop). Once
    // vanilla leaves the initial lunge state, we force it back to
    // ambient via Anchor_Enhance_EnSw_ForceAmbient so the enhanced
    // state machine can re-take control (resume GroundPursue / chain
    // another lunge if still in attack range) instead of letting
    // vanilla walk-home in a straight line to actor.home.pos.
    EnSwActionFunc lungeEntryActionFunc = nullptr;

    // JumpLunge state — Tektite-style ballistic attack. Wind-up phase
    // (kJumpWindupFrames) plays a purple telegraph in place; then
    // airborne phase applies (jumpVelXZ_x/z, jumpVelY) with gravity
    // accumulating each frame until floor/wall contact.
    //   jumpAirborne = false during wind-up, true once launched
    //   jumpVel* = per-frame velocity components (Y updated by gravity)
    bool  jumpAirborne = false;
    float jumpVelX     = 0.0f;
    float jumpVelY     = 0.0f;
    float jumpVelZ     = 0.0f;
    // Position at the start of the current airborne tick — captured
    // before physics integration for swept collision detection. See
    // Analysis/en_sw_jumplunge_wall_clip_2026-07-31.md — the previous
    // vertical-only raycast missed wall clips during upward phase and
    // during horizontal motion, letting spiders tunnel through walls
    // and fall into voids.
    Vec3f jumpPrevPos = {0.0f, 0.0f, 0.0f};

    // Sticky targeting — cached target Actor* to avoid re-picking every
    // frame. Reference model: NPC Invader's state.targetClientId. Set
    // whenever FindNearestPlayerActor returns a valid target; held for
    // up to kStickyGraceFrames after target lookup fails (rare — mostly
    // during scene transitions). Cleared when the pointer becomes stale
    // (target->update == nullptr) or grace expires.
    Actor* stickyTarget     = nullptr;
    int    stickyLossFrames = 0;

    // Idle gaze rotation — slow random yaw sweep for ground spider not
    // pursuing/attacking (vanilla func_80B0E5E0 does the same for wall
    // spider via shape.rot.z, which our post-hook doesn't override on
    // wall). Frame count is absolute (play->gameplayFrames-based) so
    // it survives state transitions cleanly.
    s16  idleGazeTargetYaw       = 0;
    int  idleGazeNextChangeFrame = 0;
    // Two-phase rhythm — spider alternates:
    //   Rest phase (isLooking=false): sit still, no rotation / no
    //     leg animation, until gameplayFrames >= idleGazeNextChangeFrame.
    //   Look phase (isLooking=true):  smooth-step yaw toward
    //     idleGazeTargetYaw; when arrived, return to rest phase with
    //     a fresh random duration (10-40 frames, matching vanilla
    //     func_80B0E5E0 unk_388 = Rand_S16Offset(10, 30)).
    bool idleGazeIsLooking = false;

    // Walk-animation gate — true only when the spider is actually
    // translating in XZ (wall crawling, ground pursuing, walk-lunge
    // dash). False during holds, idle, wind-ups, jumps, gaze rotation.
    // OverrideLimbBend in EnSwDescriptor consults this via bridge to
    // suppress the leg-lift cycle when spider is stationary — otherwise
    // legs oscillate even during idle, which looks wrong.
    bool isWalkAnimActive = false;

    // Walk-animation intensity — normalized [0..1] rate that scales
    // skelAnime.playSpeed via the post-tick write (see Bug 2 in
    // Analysis/en_sw_bugs_wall_idle_ground_intensity_vine_jump_2026-07-31.md).
    // Reset to 0 at top of each tick; state code sets it alongside
    // isWalkAnimActive when activating animation. Post-tick clamps to
    // >= kMinAnimPlaySpeed so a barely-moving spider still shows a
    // visible leg cycle (avoids "moving but frozen" uncanny valley).
    // Values:
    //   0.0  = frozen (isWalkAnimActive false OR explicit stop)
    //   0.15 = min "actually moving" floor
    //   1.0  = maximum-speed run/dash animation
    float animMotionRate = 0.0f;
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

// Per-tick dispatch. Reads current state from the per-actor map, calls
// the appropriate TickXxx handler, applies transition changes. Runs
// nothing when the actor's state is PermanentlyDisabled OR when vanilla
// has advanced actionFunc away from the ambient snapshot (deferred to
// vanilla lunge cycle).
//
// Called from Anchor_Enhance_EnSw_Tick (post-hook) each frame for
// enhanced instances. Vanilla actionFunc runs BEFORE this fires; our
// motion writes override vanilla's ambient writes (which don't touch
// world.pos anyway).
void EnSw_EnhancedStateMachine_Tick(EnSw* self, PlayState* play);

// OnInit hook — snapshots the vanilla ambient actionFunc pointer so
// per-tick dispatch can detect when vanilla advances to a non-ambient
// state (lunge cycle etc.) and yield. Called once per actor from
// Anchor_Enhance_EnSw_OnInit for enhanced instances. Safe to call more
// than once; only first call takes effect.
void EnSw_EnhancedStateMachine_SnapshotAmbient(EnSw* self);

// Cleanup entry point. Removes the actor's state block from the
// internal map. Called from actor destroy / scene transition cleanup
// hooks. No-op if the actor was never touched.
void EnSw_EnhancedStateMachine_Forget(EnSw* self);

// Diagnostic accessor — returns the actor's current state for use in
// [EEDiag] output extension (M8). Returns Uninitialized if the actor
// has no state block yet (never ticked).
EnSwState EnSw_EnhancedStateMachine_QueryState(EnSw* self);

// Walk-animation gate — true only when the spider is actively
// translating (wall pursue motion, ground pursue motion, walk-lunge
// dash). Consumed by EnSwDescriptor::OverrideLimbBend to suppress the
// leg-lift cycle when spider is stationary. Returns false if the
// actor has no state block yet (never ticked).
bool EnSw_EnhancedStateMachine_IsWalkAnimActive(EnSw* self);

}  // namespace AnchorEnemyEnhancement
