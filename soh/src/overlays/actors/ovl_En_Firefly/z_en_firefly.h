#ifndef Z_EN_FIREFLY_H
#define Z_EN_FIREFLY_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnFirefly;

typedef void (*EnFireflyActionFunc)(struct EnFirefly*, PlayState*);

typedef struct EnFirefly {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ Vec3f bodyPartsPos[3];
    /* 0x0170 */ SkelAnime skelAnime;
    /* 0x01B4 */ EnFireflyActionFunc actionFunc;
    /* 0x01B8 */ u8 auraType;
    /* 0x01B9 */ u8 onFire;
    /* 0x01BA */ s16 timer;
    /* 0x01BC */ s16 targetPitch;
    /* 0x01BE */ Vec3s jointTable[28];
    /* 0x0266 */ Vec3s morphTable[28];
    /* 0x0310 */ f32 maxAltitude;
    /* 0x0314 */ ColliderJntSph collider;
    /* 0x0344 */ ColliderJntSphElement colliderItems[1];
} EnFirefly; // size = 0x0374

typedef enum {
    /* 0 */ KEESE_FIRE_FLY,
    /* 1 */ KEESE_FIRE_PERCH,
    /* 2 */ KEESE_NORMAL_FLY,
    /* 3 */ KEESE_NORMAL_PERCH,
    /* 4 */ KEESE_ICE_FLY
} KeeseType;

// Anchor multiplayer state-machine sync (#47 / en_firefly_sync_plan.md).
// Despite the file name, this actor is the Keese (bat) — not a firefly.
// `this` is a C++ keyword. This header is transitively included from
// C++ TUs (EnemyState.cpp etc.), so the param name in the declaration
// uses `actor` instead. The implementations in z_en_firefly.c keep
// `this` per OoT decomp convention.
void EnFirefly_SetupDyingNet(struct EnFirefly* actor, PlayState* play);
s16  EnFirefly_GetStateIndex(struct EnFirefly* actor);
void EnFirefly_ApplyNetState(struct EnFirefly* actor, s16 stateIndex);

// Receiver-side suppression predicate — true when the current death
// cycle was network-driven (ENEMY_DEFEATED received and we're replaying
// the fall → die sequence). Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnFireflyDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
