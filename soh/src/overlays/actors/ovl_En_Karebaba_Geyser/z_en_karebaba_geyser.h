#ifndef Z_EN_KAREBABA_GEYSER_H
#define Z_EN_KAREBABA_GEYSER_H

#include <libultraship/libultra.h>
#include "global.h"

// Pillar 5 (GH #310) — geyser plume actor spawned by enhanced
// EnKarebaba during Spin state.
//
// Vertical acid plume from actor.world.pos (which the spawner sets to
// the Karebaba's home.pos). Cylinder AT collider damages any player
// within radius for ~30-frame lifetime. Green sparkle particles
// spawned per-frame provide the visual — no mesh, no SkelAnime, no
// object dependency beyond gameplay_keep for particle DLs.
//
// Registered via ActorDB::AddBuiltInCustomActors per Pitfall 23.
// Actor id extern `gEnKarebabaGeyserId` declared in soh/src/code/z_play.c
// alongside gEnFollowerId / gEnInvaderId / gEnTeamMarkerId.

struct EnKarebabaGeyser;

typedef struct EnKarebabaGeyser {
    /* 0x0000 */ Actor           actor;
    /* 0x014C */ ColliderCylinder collider;    // AT-typed damages players
    /* 0x0198 */ s32              lifetimeFrames;  // counts down from ~30
} EnKarebabaGeyser;

#ifdef __cplusplus
extern "C" {
#endif

extern s16 gEnKarebabaGeyserId;

void EnKarebabaGeyser_Init(Actor* thisx, PlayState* play);
void EnKarebabaGeyser_Destroy(Actor* thisx, PlayState* play);
void EnKarebabaGeyser_Update(Actor* thisx, PlayState* play);
void EnKarebabaGeyser_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
