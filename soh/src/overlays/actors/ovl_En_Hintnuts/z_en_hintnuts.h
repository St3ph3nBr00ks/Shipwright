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
// Tree Compound Room puzzle scrubs). Header decls use `actor` per the
// C++-keyword convention shared with other per-actor sync work.
//
// State encoding (1 byte on the wire):
//   0 = Wait              (idle, burrowed)
//   1 = LookAround        (peeking, far from player)
//   2 = Stand             (exposed, awaiting throw decision)
//   3 = ThrowNut          (firing En_Nutsball projectile)
//   4 = Burrow            (going underground)
//   5 = BeginRun          (unburrow after wrong-projectile reflect)
//   6 = Run               (fleeing post-wrong-hit)
//   7 = Talk              (post-defeat dialog)
//   8 = Leave             (post-talk, runs off-stage and Actor_Kills)
//   9 = Freeze            (post-correct-hit kill)
//  10 = BeginFreeze       (transition to Freeze)
//  -1 = unknown
//
// Pillar A Phase 2 pattern: the scene host runs the state machine and
// broadcasts state via ENEMY_STATE; non-scene-host clients receive the
// state index and call ApplyNetState to drive their local actor through
// the matching Setup function. Every state 0-10 is replicated so the
// scene host's transitions (including post-hit BeginRun/BeginFreeze and
// the natural Freeze→Wait reset) propagate without depending on each
// client's local collision firing the same way.
//
// PlayState* parameter is required because state 8 (Leave) needs it to
// route through SetupLeave's anim + collider setup. SetupLeave's
// recovery-heart spawn is suppressed on receive (peer is mirroring the
// scene host's local Leave; the host's local heart already replicates
// via the standard EnItem00 spawn flow).
s16  EnHintnuts_GetStateIndex(struct EnHintnuts* actor);
void EnHintnuts_ApplyNetState(struct EnHintnuts* actor, PlayState* play, s16 stateIndex);

#ifdef __cplusplus
extern "C" {
#endif
bool Anchor_ShouldSuppressHintnutsDrop(struct Actor* actor);
#ifdef __cplusplus
}
#endif

#endif
