#ifndef Z_EN_BILI_H
#define Z_EN_BILI_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnBili;

typedef void (*EnBiliActionFunc)(struct EnBili*, PlayState*);

typedef enum {
    /* 0 */ EN_BILI_LIMB_NONE,
    /* 1 */ EN_BILI_LIMB_ROOT,
    /* 2 */ EN_BILI_LIMB_INNER_HOOD,
    /* 3 */ EN_BILI_LIMB_OUTER_HOOD,
    /* 4 */ EN_BILI_LIMB_TENTACLES,
    /* 5 */ EN_BILI_LIMB_MAX
} EnBiliLimb;

typedef struct EnBili {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnBiliActionFunc actionFunc;
    /* 0x0194 */ u8 tentaclesTexIndex;
    /* 0x0195 */ u8 playFlySound;
    /* 0x0196 */ s16 timer;
    /* 0x0198 */ Vec3s jointTable[EN_BILI_LIMB_MAX];
    /* 0x01B6 */ Vec3s morphTable[EN_BILI_LIMB_MAX];
    /* 0x01D4 */ ColliderCylinder collider;
} EnBili; // size = 0x0220

typedef enum {
    /* -1 */ EN_BILI_TYPE_NORMAL = -1,
    /*  0 */ EN_BILI_TYPE_VALI_SPAWNED,
    /*  1 */ EN_BILI_TYPE_DYING
} EnBiliType;

// Anchor multiplayer state-machine sync (#128 / en_bili_sync_plan.md).
// `this` is a C++ keyword. This header is transitively included from
// C++ TUs (EnemyState.cpp etc.), so the param name in the declaration
// uses `actor` instead. The implementations in z_en_bili.c keep
// `this` per OoT decomp convention.
void EnBili_SetupDyingNet(struct EnBili* actor, PlayState* play);
s16  EnBili_GetStateIndex(struct EnBili* actor);
void EnBili_ApplyNetState(struct EnBili* actor, s16 stateIndex);

// Receiver-side suppression predicate — true when the current death
// cycle was network-driven (ENEMY_DEFEATED received and we're replaying
// the burnt → die sequence). Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnBiliDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
