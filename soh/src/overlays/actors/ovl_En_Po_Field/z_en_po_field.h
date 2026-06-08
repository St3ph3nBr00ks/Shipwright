#ifndef Z_EN_PO_FIELD_H
#define Z_EN_PO_FIELD_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnPoField;

typedef void (*EnPoFieldActionFunc)(struct EnPoField*, PlayState*);

typedef enum {
    EN_PO_FIELD_SMALL,
    EN_PO_FIELD_BIG
} EnPoFieldSize;

typedef struct {
    /* 0x0000 */ Color_RGB8 primColor;
    /* 0x0003 */ Color_RGB8 lightColor;
    /* 0x0006 */ Color_RGB8 envColor;
    /* 0x0009 */ s8 unk_9;
    /* 0x000C */ void* soulTexture;
} EnPoFieldInfo; // size = 0x10

typedef struct EnPoField {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnPoFieldActionFunc actionFunc;
    /* 0x0194 */ u8 unk_194;
    /* 0x0195 */ u8 spawnFlagIndex;
    /* 0x0196 */ s16 actionTimer;
    /* 0x0198 */ s16 flameRotation;
    /* 0x019A */ s16 flameTimer;
    /* 0x019C */ Vec3s jointTable[10];
    /* 0x01D8 */ Vec3s morphTable[10];
    /* 0x0214 */ Color_RGBA8 lightColor;
    /* 0x0218 */ Color_RGBA8 soulColor;
    /* 0x021C */ f32 scaleModifier;
    /* 0x0220 */ f32 flameScale;
    /* 0x0224 */ Vec3f flamePosition;
    /* 0x0230 */ LightNode* lightNode;
    /* 0x0234 */ LightInfo lightInfo;
    /* 0x0244 */ ColliderCylinder collider;
    /* 0x0290 */ ColliderCylinder flameCollider;
} EnPoField; // size = 0x02DC

// Anchor multiplayer state-machine sync (Field Poe).
// `this` is a C++ keyword. This header is transitively included from
// C++ TUs (EnemyState.cpp / HookHandlers.cpp), so the param name in
// the declaration uses `actor` instead. The implementations in
// z_en_po_field.c keep `this` per OoT decomp convention.
void EnPoField_SetupDyingNet(struct EnPoField* actor, PlayState* play);
s16  EnPoField_GetStateIndex(struct EnPoField* actor);
void EnPoField_ApplyNetState(struct EnPoField* actor, s16 stateIndex);

// Receiver-side suppression predicate — reserved for symmetry with
// other per-enemy sync plans. En_Po_Field's death cycle does NOT call
// Item_DropCollectibleRandom; the Big-Poe / regular-Poe soul is
// awarded via per-client `Item_Give(ITEM_POE / ITEM_BIG_POE)` during
// the soul-talk bottle interaction (z_en_po_field.c:713,717) rather
// than the EN_ITEM00 drop chain. This predicate is currently unused
// at the actor-side call sites but is exposed for symmetry with
// sibling per-enemy sync plans and as a future hook if a drop call
// is ever added to the death cycle.
// Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnPoFieldDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
