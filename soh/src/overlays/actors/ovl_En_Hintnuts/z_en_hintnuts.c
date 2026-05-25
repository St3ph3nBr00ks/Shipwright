/*
 * File: z_en_hintnuts.c
 * Overlay: ovl_En_Hintnuts
 * Description: Hint Deku Scrubs (Deku Tree)
 */

#include "z_en_hintnuts.h"
#include "objects/object_hintnuts/object_hintnuts.h"
#include "overlays/actors/ovl_En_Nutsball/z_en_nutsball.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ResourceManagerHelpers.h"

// Multiplayer targeting (#180 residual #1, KB-08-style pattern).
// Vanilla En_Hintnuts reads `actor.xzDistToPlayer` / `yawTowardsPlayer`,
// which the engine auto-computes against the LOCAL Link only. On host
// our ShouldActorUpdate hook patches these toward the nearest player
// (DummyPlayer-aware), but on peer they always reference the local
// Link. That makes peer's local Hintnut rotate toward P2 while host's
// Hintnut rotates toward P1 — the per-packet shape.rot write from host
// is then nudged back by the local Math_ApproachS, producing visible
// yaw drift. Switch the targeting reads to Anchor_GetNearestPlayerActor
// so peer and host both target the same player.
extern Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play);

#define FLAGS (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE)

void EnHintnuts_Init(Actor* thisx, PlayState* play);
void EnHintnuts_Destroy(Actor* thisx, PlayState* play);
void EnHintnuts_Update(Actor* thisx, PlayState* play);
void EnHintnuts_Draw(Actor* thisx, PlayState* play);

void EnHintnuts_SetupWait(EnHintnuts* this);
void EnHintnuts_Wait(EnHintnuts* this, PlayState* play);
void EnHintnuts_LookAround(EnHintnuts* this, PlayState* play);
void EnHintnuts_Stand(EnHintnuts* this, PlayState* play);
void EnHintnuts_ThrowNut(EnHintnuts* this, PlayState* play);
void EnHintnuts_Burrow(EnHintnuts* this, PlayState* play);
void EnHintnuts_BeginRun(EnHintnuts* this, PlayState* play);
void EnHintnuts_BeginFreeze(EnHintnuts* this, PlayState* play);
void EnHintnuts_Run(EnHintnuts* this, PlayState* play);
void EnHintnuts_Talk(EnHintnuts* this, PlayState* play);
void EnHintnuts_Leave(EnHintnuts* this, PlayState* play);
void EnHintnuts_Freeze(EnHintnuts* this, PlayState* play);

const ActorInit En_Hintnuts_InitVars = {
    ACTOR_EN_HINTNUTS,
    ACTORCAT_ENEMY,
    FLAGS,
    OBJECT_HINTNUTS,
    sizeof(EnHintnuts),
    (ActorFunc)EnHintnuts_Init,
    (ActorFunc)EnHintnuts_Destroy,
    (ActorFunc)EnHintnuts_Update,
    (ActorFunc)EnHintnuts_Draw,
    NULL,
};

