/*
 * File: z_en_karebaba_geyser.c
 * Overlay: ovl_En_Karebaba_Geyser
 * Description: Pillar 5 (GH #310) — vertical acid AoE hitbox spawned
 *              by enhanced EnKarebaba during Spin state.
 *
 * Design plan: Claude/Plans/en_karebaba_enhanced_plan.md.
 * Spawner:     soh/soh/Network/Anchor/Common/EnemyEnhancementRegistry/
 *              PerActor/EnKarebabaDescriptor.cpp (OnSpinTick).
 *
 * v1 responsibility split (2026-07-31 refactor after playtest
 * feedback on the initial hahen visual): this actor is a pure AC
 * cylinder holder at the Karebaba's stem base — visible plume
 * particles are spawned per-frame by the descriptor's OnSpinTick
 * so the rising-bubble layer can track the swinging head position
 * while the falling-dust layer independently rains from above.
 * Keeps the damage volume stable at home.pos while visual layers
 * move.
 *
 * Actor lifecycle:
 *   - Init:    set up AT cylinder collider at spawn pos (= Karebaba
 *              home.pos, set by the descriptor at spawn time).
 *   - Update:  register AT collider so any player intersecting the
 *              cylinder takes damage; count down lifetime; Actor_Kill
 *              self at zero.
 *   - Draw:    null — no mesh. Visuals live in descriptor OnSpinTick.
 *
 * MP note: spawn is deterministic (fixed home.pos + fixed frame
 * within enhanced Spin cycle) so both host and peer spawn locally
 * without per-spawn ENEMY_SPAWN broadcast. Damage flows through
 * the standard vanilla AT-collider path (host AT hit -> DAMAGE_PLAYER
 * on peer via existing infrastructure).
 */

#include "z_en_karebaba_geyser.h"
#include <libultraship/log/luslog.h>

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// ~1 second at 20 fps game rate. Matches most of the 40-frame Karebaba
// Spin window; leaves a couple frames head/tail so the AoE ends
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

    // Register AT collider so it can hit players who wander into the
    // AoE. Damage flows through the standard vanilla AT-player path.
    Collider_UpdateCylinder(&this->actor, &this->collider);
    CollisionCheck_SetAT(play, &play->colChkCtx, &this->collider.base);
}

void EnKarebabaGeyser_Draw(Actor* thisx, PlayState* play) {
    // No mesh. Visible plume is spawned per-frame by the
    // Karebaba descriptor's OnSpinTick (rising bubbles at head +
    // falling dust from above). Kept as a stub so any future
    // Draw-side polish (custom shader, screen tint, etc.) has a
    // hook point.
    (void)thisx;
    (void)play;
}
