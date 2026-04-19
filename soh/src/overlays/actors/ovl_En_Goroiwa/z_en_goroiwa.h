#ifndef Z_EN_GOROIWA_H
#define Z_EN_GOROIWA_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnGoroiwa;

typedef void (*EnGoroiwaActionFunc)(struct EnGoroiwa*, PlayState*);

typedef struct EnGoroiwa {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ EnGoroiwaActionFunc actionFunc;
    /* 0x0150 */ ColliderJntSph collider;
    /* 0x0170 */ ColliderJntSphElement colliderItems[1];
    /* 0x01B0 */ Vec3f prevUnitRollAxis;
    /* 0x01BC */ f32 prevRollAngleDiff;
    /* 0x01C0 */ f32 rollRotSpeed;
    /* 0x01C4 */ s16 waitTimer;
    /* 0x01C6 */ s16 bounceCount;
    /* 0x01C8 */ s16 collisionDisabledTimer;
    /* 0x01CA */ s16 endWaypoint;
    /* 0x01CC */ s16 currentWaypoint;
    /* 0x01CE */ s16 nextWaypoint;
    /* 0x01D0 */ s16 pathDirection;
    /* 0x01D2 */ u8 isInKokiri;
    /* 0x01D3 */ u8 stateFlags;
} EnGoroiwa; // size = 0x01D4

// Multiplayer sync helpers — see issue #153.
// State index maps to action function: 0=Roll, 1=MoveAndFallToGround, 2=Wait, 3=MoveUp, 4=MoveDown.
// Parameter named `thisx` (not `this`) because this header is included from C++ translation
// units (HookHandlers.cpp, EnemyUpdate.cpp) where `this` is a reserved word. The .c file
// can still use `this` — parameter names in declarations are cosmetic, only the type matters.
s16 EnGoroiwa_GetStateIndex(EnGoroiwa* thisx);
void EnGoroiwa_ApplyNetState(EnGoroiwa* thisx, PlayState* play, s16 stateIndex);

// Host-side handler for ENEMY_HIT_PLAYER packet (non-host→host). Applies the
// reverse-direction + PLAYER_IN_THE_WAY state transition WITHOUT damaging
// host's local Link (the non-host's Link is the one that was actually hit).
// Safe to call any time; silently no-ops when boulder is not in Roll state.
void EnGoroiwa_ProcessRemoteHit(EnGoroiwa* thisx, PlayState* play);

#endif
