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

// Phase 3 stub — always returns false (nobody is ever stunned yet).
// Phase 4a lands the PlayerStunState sidecar map on Anchor; Phase 4b
// wires this to check that map by resolving actor→clientId first.
// Used by EnSwStateMachine::TryEnterWebAttack per A15 (no-multi-stack:
// don't fire web at already-stunned target).
extern "C" int Anchor_Enhance_EnSwWeb_IsPlayerStunned(Actor* player) {
    (void)player;
    return 0;  // Phase 3: nobody's stunned yet; always allow fire.
}

// Fire helper — spawns the En_Sw_Web projectile locally on host.
// Phase 3 body: local Actor_Spawn only. Phase 4b adds
// EN_SW_WEB_FIRED packet broadcast so peers spawn their own copy
// deterministically (mirrors Dekubaba acid MP pattern per
// z_en_dekubaba_acid.c file header note).
//
// Aim yaw encoded into params via /8 quantization (params is s16,
// aimYaw is s16 — full range fits at 1:8 ratio).
extern "C" void Anchor_Enhance_EnSwWeb_FireProjectile(Actor* spider,
                                                       PlayState* play,
                                                       s16 aimYaw) {
    if (spider == nullptr || play == nullptr) return;

    // Only host spawns (A13). Peer will spawn its own copy when the
    // EN_SW_WEB_FIRED packet lands (Phase 4b).
    if (!SceneAuthority::IsMyCurrentRoomHost()) return;

    // Spawn origin: slightly forward from the spider's rear along
    // the aim direction (rear is aimed at target during wind-up per
    // TickWebAttack, so aim direction = forward from rear = toward
    // target). Offset ~15u forward so the projectile doesn't
    // immediately overlap the spider's own body.
    constexpr float kSpawnForwardOffset = 15.0f;
    const float sx = Math_SinS(aimYaw);
    const float sz = Math_CosS(aimYaw);
    const Vec3f spawnPos = {
        spider->world.pos.x + sx * kSpawnForwardOffset,
        spider->world.pos.y + 20.0f,  // ~body-center Y so web flies at Link torso
        spider->world.pos.z + sz * kSpawnForwardOffset,
    };

    // Params encodes aim yaw as yaw/8 (matches Dekubaba acid pattern).
    const s16 paramsYaw = (s16)(aimYaw / 8);

    Actor_Spawn(&play->actorCtx, play,
                gEnSwWebId,
                spawnPos.x, spawnPos.y, spawnPos.z,
                /*rot.x=*/0,
                /*rot.y=*/aimYaw,
                /*rot.z=*/0,
                paramsYaw);
}
