/**
 * GroupMovement — separation steering helper for enemy groups.
 *
 * Boids-derived (Reynolds 1987) separation rule extracted as a shared
 * helper so multiple enhanced-enemy consumers avoid re-implementing
 * neighbor scanning + 1/d² repulsion math. Reference:
 *   Reynolds, C. (1987). Flocks, herds and schools: A distributed
 *   behavioral model. SIGGRAPH '87.
 *
 * Design goal (per Plans/group_movement_helper_plan.md): maximum
 * behavior in the helper, per-consumer values (radii, weights,
 * surface flags) supplied via SeparationConfig. Adding a new
 * consumer is one config-struct + one call site.
 *
 * Phase 1 scope: separation only. Alignment / cohesion (schooling)
 * and proactive obstacle avoidance are deferred (§7 of plan doc).
 *
 * Threading + entity-agnosticism:
 *   - Runs on the game thread (called from per-actor Tick functions).
 *   - Stateless — no per-actor storage inside the helper. Each call
 *     is self-contained; iterates PlayState's actor lists live.
 *   - Actor-agnostic — helper reads only actor.world.pos and consults
 *     a caller-supplied predicate for filtering. No per-actor-type
 *     knowledge.
 */

#pragma once

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before the extern "C" block below opens.
#include "soh/Network/Anchor/Anchor.h"

extern "C" {
#include "z64.h"       // Actor, PlayState, ActorContext
#include "z64actor.h"  // Vec3f
}

namespace AnchorGroupMovement {

// -------------------------------------------------------------------
// Per-consumer tuning config
// -------------------------------------------------------------------
// All values in game world units unless noted. Consumers declare
// their own file-static constexpr instances of this struct (e.g.
// kEnSwGroundSepCfg) and pass to ComputeSeparation.
struct SeparationConfig {
    // Neighbors within this radius contribute to the separation
    // force. Beyond this, ignored (no long-range crowding).
    float neighborRadius = 60.0f;

    // Force falls off as 1/distance². Weight scales the summed
    // total; higher weight = stronger push apart. Tune against
    // pursuit force so separation dominates at contact but not at
    // range.
    float weight = 1.0f;

    // Clamp for divide-by-zero + infinite-force protection on
    // exactly-overlapping neighbors. Below this distance, force
    // magnitude is computed as if distance = minDistance (still
    // repels, but bounded).
    float minDistance = 5.0f;

    // If true, projects the resulting separation vector onto the
    // surface tangent plane (dot-product-subtract). Wall spider
    // needs this — separation force must stay along the wall's
    // surface, not push the spider off the wall. surfaceNormal
    // supplies the plane definition.
    bool  projectToSurface = false;
    Vec3f surfaceNormal    = {0.0f, 1.0f, 0.0f};  // world +Y = flat floor default
};

// -------------------------------------------------------------------
// Neighbor filtering predicate
// -------------------------------------------------------------------
// Called for every actor discovered in the category scan. Return true
// to include the candidate in the separation sum, false to skip.
// The helper itself always excludes: self, actors with update==nullptr
// (dead / dying pending Actor_Delete), actors beyond neighborRadius.
// Predicate handles anything more specific (actor-class filters etc.).
using NeighborPredicate = bool (*)(const Actor* self,
                                    const Actor* candidate);

// Prebuilt predicates for consumer convenience.

// Include only actors of the same actor id as self. Use when the
// consumer wants strict same-type flocking (schooling behavior).
bool IsSameActorId(const Actor* self, const Actor* candidate);

// Include any actor other than self, with no id/category filtering
// beyond what the helper already applies. This is the default in
// Zelda-style games where enemies don't fight each other — every
// enemy avoids overlapping every other enemy.
bool IsAnyOtherActor(const Actor* self, const Actor* candidate);

// -------------------------------------------------------------------
// Main entry point
// -------------------------------------------------------------------
// Computes a separation steering vector for `self` given its
// neighbors. Iterates the actor lists at `categories[0..categoryCount)`
// (each entry is an ACTORCAT_* value), applies the helper's internal
// filters + the caller's predicate, and sums 1/d² repulsion vectors
// from all remaining neighbors within cfg.neighborRadius.
//
// Returned vector is in world coordinates. Zero vector when no
// neighbors survive filtering. Caller typically adds this to their
// pursuit direction and re-normalizes, e.g.:
//
//     Vec3f pursuit = <existing per-tick pursuit direction>;
//     Vec3f sep;
//     AnchorGroupMovement::ComputeSeparation(
//         &self->actor, play,
//         kMyCategories, kMyCategoryCount,
//         AnchorGroupMovement::IsAnyOtherActor,
//         kMySepCfg, &sep);
//     Vec3f steer = {
//         pursuit.x + sep.x, pursuit.y + sep.y, pursuit.z + sep.z
//     };
//     // ... normalize + apply speed ...
//
// If categories == nullptr OR categoryCount == 0, the helper walks
// all synced world-actor categories from Anchor's shared
// kSyncableActorCategories set (covers ENEMY / BOSS / PROP / BG /
// NPC / SWITCH / ITEMACTION / MISC).
void ComputeSeparation(const Actor*             self,
                       PlayState*                play,
                       const u8*                 categories,
                       int                       categoryCount,
                       NeighborPredicate         predicate,
                       const SeparationConfig&   cfg,
                       Vec3f*                    outSeparation);

}  // namespace AnchorGroupMovement