static ColliderCylinderInit sCylinderInit = {
    {
        COLTYPE_HIT6,
        AT_NONE,
        AC_ON | AC_TYPE_PLAYER,
        OC1_ON | OC1_TYPE_ALL,
        OC2_TYPE_1,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK0,
        { 0x00000000, 0x00, 0x00 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_NONE,
        BUMP_ON,
        OCELEM_ON,
    },
    { 18, 32, 0, { 0, 0, 0 } },
};

static CollisionCheckInfoInit sColChkInfoInit = { 1, 18, 32, MASS_HEAVY };

static s16 sPuzzleCounter = 0;

static InitChainEntry sInitChain[] = {
    ICHAIN_F32(gravity, -1, ICHAIN_CONTINUE),
    ICHAIN_S8(naviEnemyId, 0x0A, ICHAIN_CONTINUE),
    ICHAIN_F32(targetArrowOffset, 2600, ICHAIN_STOP),
};

void EnHintnuts_Init(Actor* thisx, PlayState* play) {
    EnHintnuts* this = (EnHintnuts*)thisx;
    s32 pad;

    Actor_ProcessInitChain(&this->actor, sInitChain);
    if (this->actor.params == 0xA) {
        this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    } else {
        ActorShape_Init(&this->actor.shape, 0x0, ActorShadow_DrawCircle, 35.0f);
        SkelAnime_Init(play, &this->skelAnime, &gHintNutsSkel, &gHintNutsStandAnim, this->jointTable, this->morphTable,
                       10);
        Collider_InitCylinder(play, &this->collider);
        Collider_SetCylinder(play, &this->collider, &this->actor, &sCylinderInit);
        CollisionCheck_SetInfo(&this->actor.colChkInfo, NULL, &sColChkInfoInit);
        Actor_SetTextWithPrefix(play, &this->actor, (this->actor.params >> 8) & 0xFF);
        this->textIdCopy = this->actor.textId;
        this->actor.params &= 0xFF;
        sPuzzleCounter = 0;
        if (this->actor.textId == 0x109B) {
            if (Flags_GetClear(play, 0x9) != 0) {
                Actor_Kill(&this->actor);
                return;
            }
        }
        EnHintnuts_SetupWait(this);
        Actor_SpawnAsChild(&play->actorCtx, &this->actor, play, ACTOR_EN_HINTNUTS, this->actor.world.pos.x,
                           this->actor.world.pos.y, this->actor.world.pos.z, 0, this->actor.world.rot.y, 0, 0xA);
    }
}

void EnHintnuts_Destroy(Actor* thisx, PlayState* play) {
    EnHintnuts* this = (EnHintnuts*)thisx;

    if (this->actor.params != 0xA) {
        Collider_DestroyCylinder(play, &this->collider);

        ResourceMgr_UnregisterSkeleton(&this->skelAnime);
    }
}

void EnHintnuts_HitByScrubProjectile1(EnHintnuts* this, PlayState* play) {
    if (this->actor.textId != 0 && this->actor.category == ACTORCAT_ENEMY &&
        ((this->actor.params == 0) || (sPuzzleCounter == 2))) {
        this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
        this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY;
        Actor_ChangeCategory(play, &play->actorCtx, &this->actor, ACTORCAT_BG);
    }
}

void EnHintnuts_SetupWait(EnHintnuts* this) {
    Animation_PlayOnceSetSpeed(&this->skelAnime, &gHintNutsUpAnim, 0.0f);
    this->animFlagAndTimer = Rand_S16Offset(100, 50);
    this->collider.dim.height = 5;
    this->actor.world.pos = this->actor.home.pos;
    this->collider.base.acFlags &= ~AC_ON;
    this->actionFunc = EnHintnuts_Wait;
}

void EnHintnuts_SetupLookAround(EnHintnuts* this) {
    Animation_PlayLoop(&this->skelAnime, &gHintNutsLookAroundAnim);
    this->animFlagAndTimer = 2;
    this->actionFunc = EnHintnuts_LookAround;
}

void EnHintnuts_SetupThrowScrubProjectile(EnHintnuts* this) {
    Animation_PlayOnce(&this->skelAnime, &gHintNutsSpitAnim);
    this->actionFunc = EnHintnuts_ThrowNut;
}

void EnHintnuts_SetupStand(EnHintnuts* this) {
    Animation_MorphToLoop(&this->skelAnime, &gHintNutsStandAnim, -3.0f);
    if (this->actionFunc == EnHintnuts_ThrowNut) {
        this->animFlagAndTimer = 2 | 0x1000; // sets timer and flag
    } else {
        this->animFlagAndTimer = 1;
    }
    this->actionFunc = EnHintnuts_Stand;
}

void EnHintnuts_SetupBurrow(EnHintnuts* this) {
    Animation_MorphToPlayOnce(&this->skelAnime, &gHintNutsBurrowAnim, -5.0f);
    Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_DOWN);
    this->actionFunc = EnHintnuts_Burrow;
}

void EnHintnuts_HitByScrubProjectile2(EnHintnuts* this) {
    Animation_MorphToPlayOnce(&this->skelAnime, &gHintNutsUnburrowAnim, -3.0f);
    this->collider.dim.height = 37;
    Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_DAMAGE);
    this->collider.base.acFlags &= ~AC_ON;

    if (this->actor.params > 0 && this->actor.params < 4 && this->actor.category == ACTORCAT_ENEMY) {
        if (sPuzzleCounter == -4) {
            sPuzzleCounter = 0;
        }
        if (this->actor.params == sPuzzleCounter + 1) {
            sPuzzleCounter++;
        } else {
            if (sPuzzleCounter > 0) {
                sPuzzleCounter = -sPuzzleCounter;
            }
            sPuzzleCounter--;
        }
        this->actor.flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        this->actionFunc = EnHintnuts_BeginFreeze;
    } else {
        this->actionFunc = EnHintnuts_BeginRun;
    }
}

void EnHintnuts_SetupRun(EnHintnuts* this) {
    Animation_PlayLoop(&this->skelAnime, &gHintNutsRunAnim);
    this->animFlagAndTimer = 5;
    this->actionFunc = EnHintnuts_Run;
}

void EnHintnuts_SetupTalk(EnHintnuts* this) {
    Animation_MorphToLoop(&this->skelAnime, &gHintNutsTalkAnim, -5.0f);
    this->actionFunc = EnHintnuts_Talk;
    this->actor.speedXZ = 0.0f;
}

