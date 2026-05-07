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

namespace Anchor::Nav {

struct NavTraits {
    // Consumer-side flags — modify the actor's behaviour.
    bool eligibleForVerticalTeleport = true;
    bool eligibleForLeashRespawn     = true;
    bool useGroundFollowing          = true;
    bool useStickyTargeting          = true;

    // Emit-side flag — other actors observe this actor through the trail.
    bool leavesTrail                 = true;

    // Tunables.
    uint16_t targetStickyFrames        = 120;  // 2s at 60fps; 0 disables stickiness for this actor
    uint16_t trailLagFrames            = 30;   // smoothing window when reading from trail
    uint16_t verticalTeleportYThreshold  = 80; // require |Δy| > this before teleport considered
    uint16_t verticalTeleportDelayFrames = 90; // require mismatch to persist this long
    uint16_t approachRange             = 80;   // distance at which navigator stops closing on target
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

} // namespace Anchor::Nav
