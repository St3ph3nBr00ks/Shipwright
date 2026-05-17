/*
 * File: z_en_invader.c
 * Overlay: ovl_En_Invader
 * Description: AI Invader — hostile Link-skel NPC spawned by the AI Director.
 *
 * v1 (step 15a) — actor scaffold + Link-skel rendering + black tint.
 * No combat AI (blocked on #208); no pursuit; no nav consumption.
 * Spawned Invaders stand in idle pose, take damage from player
 * attacks, and die cleanly through the existing enemy pipeline.
 *
 * Design plan: Plans/ai_invader_plan.md (esp. §2.1 actor identity,
 * §2.7 animation strategy, §2.5 damage routing).
 *
 * Rendering: borrowed from EnFollower's draw path (z_en_follower.c).
 * Player_DrawImpl with localPlayer's tunic/boots/face for v1; the
 * VB_APPLY_TUNIC_COLOR hook (HookHandlers.cpp) detects Invader-draw
 * context via Anchor_GetCurrentlyDrawingInvader() and overrides the
 * tunic color to black before the gfx commit. Same draw-context flag
 * pattern as the NPC Follower color-leak fix and pause-Link fix.
 *
 * Callback thisx: Player_OverrideLimbDrawGameplayDefault casts to
 * Player*. Passing the localPlayer (not our EnInvader) lets the limb
 * override resolve correctly. Post-limb callback is NULL — same as
 * the pause-Link path and EnFollower's draw — to avoid the local
 * Player's hand positions being overwritten by Invader draws.
 */

#include "z_en_invader.h"
#include "objects/gameplay_keep/gameplay_keep.h"  // gPlayerAnim_link_normal_wait_free

// Director-side tick driver (C++). Empty in v1; combat AI lands here
// post-#208.
extern void Anchor_TickInvaderActor(Actor* invader, PlayState* play);

// Color-bug fix — same draw-context flag pattern as the NPC Follower
// (Anchor_FollowerNpcDrawBegin/End). Lets the VB_APPLY_TUNIC_COLOR
// hook in HookHandlers.cpp apply hostile-black tint instead of
// inheriting the previous DummyPlayer draw's GPU env color.
extern void Anchor_InvaderDrawBegin(Actor* invader);
extern void Anchor_InvaderDrawEnd(void);

