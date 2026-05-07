/**
 * NavTraits — implementation.
 *
 * Three traits rows:
 *   - kDefaultTraits: every feature on. For generic ground enemies.
 *   - kBossDefaults: consumer-side off, leavesTrail on. For any actor
 *     matching IsSyncedBossActor.
 *   - Per-actor override map for non-boss actors with specific tuning
 *     (Goroiwa, Karebaba, Dekubaba, etc.).
 *
 * GetTraitsForActor dispatches: boss → kBossDefaults; per-actor override
 * present → that row; else → kDefaultTraits.
 */

#include "NavTraits.h"
#include "ActorSyncHelpers.h"  // IsSyncedBossActor

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <unordered_map>

extern "C" {
#include "z64.h"
}

#define CVAR_NAV_ENABLED CVAR_ENHANCEMENT("Nav.Enabled")

namespace AnchorNav {

bool IsNavSystemEnabled() {
    return CVarGetInteger(CVAR_NAV_ENABLED, 0) != 0;
}

// ---------------------------------------------------------------------------
// Default traits — generic ground enemy. All features on.
// ---------------------------------------------------------------------------

static const NavTraits kDefaultTraits = {};  // struct defaults match plan §4

// ---------------------------------------------------------------------------
// Boss defaults — consumer-side OFF (bosses are hand-tuned, fragile, and
// must not be touched by the nav system). leavesTrail STAYS ON so other
// navigators can locate bosses. See feedback_emit_vs_consume_traits.md.
// ---------------------------------------------------------------------------

static const NavTraits kBossDefaults = []() {
    NavTraits t = {};
    t.eligibleForVerticalTeleport = false;
    t.eligibleForLeashRespawn     = false;
    t.useGroundFollowing          = false;
    t.useStickyTargeting          = false;
    t.targetStickyFrames          = 0;
    t.leavesTrail                 = true;  // emit-side stays on
    return t;
}();

// ---------------------------------------------------------------------------
// Per-actor override map — non-boss actors that need specific tuning.
// Keep this list narrow; defaults are the right answer for most actors.
// Plan §4 enumerates the initial entries.
// ---------------------------------------------------------------------------

static NavTraits MakeGoroiwaTraits() {
    NavTraits t = {};
    t.useStickyTargeting  = false;  // waypoint-driven path; steering must match host's track
    t.useGroundFollowing  = false;
    return t;
}

static NavTraits MakeNoVerticalTeleportTraits() {
    // Karebaba, Dekubaba, Vali (Bari), TP (Tailpasaran): stem-anchored or
    // animation-driven world.pos that would orphan if teleported.
    NavTraits t = {};
    t.eligibleForVerticalTeleport = false;
    return t;
}

static NavTraits MakeFlierTraits() {
    // Peehat, Firefly (Keese): fliers should not snap to ground.
    NavTraits t = {};
    t.useGroundFollowing = false;
    return t;
}

static NavTraits MakeFreezardTraits() {
    // Slow turn rate; longer commit reduces wagging.
    NavTraits t = {};
    t.useStickyTargeting = true;
    t.targetStickyFrames = 240;
    return t;
}

static NavTraits MakeFollowerTraits() {
    // AI Follower (ACTOR_EN_OE2 = DummyPlayer-derived):
    //   - Follower owns its own leash + state machine; LeashRespawn is
    //     enemy-only by design.
    //   - Sticky targeting helps follower commit to a leader / enemy
    //     across frames.
    //   - Trail enabled so other navigators can chase a follower if needed.
    NavTraits t = {};
    t.eligibleForLeashRespawn = false;
    t.useStickyTargeting      = true;
    t.targetStickyFrames      = 180;
    t.leavesTrail             = true;
    return t;
}

static const std::unordered_map<s16, NavTraits>& GetOverrides() {
    static const std::unordered_map<s16, NavTraits> kOverrides = {
        // Goroiwa-class: waypoint-driven; steering and stickiness must match host.
        { ACTOR_EN_GOROIWA, MakeGoroiwaTraits() },

        // Animation-anchored enemies (no vertical teleport).
        { ACTOR_EN_KAREBABA, MakeNoVerticalTeleportTraits() },
        { ACTOR_EN_DEKUBABA, MakeNoVerticalTeleportTraits() },
        { ACTOR_EN_VALI,     MakeNoVerticalTeleportTraits() },  // Bari (Big Jellyfish)
        { ACTOR_EN_TP,       MakeNoVerticalTeleportTraits() },  // Tailpasaran

        // Fliers (no ground following).
        { ACTOR_EN_PEEHAT,   MakeFlierTraits() },
        { ACTOR_EN_FIREFLY,  MakeFlierTraits() },  // Keese

        // Freezard — slow turn rate; longer commit.
        { ACTOR_EN_FZ,       MakeFreezardTraits() },

        // AI Follower (DummyPlayer-derived).
        { ACTOR_EN_OE2,      MakeFollowerTraits() },
    };
    return kOverrides;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

const NavTraits& GetTraitsForActor(s16 actorId) {
    // Boss check first — IsSyncedBossActor is the canonical predicate
    // (Common/ActorSyncHelpers.h:106). Bosses categorically use
    // kBossDefaults regardless of any override that might be present.
    if (IsSyncedBossActor(actorId)) {
        return kBossDefaults;
    }

    const auto& overrides = GetOverrides();
    auto it = overrides.find(actorId);
    if (it != overrides.end()) {
        return it->second;
    }

    return kDefaultTraits;
}

} // namespace AnchorNav