void EnHintnuts_SetupLeave(EnHintnuts* this, PlayState* play) {
    Animation_MorphToLoop(&this->skelAnime, &gHintNutsRunAnim, -5.0f);
    this->actor.speedXZ = 3.0f;
    this->animFlagAndTimer = 100;
    this->actor.world.rot.y = this->actor.shape.rot.y;
    this->collider.base.ocFlags1 &= ~OC1_ON;
    this->actor.flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_DAMAGE);
    if (!Anchor_ShouldSuppressHintnutsDrop(&this->actor)) {
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_ITEM00, this->actor.world.pos.x, this->actor.world.pos.y,
                    this->actor.world.pos.z, 0x0, 0x0, 0x0, 0x3); // recovery heart
    }
    this->actionFunc = EnHintnuts_Leave;
}

void EnHintnuts_SetupFreeze(EnHintnuts* this) {
    Animation_PlayLoop(&this->skelAnime, &gHintNutsFreezeAnim);
    this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    Actor_SetColorFilter(&this->actor, 0, 0xFF, 0, 100);
    this->actor.colorFilterTimer = 1;
    this->animFlagAndTimer = 0;
    Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_FAINT);
    if (sPuzzleCounter == -3) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        sPuzzleCounter = -4;
    }
    this->actionFunc = EnHintnuts_Freeze;
}

void EnHintnuts_Wait(EnHintnuts* this, PlayState* play) {
    s32 hasSlowPlaybackSpeed = false;
    // #180 residual #1 — target nearest player.
    Actor* nearestPlayer = Anchor_GetNearestPlayerActor(&this->actor, play);
    f32    distToNearest = Actor_WorldDistXZToActor(&this->actor, nearestPlayer);
    f32    yDistToNearest = Actor_HeightDiff(&this->actor, nearestPlayer);

    if (this->skelAnime.playSpeed < 0.5f) {
        hasSlowPlaybackSpeed = true;
    }
    if (hasSlowPlaybackSpeed && (this->animFlagAndTimer != 0)) {
        this->animFlagAndTimer--;
    }
    if (Animation_OnFrame(&this->skelAnime, 9.0f)) {
        this->collider.base.acFlags |= AC_ON;
    } else if (Animation_OnFrame(&this->skelAnime, 8.0f)) {
        Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_UP);
    }

    this->collider.dim.height = 5.0f + ((CLAMP(this->skelAnime.curFrame, 9.0f, 12.0f) - 9.0f) * 9.0f);
    if (!hasSlowPlaybackSpeed && (distToNearest < 120.0f)) {
        EnHintnuts_SetupBurrow(this);
    } else if (SkelAnime_Update(&this->skelAnime)) {
        if (distToNearest < 120.0f) {
            EnHintnuts_SetupBurrow(this);
        } else if ((this->animFlagAndTimer == 0) && (distToNearest > 320.0f)) {
            EnHintnuts_SetupLookAround(this);
        } else {
            EnHintnuts_SetupStand(this);
        }
    }
    if (hasSlowPlaybackSpeed && 160.0f < distToNearest && fabsf(yDistToNearest) < 120.0f &&
        ((this->animFlagAndTimer == 0) || (distToNearest < 480.0f))) {
        this->skelAnime.playSpeed = 1.0f;
    }
}

void EnHintnuts_LookAround(EnHintnuts* this, PlayState* play) {
    // #180 residual #1 — target nearest player.
    Actor* nearestPlayer = Anchor_GetNearestPlayerActor(&this->actor, play);
    f32    distToNearest = Actor_WorldDistXZToActor(&this->actor, nearestPlayer);

    SkelAnime_Update(&this->skelAnime);
    if (Animation_OnFrame(&this->skelAnime, 0.0f) && this->animFlagAndTimer != 0) {
        this->animFlagAndTimer--;
    }
    if ((distToNearest < 120.0f) || (this->animFlagAndTimer == 0)) {
        EnHintnuts_SetupBurrow(this);
    }
}

void EnHintnuts_Stand(EnHintnuts* this, PlayState* play) {
    // #180 residual #1 — target nearest player (peer-aware) instead of
    // local-only yawTowardsPlayer / xzDistToPlayer fields.
    Actor* nearestPlayer = Anchor_GetNearestPlayerActor(&this->actor, play);
    s16    yawToNearest  = Actor_WorldYawTowardActor(&this->actor, nearestPlayer);
    f32    distToNearest = Actor_WorldDistXZToActor(&this->actor, nearestPlayer);

    SkelAnime_Update(&this->skelAnime);
    if (Animation_OnFrame(&this->skelAnime, 0.0f) && this->animFlagAndTimer != 0) {
        this->animFlagAndTimer--;
    }
    if (!(this->animFlagAndTimer & 0x1000)) {
        Math_ApproachS(&this->actor.shape.rot.y, yawToNearest, 2, 0xE38);
    }
    if (distToNearest < 120.0f || this->animFlagAndTimer == 0x1000) {
        EnHintnuts_SetupBurrow(this);
    } else if (this->animFlagAndTimer == 0) {
        EnHintnuts_SetupThrowScrubProjectile(this);
    }
}

