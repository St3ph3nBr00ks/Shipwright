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
// Combat AI step 15d landed: combat state machine cloned from NPC
// Follower Stage 4. Locomotion (IDLE/FOLLOW/STUCK) is the
// responsibility of Agent 2 and may extend this enum further; the
// combat states defined here own slots 7-11 to match the NPC
// Follower numbering and avoid a future renumbering when the two
// state machines are consolidated post-#208.
typedef enum {
    EN_INVADER_STATE_IDLE          = 0,
    EN_INVADER_STATE_FOLLOW        = 1,   // pursuit toward target (Agent 2)
    EN_INVADER_STATE_STUCK         = 3,   // (Agent 2)
    EN_INVADER_STATE_DEAD          = 4,   // reserved
    EN_INVADER_STATE_ATTACK        = 7,   // sword swing (Stage 4)
    EN_INVADER_STATE_ENGAGE        = 8,   // pursue into strike range (Stage 4)
    EN_INVADER_STATE_BLOCK         = 9,   // shield-up defensive stance (Stage 4)
    EN_INVADER_STATE_RANGED_ATTACK = 10,  // bow shot (Stage 4)
    EN_INVADER_STATE_STANDBY       = 11,  // alert pose between combat (Stage 4)
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
    // (Player AT colliders are AT_TYPE_PLAYER).
    ColliderCylinder collider;

    // Step 15d — sword swing AT collider. Quad swept in front of NPC
    // during the active frames of the sword-swing anim. AT_TYPE_ENEMY
    // so Player AC bumpers (AC_TYPE_PLAYER) accept the hit — Player's
    // AC TYPE field describes who AC accepts hits FROM, and the
    // Invader is the "enemy" attacker in that relationship. Init /
    // destroy mirror the body collider; vertices are positioned
    // per-frame by the C++ tick driver.
    ColliderQuad atCollider;

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

    // State machine. Step 15d adds combat states; Agent 2 owns
    // IDLE/FOLLOW/STUCK locomotion.
    s32 state;

    // Step 15d — previous-tick state. Lets combat-state entry frames
    // be detected via edge (prevState != state on the entry tick).
    // Mirrors EnFollower::prevState. Updated by the dispatcher AFTER
    // state handlers run.
    s32 prevState;

    // Step 15d — health (currently 1 HP per v1 plan; combat-AI
    // variants will raise this per plan §4). Maintained as the
    // authoritative value; mirrors into actor.colChkInfo.health for
    // CollisionCheck_Damage to decrement. Damage drain in Update
    // resyncs colChkInfo.health → this->health.
    s8 health;
    s8 maxHealth;
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
