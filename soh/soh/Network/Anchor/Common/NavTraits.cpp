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
#include "NavCVars.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // NodeFlags / NODE_CLIMB_*

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <unordered_map>

extern "C" {
#include "z64.h"
}

// SoH's "climb any wall" cheat. Lives under gCheats. (NOT
// gEnhancements.ClimbAnywhere — that name doesn't exist; UI label is
// "Climb Anywhere" but the CVar is gCheats.ClimbEverything per
// SohMenuEnhancements.cpp:1738. Same flag is also OR'd into wall-flag
// bitmaps by func_80041DB8 at runtime.)
#define CVAR_CLIMB_EVERYTHING   CVAR_CHEAT("ClimbEverything")

namespace AnchorNav {

bool IsNavSystemEnabled() {
    return AnchorNavCVars::IsMasterEnabled();
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
    t.consumeRoomNavData          = false;  // bosses don't path through the static graph
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
    t.consumeRoomNavData  = false;  // path is host-authoritative waypoints, not static graph
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
    // Peehat, Firefly (Keese): fliers should not snap to ground, and the
    // ground-only RoomNavData graph doesn't represent air paths — Layer 3
    // fallback would point them at a useless walkable subgoal beneath
    // their actual flight path.
    NavTraits t = {};
    t.useGroundFollowing = false;
    t.consumeRoomNavData = false;
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
    //   - Link-rigged: eligible to swim through Underwater nodes.
    //   - Jump distance: 140u — adult Link broad-jump covers ~120u in-game,
    //     but anchors are measured node-to-node and nodes don't sit exactly
    //     at platform rims, so each anchor's dXZ adds ~20-30u of grid slop
    //     beyond the visual jump distance. Field-test (log 59) showed every
    //     anchor at one failing takeoff measured 120-153u dXZ; the prior
    //     110u cap rejected 76% of detected anchors globally. 140u admits
    //     the 120-134u cluster with safety margin against the 150u+ outliers.
    //   - Climb-surface base mask: ladder + vine + designed climb walls
    //     (matches vanilla Link). ResolveDynamicClimbMask promotes this
    //     to include GENERIC_WALL when gEnhancements.ClimbAnywhere is on.
    NavTraits t = {};
    t.eligibleForLeashRespawn = false;
    t.useStickyTargeting      = true;
    t.targetStickyFrames      = 180;
    t.eligibleForSwimming     = true;
    t.leavesTrail             = true;
    t.maxJumpDistance         = 140;
    t.climbSurfaceMask = ::AnchorNavRoom::NODE_CLIMB_LADDER
                       | ::AnchorNavRoom::NODE_CLIMB_VINE
                       | ::AnchorNavRoom::NODE_CLIMB_DESIGNATED_WALL;
    // Drop anchors: follower can route through descents the same way
    // the leader does. 200u matches both detector passes
    // (floor-floor and climb-cell) — child Link's safe-fall threshold.
    t.useDropAnchors          = true;
    t.maxDropDistance         = 200;
    // Jump anchors: follower routes through pre-baked jump pairs.
    // maxJumpDistance (140) already set above caps XZ; maxJumpUpDelta
    // 60 matches adult-Link broad-jump apex.
    t.useJumpAnchors          = true;
    t.maxJumpUpDelta          = 60;
    return t;
}

// Athletic melee — Stalfos, Wolfos. Long stride; commits to charges
// across small gaps.
static NavTraits MakeAthleticMeleeTraits() {
    NavTraits t = {};
    t.maxJumpDistance = 130;
    t.useJumpAnchors  = true;
    t.maxJumpUpDelta  = 70;
    return t;
}

// Heavy / armoured — Iron Knuckle. Slow, plodding; minimal jump
// capability. Should usually fail JumpAcross and divert via PathAround.
static NavTraits MakeHeavyMeleeTraits() {
    NavTraits t = {};
    t.maxJumpDistance = 50;
    return t;
}

// Slow shamblers — Stalchild, Redead. Don't really jump; small step-up
// distance to let them catch low ledges but no broad-jump.
static NavTraits MakeShamblerTraits() {
    NavTraits t = {};
    t.maxJumpDistance = 60;
    return t;
}

// Acrobat — Tektite. Specifically designed to leap on water/lava,
// makes wide hops a natural part of pursuit.
static NavTraits MakeAcrobatTraits() {
    NavTraits t = {};
    t.maxJumpDistance = 200;
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

        // AI Follower. Two actor IDs map to the same traits:
        //   ACTOR_EN_OE2 — DummyPlayer (a REMOTE peer's Link, rendered
        //                  on this client; rare consumer of pathfinding
        //                  but harmless to include).
        //   ACTOR_PLAYER — the LOCAL Link, when AI Follower mode is
        //                  active. HandleStateFollow / RETURN /
        //                  COLLECT_ITEM call ComputePathTo with
        //                  `&player->actor`, whose id is ACTOR_PLAYER
        //                  (= 0), NOT ACTOR_EN_OE2. Without this
        //                  override the lookup falls through to
        //                  kDefaultTraits with climbSurfaceMask=0,
        //                  the BFS gates ALL climb-surface nodes out,
        //                  and BuildAdjacencyList re-injects the P3.8
        //                  base↔top bypass — Stage 6's CLIMBING
        //                  trigger never fires because the path never
        //                  carries a NODE_CLIMB_* subgoal flag.
        //                  Field-test fix from log 27 (2026-05-12).
        { ACTOR_EN_OE2,      MakeFollowerTraits() },
        { ACTOR_PLAYER,      MakeFollowerTraits() },

        // maxJumpDistance overrides per enemy type. Defaults are 100u;
        // overrides are educated guesses pending field-test validation.
        // Athletic melee (130u): wide stride, commits to short charges.
        { ACTOR_EN_TEST,     MakeAthleticMeleeTraits() },  // Stalfos
        { ACTOR_EN_WF,       MakeAthleticMeleeTraits() },  // Wolfos

        // Heavy / armoured (50u): minimal jump.
        { ACTOR_EN_IK,       MakeHeavyMeleeTraits() },     // Iron Knuckle

        // Slow shamblers (60u).
        { ACTOR_EN_SKB,      MakeShamblerTraits() },       // Stalchild
        { ACTOR_EN_RD,       MakeShamblerTraits() },       // ReDead / Gibdo

        // Acrobat (200u): naturally leaps wide gaps.
        { ACTOR_EN_TITE,     MakeAcrobatTraits() },        // Tektite
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

    // NPC Follower (Flotilla — Plans/npc_follower_plan.md). Dynamic
    // actor id assigned at runtime by ActorDB::AddBuiltInCustomActors,
    // so it can't appear in the static `overrides` map. Resolve here:
    // NPC Follower mirrors the player-rigged Follower's traits (Link
    // skel, walks where Link walks, climbs where Link climbs).
    extern s16 gEnFollowerId;
    if (gEnFollowerId != 0 && actorId == gEnFollowerId) {
        static const NavTraits sFollowerNpcTraits = MakeFollowerTraits();
        return sFollowerNpcTraits;
    }

    const auto& overrides = GetOverrides();
    auto it = overrides.find(actorId);
    if (it != overrides.end()) {
        return it->second;
    }

    return kDefaultTraits;
}

::AnchorNavRoom::NavQueryOptions BuildNavQueryOptions(const Actor* navigator) {
    ::AnchorNavRoom::NavQueryOptions opts{};
    if (navigator == nullptr) return opts;
    const NavTraits& traits = GetTraitsForActor(navigator->id);
    opts.eligibleForSwimming = traits.eligibleForSwimming;
    opts.avoidHazardNodes    = traits.avoidHazardNodes;
    opts.climbSurfaceMask    = ResolveDynamicClimbMask(navigator->id, traits.climbSurfaceMask);
    opts.useDropAnchors      = traits.useDropAnchors;
    opts.maxDropDistance     = (float)traits.maxDropDistance;
    opts.useJumpAnchors      = traits.useJumpAnchors;
    opts.maxJumpDistance     = (float)traits.maxJumpDistance;
    opts.maxJumpUpDelta      = (float)traits.maxJumpUpDelta;
    return opts;
}

uint16_t ResolveDynamicClimbMask(s16 actorId, uint16_t baseMask) {
    // Follower inherits Link's runtime cheat — when ClimbEverything is
    // on, adult / child Link can climb any vertical wall, so the
    // follower's mask gains GENERIC_WALL. Cheat off → base mask only.
    //
    // ACTOR_PLAYER alongside ACTOR_EN_OE2 — the LOCAL AI follower's
    // navigator id is ACTOR_PLAYER (Link), not ACTOR_EN_OE2
    // (DummyPlayer). See the override-map comment for full context.
    //
    // Other consumers (none yet wired; future Skullwalltula traits will
    // include GENERIC_WALL in their static base) get baseMask unchanged.
    if (actorId == ACTOR_EN_OE2 || actorId == ACTOR_PLAYER) {
        if (CVarGetInteger(CVAR_CLIMB_EVERYTHING, 0) != 0) {
            return (uint16_t)(baseMask | ::AnchorNavRoom::NODE_CLIMB_GENERIC_WALL);
        }
    }
    return baseMask;
}

} // namespace AnchorNav
