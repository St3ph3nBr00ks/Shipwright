/**
 * EnSwWebBridge — extern "C" call sites for the En_Sw_Web projectile
 * (GH #333, ovl_En_Sw_Web). z_en_sw_web.c calls two entry points here
 * via plain `extern` forward-decls (Pitfall 7).
 *
 * Phase 2 (this file at commit time): stub bodies. Host check returns
 * true when local client is the current room host so projectile
 * despawn logic + placeholder logging fires. DetectAndApplyHit does
 * a proximity check against all synced players and returns true on
 * hit but does NOT broadcast a STUN_APPLIED packet yet — the packet
 * family lands in Phase 4b.
 *
 * See GH #333 comment 5209793604 for the design brief (A1-A15).
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates land
// in C++ linkage before overlay headers open their extern "C" blocks.
#include "soh/Network/Anchor/Anchor.h"

#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Sw_Web/z_en_sw_web.h"
}

#include <cmath>

// Host predicate for the projectile's hit-detection gate. Per A13,
// only host runs the hit check + broadcasts STUN_APPLIED; peer
// replicas are passive (motion only) so the stun state has a single
// source of truth.
extern "C" int Anchor_Enhance_EnSwWeb_IsHost(void) {
    return SceneAuthority::IsMyCurrentRoomHost() ? 1 : 0;
}

// Proximity-based hit check (Phase 2 body — no packet broadcast yet).
// Iterates synced player actors, checks 3D distance to the projectile,
// returns 1 if any player is within hitRadius. Phase 4b extends to
// resolve the victim clientId and dispatch STUN_APPLIED.
extern "C" int Anchor_Enhance_EnSwWeb_DetectAndApplyHit(Actor* projectile,
                                                          PlayState* play,
                                                          f32 hitRadius) {
    if (projectile == nullptr || play == nullptr) return 0;

    Actor* nearest = FindNearestPlayerActor(projectile, play);
    if (nearest == nullptr) return 0;

    const f32 dx = nearest->world.pos.x - projectile->world.pos.x;
    const f32 dy = nearest->world.pos.y - projectile->world.pos.y;
    const f32 dz = nearest->world.pos.z - projectile->world.pos.z;
    const f32 dist2 = dx * dx + dy * dy + dz * dz;
    const f32 r2    = hitRadius * hitRadius;

    if (dist2 > r2) return 0;

    // Phase 4b will:
    //   1. Resolve nearest -> victimClientId (own vs DummyPlayer via
    //      Anchor::GetDummyPlayerClientId).
    //   2. Check if that victim is already stunned (A15 no multi-stack).
    //   3. Broadcast STUN_APPLIED{victimClientId} and apply locally.
    // Phase 2 stub: return true so the projectile despawns on hit.
    // No stun applied yet — the state infrastructure doesn't exist.
    return 1;
}