void EnHintnuts_ThrowNut(EnHintnuts* this, PlayState* play) {
    Vec3f nutPos;
    // #180 residual #1 — target nearest player.
    Actor* nearestPlayer = Anchor_GetNearestPlayerActor(&this->actor, play);
    s16    yawToNearest  = Actor_WorldYawTowardActor(&this->actor, nearestPlayer);
    f32    distToNearest = Actor_WorldDistXZToActor(&this->actor, nearestPlayer);

    Math_ApproachS(&this->actor.shape.rot.y, yawToNearest, 2, 0xE38);
    if (distToNearest < 120.0f) {
        EnHintnuts_SetupBurrow(this);
    } else if (SkelAnime_Update(&this->skelAnime)) {
        EnHintnuts_SetupStand(this);
    } else if (Animation_OnFrame(&this->skelAnime, 6.0f)) {
        Actor* spawned;
        nutPos.x = this->actor.world.pos.x + (Math_SinS(this->actor.shape.rot.y) * 23.0f);
        nutPos.y = this->actor.world.pos.y + 12.0f;
        nutPos.z = this->actor.world.pos.z + (Math_CosS(this->actor.shape.rot.y) * 23.0f);
        spawned = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_NUTSBALL, nutPos.x, nutPos.y, nutPos.z,
                              this->actor.shape.rot.x, this->actor.shape.rot.y, this->actor.shape.rot.z, 1);
        if (spawned != NULL) {
            // SoH multiplayer diagnostic — stamp the spawning hintnut's
            // params (1/2/3 = puzzle scrub order, 0 = non-puzzle) onto
            // the nutball so collision-event logs can identify which
            // scrub fired the projectile. See z_en_nutsball.h for the
            // field's full contract.
            ((EnNutsball*)spawned)->parentParams = this->actor.params;
            Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_THROW);
        }
    }
}

void EnHintnuts_Burrow(EnHintnuts* this, PlayState* play) {
    if (SkelAnime_Update(&this->skelAnime)) {
        EnHintnuts_SetupWait(this);
    } else {
        this->collider.dim.height = 5.0f + ((3.0f - CLAMP(this->skelAnime.curFrame, 1.0f, 3.0f)) * 12.0f);
    }
    if (Animation_OnFrame(&this->skelAnime, 4.0f)) {
        this->collider.base.acFlags &= ~AC_ON;
    }

    Math_ApproachF(&this->actor.world.pos.x, this->actor.home.pos.x, 0.5f, 3.0f);
    Math_ApproachF(&this->actor.world.pos.z, this->actor.home.pos.z, 0.5f, 3.0f);
}

void EnHintnuts_BeginRun(EnHintnuts* this, PlayState* play) {
    if (SkelAnime_Update(&this->skelAnime)) {
        this->unk_196 = this->actor.yawTowardsPlayer + 0x8000;
        EnHintnuts_SetupRun(this);
    }
    Math_ApproachS(&this->actor.shape.rot.y, this->actor.yawTowardsPlayer, 2, 0xE38);
}

void EnHintnuts_BeginFreeze(EnHintnuts* this, PlayState* play) {
    if (SkelAnime_Update(&this->skelAnime)) {
        EnHintnuts_SetupFreeze(this);
    }
}

void EnHintnuts_CheckProximity(EnHintnuts* this, PlayState* play) {
    if (this->actor.category != ACTORCAT_ENEMY) {
        if ((this->collider.base.ocFlags1 & OC1_HIT) || this->actor.isTargeted) {
            this->actor.flags |= ACTOR_FLAG_TALK_OFFER_AUTO_ACCEPTED;
        } else {
            this->actor.flags &= ~ACTOR_FLAG_TALK_OFFER_AUTO_ACCEPTED;
        }
        if (this->actor.xzDistToPlayer < 130.0f) {
            this->actor.textId = this->textIdCopy;
            func_8002F2F4(&this->actor, play);
        }
    }
}

