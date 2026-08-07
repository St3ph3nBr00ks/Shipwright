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
#include "soh/Network/Anchor/Common/PlayerStunManager.h"

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

    // Phase 4a — resolve nearest to a clientId. Own Link vs DummyPlayer:
    //   - Local Link (host's own): Anchor::Instance->ownClientId
    //   - DummyPlayer (peer replica on host): GetDummyPlayerClientId
    if (Anchor::Instance == nullptr) return 0;
    uint32_t victimClientId = UINT32_MAX;
    if (gPlayState != nullptr && GET_PLAYER(gPlayState) == (Player*)nearest) {
        victimClientId = Anchor::Instance->ownClientId;
    } else {
        const uint32_t maybe = Anchor::Instance->GetDummyPlayerClientId(nearest);
        if (maybe != 0) victimClientId = maybe;
    }
    if (victimClientId == UINT32_MAX) return 0;  // not a player

    // A15 no-multi-stack — refuse to apply if already stunned.
    if (AnchorPlayerStun::IsClientStunned(victimClientId)) return 0;

    // Apply stun locally on host. Phase 4b will augment ApplyStun to
    // broadcast PLAYER_STUN_APPLIED to peers so all clients agree.
    // Source netId: v1 pass 0 (informational only — not consumed by
    // logic yet). Phase 4b consideration: if we want peer's overlay
    // to know which spider webbed them, thread the source EnSw netId
    // through here.
    AnchorPlayerStun::ApplyStun(victimClientId, /*sourceEnSwNetId*/ 0);
    return 1;
}

// Phase 4a — real body: query the PlayerStunManager. Returns 1 if the
// nearest-player actor's clientId is currently stunned. Used by
// EnSwStateMachine::TryEnterWebAttack per A15 (no-multi-stack: don't
// fire web at already-stunned target).
extern "C" int Anchor_Enhance_EnSwWeb_IsPlayerStunned(Actor* player) {
    return AnchorPlayerStun::IsActorStunned(player) ? 1 : 0;
}

// Fire helper — spawns the En_Sw_Web projectile locally on host.
// Phase 3 body: local Actor_Spawn only. Phase 4b adds
// EN_SW_WEB_FIRED packet broadcast so peers spawn their own copy
// deterministically (mirrors Dekubaba acid MP pattern per
// z_en_dekubaba_acid.c file header note).
//
// Aim: horizontal component (yaw) encoded into params via /8
// quantization; vertical component computed here and applied by
// post-spawn velocity override. Vanilla Init sets straight-line
// XZ motion; we then overwrite velocity.y to make the projectile
// aim at target's torso height regardless of spider elevation.
//
// User 2026-08-06 field-test: BIG-variant Skullwalltulas (from
// En_St→En_Sw swap) hover at ceiling height, so projectile fired
// straight-line at spider's Y sailed over child-Link's head. Fix:
// compute Y velocity so trajectory intersects target's torso.
extern "C" void Anchor_Enhance_EnSwWeb_FireProjectile(Actor* spider,
                                                       Actor* target,
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
        spider->world.pos.y + 20.0f,  // spider body-center Y
        spider->world.pos.z + sz * kSpawnForwardOffset,
    };

    // Params encodes aim yaw as yaw/8 (matches Dekubaba acid pattern).
    const s16 paramsYaw = (s16)(aimYaw / 8);

    Actor* newProj = Actor_Spawn(&play->actorCtx, play,
                                   gEnSwWebId,
                                   spawnPos.x, spawnPos.y, spawnPos.z,
                                   /*rot.x=*/0,
                                   /*rot.y=*/aimYaw,
                                   /*rot.z=*/0,
                                   paramsYaw);
    if (newProj == nullptr) return;

    // Aim-Y correction (post-Init override). Init set velocity to
    // straight-line horizontal (velocity.y = 0). Recompute so the
    // trajectory intersects target's torso.
    if (target != nullptr) {
        constexpr float kTargetTorsoOffsetY = 30.0f;  // ~Link torso above feet
        constexpr float kXZSpeed = 10.0f;             // must match z_en_sw_web.c EN_SW_WEB_XZ_SPEED
        const float targetAimY = target->world.pos.y + kTargetTorsoOffsetY;
        const float dx = target->world.pos.x - spawnPos.x;
        const float dz = target->world.pos.z - spawnPos.z;
        const float horizDist = sqrtf(dx * dx + dz * dz);
        if (horizDist > 1.0f) {
            const float flightFrames = horizDist / kXZSpeed;
            const float dy = targetAimY - spawnPos.y;
            newProj->velocity.y = dy / flightFrames;
        }
    }
}
