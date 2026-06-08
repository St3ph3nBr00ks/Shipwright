#ifndef Z_EN_CROW_H
#define Z_EN_CROW_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnCrow;

typedef void (*EnCrowActionFunc)(struct EnCrow*, PlayState*);

typedef struct EnCrow {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ Vec3f bodyPartsPos[4];
    /* 0x017C */ SkelAnime skelAnime;
    /* 0x01C0 */ EnCrowActionFunc actionFunc;
    /* 0x01C4 */ s16 timer;
    /* 0x01C6 */ s16 aimRotX;
    /* 0x01C8 */ s16 aimRotY;
    /* 0x01CA */ Vec3s jointTable[9];
    /* 0x0200 */ Vec3s morphTable[9];
    /* 0x0238 */ ColliderJntSph collider;
    /* 0x0258 */ ColliderJntSphElement colliderItems[1];
} EnCrow; // size = 0x0298

// Anchor multiplayer state-machine sync (en_crow_sync_plan / mirror of
// en_firefly_sync_plan.md §4 pattern). En_Crow is the Guay — flying bird
// enemy that dives at the player. `this` is a C++ keyword. This header
// is transitively included from C++ TUs (EnemyState.cpp etc.), so the
// param name in the declaration uses `actor` instead per Pitfall 1.
// Implementations in z_en_crow.c keep `this` per OoT decomp convention.
void EnCrow_SetupDyingNet(struct EnCrow* actor, PlayState* play);
s16  EnCrow_GetStateIndex(struct EnCrow* actor);
void EnCrow_ApplyNetState(struct EnCrow* actor, s16 stateIndex);

// Receiver-side suppression predicate — true when the current death
// cycle was network-driven (ENEMY_DEFEATED received and we're replaying
// the Damaged -> Die sequence). EnCrow_Die uses this to skip
// Item_DropCollectibleRandom so host's authoritative ITEM_DROP_SYNC
// isn't double-applied. Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnCrowDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