void EnHintnuts_Run(EnHintnuts* this, PlayState* play) {
    s32 temp_ret;
    s16 diffRotInit;
    s16 diffRot;
    f32 phi_f0;

    SkelAnime_Update(&this->skelAnime);
    temp_ret = Animation_OnFrame(&this->skelAnime, 0.0f);
    if (temp_ret != 0 && this->animFlagAndTimer != 0) {
        this->animFlagAndTimer--;
    }
    if ((temp_ret != 0) || (Animation_OnFrame(&this->skelAnime, 6.0f))) {
        Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_WALK);
    }

    Math_StepToF(&this->actor.speedXZ, 7.5f, 1.0f);
    if (Math_SmoothStepToS(&this->actor.world.rot.y, this->unk_196, 1, 0xE38, 0xB6) == 0) {
        if (this->actor.bgCheckFlags & 0x20) {
            this->unk_196 = Actor_WorldYawTowardPoint(&this->actor, &this->actor.home.pos);
        } else if (this->actor.bgCheckFlags & 8) {
            this->unk_196 = this->actor.wallYaw;
        } else if (this->animFlagAndTimer == 0) {
            diffRotInit = Actor_WorldYawTowardPoint(&this->actor, &this->actor.home.pos);
            diffRot = diffRotInit - this->actor.yawTowardsPlayer;
            if (ABS(diffRot) >= 0x2001) {
                this->unk_196 = diffRotInit;
            } else {
                phi_f0 = (0.0f <= (f32)diffRot) ? 1.0f : -1.0f;
                this->unk_196 = (s16)((phi_f0 * -8192.0f) + (f32)this->actor.yawTowardsPlayer);
            }
        } else {
            this->unk_196 = (s16)(this->actor.yawTowardsPlayer + 0x8000);
        }
    }

    this->actor.shape.rot.y = this->actor.world.rot.y + 0x8000;
    if (Actor_ProcessTalkRequest(&this->actor, play)) {
        // Host-authoritative sync: peer must NOT call SetupTalk locally —
        // host's continuing Run broadcasts would revert peer back, and
        // the give-up branch below would then fire SetupBurrow mid-
        // dialog ("hintnut re-enters its nest", logs 214 Bug 2). Peer
        // routes the talk request through TALK_REQUEST → host runs
        // SetupTalk → ENEMY_STATE round-trip drives peer's transition.
        if (Anchor_ShouldSuppressHintnutsLocalAI(&this->actor)) {
            Anchor_NotifyTalkRequest(&this->actor);
        } else {
            EnHintnuts_SetupTalk(this);
        }
    } else if (this->animFlagAndTimer == 0 && Actor_WorldDistXZToPoint(&this->actor, &this->actor.home.pos) < 20.0f &&
               fabsf(this->actor.world.pos.y - this->actor.home.pos.y) < 2.0f) {
        this->actor.speedXZ = 0.0f;
        if (this->actor.category == ACTORCAT_BG) {
            this->actor.flags &=
                ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY | ACTOR_FLAG_TALK_OFFER_AUTO_ACCEPTED);
            this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE;
            Actor_ChangeCategory(play, &play->actorCtx, &this->actor, ACTORCAT_ENEMY);
        }
        EnHintnuts_SetupBurrow(this);
    } else {
        EnHintnuts_CheckProximity(this, play);
    }
}

void EnHintnuts_Talk(EnHintnuts* this, PlayState* play) {
    SkelAnime_Update(&this->skelAnime);
    Math_SmoothStepToS(&this->actor.shape.rot.y, this->actor.yawTowardsPlayer, 0x3, 0x400, 0x100);
    if (Message_GetState(&play->msgCtx) == TEXT_STATE_EVENT) {
        // Host-authoritative sync: peer must NOT call SetupLeave locally.
        // SetupLeave's body spawns a recovery heart; under host-
        // authoritative the host's stale Talk broadcasts revert peer's
        // local Leave back to Talk, peer's Talk reads TEXT_STATE_EVENT
        // again, fires SetupLeave again — heart-spawn loop runs every
        // 50ms until the actor table overflows (logs 216, ~280 hearts
        // in 14s, exception 0xC0000005). Peer routes through DIALOG_END;
        // host runs SetupLeave on its actor and broadcasts state=Leave
        // back through ENEMY_STATE. Peer's local Leave actionFunc then
        // drives Message_CloseTextbox + Actor_Kill at the natural
        // run-off-stage moment.
        if (Anchor_ShouldSuppressHintnutsLocalAI(&this->actor)) {
            Anchor_NotifyDialogEnd(&this->actor);
        } else {
            EnHintnuts_SetupLeave(this, play);
        }
    }
}

void EnHintnuts_Leave(EnHintnuts* this, PlayState* play) {
    s16 temp_a1;

    SkelAnime_Update(&this->skelAnime);
    if (this->animFlagAndTimer != 0) {
        this->animFlagAndTimer--;
    }
    if (Animation_OnFrame(&this->skelAnime, 0.0f) || Animation_OnFrame(&this->skelAnime, 6.0f)) {
        Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_WALK);
    }
    if (this->actor.bgCheckFlags & 8) {
        temp_a1 = this->actor.wallYaw;
    } else {
        temp_a1 = this->actor.yawTowardsPlayer - Camera_GetCamDirYaw(GET_ACTIVE_CAM(play)) - 0x8000;
        if (ABS(temp_a1) >= 0x4001) {
            temp_a1 = Camera_GetCamDirYaw(GET_ACTIVE_CAM(play)) + 0x8000;
        } else {
            temp_a1 = Camera_GetCamDirYaw(GET_ACTIVE_CAM(play)) - (temp_a1 >> 1) + 0x8000;
        }
    }
    Math_ScaledStepToS(&this->actor.shape.rot.y, temp_a1, 0x800);
    this->actor.world.rot.y = this->actor.shape.rot.y;
    if ((this->animFlagAndTimer == 0) || (this->actor.projectedPos.z < 0.0f)) {
        Message_CloseTextbox(play);
        if (this->actor.params == 3) {
            Flags_SetClear(play, this->actor.room);
            sPuzzleCounter = 3;
        }
        if (this->actor.child != NULL) {
            Actor_ChangeCategory(play, &play->actorCtx, this->actor.child, ACTORCAT_PROP);
        }
        Actor_Kill(&this->actor);
        GameInteractor_ExecuteOnEnemyDefeat(&this->actor);
    }
}

