#ifndef Z_EN_BIGOKUTA_H
#define Z_EN_BIGOKUTA_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnBigokuta;

typedef void (*EnBigokutaActionFunc)(struct EnBigokuta*, PlayState*);

typedef struct EnBigokuta {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnBigokutaActionFunc actionFunc;
    /* 0x0194 */ s8 unk_194;
    /* 0x0195 */ u8 unk_195;
    /* 0x0196 */ s16 unk_196;
    /* 0x0198 */ s16 unk_198;
    /* 0x019A */ s16 unk_19A;
    /* 0x019C */ Vec3s jointTable[20];
    /* 0x0214 */ Vec3s morphTable[20];
    /* 0x028C */ ColliderJntSph collider;
    /* 0x02AC */ ColliderJntSphElement element;
    /* 0x02EC */ ColliderCylinder cylinder[2];
} EnBigokuta; // size = 0x0384

// Anchor multiplayer state-machine sync (#130 / en_bigokuta sync work).
// `this` is a C++ keyword. This header is transitively included from
// C++ TUs (EnemyState.cpp / HookHandlers.cpp), so the param name in
// the declaration uses `actor` instead. The implementations in
// z_en_bigokuta.c keep `this` per OoT decomp convention.
void EnBigokuta_SetupDyingNet(struct EnBigokuta* actor, PlayState* play);
s16  EnBigokuta_GetStateIndex(struct EnBigokuta* actor);
void EnBigokuta_ApplyNetState(struct EnBigokuta* actor, s16 stateIndex);

// Receiver-side suppression predicate — true when the current death
// cycle was network-driven (ENEMY_DEFEATED received and we're replaying
// the death-anim → shrink → heart-container-reveal sequence). Used by
// the death actionFunc (func_809BE26C) to skip the random item drop
// (host's authoritative ITEM_DROP_SYNC isn't double-applied). The
// Flags_SetClear room-clear flag still fires per-client (cooperative
// heart container reveal).
// Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnBigokutaDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
