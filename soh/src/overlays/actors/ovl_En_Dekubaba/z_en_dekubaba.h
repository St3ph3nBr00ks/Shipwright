#ifndef Z_EN_DEKUBABA_H
#define Z_EN_DEKUBABA_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnDekubaba;

typedef void (*EnDekubabaActionFunc)(struct EnDekubaba*, PlayState*);

typedef enum {
    /* 0 */ DEKUBABA_NORMAL,
    /* 1 */ DEKUBABA_BIG
} DekuBabaType;

typedef struct EnDekubaba {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ Vec3f bodyPartsPos[4];
    /* 0x017C */ SkelAnime skelAnime;
    /* 0x01C0 */ EnDekubabaActionFunc actionFunc;
    /* 0x01C4 */ char pad[0x2];
    /* 0x01C6 */ s16 timer;
    /* 0x01C8 */ s16 targetSwayAngle;
    /* 0x01CA */ s16 stemSectionAngle[3]; // Used to calculate the position of the stem sections and head with spherical trigonometry
    /* 0x01D0 */ Vec3s jointTable[8];
    /* 0x0200 */ Vec3s morphTable[8];
    /* 0x0230 */ f32 size; // Used everywhere to rescale offsets etc. for Big ones
    /* 0x0234 */ CollisionPoly* boundFloor;
    /* 0x0238 */ ColliderJntSph collider;
    /* 0x0258 */ ColliderJntSphElement colliderElements[7];
} EnDekubaba; // size = 0x0418

// Anchor multiplayer state-machine sync (KB-08 / #7).
s16  EnDekubaba_GetStateIndex(struct EnDekubaba* actor);
void EnDekubaba_ApplyNetState(struct EnDekubaba* actor, s16 stateIndex);

// Receive-side death cycle. Called from HandlePacket_EnemyDefeated
// instead of Actor_Kill so peer plays the natural death animation:
//   Path A (small Babas + most kills): ShrinkDie animation, drops
//          Deku Nuts (1× small / 3× big).
//   Path B (big Babas struck mid-StunnedVertical): PrunedSomersault
//          → DeadStickDrop, severed head somersaults to ground +
//          drops a Deku Stick.
void EnDekubaba_SetupDyingNet(struct EnDekubaba* actor, PlayState* play);

#endif
