/**
 * NavTraits — per-actor navigation feature flags + master CVar wiring.
 *
 * The navigation system's policy layer. Each navigator (synced enemy,
 * AI Follower, future ally NPC) reads its NavTraits row to decide which
 * nav features apply.
 *
 * Two axes per actor:
 *   - Consumer-side flags: do nav features modify this actor's behaviour?
 *     (useStickyTargeting, useGroundFollowing, eligibleForVerticalTeleport,
 *     eligibleForLeashRespawn). Bosses always have these OFF.
 *   - Emit-side flag: do other actors observe this actor through the
 *     system? (leavesTrail). Bosses HAVE this on so other navigators can
 *     find them.
 *
 * Default-off policy: every nav feature CVar defaults to 0 and stays
 * default-off permanently per Flotilla policy for vanilla-altering
 * features. Master CVar (gEnhancements.Nav.Enabled) is the kill switch.
 *
 * See:
 *   - Claude/Plans/nav_system_implementation_plan.md §4
 *   - GitHub #206 (parent tracker)
 *   - feedback memory: feedback_emit_vs_consume_traits.md
 *   - feedback memory: feedback_bosses_excluded_from_ai_extensions.md
 *   - feedback memory: feedback_vanilla_altering_default_off.md
 */

#pragma once

#include <cstdint>

#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // AnchorNavRoom::NavQueryOptions

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

struct NavTraits {
    // Consumer-side flags — modify the actor's behaviour.
    bool eligibleForVerticalTeleport = true;
    bool eligibleForLeashRespawn     = true;
    bool useGroundFollowing          = true;
    bool useStickyTargeting          = true;

    // Layer 3 (RoomNavData) consumer-side flags. Per
    // Plans/room_nav_data_plan.md §9. All three default to the
    // most-common-case settings for ground-bound non-aquatic
    // hazard-vulnerable navigators.
    //   - consumeRoomNavData: navigator participates in the Layer 3
    //     fallback when LOS and trail both fail. Off for fliers /
    //     waypoint-driven actors / bosses (the ground graph doesn't
    //     model their navigable space).
    //   - eligibleForSwimming: Underwater nodes are valid pass-through
    //     and destination. On for Link-rigged navigators (AI Follower,
    //     NPC Invader); future per-actor opt-in for canonically aquatic
    //     enemies.
    //   - avoidHazardNodes: hazard nodes are limited to kHazardEscapeHops
    //     pass-through and rejected as destinations. Off for heat-/ice-
    //     resistant navigators (none v1).
    bool consumeRoomNavData          = true;
    bool eligibleForSwimming         = false;
    bool avoidHazardNodes            = true;

    // Climb-surface permission mask (schema v7+ /
    // climb_surface_nav_grid_plan Stage 5). Bitmap of NODE_CLIMB_*
    // type bits the navigator may traverse via the wall-grid nodes.
    // 0 = no climb capability (climb edges skipped during BFS
    // expansion; equivalent to the pre-v7 behaviour of "ground only").
    //
    // This is the BASE mask. ResolveDynamicClimbMask layers session-
    // dependent bits on top (e.g. follower picks up GENERIC_WALL when
    // gEnhancements.ClimbAnywhere is on). ComputePathTo calls the
    // resolver before BFS and stashes the snapshot into
    // NavPath::computedClimbMask so the consumer can re-AND at
    // engage time.
    //
    // Default 0 — consumer-side opt-in per actor. Bosses leave this
    // zero (kBossDefaults); kDefaultTraits leaves it zero (most
    // ground enemies don't climb walls).
    uint16_t climbSurfaceMask        = 0;

    // Drop-anchor consumer flag. When true, this actor's BFS adjacency
    // list gains directed highPos→landingPos edges from the room's
    // DropAnchor table (RoomNavData.cpp DetectDropAnchors). The
    // navigator can plan descent routes — e.g. follow a leader who
    // jumped off a cliff via the same drop anchor the leader used.
    // Anchors with dy > `maxDropDistance` are skipped (per-actor cap
    // for fragile / heavy navigators).
    //
    // Default false — opt-in per actor. Bosses leave this off
    // (kBossDefaults). Follower (MakeFollowerTraits) turns it on.
    // Drop edges are DIRECTED: high→low only; ascent requires a
    // separate climb or ground path.
    bool useDropAnchors              = false;
    uint16_t maxDropDistance         = 200;  // max safe Y delta (units); matches kDropMaxDeltaY default

