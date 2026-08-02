/*
 * File: z_en_dekubaba_seed.c
 * Overlay: ovl_En_Dekubaba_Seed
 * Description: Pillar 5 (GH #318) — seed projectile fired by
 *              enhanced EnDekubaba during SeedFire state; on land,
 *              spawns a new EN_DEKUBABA child at landing position.
 *
 * Design plan: Claude/Plans/en_dekubaba_enhanced_plan.md §Feature C.
 * Spawner:     soh/soh/Network/Anchor/Common/EnemyEnhancementRegistry/
 *              PerActor/EnDekubabaDescriptor.cpp (OnSeedFireTick).
 *
 * v1 architecture (per user 2026-07-31 spec):
 *   - Straight-line trajectory, no gravity, no damage
 *   - Landing target passed via actor.world.rot.y (spawner-provided
 *     aim yaw); actor computes distance-based time-of-flight
 *   - On timeout at landing, invokes parent's OnSeedLanded callback
 *     which spawns the child Dekubaba at (this->actor.world.pos)
 *   - Ignores mid-flight geometry (per user Q7 answer)
 *
 * MP note: spawn is deterministic (fixed origin = Dekubaba head +
 * shipped aim yaw + shipped landing target via ENEMY_STATE) so both
 * host and peer spawn locally without per-spawn ENEMY_SPAWN broadcast.
 * Child Dekubaba spawn IS broadcast — via existing ENEMY_SPAWN
 * infrastructure — because host is the sole authority for that spawn
 * per sync-rule 1.
 */

#include "z_en_dekubaba_seed.h"
#include <libultraship/log/luslog.h>

// Pillar 5 (#318) — descriptor callback shims.
extern void Anchor_Enhance_EnDekubaba_OnSeedLanded(Actor* parent, PlayState* play,
                                                     float x, float y, float z);
// Bug 8 fix (2026-08-02) — consume the pending-parent thread-local
// set by descriptor's OnSeedFireTick immediately before Actor_Spawn.
// Returns the parent Dekubaba's Actor* or NULL if spawned outside
// the descriptor path (defensive fallback).
extern Actor* Anchor_Enhance_EnDekubaba_ConsumePendingSeedParent(void);

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

#define EN_DEKUBABA_SEED_SPEED             12.0f
#define EN_DEKUBABA_SEED_LIFETIME_FRAMES   180  // safety cap ~9s @ 20fps

// Bug 10 fix (2026-08-02) — trail particles during flight. Draw is
// a stub (Deku Nut mesh deferred per Feature C §"deferred polish");
// projectile was invisible flying from Dekubaba mouth to landing.
// Add per-frame EffectSsGSplash trail so player sees the seed arc.
// Tan/brown tint (nut color) distinguishes from acid's green trail.
//
// Bug 2 fix (2026-08-02) — trail spawn every frame (was every 2) +
// scale bumped 150 → 250 to match acid trail visibility. User
// reported "no visible seed model flying, something on the floor" —
// the sparse low-scale trail was almost invisible above ankle.
#define SEED_TRAIL_PERIOD_FRAMES 1
static Color_RGBA8 sSeedTrailPrimColor = { 200, 150,  70, 220 };  // brighter nut tan
static Color_RGBA8 sSeedTrailEnvColor  = { 100,  60,  25, 255 };  // dark shadow
#define SEED_TRAIL_SPLASH_TYPE  2
#define SEED_TRAIL_SPLASH_SCALE 250   // matches acid trail visibility

