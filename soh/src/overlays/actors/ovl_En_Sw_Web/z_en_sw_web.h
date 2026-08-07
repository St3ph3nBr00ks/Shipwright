#ifndef Z_EN_SW_WEB_H
#define Z_EN_SW_WEB_H

#include <libultraship/libultra.h>
#include "global.h"

// GH #333 — stun/web projectile fired by enhanced En_Sw (Skullwalltula)
// during the WebAttack state. Straight-line motion toward the target
// (no gravity, no arc — mirrors "web shot" thematic). Distance-check
// hit detection (proximity-based; no vanilla damage pipeline
// involvement per A2/A4/A13 — pure stun). Host-authoritative fire
// decision: only host's copy of the projectile runs the hit check;
// peer replicas are passive (motion only) and stun state arrives
// via the PLAYER_STUN_APPLIED broadcast.
//
// Lifetime end conditions (all despawn silently):
//   - Player intersection (hit) — host broadcasts STUN_APPLIED
//   - Wall/floor/ceiling contact (bgCheckFlags & (1|8|0x10))
//   - Timeout (60 frames = ~1s at 60fps)
//
// Registered via ActorDB::AddBuiltInCustomActors per Pitfall 23.
// Actor id extern `gEnSwWebId` declared in soh/src/code/z_play.c.

struct EnSwWeb;

typedef struct EnSwWeb {
    /* 0x0000 */ Actor            actor;
    /* 0x014C */ ColliderCylinder collider;        // OC-typed (physical); active on host only
    /* 0x0198 */ s32              lifetimeFrames;  // counts down; despawns at 0
} EnSwWeb;

#ifdef __cplusplus
extern "C" {
#endif

extern s16 gEnSwWebId;

void EnSwWeb_Init(Actor* thisx, PlayState* play);
void EnSwWeb_Destroy(Actor* thisx, PlayState* play);
void EnSwWeb_Update(Actor* thisx, PlayState* play);
void EnSwWeb_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
