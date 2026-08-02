/*
 * File: z_en_dekubaba_acid.c
 * Overlay: ovl_En_Dekubaba_Acid
 * Description: Pillar 5 (GH #308) — acid vomit projectile fired by
 *              enhanced EnDekubaba during acid-vomit state.
 *
 * Design plan: Claude/Plans/en_dekubaba_enhanced_plan.md.
 * Spawner:     soh/soh/Network/Anchor/Common/EnemyEnhancementRegistry/
 *              PerActor/EnDekubabaDescriptor.cpp (OnAcidVomitTick).
 *
 * v1 architecture (mirrors EnKarebabaGeyser pattern with mobile
 * variant): AC-collider projectile that moves in a shallow arc.
 *
 * Motion:
 *   - Init reads `params` as quantized aim yaw (params × 8 → s16 yaw).
 *   - Init sets initial XZ velocity from yaw (~8u/frame) and Y
 *     velocity (+4u/frame upward) with gravity (-0.7u/frame²) so the
 *     projectile arcs down and reaches ~300u XZ before ground impact.
 *   - Actor_MoveXZGravity applies velocity + gravity each frame.
 *
 * Lifetime end:
 *   - Ground contact (bgCheckFlags & 1) → Actor_Kill + splash burst
 *   - Wall contact  (bgCheckFlags & 8) → Actor_Kill + splash burst
 *   - Timeout       (60 frames) → Actor_Kill silent (bomb-out fallback)
 *
 * Damage: contact-cylinder AT collider (AT_TYPE_ENEMY, dmgFlags
 * 0xFFCFFFFF matches enemy contact damage) — 8 damage / 2 hearts,
 * same as vanilla Dekubaba lunge. Player takes damage on first
 * intersection; the projectile despawns immediately after (Actor_Kill
 * on AT hit) so it can't multi-tick.
 *
 * MP note: spawn is deterministic (fixed origin at Dekubaba head
 * position + fixed frame within AcidVomit state + shipped aim yaw
 * via ENEMY_STATE) so both host and peer spawn locally without
 * per-spawn ENEMY_SPAWN broadcast. Damage flows through the standard
 * host-authoritative AC-collider path (Path A → DAMAGE_PLAYER packet
 * to peer).
 */

#include "z_en_dekubaba_acid.h"
#include <libultraship/log/luslog.h>

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// Motion parameters — see file header.
#define EN_DEKUBABA_ACID_XZ_SPEED         8.0f
#define EN_DEKUBABA_ACID_INITIAL_Y_SPEED  4.0f
#define EN_DEKUBABA_ACID_GRAVITY          -0.7f

// Pillar 5 (#308) Bug 1 fix (2026-08-01) — trail particles during
// flight. Draw is a stub (no mesh); user reported "acid not observed"
// because between telegraph and ground impact the projectile was
// invisible. Fix: spawn green splash particle every 2 frames using
// the same AcidVisuals palette the descriptor uses for telegraph
// mouth spit. C-side inline copy of the C++ header constants.
#define ACID_TRAIL_PERIOD_FRAMES 2
static Color_RGBA8 sAcidTrailPrimColor = { 170, 240, 110, 220 };
static Color_RGBA8 sAcidTrailEnvColor  = {  50,  90,  30, 255 };
#define ACID_TRAIL_SPLASH_TYPE  2
#define ACID_TRAIL_SPLASH_SCALE 250

// Lifetime — 60 frames max (3 sec at 20fps, 1 sec at 60fps).
// Ground / wall contact terminates earlier.
#define EN_DEKUBABA_ACID_LIFETIME_FRAMES  60

// AT collider — small mobile cylinder that damages Link on contact.
//
// dmgFlags 0x00000200 = electric/environmental class — matches
// Karebaba geyser behavior per user 2026-07-31 Q1: acid attack cannot
// be blocked by shield. Regular shield AC quads don't defend against
// this damage class (mirrors how wall-of-water / electric-shock pass
// through). Player takes full damage on contact regardless of facing
// direction or shield state.
//
// This is intentional design — acid vomit is an unblockable spit
// that requires positional dodge (walk out of the arc), not shield
// deflection. Consistent with Karebaba's AoE geyser.
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
        { 0x00000200, 0x00, 0x08 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NONE,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { 15, 20, 0, { 0, 0, 0 } },  // radius 15, height 20
};

