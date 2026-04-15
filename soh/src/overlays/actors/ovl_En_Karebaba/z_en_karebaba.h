#ifndef Z_EN_KAREBABA_H
#define Z_EN_KAREBABA_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnKarebaba;

typedef void (*EnKarebabaActionFunc)(struct EnKarebaba*, PlayState*);

typedef struct EnKarebaba {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnKarebabaActionFunc actionFunc;
    /* 0x0194 */ Vec3s jointTable[8];
    /* 0x01C4 */ Vec3s morphTable[8];
    /* 0x01F4 */ CollisionPoly* boundFloor;
    /* 0x01F8 */ ColliderCylinder headCollider;
    /* 0x0244 */ ColliderCylinder bodyCollider;
} EnKarebaba; // size = 0x0290

// Network helpers — called from EnemyUpdate.cpp and HookHandlers.cpp.
// GetStateIndex maps the actionFunc pointer to an integer index (0–9).
// ApplyNetState drives the Karebaba into a given state without firing
// game-logic side effects (no OnEnemyDefeat, no item drops).
// SetupDyingNet is the network-safe death trigger: same as SetupDying but
// without GameInteractor_ExecuteOnEnemyDefeat, used by HandlePacket_EnemyDefeated
// on non-host clients to start the natural death→respawn cycle.
s16  EnKarebaba_GetStateIndex(struct EnKarebaba* actor);
void EnKarebaba_ApplyNetState(struct EnKarebaba* actor, s16 stateIndex, s16 params);
void EnKarebaba_SetupDyingNet(struct EnKarebaba* actor);
// SetupDeadItemDrop is the public entry point used by the pendingKillNetIds path
// (Fix 34) to skip the Dying animation and jump directly to the dead-countdown
// phase in ACTORCAT_MISC, preventing premature respawn detection in OnActorUpdate.
void EnKarebaba_SetupDeadItemDrop(struct EnKarebaba* actor, PlayState* play);
// SetupRegrowNet skips the remaining DeadItemDrop/Dead countdown and jumps
// directly to Regrow, called by HandlePacket_EnemyRespawn on non-host clients.
void EnKarebaba_SetupRegrowNet(struct EnKarebaba* actor);

#endif
