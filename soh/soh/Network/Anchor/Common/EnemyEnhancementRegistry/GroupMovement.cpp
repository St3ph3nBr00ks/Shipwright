/**
 * GroupMovement.cpp — see GroupMovement.h for API documentation and
 * Plans/group_movement_helper_plan.md for design rationale.
 *
 * Phase 1: separation only. Iterates caller-supplied actor categories
 * (or the full kSyncableActorCategories set as default), applies the
 * caller's predicate + helper-internal filters (self / dying),
 * accumulates 1/d² repulsion vectors from neighbors within
 * cfg.neighborRadius.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates
// are declared in C++ linkage before any extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "GroupMovement.h"

// kSyncableActorCategories default when caller passes categories=nullptr.
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"

#include <cmath>

namespace AnchorGroupMovement {

// -------------------------------------------------------------------
// Predicate helpers
// -------------------------------------------------------------------

bool IsSameActorId(const Actor* self, const Actor* candidate) {
    if (self == nullptr || candidate == nullptr) return false;
    return self->id == candidate->id;
}

bool IsAnyOtherActor(const Actor* self, const Actor* candidate) {
    if (self == nullptr || candidate == nullptr) return false;
    return self != candidate;
}

// -------------------------------------------------------------------
// ComputeSeparation
// -------------------------------------------------------------------

void ComputeSeparation(const Actor*             self,
                       PlayState*                play,
                       const u8*                 categories,
                       int                       categoryCount,
                       NeighborPredicate         predicate,
                       const SeparationConfig&   cfg,
                       Vec3f*                    outSeparation) {
    if (outSeparation == nullptr) return;
    outSeparation->x = 0.0f;
    outSeparation->y = 0.0f;
    outSeparation->z = 0.0f;

    if (self == nullptr || play == nullptr) return;

    // Default to the full synced-actor category set when caller
    // passes nullptr. This is the "any other enemy in scene" flocking
    // semantic per Plans §8 Q3 (Zelda has no meaningful enemy-vs-enemy
    // hostility → any enemy avoids overlapping any other enemy).
    const u8* catList  = (categories != nullptr) ? categories
                                                  : kSyncableActorCategories;
    const int catCount = (categories != nullptr)
                            ? categoryCount
                            : (int)kSyncableActorCategoriesCount;

    const float r2       = cfg.neighborRadius * cfg.neighborRadius;
    const float minDist2 = cfg.minDistance   * cfg.minDistance;

    // Walk each category's live actor list, apply filters, accumulate
    // repulsion. O(n) per category — total O(sum of category sizes)
    // per call. Typical scene: ≤10 same-type enemies + ≤20 total in
    // synced categories → tens of iterations per tick. Trivial cost.
    for (int ci = 0; ci < catCount; ci++) {
        const u8 cat = catList[ci];
        Actor* iter  = play->actorCtx.actorLists[cat].head;
        while (iter != nullptr) {
            Actor* const nextIter = iter->next;  // capture up front
                                                  // (iter may unlink
                                                  // itself during
                                                  // predicate calls in
                                                  // theory)

            if (iter == self) { iter = nextIter; continue; }
            if (iter->update == nullptr) {
                // Dead / dying (post Actor_Kill, pre Actor_Delete).
                // Universally correct to skip — helper-internal filter.
                iter = nextIter;
                continue;
            }
            if (predicate != nullptr && !predicate(self, iter)) {
                iter = nextIter;
                continue;
            }

            const float dx = self->world.pos.x - iter->world.pos.x;
            const float dy = self->world.pos.y - iter->world.pos.y;
            const float dz = self->world.pos.z - iter->world.pos.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2 || d2 < 0.0001f) {  // out of range OR exact overlap
                iter = nextIter;
                continue;
            }

            // Force magnitude ~ 1/d². Clamp denominator at minDist² so
            // near-overlap doesn't blow up. Direction is away from
            // neighbor.
            const float denom = (d2 < minDist2) ? minDist2 : d2;
            const float mag   = cfg.weight / denom;

            // Unit direction (self←iter). Normalize by actual d
            // (not clamped) so direction is correct even inside the
            // minDistance zone.
            const float d = std::sqrt(d2);
            outSeparation->x += (dx / d) * mag;
            outSeparation->y += (dy / d) * mag;
            outSeparation->z += (dz / d) * mag;

            iter = nextIter;
        }
    }

    // Optional surface projection — subtract normal-parallel component
    // so the separation vector lies in the surface's tangent plane.
    // Wall spider needs this or separation would push it off the wall.
    if (cfg.projectToSurface) {
        const float ndot = outSeparation->x * cfg.surfaceNormal.x +
                            outSeparation->y * cfg.surfaceNormal.y +
                            outSeparation->z * cfg.surfaceNormal.z;
        outSeparation->x -= cfg.surfaceNormal.x * ndot;
        outSeparation->y -= cfg.surfaceNormal.y * ndot;
        outSeparation->z -= cfg.surfaceNormal.z * ndot;
    }

    // TODO(#307): if field-test shows tight clusters producing
    // over-steering (e.g., spiders orbiting Link because separation
    // magnitude overwhelms pursuit force), add SeparationConfig::maxForce
    // and clamp outSeparation magnitude before return. Deferred YAGNI
    // per plan §8 Q5.
}

}  // namespace AnchorGroupMovement
