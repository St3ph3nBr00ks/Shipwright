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

// Phase 4 — state-machine tick. Defined in
// soh/soh/Network/Anchor/AIFollowerNPC/FollowerNPC.cpp (C++); exposed
// to C via extern "C" wrapper. Forward-decl matches the .h there.
extern void Anchor_TickFollowerNpcActor(Actor* npc, PlayState* play);

// Color-bug fix — set/clear a draw-context flag around our
// Player_DrawImpl call so the VB_APPLY_TUNIC_COLOR hook in
// HookHandlers.cpp can apply the OWNER's color instead of inheriting
// the previous DummyPlayer draw's GPU env color. See
// soh/soh/Network/Anchor/AIFollowerNPC/FollowerNPC.h for full
// rationale; mirrors the pause-menu fix for the same root cause.
extern void Anchor_FollowerNpcDrawBegin(Actor* npc);
extern void Anchor_FollowerNpcDrawEnd(void);

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
    this->currentAnim       = 0;     // kNone — first EnsureAnimation will fire
    this->currentAnimType   = 0;     // PLAYER_ANIMTYPE_0 (unarmed) baseline
    this->syncedSpeedXZ     = 0.0f;
    this->stepPhase         = 0.0f;
    this->stopAnimPlaying   = 0;
    this->prevState         = EN_FOLLOWER_STATE_IDLE;
    this->idleBlendPhase    = 0.0f;
    this->idleTicks         = 0;
    this->nextFidgetIdx     = 0;
    this->headLimbRot.x     = 0; this->headLimbRot.y  = 0; this->headLimbRot.z  = 0;
    this->upperLimbRot.x    = 0; this->upperLimbRot.y = 0; this->upperLimbRot.z = 0;
    this->hoistContext      = HOIST_CONTEXT_GROUND;
    this->hoistTargetPos.x  = 0.0f; this->hoistTargetPos.y = 0.0f; this->hoistTargetPos.z = 0.0f;
    this->hoistEntryYaw     = 0;

    // Player-equivalent scale (matches Link). 0.01f. Same as the pause
    // menu preview and as Player_Init does for the real Link.
    Actor_SetScale(thisx, 0.01f);
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 24.0f);
    thisx->shape.shadowAlpha = 255;

    // Gravity. Without this, Actor_MoveXZGravity adds 0.0 to velocity.y
    // every frame — the NPC just hovers at spawn altitude. -2.0f matches
    // Player_Init's value (most NPCs use -1.0f, but Link-skel actors
    // should feel like Link). minVelocityY isn't set here — the default
    // (0 → no floor) is fine because Actor_UpdateBgCheckInfo (called in
    // FollowerNPC.cpp tick) clamps Y to floor.
    thisx->gravity = -2.0f;

    // Load Link skel for the current age. gPlayerSkelHeaders is indexed
    // [0]=adult [1]=child to match LINK_AGE_ADULT/CHILD. Animation =
    // standard idle wait (will be swapped by state-machine in Phase 4
    // when FOLLOW / CLIMBING / etc. need different anims).
    //
    // Flags = 9 matches z_player_lib.c:1988's pattern (the only known-
    // good call site for player-skel init from a custom actor context).
    // Init anim is wait_free (not wait). The non-_free wait anim is
    // the "fighter" idle pose with shield raised — used by Link when
    // sword+shield are drawn. Our NPC has no combat in v1, so the
    // arms-down _free variant is correct. (Player's anim lookup table
    // at z_player.c:580-587 uses _free for PLAYER_ANIMTYPE_UNARMED.)
    SkelAnime_InitLink(play, &this->skelAnime,
                       gPlayerSkelHeaders[this->linkAge],
                       (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free,
                       9 /* flags */,
                       this->jointTable, this->morphTable,
                       PLAYER_LIMB_MAX);

    // CRITICAL: SkelAnime_InitLink internally calls LinkAnimation_Change
    // with endFrame=0 (z_skelanime.c:1146) — that sets the anim up but
    // FREEZES it at frame 0 (no advance possible in LinkAnimation_Loop
    // because curFrame >= animLength is always true when both are 0).
    // LinkAnimation_PlayLoop overrides with proper endFrame.
    LinkAnimation_PlayLoop(play, &this->skelAnime,
                           (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free);
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

    // Phase 4: state-machine tick (locomotion + IDLE/FOLLOW transitions).
    // C++ implementation in soh/soh/Network/Anchor/AIFollowerNPC/.
    // For peer replicas (mPeerFollowerNpcs entries), this no-ops and
    // returns — pos comes from FOLLOWER_NPC_STATE packets instead.
    Anchor_TickFollowerNpcActor(thisx, play);

    // Animation tick — runs for owner AND peer replicas alike so the
    // replica plays the idle wait anim locally without needing
    // animation data over the wire. Phase 4 still uses the default
    // wait anim (Phase 1.5's SkelAnime_InitLink configured); Phase 5+
    // will swap animations on state transitions.
    LinkAnimation_Update(play, &this->skelAnime);
}

void EnFollower_Draw(Actor* thisx, PlayState* play) {
    EnFollower* this = (EnFollower*)thisx;

    // Inherit the local player's equipment state for v1. The override
    // callback (Player_OverrideLimbDrawGameplayDefault) casts `thisx`
    // to Player* and reads Player-specific fields (leftHandType,
    // upperLimbRot, etc.) — passing our EnFollower* would crash;
    // passing the local player's Player* makes the NPC mirror Link's
    // equipment visuals (sword, shield, boots) for v1. Phase 2+ may
    // broadcast the NPC's own equipment via FOLLOWER_NPC_STATE and
    // track it independently.
    //
    // CRITICAL: post-limb callback is NULL (matching the pause-menu
    // Link preview path at z_player_lib.c:2183). Player_PostLimbDrawGameplay
    // WRITES BACK to the Player struct's leftHandPos / meleeWeaponInfo
    // / hooked-actor positions — this is the data the engine reads
    // next frame to position weapon swings, projectile spawns, and the
    // first-person camera anchor. Passing it with localPlayer as thisx
    // makes the NPC's hand positions overwrite the real Player's every
    // frame, so swords swing from the NPC, slingshot/deku-nut shots
    // spawn at the NPC, first-person camera anchors to the NPC. Field-
    // tested 2026-05-16 (log 127): all four symptoms confirmed; fix
    // is to pass NULL post-limb. NPC loses sword-swing trail and
    // bottle rendering (post-limb side effects) but those don't apply
    // to v1 (NPC is invulnerable / no combat).
    //
    // Override callback's writes (sLeftHandType / sRightHandType /
    // D_80160000 file-statics) are transient — overwritten on the
    // next real-Player draw — so leaving Override_Default in place
    // is safe. Hand-attached display lists (sword/shield model) still
    // render via the override path.
    Player* localPlayer = GET_PLAYER(play);

    // Sample localPlayer state for diagnostics (visible via
    // ImGui actor viewer; not used in rendering directly).
    this->currentTunic = localPlayer->currentTunic;
    this->currentBoots = localPlayer->currentBoots;
    this->currentFace  = localPlayer->actor.shape.face;

    // Head-look swap. Save/swap/restore localPlayer's head + upper
    // rotation around our draw so the NPC's head turns independently
    // toward ITS leader. Mirror of the face-texture save/swap/restore
    // pattern. Re-enabled after isolating the idle-anim bug to the
    // idle blend (now disabled separately).
    Vec3s savedHead  = localPlayer->headLimbRot;
    Vec3s savedUpper = localPlayer->upperLimbRot;
    localPlayer->headLimbRot  = this->headLimbRot;
    localPlayer->upperLimbRot = this->upperLimbRot;

    Anchor_FollowerNpcDrawBegin(thisx);
    Player_DrawImpl(play,
                    this->skelAnime.skeleton,
                    this->skelAnime.jointTable,
                    this->skelAnime.dListCount,
                    0 /* lod */,
                    this->currentTunic,
                    this->currentBoots,
                    this->currentFace,
                    Player_OverrideLimbDrawGameplayDefault,
                    NULL /* post-limb: see comment above — must be NULL */,
                    localPlayer /* thisx for callbacks */);
    Anchor_FollowerNpcDrawEnd();

    // Restore. Scoped to this one draw call.
    localPlayer->headLimbRot  = savedHead;
    localPlayer->upperLimbRot = savedUpper;
}
