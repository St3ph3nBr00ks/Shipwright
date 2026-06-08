#ifndef Z_EN_FW_H
#define Z_EN_FW_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnFw;

// Param names omitted — `this` is a reserved keyword in C++ and this
// header is transitively included from C++ TUs (DamageEnemy.cpp /
// HookHandlers.cpp / EnemyState.cpp). Per Pitfall 1.
typedef void (*EnFwActionFunc)(struct EnFw*, PlayState*);

typedef struct {
    /* 0x0000 */ u8 type;
    /* 0x0001 */ u8 timer;
    /* 0x0002 */ u8 initialTimer;
    /* 0x0004 */ f32 scale;
    /* 0x0008 */ f32 scaleStep;
    /* 0x000C */ Color_RGBA8 color;
    /* 0x0010 */ char unk_10[4];
    /* 0x0014 */ Vec3f pos;
    /* 0x0020 */ Vec3f velocity;
    /* 0x002C */ Vec3f accel;
                 u32 epoch;
} EnFwEffect;

typedef struct EnFw {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnFwActionFunc actionFunc;
    /* 0x0194 */ ColliderJntSph collider;
    /* 0x01B4 */ ColliderJntSphElement sphs[1];
    /* 0x01F4 */ Vec3f bompPos;
    /* 0x0200 */ u8 lastDmgHook;
    /* 0x0202 */ s16 runDirection;
    /* 0x0204 */ s16 bounceCnt;
    /* 0x0206 */ char unk_206[0x2];
    /* 0x0208 */ s16 damageTimer;
    /* 0x020A */ s16 explosionTimer;
    /* 0x020C */ char unk_20C[0x2];
    /* 0x020E */ s16 slideTimer;
    /* 0x0210 */ s16 slideSfxTimer;
    /* 0x0212 */ s16 returnToParentTimer;
    /* 0x0214 */ s16 turnAround;
    /* 0x0218 */ f32 runRadius;
    /* 0x021C */ Vec3s jointTable[11];
    /* 0x025E */ Vec3s morphTable[11];
    /* 0x02A0 */ EnFwEffect effects[20];
} EnFw; // size = 0x0700

// Anchor multiplayer state-machine sync (En_Fw — Flare Dancer core/wisp
// child of En_Fd). `this` is a C++ keyword. This header is transitively
// included from C++ TUs (EnemyState.cpp / HookHandlers.cpp), so the
// param name in the declaration uses `actor` instead. The implementations
// in z_en_fw.c keep `this` per OoT decomp convention. See Pitfall 1.
void EnFw_SetupDyingNet(struct EnFw* actor, PlayState* play);
s16  EnFw_GetStateIndex(struct EnFw* actor);
void EnFw_ApplyNetState(struct EnFw* actor, s16 stateIndex);

// Receiver-side suppression predicate — true when the current En_Fw
// death cycle was network-driven (peer received ENEMY_DEFEATED and is
// replaying the explosion-countdown sequence inside EnFw_Run). The
// random 0xA0 drop call at z_en_fw.c:271 is gated by this so host's
// authoritative ITEM_DROP_SYNC isn't double-applied. Mirrors
// Anchor_ShouldSuppressEnStDrop. Defined extern "C" in
// Bridge/EnemySyncBridge.cpp. The bomb spawn at line 264 and the
// FLG_COREDEAD bit-set at line 270 (signal-back to local En_Fd parent)
// are intentionally NOT gated — bombs are per-client (En_Bom has its
// own sync) and FLG_COREDEAD fires on each client's local En_Fd parent.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnFwDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
