/**
 * DistanceMath — small inline helpers for actor-to-actor distance math.
 *
 * The Anchor codebase historically grew a half-dozen private `DistSq`
 * lambdas (3D) plus an unrelated cluster of inline `sqrtf(SQ(dx) + SQ(dz))`
 * (XZ) sites. Same call shape, two different semantics. Call sites that
 * needed XZ were often written 3D-style and vice versa, which is the
 * exact bug class this header exists to remove.
 *
 * The rule when reading code that uses these:
 *   - `*XZ*`  — horizontal plane only. Use for AI gates that should
 *               ignore vertical separation (e.g. follow-distance, ladder
 *               proximity, enemy targeting where Y is bob/animation).
 *   - `*3D*`  — full Euclidean. Use when Y separation is meaningful
 *               (e.g. leash distance, jump-down candidate ranking).
 *
 * Compare-against-radius work should use the `*Sq` variants and square
 * the radius once — same pattern as `actor->xyzDistToPlayerSq`.
 */
#pragma once

#include <cmath>

extern "C" {
#include "z64math.h"
}

namespace AnchorDist {

inline float DistXZSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

inline float DistXZ(const Vec3f& a, const Vec3f& b) {
    return std::sqrt(DistXZSq(a, b));
}

inline float Dist3DSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

inline float Dist3D(const Vec3f& a, const Vec3f& b) {
    return std::sqrt(Dist3DSq(a, b));
}

}  // namespace AnchorDist
