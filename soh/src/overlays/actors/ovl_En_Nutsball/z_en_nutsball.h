#ifndef Z_EN_NUTSBALL_H
#define Z_EN_NUTSBALL_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnNutsball;

typedef void (*EnNutsballActionFunc)(struct EnNutsball*, PlayState*);

typedef struct EnNutsball {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ EnNutsballActionFunc actionFunc;
    /* 0x0150 */ s8 objBankIndex;
    /* 0x0152 */ s16 timer;
    /* 0x0154 */ ColliderCylinder collider;
    // SoH multiplayer diagnostics. The spawning enemy stamps its own
    // params here just after Actor_Spawn returns so collision-event logs
    // can identify which scrub fired which nut. The vanilla nutsball
    // doesn't otherwise know its origin. Default -1 (unset) for safety —
    // legitimate scrub params are 0/1/2/3/0xA so -1 is unambiguous.
    /* 0x01A0 */ s16 parentParams;
} EnNutsball; // size = 0x01A4

#endif
