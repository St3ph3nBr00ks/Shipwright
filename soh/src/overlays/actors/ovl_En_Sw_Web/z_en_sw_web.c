/*
 * File: z_en_sw_web.c
 * Overlay: ovl_En_Sw_Web
 * Description: GH #333 — stun/web projectile fired by enhanced En_Sw
 *              (Skullwalltula) during the WebAttack state.
 *
 * Design brief: GH #333 comment 5209793604 (all A1-A15 resolved).
 * Spawner:      soh/soh/Network/Anchor/Common/EnemyEnhancementRegistry/
 *               PerActor/EnSwStateMachine.cpp (TickWebAttack, Phase 3).
 *
 * Motion (A2 reference Dekubaba acid, adapted for straight-line):
 *   - Init reads `params` as quantized aim yaw (params × 8 → s16 yaw).
 *   - XZ velocity ~10u/frame from yaw. NO gravity (straight-line, not
 *     arc — differs from Dekubaba acid). Y velocity = 0.
 *   - Actor_MoveXZGravity applies velocity each frame (gravity = 0 is
 *     effectively straight-line).
 *
 * Hit detection (A13 host-authoritative):
 *   - Host-only per-tick distance check against all synced player
 *     actors (local Link + DummyPlayers via FindNearestPlayerActor).
 *   - On hit within kWebHitRadius: bridge fires broadcast via
 *     Anchor_Enhance_EnSwWeb_ApplyStun which resolves victim clientId
 *     and dispatches STUN_APPLIED. Projectile despawns silently.
 *   - Peer replicas skip the hit check entirely (motion only) — stun
 *     arrives via PLAYER_STUN_APPLIED from host.
 *
 * Lifetime end conditions (all Actor_Kill silent — no impact VFX):
 *   - Player hit (host only)
 *   - Wall / floor / ceiling contact
 *   - Timeout (60 frames)
 *
 * MP note: host spawns via existing ENEMY_SPAWN pipeline (Anchor sync
 * registers this actor id in IsSyncedWorldActor is NOT needed —
 * projectile is short-lived and peer's local copy is driven by
 * position sync via ENEMY_UPDATE if admitted, OR by deterministic
 * spawn params if not. v1 approach: admit to sync so peers see the
 * projectile fly through their scene; hit detection stays host-only.
 */

#include "z_en_sw_web.h"
#include <libultraship/log/luslog.h>

// Forward-decls of Anchor bridge helpers (Pitfall 7 — plain extern in
// .c files; NOT extern "C"). Bodies live in
// EnSwWebBridge.cpp / SceneAuthority.
extern int  Anchor_Enhance_EnSwWeb_IsHost(void);
extern int  Anchor_Enhance_EnSwWeb_DetectAndApplyHit(Actor* projectile,
                                                       PlayState* play,
                                                       f32 hitRadius);

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// Motion — straight-line, no arc.
#define EN_SW_WEB_XZ_SPEED         10.0f

// Hit detection — proximity radius. Matches vanilla Link body cylinder
// (~25u), slightly generous so glancing shots still register.
#define EN_SW_WEB_HIT_RADIUS       30.0f

// Lifetime — 60 frames max (~1s at 60fps). Timeout is a safety net;
// most projectiles hit a wall or fly out of the room first.
#define EN_SW_WEB_LIFETIME_FRAMES  60

// Placeholder trail visuals (v1) — white dust particles trailing the
// projectile so the player can see it in flight. When a proper web
// asset lands (v2), replace with a spherical mesh in Draw. Palette
// matches "spider silk" whitish-gray.
#define WEB_TRAIL_PERIOD_FRAMES    2
static Color_RGBA8 sWebTrailPrimColor = { 240, 240, 240, 220 };
static Color_RGBA8 sWebTrailEnvColor  = { 180, 180, 180, 255 };
#define WEB_TRAIL_DUST_SCALE       80
#define WEB_TRAIL_DUST_SCALESTEP   4
#define WEB_TRAIL_DUST_LIFE        12