void EnDekubabaSeed_Init(Actor* thisx, PlayState* play) {
    EnDekubabaSeed* this = (EnDekubabaSeed*)thisx;

    // Read quantized aim yaw from params (spawner encoded as yaw/8).
    const s16 aimYaw = (s16)(this->actor.params * 8);
    this->actor.world.rot.y = aimYaw;
    this->actor.shape.rot.y = aimYaw;

    // Straight-line velocity — no gravity.
    this->actor.gravity  = 0.0f;
    this->actor.speedXZ  = EN_DEKUBABA_SEED_SPEED;
    this->actor.velocity.x = Math_SinS(aimYaw) * EN_DEKUBABA_SEED_SPEED;
    this->actor.velocity.y = 0.0f;
    this->actor.velocity.z = Math_CosS(aimYaw) * EN_DEKUBABA_SEED_SPEED;

    this->lifetimeFrames   = EN_DEKUBABA_SEED_LIFETIME_FRAMES;

    // Bug 8 fix (2026-08-02) — consume pending parent pointer set by
    // descriptor's OnSeedFireTick immediately before Actor_Spawn.
    // Without this, parentDekubaba stayed NULL and OnSeedLanded's
    // parent-tracking (spawnedChildActor field on descriptor state)
    // never got populated → child-not-active gate could never fire.
    this->parentDekubaba = Anchor_Enhance_EnDekubaba_ConsumePendingSeedParent();
    this->landingTarget.x = 0.0f;
    this->landingTarget.y = 0.0f;
    this->landingTarget.z = 0.0f;

    LUSLOG_INFO("[DekubabaSeed] Init pos=(%.0f,%.0f,%.0f) yaw=%d parent=%p",
                this->actor.world.pos.x, this->actor.world.pos.y,
                this->actor.world.pos.z, aimYaw,
                (void*)this->parentDekubaba);
}

void EnDekubabaSeed_Destroy(Actor* thisx, PlayState* play) {
    (void)thisx;
    (void)play;
}

void EnDekubabaSeed_Update(Actor* thisx, PlayState* play) {
    EnDekubabaSeed* this = (EnDekubabaSeed*)thisx;

    // Lifetime — safety cap. Fires the on-land callback at expiration
    // regardless (spawn child wherever the projectile happens to be).
    this->lifetimeFrames--;
    if (this->lifetimeFrames <= 0) {
        // Fire on-land callback. Parent NULL fallback → descriptor
        // uses nearest-Dekubaba lookup. Spawn child at current pos.
        Anchor_Enhance_EnDekubaba_OnSeedLanded(
            this->parentDekubaba, play,
            this->actor.world.pos.x,
            this->actor.world.pos.y,
            this->actor.world.pos.z);
        Actor_Kill(&this->actor);
        return;
    }

    // Bug 10 fix (2026-08-02) — trail particle for in-flight visual.
    // Draw is a stub; without this, the projectile is invisible from
    // launch to landing. Small tan splash every 2 frames traces the
    // arc.
    if ((play->gameplayFrames % SEED_TRAIL_PERIOD_FRAMES) == 0) {
        Vec3f trailPos = this->actor.world.pos;
        EffectSsGSplash_Spawn(play, &trailPos,
                               &sSeedTrailPrimColor, &sSeedTrailEnvColor,
                               SEED_TRAIL_SPLASH_TYPE,
                               SEED_TRAIL_SPLASH_SCALE);
    }

    // Move projectile — straight-line, no gravity.
    Actor_MoveXZGravity(&this->actor);

    // Ground contact detection — bgCheck for early-land at floor.
    // Flags = 5 (floor + wall) matches vanilla Dekubaba convention.
    Actor_UpdateBgCheckInfo(play, &this->actor, 10.0f, 15.0f, 15.0f, 5);
    if (this->actor.bgCheckFlags & 1) {
        // Ground hit — spawn child here.
        Anchor_Enhance_EnDekubaba_OnSeedLanded(
            this->parentDekubaba, play,
            this->actor.world.pos.x,
            this->actor.world.pos.y,
            this->actor.world.pos.z);
        Actor_Kill(&this->actor);
        return;
    }

    // Spin visual (rotate around vertical axis for tumbling nut look).
    this->actor.shape.rot.y += 0x1000;
}

void EnDekubabaSeed_Draw(Actor* thisx, PlayState* play) {
    // No mesh in v1 — visual placeholder. Object dekunuts flower/nut
    // DList (per user C1) will be wired here in a follow-up polish
    // pass; requires resolving the exact DList symbol name from
    // object_dekunuts. For v1 the seed is invisible in flight but
    // the on-land Dekubaba spawn IS visible.
    (void)thisx;
    (void)play;
}