    // Jump-anchor consumer flag (Plans/jump_anchor_plan.md). When true,
    // this actor's BFS adjacency list gains bidirectional edges from
    // the room's JumpAnchor table — pre-baked, arc-clearance validated
    // traversal pairs. Per-actor caps filter at BFS time:
    //   - `maxJumpDistance` (existing field): horizontal XZ cap
    //   - `maxJumpUpDelta` (new): upward Y reach cap
    // Downward Y is bounded by the scanner (`kJumpDownMax = 200u`).
    //
    // Default false — opt-in per actor. Bosses leave this off
    // (kBossDefaults). Follower turns it on.
    bool useJumpAnchors              = false;
    uint16_t maxJumpUpDelta          = 50;   // upward reach cap (child Link ~50, adult ~70)

    // Emit-side flag — other actors observe this actor through the trail.
    bool leavesTrail                 = true;

    // Tunables.
    uint16_t targetStickyFrames        = 120;  // 2s at 60fps; 0 disables stickiness for this actor
    uint16_t trailLagFrames            = 30;   // smoothing window when reading from trail
    uint16_t verticalTeleportYThreshold  = 80; // require |Δy| > this before teleport considered
    uint16_t verticalTeleportDelayFrames = 90; // require mismatch to persist this long
    uint16_t approachRange             = 80;   // distance at which navigator stops closing on target
    uint16_t maxJumpDistance           = 100;  // horizontal jump cap (units) — used by JumpResolver
                                                // to decide whether a navigator can clear a void
                                                // ahead. Adult-Link broad-jump distance is ~120u;
                                                // most enemies can manage less. Per-actor override
                                                // for athletic / sluggish navigators.
};

// Returns the traits row for this actor. If IsSyncedBossActor(actorId) is
// true, returns kBossDefaults (every consumer flag false; leavesTrail still
// true so other navigators can locate bosses). Otherwise checks the
// per-actor override map; falls through to kDefaultTraits if no entry.
const NavTraits& GetTraitsForActor(s16 actorId);

// Resolve the dynamic (session-dependent) climb-surface mask for this
// actor. Starts from the static `baseMask` (typically traits.climbSurfaceMask)
// and overlays bits that depend on runtime state — currently only the
// follower's gEnhancements.ClimbAnywhere CVar promoting GENERIC_WALL on top
// of the base LADDER|VINE|DESIGNATED_WALL. For all other actors returns
// `baseMask` unchanged.
//
// Call site contract: ComputePathTo (and the future climb-engagement
// check) calls this immediately before passing the mask to BFS, then
// stashes the result into NavPath::computedClimbMask so the consumer
// can re-AND defensively at engage time. Mid-session CVar flips don't
// invalidate in-flight paths automatically; the consumer either accepts
// the stale mask until the path is exhausted, or recomputes on its own
// schedule. (Plan open question Q8.)
uint16_t ResolveDynamicClimbMask(s16 actorId, uint16_t baseMask);

// Build a BFS NavQueryOptions from the navigator's traits row, including
// the dynamic climb-mask resolution (ClimbEverything cheat etc.). Returns
// defaulted options when `navigator` is null. Callers that need
// non-defaulted climb mask in a sub-component (e.g. a non-pathing BFS
// helper) can overwrite the field afterwards; ComputePathTo additionally
// stashes the resolved mask onto NavPath::computedClimbMask so consumers
// can re-AND at engage time.
::AnchorNavRoom::NavQueryOptions BuildNavQueryOptions(const Actor* navigator);

// Master gate for the entire navigation system. Every per-feature query
// must check this AND its own CVar. Default off; ships and stays off
// permanently.
bool IsNavSystemEnabled();

} // namespace AnchorNav