void EnHintnuts_Freeze(EnHintnuts* this, PlayState* play) {
    this->actor.colorFilterTimer = 1;
    SkelAnime_Update(&this->skelAnime);
    if (Animation_OnFrame(&this->skelAnime, 0.0f)) {
        Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_FAINT);
    }
    if (this->animFlagAndTimer == 0) {
        if (sPuzzleCounter == 3) {
            if (this->actor.child != NULL) {
                Actor_ChangeCategory(play, &play->actorCtx, this->actor.child, ACTORCAT_PROP);
            }
            this->animFlagAndTimer = 1;
        } else if (sPuzzleCounter == -4) {
            this->animFlagAndTimer = 2;
        }
    } else if (Math_StepToF(&this->actor.world.pos.y, this->actor.home.pos.y - 35.0f, 7.0f) != 0) {
        if (this->animFlagAndTimer == 1) {
            Actor_Kill(&this->actor);
        } else {
            this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED;
            this->actor.flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
            this->actor.colChkInfo.health = sColChkInfoInit.health;
            this->actor.colorFilterTimer = 0;
            EnHintnuts_SetupWait(this);
        }
    }
}

void EnHintnuts_ColliderCheck(EnHintnuts* this, PlayState* play) {
    if (this->collider.base.acFlags & AC_HIT) {
        this->collider.base.acFlags &= ~AC_HIT;
        Actor_SetDropFlag(&this->actor, &this->collider.info, 1);
        if (this->collider.base.ac->id != ACTOR_EN_NUTSBALL) {
            // Non-nutball hit (sword / deku stick / slingshot / etc.).
            // On peer this would briefly burrow the local actor; host's
            // next ENEMY_STATE snaps it back. Acceptable transient
            // flicker for v1 — only the puzzle-progressing nutball hit
            // is routed through PROJECTILE_HIT_ENEMY.
            EnHintnuts_SetupBurrow(this);
        } else {
            // Nutball hit. Host runs the authoritative HitByScrubProjectile1+2;
            // peer routes the hit through PROJECTILE_HIT_ENEMY so host's
            // copy of THIS hintnut transitions, then broadcasts the new
            // state via ENEMY_STATE. Peer suppresses its own local
            // transition to avoid fighting host's state machine
            // (Option A bidirectional caused state oscillation — see
            // logs 208/213 + ActorSyncHelpers comment).
            if (Anchor_ShouldSuppressHintnutsLocalAI(&this->actor)) {
                Anchor_NotifyProjectileHitEnemy(&this->actor, ACTOR_EN_NUTSBALL);
            } else {
                EnHintnuts_HitByScrubProjectile1(this, play);
                EnHintnuts_HitByScrubProjectile2(this);
            }
        }
    } else if (play->actorCtx.unk_02 != 0) {
        // Global "all hintnuts hit" trigger — vanilla debug-ish path.
        // Same single-authority gate as the nutball branch.
        if (Anchor_ShouldSuppressHintnutsLocalAI(&this->actor)) {
            Anchor_NotifyProjectileHitEnemy(&this->actor, ACTOR_EN_NUTSBALL);
        } else {
            EnHintnuts_HitByScrubProjectile1(this, play);
            EnHintnuts_HitByScrubProjectile2(this);
        }
    }
}

void EnHintnuts_Update(Actor* thisx, PlayState* play) {
    EnHintnuts* this = (EnHintnuts*)thisx;
    s32 pad;

    if (this->actor.params != 0xA) {
        EnHintnuts_ColliderCheck(this, play);
        this->actionFunc(this, play);
        if (this->actionFunc != EnHintnuts_Freeze && this->actionFunc != EnHintnuts_BeginFreeze) {
            Actor_MoveXZGravity(&this->actor);
            Actor_UpdateBgCheckInfo(play, &this->actor, 20.0f, this->collider.dim.radius, this->collider.dim.height,
                                    0x1D);
        }
        Collider_UpdateCylinder(&this->actor, &this->collider);
        if (this->collider.base.acFlags & AC_ON) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &this->collider.base);
        }
        CollisionCheck_SetOC(play, &play->colChkCtx, &this->collider.base);
        if (this->actionFunc == EnHintnuts_Wait) {
            Actor_SetFocus(&this->actor, this->skelAnime.curFrame);
        } else if (this->actionFunc == EnHintnuts_Burrow) {
            Actor_SetFocus(&this->actor,
                           20.0f - ((this->skelAnime.curFrame * 20.0f) / Animation_GetLastFrame(&gHintNutsBurrowAnim)));
        } else {
            Actor_SetFocus(&this->actor, 20.0f);
        }
    }
}

