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
void EnHintnuts_ApplyNetState(struct EnHintnuts* actor, PlayState* play, s16 stateIndex);

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
// Test 199 fix: peer→host nutsball-hit propagation. Called from
// EnHintnuts_ColliderCheck when a reflected nutsball lands on the local
// hintnut. On non-host, sends an ENEMY_HIT_PLAYER packet so the host can
// fire HitByScrubProjectile1+2 on its authoritative hintnut, advancing
// sPuzzleCounter and broadcasting the resulting state transition. No-op
// on host (host's local HitByScrubProjectile already runs naturally).
void Anchor_NotifyHintnutsNutsballHit(struct Actor* hintnut);

// Test 199 fix: host-side application of a peer-reported nutsball hit.
// Wraps the (file-static) HitByScrubProjectile1 + HitByScrubProjectile2
// pair so the receive handler in Packets/EnemyHitPlayer.cpp can invoke
// them on the host's authoritative hintnut without duplicating their
// bodies into C++.
void EnHintnuts_ProcessRemoteNutsballHit(struct EnHintnuts* hintnut, PlayState* play);

// Test 200 fix: sPuzzleCounter accessor pair for ENEMY_STATE host-
// authoritative sync. The puzzle counter is a file-static global
// modified locally by HitByScrubProjectile2 (on hit) and SetupFreeze
// (the -3 → -4 wrong-order bump). Peer increments only on its own
// local hits, but the natural reset path inside EnHintnuts_Freeze
// requires sPuzzleCounter == -4 — without sync, peer never reaches
// -4 (its host-side propagated hits don't bump peer's local counter)
// and its hintnuts stay frozen indefinitely while host's reset.
//
// Host stamps GetPuzzleCounter() into ENEMY_STATE every broadcast for
// hintnuts; peer's HandlePacket_EnemyUpdate calls SetPuzzleCounter()
// to overwrite local. After the natural Freeze→Wait reset on host,
// host's broadcast carries -4 (or 0 post-reset, depending on timing),
// and peer's local Freeze actionFunc sees the same value, fires the
// same reset path, and the actor lifts back up to home.
s16  EnHintnuts_GetPuzzleCounter(void);
void EnHintnuts_SetPuzzleCounter(s16 value);
#ifdef __cplusplus
}
#endif

#endif