// OC-only collider (A13). Registered as OC1_TYPE_PLAYER so the vanilla
// collision system knows this can overlap Player. We don't rely on
// OC-flags for hit detection — proximity check in Update is used
// instead (cleaner + guaranteed to fire even if the vanilla OC
// dispatch is bypassed for some reason). Collider is kept ONLY so
// vanilla's physics pipeline doesn't allow Link to walk through the
// projectile at zero cost — soft-block preserves the "web is a
// thing" feel even for a v1 lacking a visual mesh.
static ColliderCylinderInit sColliderInit = {
    {
        COLTYPE_NONE,
        AT_NONE,
        AC_NONE,
        OC1_ON | OC1_TYPE_PLAYER,
        OC2_TYPE_1,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK0,
        { 0x00000000, 0x00, 0x00 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_NONE,
        BUMP_NONE,
        OCELEM_ON,
    },
    { 15, 30, 0, { 0, 0, 0 } },  // radius 15, height 30
};

void EnSwWeb_Init(Actor* thisx, PlayState* play) {
    EnSwWeb* this = (EnSwWeb*)thisx;

    // Read quantized aim yaw from params (spawner encoded as yaw/8).
    const s16 aimYaw = (s16)(this->actor.params * 8);
    this->actor.world.rot.y = aimYaw;
    this->actor.shape.rot.y = aimYaw;

    // Straight-line velocity from aim yaw.
    this->actor.speedXZ    = EN_SW_WEB_XZ_SPEED;
    this->actor.velocity.x = Math_SinS(aimYaw) * EN_SW_WEB_XZ_SPEED;
    this->actor.velocity.y = 0.0f;
    this->actor.velocity.z = Math_CosS(aimYaw) * EN_SW_WEB_XZ_SPEED;
    this->actor.gravity    = 0.0f;

    this->lifetimeFrames = EN_SW_WEB_LIFETIME_FRAMES;

    Collider_InitCylinder(play, &this->collider);
    Collider_SetCylinder(play, &this->collider, &this->actor, &sColliderInit);

    LUSLOG_INFO("[EnSwWeb] Init pos=(%.0f,%.0f,%.0f) yaw=%d params=%d",
                this->actor.world.pos.x, this->actor.world.pos.y,
                this->actor.world.pos.z, aimYaw, this->actor.params);
}

void EnSwWeb_Destroy(Actor* thisx, PlayState* play) {
    EnSwWeb* this = (EnSwWeb*)thisx;
    Collider_DestroyCylinder(play, &this->collider);
}

void EnSwWeb_Update(Actor* thisx, PlayState* play) {
    EnSwWeb* this = (EnSwWeb*)thisx;

    // Timeout — bomb-out to prevent orphan projectiles.
    this->lifetimeFrames--;
    if (this->lifetimeFrames <= 0) {
        Actor_Kill(&this->actor);
        return;
    }

    // Trail particle every N frames — placeholder visual per A11 v1
    // (no mesh; particles show trajectory).
    if ((play->gameplayFrames % WEB_TRAIL_PERIOD_FRAMES) == 0) {
        Vec3f trailPos = this->actor.world.pos;
        Vec3f dustVel   = { 0.0f, 0.0f, 0.0f };
        Vec3f dustAccel = { 0.0f, 0.0f, 0.0f };
        EffectSsDust_Spawn(play,
                            /*drawFlags*/ 0,
                            &trailPos, &dustVel, &dustAccel,
                            &sWebTrailPrimColor,
                            &sWebTrailEnvColor,
                            WEB_TRAIL_DUST_SCALE,
                            WEB_TRAIL_DUST_SCALESTEP,
                            WEB_TRAIL_DUST_LIFE,
                            /*updateMode*/ 0);
    }

    // Straight-line motion — Actor_MoveXZGravity with gravity=0.
    // BgCheck test for wall / floor / ceiling → despawn silently.
    Actor_MoveXZGravity(&this->actor);
    Actor_UpdateBgCheckInfo(play, &this->actor, 10.0f, 15.0f, 15.0f,
                             /*flags*/ 1 | 8 | 0x10);

    if (this->actor.bgCheckFlags & (1 | 8 | 0x10)) {
        // Any BG contact → despawn silently.
        Actor_Kill(&this->actor);
        return;
    }

    // Update OC-collider position (physical body soft-block; not the
    // hit-detection path).
    Collider_UpdateCylinder(&this->actor, &this->collider);
    CollisionCheck_SetOC(play, &play->colChkCtx, &this->collider.base);

    // Host-authoritative hit detection (A13). Peer replicas skip this
    // entirely — stun arrives via PLAYER_STUN_APPLIED broadcast.
    if (Anchor_Enhance_EnSwWeb_IsHost()) {
        if (Anchor_Enhance_EnSwWeb_DetectAndApplyHit(
                &this->actor, play, EN_SW_WEB_HIT_RADIUS)) {
            // Bridge broadcast STUN_APPLIED for whichever player was
            // in range. Despawn silently.
            LUSLOG_INFO("[EnSwWeb] Hit at (%.0f,%.0f,%.0f) — stun applied",
                        this->actor.world.pos.x,
                        this->actor.world.pos.y,
                        this->actor.world.pos.z);
            Actor_Kill(&this->actor);
            return;
        }
    }
}

void EnSwWeb_Draw(Actor* thisx, PlayState* play) {
    // v1 placeholder — no mesh. Trail particles in Update provide
    // visual trajectory. v2: swap in a spherical web mesh with the
    // reusable web texture (search delegated to background agent).
    (void)thisx;
    (void)play;
}