#define FLAGS                                                                                                          \
    (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// Body bumper. AC_TYPE_PLAYER so Player's sword/projectile AT
// colliders register hits (Player AT is AT_TYPE_PLAYER; the AC's
// TYPE field describes who AC accepts hits FROM).
//
// Geometry mirrors EnFollower's body cylinder (radius 12, height 60,
// yShift 0) so vanilla enemy collision tuning carries over.
static ColliderCylinderInit sColliderInit = {
    {
        COLTYPE_HIT5,
        AT_NONE,
        AC_ON | AC_TYPE_PLAYER,
        OC1_ON | OC1_TYPE_ALL,
        OC2_TYPE_1,            // standard vanilla-enemy pattern (matches Stalfos z_en_test.c:149)
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK1,
        { 0x00000000, 0x00, 0x00 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_NONE,
        BUMP_ON,
        OCELEM_ON,
    },
    { 12, 60, 0, { 0, 0, 0 } },
};

// Step 15d — sword swing AT collider (quad). Same shape as
// EnFollower's sAtColliderInit but flipped to enemy alignment:
// AT_TYPE_ENEMY so Player AC bumpers (AC_TYPE_PLAYER) accept hits
// from this Invader. dmgFlags 0x00000100 = standard sword damage.
// Quad vertices are positioned per-frame by the C++ tick driver
// (PositionAttackQuad) during the active frames of the swing anim.
static ColliderQuadInit sAtColliderInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_ENEMY,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_1,
        COLSHAPE_QUAD,
    },
    {
        ELEMTYPE_UNK2,
        // dmgFlags=0x100 (standard sword flag); effect=0; damage=0x08.
        // Damage units are 16-per-heart, so 0x08 = 1/2 heart per swing.
        // Was 0x01 (1/16 heart, ~6%) — user reported "less than 1/4
        // heart" 2026-05-17. Vanilla Stalfos's sword AT is 4 damage
        // (1/4 heart); Invader is intentionally a touch deadlier to
        // sell the "hunter" identity.
        { 0x00000100, 0x00, 0x08 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NORMAL,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
};

void EnInvader_Init(Actor* thisx, PlayState* play) {
    EnInvader* this = (EnInvader*)thisx;

    this->state           = EN_INVADER_STATE_IDLE;
    this->prevState       = EN_INVADER_STATE_IDLE;
    this->linkAge         = (s8)gSaveContext.linkAge;
    this->currentTunic    = PLAYER_TUNIC_KOKIRI;
    this->currentBoots    = PLAYER_BOOTS_KOKIRI;
    this->currentFace     = 0;

    // Phase 2 — animation + state-machine bookkeeping (cloned from
    // EnFollower_Init). Zero-initialize so first EnsureAnimation tick
    // fires (currentAnim==0==kNone) and IDLE→FOLLOW edge detection
    // has a defined prevState baseline.
    this->currentAnim     = 0;  // kNone
    this->currentAnimType = 0;  // _free (unarmed by default; combat states set 1)
    this->stopAnimPlaying = 0;
    this->stepPhase       = 0.0f;
    this->idleTicks       = 0;
    this->headLimbRot.x   = 0;
    this->headLimbRot.y   = 0;
    this->headLimbRot.z   = 0;
    this->upperLimbRot.x  = 0;
    this->upperLimbRot.y  = 0;
    this->upperLimbRot.z  = 0;

    // Item 3 — idle breathing blend. Phase advances each tick via
    // TickIdleBlend; blendTable used by LinkAnimation_BlendToJoint.
    this->idleBlendPhase  = 0.0f;
    // blendTable members default to 0 via struct-zero on actor alloc;
    // no explicit clear needed.

    // Item 5 (2026-05-17) — health scales with Link's heart capacity.
    // Clones FollowerNpcMaxHealthFromLink (FollowerNPC.cpp:82): hearts =
    // gSaveContext.healthCapacity / 16, clamped to [3, 20]. Previously
    // hardcoded to 1 HP (v1 one-shot kill); now matches Link so the
    // BLOCK state (HP ≤ 50% threshold) is reachable and the Invader
    // feels appropriately tough at high heart counts.
    {
        int hearts = (int)gSaveContext.healthCapacity / 16;
        if (hearts < 3)  hearts = 3;
        if (hearts > 20) hearts = 20;
        this->health    = (s8)hearts;
        this->maxHealth = (s8)hearts;
    }

    // Phase 4 — animation parity. LEDGE_HOIST state + fidget rotation
    // + jump tracking. All zero-init so the first transition into
    // these states picks fresh values.
    this->hoistContext    = 0;  // INV_HOIST_CONTEXT_GROUND
    this->hoistTargetPos.x = 0.0f;
    this->hoistTargetPos.y = 0.0f;
    this->hoistTargetPos.z = 0.0f;
    this->hoistEntryYaw   = 0;
    this->nextFidgetIdx   = 0;
    this->jumpInProgress  = 0;

    // Parity gap 4 — death cause. 0 = generic; TickDEAD sets to 1
    // when the death triggered from SWIMMING.
    this->deathCause      = 0;

    // Player-equivalent scale + shadow. Matches Link's footprint so
    // the Invader visually reads as a hostile Link, not a giant.
    Actor_SetScale(thisx, 0.01f);
    ActorShape_Init(&thisx->shape, 0.0f, ActorShadow_DrawCircle, 24.0f);
    thisx->shape.shadowAlpha = 255;

    // Gravity. Without this, Actor_MoveXZGravity (called by the
    // future combat-AI tick) wouldn't pull the Invader to the floor.
    thisx->gravity = -2.0f;

    // Load Link skel for the spawn-time age. _free variant of the
    // idle wait — arms-down baseline (the non-_free wait is the
    // fighter idle with shield raised; we don't have those yet in
    // v1). Flags=9 matches the known-good init pattern from
    // z_player_lib.c:1988 and EnFollower_Init.
    SkelAnime_InitLink(play, &this->skelAnime,
                       gPlayerSkelHeaders[this->linkAge],
                       (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free,
                       9 /* flags */,
                       this->jointTable, this->morphTable,
                       PLAYER_LIMB_MAX);

    // CRITICAL — SkelAnime_InitLink internally calls LinkAnimation_Change
    // with endFrame=0, which freezes the animation at frame 0. Call
    // LinkAnimation_PlayLoop afterward to set a proper endFrame so
    // the anim cycles. Same pattern as EnFollower_Init.
    LinkAnimation_PlayLoop(play, &this->skelAnime,
                           (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free);

    // Body collider. AC registration is per-tick (CollisionCheck_SetAC
    // in Update); init here.
    Collider_InitCylinder(play, &this->collider);
    Collider_SetCylinder(play, &this->collider, &this->actor, &sColliderInit);

    // Step 15d — sword AT collider. Vertices positioned per-frame by
    // the ATTACK state's tick handler; quad zeroed at init.
    Collider_InitQuad(play, &this->atCollider);
    Collider_SetQuad(play, &this->atCollider, &this->actor, &sAtColliderInit);

    // Health init (Item 5 — heart-scaling block above sets this->health
    // and this->maxHealth based on gSaveContext.healthCapacity).
    // colChkInfo.health is what CollisionCheck_Damage decrements;
    // Actor_Kill fires when it hits 0. Mirrored into this->health for
    // C++ combat code (e.g. BLOCK threshold-check at HP ≤ 50%).
    this->actor.colChkInfo.health = this->health;
    this->actor.colChkInfo.damage = 0;
}

void EnInvader_Destroy(Actor* thisx, PlayState* play) {
    EnInvader* this = (EnInvader*)thisx;
    Collider_DestroyCylinder(play, &this->collider);
    Collider_DestroyQuad(play, &this->atCollider);
}

void EnInvader_Update(Actor* thisx, PlayState* play) {
    EnInvader* this = (EnInvader*)thisx;

    // State machine + locomotion tick (C++). Phase 2: writes
    // speedXZ + shape.rot.y based on chase-nearest-player; also runs
    // G-guards (cutscene freeze, leash teleport, stuck nudge) and
    // selects + ensures the appropriate animation.
    Anchor_TickInvaderActor(thisx, play);

    // Animation tick — runs every frame so the current anim cycles.
    LinkAnimation_Update(play, &this->skelAnime);

    // Phase 2 — apply locomotion. Standard OoT NPC pattern: speedXZ +
    // world.rot.y give a per-frame velocity vector; gravity pulls Y
    // to the floor. The C++ tick has already written shape.rot.y
    // (visual) and world.rot.y (locomotion) for FOLLOW/STUCK, or
    // zeroed speedXZ for IDLE / G-guards.
    //
    // Phase 4 — skip Actor_MoveXZGravity for SWIMMING / LEDGE_HOIST.
    //   SWIMMING:    the handler clamps Y to (surface - swimDepth) each
    //                tick; gravity would fight that clamp and produce
    //                vertical jitter / sink-and-rise oscillation.
    //   LEDGE_HOIST: the handler lerps pos from hoistStartPos to
    //                hoistTargetPos over the anim's curFrame/endFrame
    //                ratio; gravity would drag Y down each tick, undoing
    //                the lerp's upward motion and causing the mantle to
    //                visually fail.
    // Matches NPC Follower's same skip (FollowerNPC.cpp pre-MoveXZGravity
    // gate excludes these two states).
    if (this->state != EN_INVADER_STATE_SWIMMING &&
        this->state != EN_INVADER_STATE_LEDGE_HOIST) {
        Actor_MoveXZGravity(&this->actor);
    }
    // Update collision-with-ground / floor altitude. Without this
    // the actor's Y can drift away from the floor on slopes / steps.
    // Flags=4 matches the standard NPC pattern (z_en_md.c:889,
    // z_en_follower.c-via-FollowerNPC.cpp:4339).
    Actor_UpdateBgCheckInfo(play, &this->actor, 26.0f /* wallCheckHeight */,
                            10.0f /* wallCheckRadius */,
                            50.0f /* ceilingCheckHeight */, 4 /* flags */);

    // Body collider — update cylinder pos from world.pos, then drain
    // AC_HIT and register for the next collision frame.
    Collider_UpdateCylinder(&this->actor, &this->collider);

    // Drain AC hit. The CollisionCheck pre-update pass writes a
    // damage value to colChkInfo.damage and sets AC_HIT on the
    // collider whenever a Player AT lands on our AC. We must clear
    // the flag and decrement health manually — without this, the
    // hit registers but health never moves and the Invader is
    // effectively invincible (log 227 symptom). Same pattern as
    // FollowerNPC.cpp's drain at lines 4356-4405.
    //
    // v1: any non-zero damage value = 1 HP loss → one-shot kill on
    // any Player AT (sword, slingshot, deku stick, bombs, etc.).
    // Combat-AI variants will tune per-weapon damage values later.
    if (this->collider.base.acFlags & AC_HIT) {
        this->collider.base.acFlags &= ~AC_HIT;
        if (this->actor.colChkInfo.damage > 0) {
            this->actor.colChkInfo.health -= 1;
        }
        this->actor.colChkInfo.damage = 0;
    }

    // Register AC for the next collision frame.
    CollisionCheck_SetAC(play, &play->colChkCtx, &this->collider.base);

    // Step 15d — mirror colChkInfo.health back into this->health so
    // C++ combat code reading EnInvader::health sees the
    // post-collision value (e.g. BLOCK threshold check).
    this->health = this->actor.colChkInfo.health;

    // Death check — closes parity gap 3. Previously this called
    // Actor_Kill immediately when health <= 0; the Invader popped out
    // of existence with no death anim. Now we transition to the DEAD
    // state and let TickDEAD play the death anim hold (~3s) before
    // the actual Actor_Kill fires. The OnActorKill broadcast pipeline
    // (ENEMY_DEFEATED to peers + Director::OnEnemyRemoved bookkeeping)
    // runs from TickDEAD's terminal Actor_Kill call.
    //
    // Race-B host-authoritative path is unchanged — the C-side drain
    // above still decrements colChkInfo.health, and we still trip the
    // transition here. Only the timing of the Actor_Kill shifts (now
    // deferred ~3s instead of immediate).
    if (this->actor.colChkInfo.health <= 0 &&
        this->state != EN_INVADER_STATE_DEAD) {
        this->state = EN_INVADER_STATE_DEAD;
    }
}

void EnInvader_Draw(Actor* thisx, PlayState* play) {
    EnInvader* this = (EnInvader*)thisx;

    // Inherit local player's equipment state. Same rationale as
    // EnFollower_Draw — Player_OverrideLimbDrawGameplayDefault casts
    // thisx to Player*, so the callback thisx MUST be the local
    // Player* and post-limb callback MUST be NULL (otherwise the
    // local Player's hand positions get overwritten each Invader
    // draw frame). See z_en_follower.c for the full diagnosis.
    Player* localPlayer = GET_PLAYER(play);

    this->currentTunic = localPlayer->currentTunic;
    this->currentBoots = localPlayer->currentBoots;
    this->currentFace  = localPlayer->actor.shape.face;

    // Parity gap 2 — head-look swap. Save/swap/restore localPlayer's
    // head + upper rotation around our draw so the Invader's head
    // turns independently toward ITS target. Same shape as
    // EnFollower_Draw lines 291-317. The per-tick computation that
    // writes this->headLimbRot/upperLimbRot happens in
    // Anchor_TickInvaderActor (TickHeadLookAtTarget helper).
    Vec3s savedHead  = localPlayer->headLimbRot;
    Vec3s savedUpper = localPlayer->upperLimbRot;
    localPlayer->headLimbRot  = this->headLimbRot;
    localPlayer->upperLimbRot = this->upperLimbRot;

    // Set draw-context flag so VB_APPLY_TUNIC_COLOR knows this draw
    // is an Invader and applies the hostile-black tint instead of
    // inheriting the previous draw's env color.
    Anchor_InvaderDrawBegin(thisx);
    Player_DrawImpl(play,
                    this->skelAnime.skeleton,
                    this->skelAnime.jointTable,
                    this->skelAnime.dListCount,
                    0 /* lod */,
                    this->currentTunic,
                    this->currentBoots,
                    this->currentFace,
                    Player_OverrideLimbDrawGameplayDefault,
                    NULL /* post-limb: MUST be NULL — see comment above */,
                    localPlayer);
    Anchor_InvaderDrawEnd();

    // Restore. Scoped to this one draw call so Player's own next draw
    // uses the user's actual head/upper rotation.
    localPlayer->headLimbRot  = savedHead;
    localPlayer->upperLimbRot = savedUpper;
}