s32 EnHintnuts_OverrideLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    Vec3f vec;
    f32 curFrame;
    EnHintnuts* this = (EnHintnuts*)thisx;

    if (limbIndex == 5 && this->actionFunc == EnHintnuts_ThrowNut) {
        curFrame = this->skelAnime.curFrame;
        if (curFrame <= 6.0f) {
            vec.y = 1.0f - (curFrame * 0.0833f);
            vec.z = 1.0f + (curFrame * 0.1167f);
            vec.x = 1.0f + (curFrame * 0.1167f);
        } else if (curFrame <= 7.0f) {
            curFrame -= 6.0f;
            vec.y = 0.5f + curFrame;
            vec.z = 1.7f - (curFrame * 0.7f);
            vec.x = 1.7f - (curFrame * 0.7f);
        } else if (curFrame <= 10.0f) {
            vec.y = 1.5f - ((curFrame - 7.0f) * 0.1667f);
            vec.z = 1.0f;
            vec.x = 1.0f;
        } else {
            return false;
        }
        Matrix_Scale(vec.x, vec.y, vec.z, MTXMODE_APPLY);
    }
    return false;
}

void EnHintnuts_Draw(Actor* thisx, PlayState* play) {
    EnHintnuts* this = (EnHintnuts*)thisx;

    if (this->actor.params == 0xA) {
        Gfx_DrawDListOpa(play, gHintNutsFlowerDL);
    } else {
        SkelAnime_DrawSkeletonOpa(play, &this->skelAnime, EnHintnuts_OverrideLimbDraw, NULL, this);
    }
}

// =============================================================================
// Anchor multiplayer state-machine sync (en_hintnuts_sync).
// =============================================================================

s16 EnHintnuts_GetStateIndex(EnHintnuts* this) {
    if (this->actionFunc == EnHintnuts_Wait)        return 0;
    if (this->actionFunc == EnHintnuts_LookAround)  return 1;
    if (this->actionFunc == EnHintnuts_Stand)       return 2;
    if (this->actionFunc == EnHintnuts_ThrowNut)    return 3;
    if (this->actionFunc == EnHintnuts_Burrow)      return 4;
    if (this->actionFunc == EnHintnuts_BeginRun)    return 5;
    if (this->actionFunc == EnHintnuts_Run)         return 6;
    if (this->actionFunc == EnHintnuts_Talk)        return 7;
    if (this->actionFunc == EnHintnuts_Leave)       return 8;
    if (this->actionFunc == EnHintnuts_Freeze)      return 9;
    if (this->actionFunc == EnHintnuts_BeginFreeze) return 10;
    return -1;
}

// Replicate the ENEMY → BG re-categorization that
// EnHintnuts_HitByScrubProjectile1 (z_en_hintnuts.c:127-134) performs
// when the scene host's local nutball collision converts a hintnut
// into a talkable NPC. Without this, peer's hintnut reaches BeginRun /
// Run / Talk via state-sync but stays ACTORCAT_ENEMY + HOSTILE — the
// lock-on diamond renders yellow, Z-target doesn't expose the talk
// option, and Actor_ProcessTalkRequest never fires for peer.
//
// Called from ApplyNetState's BG-class cases (5/6/7/8). Idempotent: a
// second call once already in BG short-circuits. The reverse transition
// (BG → ENEMY when the give-up Run path returns to Burrow at
// z_en_hintnuts.c:441-446) runs on every client locally inside the
// Run actionFunc, so peer naturally reverts to ENEMY when the local
// give-up condition fires.
static void EnHintnuts_NetTransitionToBg(EnHintnuts* this, PlayState* play) {
    if (this->actor.category != ACTORCAT_ENEMY) {
        return;
    }
    this->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_FRIENDLY;
    Actor_ChangeCategory(play, &play->actorCtx, &this->actor, ACTORCAT_BG);
}

// Puzzle-counter accessors for SoH multiplayer sync. See z_en_hintnuts.h
// for the full contract — broadcaster reads via Get, receiver writes
// via Set, both keep the file-static sPuzzleCounter in lockstep across
// clients so the puzzle's branch logic resolves identically.
s16 EnHintnuts_GetPuzzleCounter(void) {
    return sPuzzleCounter;
}
void EnHintnuts_SetPuzzleCounter(s16 value) {
    sPuzzleCounter = value;
}

