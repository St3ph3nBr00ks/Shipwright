#ifndef Z_EN_INVADER_H
#define Z_EN_INVADER_H

#include <libultraship/libultra.h>
#include "global.h"
#include "z64player.h"  // PLAYER_LIMB_BUF_COUNT, PLAYER_LIMB_MAX

// SoH AI Invader — hostile Link-skel NPC spawned by the AI Director.
// See Plans/ai_invader_plan.md.
//
// v1 (step 15a) scope: actor scaffold only.
//   - Registers as a custom actor via ActorDB (dynamic ID at runtime).
//   - Spawned by the AI Director's InvaderDescriptor on host-only.
//   - Renders as a black-tinted Link skeleton (player-skel reuse, no
//     new asset bundle — same pattern as the NPC Follower).
//   - Holds idle pose; no combat AI, no pursuit, no nav consumption.
//   - ACTORCAT_ENEMY: rides existing ENEMY_SPAWN/STATE/DEFEATED packet
//     family; takes damage from Player AC_TYPE_PLAYER attacks; dies
//     cleanly through Director::OnEnemyRemoved.
//   - 1 HP for v1 — one-shot kill confirms the spawn/sync/defeat
//     pipeline end-to-end without complicating early validation.
//
// Combat AI (state machine, target selection, attack, retreat) is
// blocked on #208 — see plan §2.2.
typedef enum {
    EN_INVADER_STATE_IDLE = 0,
} EnInvaderAIState;

struct EnInvader;

typedef struct EnInvader {
    Actor actor;

    // Link skel runtime state. Same sizing as EnFollower so
    // SkelAnime_InitLink works without resizing.
    SkelAnime skelAnime;
    Vec3s     jointTable[PLAYER_LIMB_BUF_COUNT];
    Vec3s     morphTable[PLAYER_LIMB_BUF_COUNT];

    // Body bumper. AC_TYPE_PLAYER so Link's sword swings register hits
    // (Player AT colliders are AT_TYPE_PLAYER). No AT collider for v1
    // — Invader is non-aggressive while combat AI is deferred.
    ColliderCylinder collider;

    // Linkage age — matches gSaveContext.linkAge at spawn time. Drives
    // skel variant selection (adult vs child Link). Invader inherits
    // the local player's age for now; future variants may force-pick
    // adult per plan §2.7.
    s8 linkAge;

    // Snapshot of localPlayer's tunic/boots/face fed to Player_DrawImpl
    // each draw. Mirrors EnFollower's diagnostic fields; the
    // VB_APPLY_TUNIC_COLOR hook overrides the tunic color downstream
    // to apply the Invader's hostile-black tint.
    s8 currentTunic;
    s8 currentBoots;
    s8 currentFace;

    // State machine — currently IDLE only. Combat AI (post-#208)
    // expands this to SPAWN / TRACK / ENGAGE / ATTACK / etc.
    s32 state;
} EnInvader;

#ifdef __cplusplus
extern "C" {
#endif

// Dynamic actor id allocated by ActorDB::AddBuiltInCustomActors. Defined
// in soh/src/code/z_play.c alongside gEnPartnerId and gEnFollowerId.
extern s16 gEnInvaderId;

void EnInvader_Init(Actor* thisx, PlayState* play);
void EnInvader_Destroy(Actor* thisx, PlayState* play);
void EnInvader_Update(Actor* thisx, PlayState* play);
void EnInvader_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
