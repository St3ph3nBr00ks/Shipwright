#ifndef Z_EN_FD_H
#define Z_EN_FD_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnFd;

// Param names omitted — `this` is a reserved keyword in C++ and this
// header is transitively included from C++ TUs (DamageEnemy.cpp /
// HookHandlers.cpp / EnemyState.cpp). Per Pitfall 1.
typedef void (*EnFdActionFunc)(struct EnFd*, PlayState*);

typedef enum {
    FD_EFFECT_NONE,
    FD_EFFECT_FLAME,
    FD_EFFECT_DOT
} FDEffectType;

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
} EnFdEffect; // size = 0x38

typedef struct EnFd {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnFdActionFunc actionFunc;
    /* 0x0194 */ ColliderJntSph collider;
    /* 0x01B4 */ ColliderJntSphElement colSphs[12];
    /* 0x04B4 */ u8 coreActive;
    /* 0x04B6 */ s16 initYawToInitPos;
    /* 0x04B8 */ s16 curYawToInitPos;
    /* 0x04BA */ s16 runDir;
    /* 0x04BC */ s16 firstUpdateFlag;
    /* 0x04BE */ s16 spinTimer;
    /* 0x04C0 */ s16 circlesToComplete;
    /* 0x04C2 */ s16 invincibilityTimer;
    /* 0x04C4 */ s16 attackTimer;
    /* 0x04C8 */ f32 runRadius;
    /* 0x04CC */ f32 fadeAlpha;
    /* 0x04D0 */ Vec3f corePos;
    /* 0x04DC */ Vec3s jointTable[27];
    /* 0x057E */ Vec3s morphTable[27];
    /* 0x0620 */ EnFdEffect effects[200];
} EnFd; // size = 0x31E0

// Anchor multiplayer state-machine sync (En_Fd — Flare Dancer enflamed
// shell). `this` is a C++ keyword. This header is transitively included
// from C++ TUs (EnemyState.cpp / HookHandlers.cpp), so the param name
// in the declaration uses `actor` instead. The implementations in
// z_en_fd.c keep `this` per OoT decomp convention. See Pitfall 1.
void EnFd_SetupDyingNet(struct EnFd* actor, PlayState* play);
s16  EnFd_GetStateIndex(struct EnFd* actor);
void EnFd_ApplyNetState(struct EnFd* actor, s16 stateIndex);

// Receiver-side suppression predicate — reserved for API consistency
// with sibling per-enemy sync plans. En_Fd itself does NOT call
// Item_DropCollectibleRandom — the actual loot drop happens in
// ACTOR_EN_FW (the Flare Dancer core/wisp child spawned via
// EnFd_SpawnCore at z_en_fd.c:222). En_Fw's drop call at
// z_en_fw.c:271 will gate via its own Anchor_ShouldSuppressEnFwDrop
// in a follow-up per-actor sync pass when En_Fw is admitted.
// This predicate is currently unused at the actor-side call sites
// but is exposed for symmetry with sibling per-enemy sync plans and
// as a future hook if a drop call is ever added to the En_Fd death
// cycle. Defined extern "C" in Bridge/EnemySyncBridge.cpp.
#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressEnFdDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
