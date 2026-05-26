#ifndef Z_EN_DEKUNUTS_H
#define Z_EN_DEKUNUTS_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnDekunuts;

typedef void (*EnDekunutsActionFunc)(struct EnDekunuts*, PlayState*);

typedef struct EnDekunuts {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnDekunutsActionFunc actionFunc;
    /* 0x0194 */ u8 playWalkSound;
    /* 0x0195 */ u8 runAwayCount;
    /* 0x0196 */ s16 animFlagAndTimer;
    /* 0x0198 */ s16 runDirection;
    /* 0x019A */ s16 shotsPerRound;
    /* 0x019C */ Vec3s jointTable[25];
    /* 0x0232 */ Vec3s morphTable[25];
    /* 0x02C8 */ ColliderCylinder collider;
} EnDekunuts; // size = 0x0314

// Anchor multiplayer state-machine sync (#135 / en_dekunuts_sync_plan.md).
// SetupDyingNet triggers the natural death animation on a non-host
// receiver without echoing GameInteractor_ExecuteOnEnemyDefeat.
// GetStateIndex / ApplyNetState mirror the EnDekubaba/Karebaba pattern.
// `this` is a C++ keyword — header decls use `actor`. Implementations
// in z_en_dekunuts.c keep `this` per OoT decomp convention.
void EnDekunuts_SetupDyingNet(struct EnDekunuts* actor, PlayState* play);
s16  EnDekunuts_GetStateIndex(struct EnDekunuts* actor);
void EnDekunuts_ApplyNetState(struct EnDekunuts* actor, s16 stateIndex);

#endif
