/**
 * StuckRecovery — shared TickSTUCK dispatch for scripted-position AI
 * actors (NPC Follower, NPC Invader; future hostile / friendly actors
 * with substrate paths).
 *
 * The NPC Follower and NPC Invader TickSTUCK functions had drifted into
 * near-duplicate copies of the same three-tier recovery body:
 *
 *   Cycle 1 (default)             — yaw-nudge toward target + path reset
 *   Cycle 2 (default)             — cursor advance + yaw-nudge (path kept)
 *   Cycle 3+ (default)            — teleport to current subgoal, or
 *                                    fallback dest if path is empty
 *   Cycle 1+ (vertical-dominant)  — see GetStuckAction overload doc
 *
 * What this helper owns:
 *   - Vertical-dominant predicate (IsVerticalDominantSeparation).
 *   - StuckCycleAction dispatch via GetStuckAction overload.
 *   - Cursor-advance edge-triggering + MarkCursorAdvanced.
 *   - Nudge math (yaw + Math_SinS/CosS × nudgeDist write to world.pos).
 *   - Cycle-1 path reset (cycle 2 keeps the cursor-advanced path).
 *   - stuckCheckPos / lastStuckCheckFrame refresh.
 *   - Diagnostic SPDLOG_INFO for each tier.
 *
 * What the caller still owns:
 *   - The actor's state-machine enum write (FOLLOW / IDLE — the helper
 *     can't know which enum constant to set).
 *   - Pre-check early-outs (e.g. Invader's target-lost → IDLE).
 *   - The teleport mechanism: the consumer supplies a callback that
 *     writes world.pos + does the BG snap + handles its own path
 *     reset and any cross-scene work (FollowerNPC's TeleportNpcTo
 *     wraps cross-scene; Invader does inline pos+bgcheck).
 *
 * After RunStuckRecoveryStep returns:
 *   - If `outTeleported == true`: helper fired the cycle 3+ teleport
 *     via the caller's callback. Caller should set state to FOLLOW
 *     and return (no further work in TickSTUCK).
 *   - If `outTeleported == false`: helper did cursor-advance (cycle 2)
 *     and/or nudge (all cycles below threshold). Caller should set
 *     state to FOLLOW and return.
 *
 * The two are functionally identical for the dispatcher — caller's
 * post-helper code path is the same. The flag is exposed for
 * diagnostics / future per-tier extensions.
 */

#pragma once

#include <cstdint>

#include "StuckEscalation.h"
#include "NavOrDirect.h"  // AnchorAI::NavState

extern "C" {
#include "z64.h"  // Actor, PlayState, Vec3f
}

namespace AnchorAI {

// Caller-provided teleport callback. The helper invokes this for the
// cycle 3+ teleport tier. Implementation should perform the entire
// teleport — world.pos write, BG-check snap, speedXZ zero, AND any
// path reset the caller wants. The helper does NOT reset the path
// itself for the teleport tier because consumers' teleport semantics
// differ (e.g. NPC Follower's TeleportNpcTo wraps cross-scene
// handling; NPC Invader does inline pos + Actor_UpdateBgCheckInfo).
//
// `reason` is "next subgoal" when the dest is the path's current
// waypoint, "fallback (path empty)" when it's the caller-supplied
// fallback target. Useful for log strings inside the callback.
using StuckTeleportFn = void (*)(void* user,
                                  Actor* actor,
                                  PlayState* play,
                                  const Vec3f& dest,
                                  const char* reason);

struct StuckRecoveryConfig {
    StuckCycleState&  cycle;
    AnchorAI::NavState& nav;
    Vec3f&            stuckCheckPos;
    uint64_t&         lastStuckCheckFrame;
    uint64_t          curFrame;        // Anchor::gameFrameCounter snapshot
    int               escalationThreshold;
    float             nudgeDist;
    const char*       logPrefix;       // "FollowerNPC", "Invader"
    StuckTeleportFn   teleportFn;
    void*             teleportUser;
};

// Returns true when the cycle 3+ teleport tier fired (caller's
// teleportFn was invoked). Returns false otherwise (cursor-advance
// and/or nudge fired). In both cases the caller should subsequently
// set its state-machine enum to FOLLOW.
bool RunStuckRecoveryStep(Actor* actor,
                          const Vec3f& targetOrLeaderPos,
                          PlayState* play,
                          StuckRecoveryConfig& cfg);

}  // namespace AnchorAI
