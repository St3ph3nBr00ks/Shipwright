/**
 * GroundFollow — shared helper for enemy-enhancement actors that need
 * to stick to the ground surface while translating in XZ.
 *
 * Extracted 2026-07-31 from En_Sw's TickGroundPursue (see
 * EnSwStateMachine.cpp:1389-1425 for the original pattern). Second
 * consumer is Dekubaba's DetachedSquirm state (Feature B, #309) —
 * the extraction rule (2-consumer point) triggers.
 *
 * API:
 *   - Probe(actor, play, ...)          → non-mutating raycast; returns
 *                                        floor Y + normal
 *   - ProbeAndSnap(actor, play, ...)   → probes then writes actor.pos.y
 *                                        (convenience for the common
 *                                        "snap to floor each tick" case)
 *
 * Both cast a ray from `actor.pos + (0, +5, 0)` downward by
 * `probeDepth` (default 200u — matches En_Sw's kGroundFollowProbe).
 * The +5u start-Y prevents self-intersection when actor is already
 * flush on ground.
 *
 * Floor test: `BgCheck_EntityLineTest1` for ground-only, with a
 * normal.y threshold (default 0.5f = ~60° from vertical) to reject
 * near-vertical polys the raycast might hit if actor is teetering on
 * a steep slope.
 */

#pragma once

#include "soh/Network/Anchor/Anchor.h"  // Pitfall 40

extern "C" {
#include "z64.h"
#include "macros.h"      // COLPOLY_GET_NORMAL
#include "functions.h"   // BgCheck_EntityLineTest1
}

namespace AnchorEnemyEnhancement {
namespace GroundFollow {

// Same threshold En_Sw uses (kWallNormalYThreshold). Polys with
// normal.y above this count as walkable ground; below, treated as
// wall / steep slope.
inline constexpr float kDefaultNormalYThreshold = 0.5f;
inline constexpr float kDefaultProbeDepth       = 200.0f;
inline constexpr float kDefaultStartYOffset     = 5.0f;

struct Result {
    bool          onFloor      = false;
    float         floorY       = 0.0f;
    Vec3f         floorNormal  = { 0.0f, 1.0f, 0.0f };
    CollisionPoly* floorPoly   = nullptr;
    s32           floorBgId    = 0;
};

// Non-mutating raycast — probes for ground under the actor and
// returns the hit info. Caller decides what to do (snap Y, transition
// state on no-floor, apply slope-aware orientation, etc.).
inline Result Probe(Actor* actor, PlayState* play,
                     float probeDepth       = kDefaultProbeDepth,
                     float normalYThreshold = kDefaultNormalYThreshold) {
    Result r;
    if (actor == nullptr || play == nullptr) return r;

    Vec3f from = actor->world.pos;
    from.y += kDefaultStartYOffset;
    Vec3f to = {
        actor->world.pos.x,
        actor->world.pos.y - probeDepth,
        actor->world.pos.z,
    };
    Vec3f hit = { 0.0f, 0.0f, 0.0f };
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hit, &poly,
                                 1, 1, 1, 0, &bgId) && poly != nullptr) {
        const float ny = COLPOLY_GET_NORMAL(poly->normal.y);
        if (ny > normalYThreshold) {
            r.onFloor     = true;
            r.floorY      = hit.y;
            r.floorNormal = { COLPOLY_GET_NORMAL(poly->normal.x), ny,
                              COLPOLY_GET_NORMAL(poly->normal.z) };
            r.floorPoly   = poly;
            r.floorBgId   = bgId;
        }
    }
    return r;
}

// Convenience — probes then writes `actor.world.pos.y = floorY + bodyOffset`
// when a floor was hit. Returns true iff the snap happened. Caller can
// use the return to decide airborne-recovery behavior (stun, fall,
// transition state, etc.).
inline bool ProbeAndSnap(Actor* actor, PlayState* play,
                          float bodyOffset       = 0.0f,
                          float probeDepth       = kDefaultProbeDepth,
                          float normalYThreshold = kDefaultNormalYThreshold) {
    if (actor == nullptr) return false;
    Result r = Probe(actor, play, probeDepth, normalYThreshold);
    if (r.onFloor) {
        actor->world.pos.y = r.floorY + bodyOffset;
    }
    return r.onFloor;
}

}  // namespace GroundFollow
}  // namespace AnchorEnemyEnhancement
