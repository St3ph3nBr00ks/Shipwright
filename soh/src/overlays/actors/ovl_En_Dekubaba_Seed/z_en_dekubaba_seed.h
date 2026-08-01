#ifndef Z_EN_DEKUBABA_SEED_H
#define Z_EN_DEKUBABA_SEED_H

#include <libultraship/libultra.h>
#include "global.h"

// Pillar 5 (GH #318) — seed projectile actor. Fired by enhanced
// EnDekubaba's SeedFire state; on land, spawns a new EN_DEKUBABA
// child at the landing position.
//
// v1 design (per user 2026-07-31):
//   - Straight-line trajectory from Dekubaba head to landing target
//     (no gravity arc — avoids visual clipping through walls, exact
//     landing predictable)
//   - No damage (AT_NONE — pure spawner projectile)
//   - Ignores mid-flight geometry (spawn child at pre-computed landing
//     regardless of walls hit)
//   - Timeout-based despawn if landing coord not reachable
//
// Motion:
//   - Init reads actor.world.rot.y (spawner-provided aim yaw)
//   - Constant velocity along that yaw at kSeedProjectileSpeed
//   - Time-of-flight = distance-to-landing / speed
//   - At timeout, calls Anchor_Enhance_EnDekubaba_OnSeedLanded on
//     the parent Dekubaba (via actor.parent pointer)
//
// Model: reuses object_dekunuts flower/nut geometry per user C1
// answer. Visual is a small nut/seed sprite spinning as it flies.
//
// Registered via ActorDB::AddBuiltInCustomActors per Pitfall 23.
// Actor id extern `gEnDekubabaSeedId` declared in soh/src/code/z_play.c.

struct EnDekubabaSeed;

typedef struct EnDekubabaSeed {
    /* 0x0000 */ Actor    actor;
    /* 0x014C */ Vec3f    landingTarget;   // pre-computed spawn target
    /* 0x0158 */ s32      timeToLandFrames; // countdown to landing
    /* 0x015C */ s32      lifetimeFrames;   // hard-cap timeout
    /* 0x0160 */ Actor*   parentDekubaba;   // for on-land callback
} EnDekubabaSeed;

#ifdef __cplusplus
extern "C" {
#endif

extern s16 gEnDekubabaSeedId;

void EnDekubabaSeed_Init(Actor* thisx, PlayState* play);
void EnDekubabaSeed_Destroy(Actor* thisx, PlayState* play);
void EnDekubabaSeed_Update(Actor* thisx, PlayState* play);
void EnDekubabaSeed_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
