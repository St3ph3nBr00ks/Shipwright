#ifndef Z_EN_FOLLOWER_H
#define Z_EN_FOLLOWER_H

#include <libultraship/libultra.h>
#include "global.h"

// SoH NPC Follower companion (Flotilla — see Plans/npc_follower_plan.md).
//
// v1 scope:
//   - Registers as a custom actor via ActorDB (dynamic ID at runtime).
//   - Spawned on CVar toggle gEnhancements.AI.FollowerNPC.Enabled (Phase 2).
//   - Pure pathfinding (v1 has no combat; invulnerable).
//   - Uses Link's skel for animation richness (Phase 1.5 rendering work).
//   - Locomotion via Actor_MoveXZGravity + direct world.pos writes
//     (NOT stick injection — this NPC is the Invader's locomotion testbed).
//
// State machine (5 states defined; v1 implements IDLE / FOLLOW / CLIMBING /
// STUCK; DEAD is a reserved enum slot for v2 combat work).
typedef enum {
    EN_FOLLOWER_STATE_IDLE     = 0,
    EN_FOLLOWER_STATE_FOLLOW   = 1,
    EN_FOLLOWER_STATE_CLIMBING = 2,
    EN_FOLLOWER_STATE_STUCK    = 3,
    EN_FOLLOWER_STATE_DEAD     = 4,
} EnFollowerAIState;

struct EnFollower;

typedef void (*EnFollowerActionFunc)(struct EnFollower*, PlayState*);

typedef struct EnFollower {
    /* 0x000 */ Actor actor;

    // Link skel runtime state. Link's skel has 21 limbs (child + adult share
    // the same count); jointTable/morphTable sized accordingly. Phase 1.5
    // will initialize the SkelAnime; Phase 1 scaffold leaves it zeroed.
    /* 0x14C */ SkelAnime skelAnime;
    /* 0x190 */ Vec3s     jointTable[21];
    /* 0x202 */ Vec3s     morphTable[21];

    // Owner-client bookkeeping. Set by Anchor at spawn time.
    /* 0x274 */ u32 ownerClientId;
    /* 0x278 */ u32 netId;

    // State machine.
    /* 0x27C */ s32                  state;
    /* 0x280 */ EnFollowerActionFunc actionFunc;

    // Linkage age — matches gSaveContext.linkAge at spawn time. Drives skel
    // variant selection.
    /* 0x288 */ s8 linkAge;

    // v2 reserved (combat redesign): health, downed timer, etc. Sized to
    // ensure forward-compat with the FOLLOWER_NPC_STATE packet schema's
    // `health` + `deathFlag` fields without struct migration later.
    /* 0x289 */ s8 reservedHealth;
    /* 0x28A */ u8 reservedDeathFlag;
} EnFollower;

#ifdef __cplusplus
extern "C" {
#endif

// Dynamic actor id allocated by ActorDB::AddBuiltInCustomActors. Defined in
// soh/src/code/z_play.c alongside gEnPartnerId.
extern s16 gEnFollowerId;

void EnFollower_Init(Actor* thisx, PlayState* play);
void EnFollower_Destroy(Actor* thisx, PlayState* play);
void EnFollower_Update(Actor* thisx, PlayState* play);
void EnFollower_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
