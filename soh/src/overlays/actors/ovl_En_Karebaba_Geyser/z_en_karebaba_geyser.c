/*
 * File: z_en_karebaba_geyser.c
 * Overlay: ovl_En_Karebaba_Geyser
 * Description: Pillar 5 (GH #310) — vertical acid plume spawned by
 *              enhanced EnKarebaba during Spin state. Damages any
 *              player within a small cylinder around the Karebaba's
 *              stem base for ~30 frames.
 *
 * Design plan: Claude/Plans/en_karebaba_enhanced_plan.md.
 * Spawner:     soh/soh/Network/Anchor/Common/EnemyEnhancementRegistry/
 *              PerActor/EnKarebabaDescriptor.cpp (OnSpinTick).
 *
 * No mesh — Draw is nullptr. Visual is entirely particle bursts spawned
 * per-frame. Actor's only jobs are:
 *   - Hold a cylinder AT collider (damages any player who steps in).
 *   - Spawn hahen debris particles upward from actor.world.pos each
 *     tick for the plume visual.
 *   - Count down a lifetime and Actor_Kill self.
 *
 * MP note: spawn is deterministic (fixed home.pos + fixed frame within
 * enhanced Spin cycle) so both host and peer spawn locally without
 * per-spawn ENEMY_SPAWN broadcast. Damage is host-authoritative via
 * standard AC→DummyPlayer path.
 */

#include "z_en_karebaba_geyser.h"
#include "overlays/effects/ovl_Effect_Ss_Hahen/z_eff_ss_hahen.h"
#include <libultraship/log/luslog.h>

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// ~1 second at 20 fps game rate. Matches most of the 40-frame Karebaba
// Spin window; leaves a couple frames head/tail so the plume ends
// slightly before Spin exits.
#define EN_KAREBABA_GEYSER_LIFETIME_FRAMES 30

// Cylinder AT dimensions per plan §"Design":
//   radius 60u — small AoE, requires standing near the Karebaba.
//   height 120u — reaches roughly Link's head at normal Karebaba
//                 stem scale.
static ColliderCylinderInit sColliderInit = {
    {
        COLTYPE_HARD,
        AT_ON | AT_TYPE_ENEMY,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_1,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK0,
        { 0xFFCFFFFF, 0x00, 0x08 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NONE,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { 60, 120, 0, { 0, 0, 0 } },
};

void EnKarebabaGeyser_Init(Actor* thisx, PlayState* play) {
    EnKarebabaGeyser* this = (EnKarebabaGeyser*)thisx;

    this->actor.gravity = 0.0f;  // stays at home.pos.y
    this->lifetimeFrames = EN_KAREBABA_GEYSER_LIFETIME_FRAMES;

    Collider_InitCylinder(play, &this->collider);
    Collider_SetCylinder(play, &this->collider, &this->actor, &sColliderInit);

    LUSLOG_INFO("[KarebabaGeyser] Init pos=(%.0f,%.0f,%.0f) lifetime=%d",
                this->actor.world.pos.x, this->actor.world.pos.y,
                this->actor.world.pos.z, this->lifetimeFrames);
}

void EnKarebabaGeyser_Destroy(Actor* thisx, PlayState* play) {
    EnKarebabaGeyser* this = (EnKarebabaGeyser*)thisx;
    Collider_DestroyCylinder(play, &this->collider);
}

void EnKarebabaGeyser_Update(Actor* thisx, PlayState* play) {
    EnKarebabaGeyser* this = (EnKarebabaGeyser*)thisx;

    // Countdown — Actor_Kill when lifetime expires.
    this->lifetimeFrames--;
    if (this->lifetimeFrames <= 0) {
        Actor_Kill(&this->actor);
        return;
    }

    // Spawn visual plume particles going straight up from the plume
    // origin. Two particles per frame at slight XZ offsets so the
    // plume reads as a column rather than a single trail. Random
    // scale variance provides visual noise. Uses the vanilla hahen
    // burst helper — no new effect asset needed. Green tint would
    // require a custom EffectSs; deferred to Phase 4 polish.
    for (s32 i = 0; i < 2; i++) {
        Vec3f burstPos = this->actor.world.pos;
        burstPos.x += (Rand_ZeroOne() - 0.5f) * 40.0f;  // ±20u XZ jitter
        burstPos.z += (Rand_ZeroOne() - 0.5f) * 40.0f;
        burstPos.y += Rand_ZeroOne() * 60.0f;           // 0..60u height
        EffectSsHahen_SpawnBurst(play, &burstPos, 3.0f, 0, 12, 5, 1,
                                  HAHEN_OBJECT_DEFAULT, 10, NULL);
    }

    // Register the AT collider so it can hit players who wander into
    // the plume. Damage flows through the standard vanilla AT→player
    // path — DummyPlayer's AC on peer receives via existing
    // ENEMY_HIT_PLAYER pipeline.
    Collider_UpdateCylinder(&this->actor, &this->collider);
    CollisionCheck_SetAT(play, &play->colChkCtx, &this->collider.base);
}

void EnKarebabaGeyser_Draw(Actor* thisx, PlayState* play) {
    // No mesh. Visual is purely the per-frame hahen particles spawned
    // in Update. Kept as a stub so ActorDBInit registration + any
    // future Draw-side polish (custom green mesh, screen shader, etc.)
    // has a hook point.
    (void)thisx;
    (void)play;
}
