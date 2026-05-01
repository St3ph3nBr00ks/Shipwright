#ifndef Z_EN_HINTNUTS_H
#define Z_EN_HINTNUTS_H

#include <libultraship/libultra.h>
#include "global.h"

struct EnHintnuts;

typedef void (*EnHintnutsActionFunc)(struct EnHintnuts*, PlayState*);

typedef struct EnHintnuts {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ EnHintnutsActionFunc actionFunc;
    /* 0x0194 */ s16 animFlagAndTimer; // 0x1000 bit denotes that projectile has been thrown
    /* 0x0196 */ s16 unk_196;
    /* 0x0198 */ u16 textIdCopy;
    /* 0x019A */ Vec3s jointTable[10];
    /* 0x01D6 */ Vec3s morphTable[10];
    /* 0x0214 */ ColliderCylinder collider;
} EnHintnuts; // size = 0x0260

// Anchor multiplayer state-machine sync (en_hintnuts_sync — Inside Deku
// Tree Compound Room puzzle scrubs). Mirrors EnDekunuts pattern from
// #135 (commit b3620eb2d), but the actor is ACTOR_EN_HINTNUTS (0x0192 =
// 402), distinct from ACTOR_EN_DEKUNUTS (0x0060 = 96). Header decls use
// `actor` per the same C++-keyword convention as the other recent
// per-actor sync work.
//
// State encoding (1 byte on the wire):
//   0 = Wait              (idle, burrowed)
//   1 = LookAround        (peeking, far from player)
//   2 = Stand             (exposed, awaiting throw decision)
//   3 = ThrowNut          (firing En_Nutsball projectile)
//   4 = Burrow            (going underground)
//   5 = BeginRun          (unburrow after wrong-projectile reflect)
//   6 = Run               (fleeing post-wrong-hit)
//   7 = Talk              (post-defeat dialog)        — local-only, gated
//   8 = Leave             (post-talk, spawns recovery heart) — local-only, gated
//   9 = Freeze            (post-correct-hit kill)     — local-only, gated
//  10 = BeginFreeze       (transition to Freeze)      — local-only, gated
//  -1 = unknown
//
// States 7-10 are sPuzzleCounter-driven; not safely re-applicable from
// outside the actor's natural collision flow (BeginFreeze sets
// sPuzzleCounter via HitByScrubProjectile2). On the receive side these
// are blocked from ApplyNetState; local logic drives the death-class
// transitions naturally because both clients spawn matching En_Nutsball
// projectiles (admitted to sync) and apply the same hits independently.
s16  EnHintnuts_GetStateIndex(struct EnHintnuts* actor);
void EnHintnuts_ApplyNetState(struct EnHintnuts* actor, s16 stateIndex);

// Receive-side death cycle (KB-16 / SetupDyingNet pattern). Called from
// HandlePacket_EnemyDefeated instead of generic Actor_Kill so peer plays
// the natural Run-and-Leave animation (the puzzle's "scrub runs off
// stage after correct reflect" sequence) rather than disappearing
// instantly. Routes through SetupLeave with a Freeze-state guard.
void EnHintnuts_SetupDyingNet(struct EnHintnuts* actor, PlayState* play);

#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressHintnutsDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