void EnHintnuts_ApplyNetState(EnHintnuts* this, PlayState* play, s16 stateIndex) {
    // Pillar A Phase 2 pattern — every state replicates from the scene
    // host's broadcast. Cases below mirror the natural Setup helpers
    // that the local actionFuncs run through, EXCEPT for the side
    // effects that belong only to the broadcaster:
    //   - sPuzzleCounter mutations in HitByScrubProjectile2 (the
    //     "this scrub got hit by a nut" counter) — those run only on
    //     the scene host whose local nutball lands; peer's counter
    //     remains 0 unless peer had its own local hit.
    //   - The recovery-heart drop in SetupLeave — peer's heart drop
    //     would duplicate the scene host's drop on the floor. Inlined
    //     setup below skips it.
    //
    // Re-entry guard for state 3 (ThrowNut): if local is already in
    // ThrowNut, don't restart the animation. SetupThrowScrubProjectile
    // calls Animation_PlayOnce(...) which resets curFrame to 0; per-
    // packet re-apply during a sustained match would reset the anim
    // mid-throw and the En_Nutsball projectile spawn at frame 6 would
    // never land. The driver's curState != netStateIndex check should
    // already prevent this; the explicit re-entry guard is belt-and-
    // suspenders.
    switch (stateIndex) {
        case 0:
            // Mirror the flag restoration block inside EnHintnuts_Freeze
            // (z_en_hintnuts.c:500-503). Vanilla Freeze's body restores
            // ATTENTION_ENABLED + UPDATE_CULLING_DISABLED + heals before
            // calling SetupWait on the natural reset path. Under host-
            // authoritative sync, host advances state Freeze→Wait and
            // peer applies the new state here — peer's actor never runs
            // through Freeze's restoration body, so without this block
            // peer ends up in Wait with ATTENTION_ENABLED still cleared
            // (= no Z-target lock-on after a wrong-order puzzle reset,
            // logs 214 Bug 1). Idempotent: no-op when peer arrived at
            // Wait via the normal path (flags were already correct).
            this->actor.flags |= ACTOR_FLAG_ATTENTION_ENABLED;
            this->actor.flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
            this->actor.colChkInfo.health = sColChkInfoInit.health;
            this->actor.colorFilterTimer  = 0;
            EnHintnuts_SetupWait(this);
            break;
        case 1: EnHintnuts_SetupLookAround(this);              break;
        case 2: EnHintnuts_SetupStand(this);                   break;
        case 3:
            if (this->actionFunc != EnHintnuts_ThrowNut) {
                EnHintnuts_SetupThrowScrubProjectile(this);
            }
            break;
        case 4: EnHintnuts_SetupBurrow(this);                  break;
        case 5:
            // BeginRun has no public Setup helper. Mirror the natural
            // entry from HitByScrubProjectile2's BeginRun branch
            // (z_en_hintnuts.c:172-194) — same unburrow anim + collider
            // setup, then actionFunc = BeginRun. Skip the puzzle-scrub
            // sPuzzleCounter mutation (those happen on the scene host
            // when its local nutball collision fires).
            EnHintnuts_NetTransitionToBg(this, play);
            Animation_MorphToPlayOnce(&this->skelAnime, &gHintNutsUnburrowAnim, -3.0f);
            this->collider.dim.height = 37;
            Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_DAMAGE);
            this->collider.base.acFlags &= ~AC_ON;
            this->actionFunc = EnHintnuts_BeginRun;
            break;
        case 6:
            EnHintnuts_NetTransitionToBg(this, play);
            EnHintnuts_SetupRun(this);
            break;
        case 7:
            EnHintnuts_NetTransitionToBg(this, play);
            EnHintnuts_SetupTalk(this);
            break;
        case 8:
            // Inlined SetupLeave minus the recovery-heart Actor_Spawn.
            // Mirrors SetupLeave at z_en_hintnuts.c:210-223 so the
            // walk-off-stage animation (gHintNutsRunAnim, speedXZ=3.0,
            // animFlagAndTimer=100) and final Actor_Kill cycle play
            // out on peer in lockstep with the scene host. The heart
            // drop is the scene host's responsibility — its EnItem00
            // spawn flows through normal item-replication so peers
            // see exactly one heart on the floor.
            // Idempotent BG transition for the rare case state-sync
            // skips directly from a non-BG state to Leave (e.g. peer
            // joined mid-Run and the ext->netStateIndex flips 6 → 8
            // before peer's case-6 apply lands).
            EnHintnuts_NetTransitionToBg(this, play);
            Animation_MorphToLoop(&this->skelAnime, &gHintNutsRunAnim, -5.0f);
            this->actor.speedXZ = 3.0f;
            this->animFlagAndTimer = 100;
            this->actor.world.rot.y = this->actor.shape.rot.y;
            this->collider.base.ocFlags1 &= ~OC1_ON;
            this->actor.flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
            Audio_PlayActorSound2(&this->actor, NA_SE_EN_NUTS_DAMAGE);
            this->actionFunc = EnHintnuts_Leave;
            break;
        case 9: EnHintnuts_SetupFreeze(this);                  break;
        case 10:
            // BeginFreeze has no public Setup; in vanilla it just plays
            // the unburrow anim then transitions to Freeze on anim end.
            // Apply Freeze directly so the visual (frozen-blue stunned
            // pose) is immediately consistent on peer. Skipping the
            // BeginFreeze→Freeze transition costs ~10 frames of
            // unburrow-anim visibility on peer; acceptable trade for
            // not having to add a public BeginFreeze setup helper.
            EnHintnuts_SetupFreeze(this);
            break;
        default: break;
    }
}
