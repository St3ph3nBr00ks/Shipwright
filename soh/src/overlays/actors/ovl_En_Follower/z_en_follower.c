/*
 * File: z_en_follower.c
 * Overlay: ovl_En_Follower
 * Description: SoH NPC Follower companion (Flotilla NPC mode).
 *
 * v1 PHASE 1.5 — actor scaffold + Link skeleton rendering. No AI tick
 * (state machine lands in Phase 4); no CVar wiring (Phase 2). Spawned
 * actors stand in T-pose if no animation has played yet, else cycle
 * the standard idle wait animation.
 *
 * Design plan: Plans/npc_follower_plan.md.
 * Branch: feature/npc-follower (off development-multiplayer @ af57a20e7).
 *
 * Rendering approach: borrowed from the pause-menu Link preview path
 * (z_player_lib.c func_80091738 / Player_DrawPause), refined for an
 * in-world NPC.
 *
 *   - SkelAnime_InitLink with gPlayerSkelHeaders[gSaveContext.linkAge]
 *     and gPlayerAnim_link_normal_wait as the default idle anim.
 *   - LinkAnimation_Update each tick (state machine in later phases
 *     will swap animations based on EnFollowerAIState).
 *   - Player_DrawImpl with the local player's tunic/boots/face for
 *     v1 — the NPC visually matches whatever the local Link is
 *     wearing. Future phases may track these independently per NPC.
 *
 * Callback thisx: Player_OverrideLimbDrawGameplayDefault and
 * Player_PostLimbDrawGameplay both cast `thisx` to Player* and
 * dereference Player-specific fields (leftHandDLists, stateFlags1,
 * etc.). Passing our EnFollower* would crash; passing the local
 * player's Player* works and the NPC inherits the equipment-draw
 * semantics. This is identical to the DummyPlayer pattern's effect
 * on remote-player rendering (peer's Link wears what the local Link
 * sees of them, via the client.* fields).
 *
 * gSegments[6] (Link object) is set by the real Player's tick each
 * frame, so as long as the local player exists in the scene the
 * segment pointer is valid for our draw. No additional DMA needed.
 */

#include "z_en_follower.h"
#include "objects/gameplay_keep/gameplay_keep.h"  // gPlayerAnim_link_normal_wait

// gPlayerSkelHeaders[], Player_DrawImpl, Player_OverrideLimbDrawGameplayDefault,
// and Player_PostLimbDrawGameplay are all declared in standard headers
// (variables.h + functions.h) pulled by global.h via z_en_follower.h.
// No additional extern decls needed.

#define FLAGS                                                                                                          \
    (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void EnFollower_Init(Actor* thisx, PlayState* play) {
    EnFollower* this = (EnFollower*)thisx;

    // Phase 1.5: minimal initialization. Network ownership fields and
    // linkAge get populated by Anchor's SpawnFollowerNPC helper at the
    // call site (Phase 2 wiring); leave defaults here so a manually-
    // spawned actor (debug console) still renders sanely.
    this->state             = EN_FOLLOWER_STATE_IDLE;
    this->actionFunc        = NULL;
    this->ownerClientId     = 0;
    this->netId             = 0;
    this->linkAge           = (s8)gSaveContext.linkAge;
    this->currentTunic      = PLAYER_TUNIC_KOKIRI;
    this->currentBoots      = PLAYER_BOOTS_KOKIRI;
    this->currentFace       = 0;
    this->reservedHealth    = 0;
    this->reservedDeathFlag = 0;

    // Player-equivalent scale (matches Link). 0.01f. Same as the pause
    // menu preview and as Player_Init does for the real Link.
    Actor_SetScale(thisx, 0.01f);
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 24.0f);
    thisx->shape.shadowAlpha = 255;

    // Load Link skel for the current age. gPlayerSkelHeaders is indexed
    // [0]=adult [1]=child to match LINK_AGE_ADULT/CHILD. Animation =
    // standard idle wait (will be swapped by state-machine in Phase 4
    // when FOLLOW / CLIMBING / etc. need different anims).
    //
    // Flags = 9 matches z_player_lib.c:1988's pattern (the only known-
    // good call site for player-skel init from a custom actor context).
    SkelAnime_InitLink(play, &this->skelAnime,
                       gPlayerSkelHeaders[this->linkAge],
                       (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait,
                       9 /* flags */,
                       this->jointTable, this->morphTable,
                       PLAYER_LIMB_MAX);
}

void EnFollower_Destroy(Actor* thisx, PlayState* play) {
    // No dynamic resources to clean up; SkelAnime joint/morph tables
    // live inline in the EnFollower struct so they free with the
    // actor itself.
    (void)thisx;
    (void)play;
}

void EnFollower_Update(Actor* thisx, PlayState* play) {
    EnFollower* this = (EnFollower*)thisx;

    // Phase 1.5: advance the idle animation only. No AI tick yet —
    // state machine + locomotion arrive in Phase 4.
    LinkAnimation_Update(play, &this->skelAnime);
}

void EnFollower_Draw(Actor* thisx, PlayState* play) {
    EnFollower* this = (EnFollower*)thisx;

    // Inherit the local player's equipment state for v1. The override
    // callback (Player_OverrideLimbDrawGameplayDefault) and post-limb
    // callback (Player_PostLimbDrawGameplay) cast `thisx` to Player*
    // and access Player-specific fields (leftHandDLists, stateFlags1,
    // leftHandType, etc.). Passing our EnFollower* would crash; passing
    // the local player's Player* makes the NPC mirror Link's equipment
    // visuals — sword, shield, boots, hand state — which is correct
    // for v1. Phase 2+ may broadcast the NPC's own equipment via the
    // FOLLOWER_NPC_STATE packet and track it independently.
    Player* localPlayer = GET_PLAYER(play);

    // Sample localPlayer state for diagnostics (visible via
    // ImGui actor viewer; not used in rendering directly).
    this->currentTunic = localPlayer->currentTunic;
    this->currentBoots = localPlayer->currentBoots;
    this->currentFace  = localPlayer->actor.shape.face;

    Player_DrawImpl(play,
                    this->skelAnime.skeleton,
                    this->skelAnime.jointTable,
                    this->skelAnime.dListCount,
                    0 /* lod */,
                    this->currentTunic,
                    this->currentBoots,
                    this->currentFace,
                    Player_OverrideLimbDrawGameplayDefault,
                    Player_PostLimbDrawGameplay,
                    localPlayer /* thisx for callbacks */);
}
