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

// Master gate for the entire navigation system. Every per-feature query
// must check this AND its own CVar. Default off; ships and stays off
// permanently.
bool IsNavSystemEnabled();

} // namespace AnchorNav
