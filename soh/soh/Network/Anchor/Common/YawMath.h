/**
 * YawMath — small inline helpers for actor-to-actor angle math.
 *
 * Sibling of DistanceMath.h. Kept separate so that header's "distance
 * math only" semantic invariant (see DistanceMath.h preamble) isn't
 * muddied by angle additions, and so future angle helpers (pitch-
 * toward, sweep-arc, facing predicates) have a natural home that
 * doesn't cross the distance/yaw boundary.
 *
 * Tier 1 refactor (2026-06-04) — extracted from NPC Follower / NPC
 * Invader, where byte-identical inline YawTowardTarget definitions
 * had been duplicated. See Plans/npc_helpers_tier1_extract_2026-06-03.md.
 */
#pragma once

extern "C" {
#include "z64math.h"   // Vec3f
#include "functions.h" // Math_Atan2S
}

namespace AnchorYaw {

// Yaw (s16 binary angle) toward target XZ from observer position.
// Vanilla `Math_Atan2S(dz, dx)` convention — Z first, X second. Y is
// ignored; this is a pure XZ-plane facing computation.
inline s16 YawTowardTarget(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

}  // namespace AnchorYaw