void EnDekubabaAcid_Init(Actor* thisx, PlayState* play) {
    EnDekubabaAcid* this = (EnDekubabaAcid*)thisx;

    // Read quantized aim yaw from params (spawner encoded as yaw/8).
    // Reconstruct full s16 yaw.
    const s16 aimYaw = (s16)(this->actor.params * 8);
    this->actor.world.rot.y = aimYaw;
    this->actor.shape.rot.y = aimYaw;

    // Initial velocity — XZ from aim yaw, Y upward for arc.
    this->actor.speedXZ  = EN_DEKUBABA_ACID_XZ_SPEED;
    this->actor.velocity.x = Math_SinS(aimYaw) * EN_DEKUBABA_ACID_XZ_SPEED;
    this->actor.velocity.y = EN_DEKUBABA_ACID_INITIAL_Y_SPEED;
    this->actor.velocity.z = Math_CosS(aimYaw) * EN_DEKUBABA_ACID_XZ_SPEED;
    this->actor.gravity  = EN_DEKUBABA_ACID_GRAVITY;

    this->lifetimeFrames = EN_DEKUBABA_ACID_LIFETIME_FRAMES;

    Collider_InitCylinder(play, &this->collider);
    Collider_SetCylinder(play, &this->collider, &this->actor, &sColliderInit);

    LUSLOG_INFO("[DekubabaAcid] Init pos=(%.0f,%.0f,%.0f) yaw=%d params=%d",
                this->actor.world.pos.x, this->actor.world.pos.y,
                this->actor.world.pos.z, aimYaw, this->actor.params);
}

void EnDekubabaAcid_Destroy(Actor* thisx, PlayState* play) {
    EnDekubabaAcid* this = (EnDekubabaAcid*)thisx;
    Collider_DestroyCylinder(play, &this->collider);
}

void EnDekubabaAcid_Update(Actor* thisx, PlayState* play) {
    EnDekubabaAcid* this = (EnDekubabaAcid*)thisx;

    // Timeout — bomb-out to prevent orphan projectiles.
    this->lifetimeFrames--;
    if (this->lifetimeFrames <= 0) {
        Actor_Kill(&this->actor);
        return;
    }

    // Bug 1 fix (2026-08-01) — trail particle at current position.
    // Fires every ACID_TRAIL_PERIOD_FRAMES frames so player can see
    // the projectile in flight. Uses same green splash config as
    // the descriptor's telegraph mouth spit.
    if ((play->gameplayFrames % ACID_TRAIL_PERIOD_FRAMES) == 0) {
        Vec3f trailPos = this->actor.world.pos;
        EffectSsGSplash_Spawn(play, &trailPos,
                               &sAcidTrailPrimColor, &sAcidTrailEnvColor,
                               ACID_TRAIL_SPLASH_TYPE,
                               ACID_TRAIL_SPLASH_SCALE);
    }

    // Ballistic arc — Actor_MoveXZGravity applies velocity + gravity.
    // BgCheck_Update tests floor / wall / ceiling intersections.
    Actor_MoveXZGravity(&this->actor);
    // bgCheck flags = 5 (floor + wall) — vanilla convention.
    Actor_UpdateBgCheckInfo(play, &this->actor, 10.0f, 15.0f, 15.0f, 5);

    // Ground / wall contact → despawn (with room for future splash
    // VFX at impact position — descriptor could handle via a
    // callback if desired).
    if (this->actor.bgCheckFlags & 1) {
        // Ground contact.
        LUSLOG_INFO("[DekubabaAcid] Ground contact at (%.0f,%.0f,%.0f)",
                    this->actor.world.pos.x, this->actor.world.pos.y,
                    this->actor.world.pos.z);
        Actor_Kill(&this->actor);
        return;
    }
    if (this->actor.bgCheckFlags & 8) {
        // Wall contact.
        LUSLOG_INFO("[DekubabaAcid] Wall contact at (%.0f,%.0f,%.0f)",
                    this->actor.world.pos.x, this->actor.world.pos.y,
                    this->actor.world.pos.z);
        Actor_Kill(&this->actor);
        return;
    }

    // Register AT collider — hits Link if he intersects. Single-hit
    // policy: on hit, despawn (checked via collider.base.atFlags &
    // AT_HIT). Vanilla path A infrastructure then broadcasts
    // DAMAGE_PLAYER to peer.
    Collider_UpdateCylinder(&this->actor, &this->collider);
    CollisionCheck_SetAT(play, &play->colChkCtx, &this->collider.base);

    if (this->collider.base.atFlags & AT_HIT) {
        LUSLOG_INFO("[DekubabaAcid] AT hit — despawning");
        Actor_Kill(&this->actor);
        return;
    }
}

void EnDekubabaAcid_Draw(Actor* thisx, PlayState* play) {
    // No mesh — visual is provided by trail particles spawned each
    // frame in Update (below). Kept as a stub so any future Draw-side
    // polish (custom softsprite texture, custom shader) has a hook.
    (void)thisx;
    (void)play;
}
