/*
 * File: z_en_follower.c
 * Overlay: ovl_En_Follower
 * Description: SoH NPC Follower companion (Flotilla NPC mode).
 *
 * v1 PHASE 1 SCAFFOLD — registers, can be spawned, no rendering, no AI.
 *
 * Design plan: Plans/npc_follower_plan.md.
 * Branch: feature/npc-follower (off development-multiplayer @ af57a20e7).
 *
 * Phasing within v1:
 *   Phase 1 (this commit): actor scaffold (registration via ActorDB).
 *     Init/Destroy/Update/Draw are stubs. Spawned actor is invisible.
 *   Phase 1.5: Link skel rendering — SkelAnime_Init + DrawFlexLimb.
 *   Phase 2: CVar toggle wiring (spawn at leader pos / despawn).
 *   Phase 3: Network sync packets (SPAWN / STATE / DESPAWN).
 *   Phase 4: IDLE + FOLLOW state machine using Actor_MoveXZGravity.
 *   Phase 5: STUCK + recovery harness.
 *   Phase 6: CLIMBING (scripted-climb driver — plays real anims).
 *   Phase 7: DEAD state stub (v1 invulnerable; v2 adds combat).
 *   Phase 8: G-guards + debug-draw overlay.
 *
 * The NPC dispatches its tick to AIFollowerNPC/FollowerNPC.cpp via a C++
 * shim once the state machine lands (Phase 4). For Phase 1, Update is a
 * no-op — the actor stays where it's spawned.
 */

#include "z_en_follower.h"

#define FLAGS                                                                                                          \
    (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void EnFollower_Init(Actor* thisx, PlayState* play) {
    EnFollower* this = (EnFollower*)thisx;

    // Phase 1: minimal initialization. Network ownership fields and
    // linkAge get populated by Anchor's SpawnFollowerNPC helper at the
    // call site (Phase 2 wiring).
    this->state             = EN_FOLLOWER_STATE_IDLE;
    this->actionFunc        = NULL;
    this->ownerClientId     = 0;
    this->netId             = 0;
    this->linkAge           = 0;  // adult default; Phase 2 will set from gSaveContext.linkAge
    this->reservedHealth    = 0;
    this->reservedDeathFlag = 0;

    // Standard NPC scale (matches Link). 0.01f puts the actor at ~Link's
    // proportions — confirmed by inspecting Player's ActorShape init.
    Actor_SetScale(thisx, 0.01f);
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 24.0f);
    thisx->shape.shadowAlpha = 255;
}

void EnFollower_Destroy(Actor* thisx, PlayState* play) {
    // Phase 1: no resources to clean up. SkelAnime/colliders added in
    // Phase 1.5 / Phase 6 will need teardown here.
    (void)thisx;
    (void)play;
}

void EnFollower_Update(Actor* thisx, PlayState* play) {
    // Phase 1: no AI tick. Actor stays at spawn position.
    //
    // Phase 4+ will dispatch here to a C++ shim
    // (Anchor::TickFollowerNPC) that runs the state machine,
    // G-guards, and substrate-path consumption. Same shape as
    // the existing player-Follower's TickFollower call from
    // OnGameFrameUpdate, but parameterized on this EnFollower
    // pointer instead of operating on the Player actor.
    (void)thisx;
    (void)play;
}

void EnFollower_Draw(Actor* thisx, PlayState* play) {
    // Phase 1: no rendering. Actor is invisible at spawn position.
    //
    // Phase 1.5 will:
    //   - SkelAnime_InitLink (or equivalent) with the appropriate
    //     child / adult Link skeleton resource based on this->linkAge.
    //   - Call SkelAnime_DrawFlexLimb here with the BakedPlayerModel
    //     pipeline (so cosmetic-sync packs apply to NPC followers too
    //     via gEnhancements.AI.FollowerNPC.ModelFolder).
    (void)thisx;
    (void)play;
}
