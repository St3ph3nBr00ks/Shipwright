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
// Step 15c (Phase 2): IDLE / FOLLOW / STUCK locomotion + G-guard
// safety harness. Cloned from EnFollower's IDLE/FOLLOW/STUCK shape
// (Agent 2).
// Step 15d (Phase 3): combat states ATTACK / BLOCK / ENGAGE /
// RANGED_ATTACK / STANDBY cloned from NPC Follower Stage 4
// (Agent 3). Slot numbering matches NPC Follower to avoid future
// renumbering when the two state machines are consolidated
// post-#208. Note: STUCK is slot 3 (not 2), DEAD is slot 4.
typedef enum {
    EN_INVADER_STATE_IDLE          = 0,
    EN_INVADER_STATE_FOLLOW        = 1,   // chase target (Agent 2)
    EN_INVADER_STATE_STUCK         = 3,   // single-tick nudge recovery (Agent 2)
    EN_INVADER_STATE_DEAD          = 4,   // reserved for future death sequence
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

    // State machine — IDLE / FOLLOW / STUCK (Agent 2) +
    // ATTACK / ENGAGE / BLOCK / RANGED_ATTACK / STANDBY (Agent 3).
    s32 state;

    // Previous-tick state. Used by Agent 2 to detect FOLLOW→IDLE
    // transitions (stop-anim fires on the transition frame), and by
    // Agent 3 to detect combat-state entry frames via edge
    // (prevState != state on entry tick). Mirrors EnFollower::prevState.
    // Updated by the dispatcher AFTER state handlers run.
    s32 prevState;

    // ── Animation tracking (Phase 2 — cloned from EnFollower) ────────
    // Last animation kind set on this actor (InvaderAnim enum value
    // stored as int; matches the C++ enum class InvaderAnim in
    // Invader.cpp). Init = 0 = kNone — first EnsureAnimation call
    // will fire and swap from the kWait set up by SkelAnime_InitLink.
    s32 currentAnim;
    // Last modelAnimType used when picking the current anim. Mirrors
    // Player.modelAnimType — 0=unarmed/free, 1/2=fighter. Agent 3's
    // combat states set 1 (fighter) so STANDBY/ATTACK/BLOCK render
    // with sword+shield stance; Agent 2's locomotion uses 0 (free).
    s8  currentAnimType;
    // Set true while a one-shot stop anim is playing in IDLE state.
    // Blocks EnsureAnimation from switching to kWait until the stop
    // anim completes.
    u8  stopAnimPlaying;
    // Step phase counter for foot-down detection (Player's unk_868
    // equivalent at z_player.c:8105). Used for stop-anim L/R selection.
    f32 stepPhase;
    // Idle tick counter — counts ticks while NPC has been playing
    // kWait. Reserved for future fidget-anim rotation (cloned from
    // EnFollower; v1 doesn't rotate fidgets but kept for API parity).
    u32 idleTicks;

    // Independent head-look-at-target. Same pattern as EnFollower:
    // computed each tick toward target's relative direction, written
    // into Player's headLimbRot/upperLimbRot via the draw save/swap.
    // For v1 the Invader doesn't swap these into the draw (Agent 1
    // owns draw); reserved field kept so future polish can engage.
    Vec3s headLimbRot;
    Vec3s upperLimbRot;

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
