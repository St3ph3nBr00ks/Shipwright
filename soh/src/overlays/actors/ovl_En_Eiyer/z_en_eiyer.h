#ifndef Z_EN_EIYER_H
#define Z_EN_EIYER_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnEiyer;

typedef void (*EnEiyerActionFunc)(struct EnEiyer*, PlayState*);

typedef struct EnEiyer {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelanime;
    /* 0x0190 */ EnEiyerActionFunc actionFunc;
    /* 0x0194 */ s16 timer;
    /* 0x0196 */ s16 targetYaw;
    /* 0x0198 */ Vec3s jointTable[19];
    /* 0x020A */ Vec3s morphTable[19];
    /* 0x027C */ Vec3f basePos;
    /* 0x0288 */ ColliderCylinder collider;
} EnEiyer; // size = 0x02D4

// Anchor multiplayer state-machine sync (#137 / en_eiyer_sync_plan).
// `this` is a C++ keyword. This header is transitively included from
// C++ TUs (EnemyState.cpp, HookHandlers.cpp), so the param name in the
// declaration uses `actor`. Implementations in z_en_eiyer.c keep `this`
// per OoT decomp convention.
s16  EnEiyer_GetStateIndex(struct EnEiyer* actor);
void EnEiyer_ApplyNetState(struct EnEiyer* actor, PlayState* play, s16 stateIndex);

#endif
