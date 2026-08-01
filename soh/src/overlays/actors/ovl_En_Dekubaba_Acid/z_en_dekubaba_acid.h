#ifndef Z_EN_DEKUBABA_ACID_H
#define Z_EN_DEKUBABA_ACID_H

#include <libultraship/libultra.h>
#include "global.h"

// Pillar 5 (GH #308) — acid vomit projectile actor spawned by
// enhanced EnDekubaba during acid-vomit attack state.
//
// Ballistic arc from actor.world.pos (which the spawner sets to the
// Dekubaba's head position at fire moment). Straight-line XZ velocity
// derived from the spawn yaw (params-encoded) + downward-arcing Y
// under Actor_MoveXZGravity. Cylinder AT collider damages any player
// within radius. Life terminated by ground contact, wall contact, or
// timeout.
//
// Registered via ActorDB::AddBuiltInCustomActors per Pitfall 23.
// Actor id extern `gEnDekubabaAcidId` declared in soh/src/code/z_play.c
// alongside the other custom actor ids.

struct EnDekubabaAcid;

typedef struct EnDekubabaAcid {
    /* 0x0000 */ Actor            actor;
    /* 0x014C */ ColliderCylinder collider;        // AT-typed damages players
    /* 0x0198 */ s32              lifetimeFrames;  // counts down; despawns at 0
} EnDekubabaAcid;

#ifdef __cplusplus
extern "C" {
#endif

extern s16 gEnDekubabaAcidId;

void EnDekubabaAcid_Init(Actor* thisx, PlayState* play);
void EnDekubabaAcid_Destroy(Actor* thisx, PlayState* play);
void EnDekubabaAcid_Update(Actor* thisx, PlayState* play);
void EnDekubabaAcid_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
