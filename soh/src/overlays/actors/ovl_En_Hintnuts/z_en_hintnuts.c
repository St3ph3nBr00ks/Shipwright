/*
 * File: z_en_hintnuts.c
 * Overlay: ovl_En_Hintnuts
 * Description: Hint Deku Scrubs (Deku Tree)
 */

#include "z_en_hintnuts.h"
#include "objects/object_hintnuts/object_hintnuts.h"
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
extern bool Anchor_IsEffectiveHost(void);

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
    // #180 residual #1 — target nearest player (local Link OR DummyPlayer).
    // DummyPlayers register a shield AC collider when their remote player
    // is in PLAYER_STATE1_SHIELDING (DummyPlayer_Update calls
    // Player_SetModelsForHoldingShield), so aiming the nut at a peer's
    // DummyPlayer is bouncible just like a local-Link target.
    Actor* nearestPlayer = Anchor_GetNearestPlayerActor(&this->actor, play);
    s16    yawToNearest  = Actor_WorldYawTowardActor(&this->actor, nearestPlayer);
    f32    distToNearest = Actor_WorldDistXZToActor(&this->actor, nearestPlayer);

    Math_ApproachS(&this->actor.shape.rot.y, yawToNearest, 2, 0xE38);

    // Multiplayer fix: the "burrow when player too close" check is host-
    // authoritative. Without this gate, peer's local Hintnut sees its
    // local Link as close (regardless of host's view) and calls
    // SetupBurrow; the receive driver in HookHandlers.cpp then
    // immediately overrides peer back to ThrowNut from host's net state,
    // causing a per-frame Burrow↔ThrowNut bounce. The throw animation
    // gets restarted by SetupThrowScrubProjectile each cycle, so
    // Animation_OnFrame(6) never lands cleanly and no nutsball spawns.
    // Symptom (user's bug 1): Hintnut plays fire animation but no
    // projectile fires when peer is close.
    //
    // Symptom (user's bug 2 — same root): if a nut DOES spawn while peer
    // is close, it spawns inside peer's body cylinder and hits body before
    // shield, missing AT_BOUNCED.
    //
    // Fix: only host runs the burrow-on-close check. Peers follow via
    // state-sync. host's Anchor_GetNearestPlayerActor sees both players
    // (local + DummyPlayers) so the host correctly burrows when ANY
    // player is too close.
    if (Anchor_IsEffectiveHost() && distToNearest < 120.0f) {
        EnHintnuts_SetupBurrow(this);
    } else if (SkelAnime_Update(&this->skelAnime)) {
        EnHintnuts_SetupStand(this);
    } else if (Animation_OnFrame(&this->skelAnime, 6.0f)) {
        nutPos.x = this->actor.world.pos.x + (Math_SinS(this->actor.shape.rot.y) * 23.0f);
        nutPos.y = this->actor.world.pos.y + 12.0f;
        nutPos.z = this->actor.world.pos.z + (Math_CosS(this->actor.shape.rot.y) * 23.0f);
        if (Actor_Spawn(&play->actorCtx, play, ACTOR_EN_NUTSBALL, nutPos.x, nutPos.y, nutPos.z, this->actor.shape.rot.x,
                        this->actor.shape.rot.y, this->actor.shape.rot.z, 1) != NULL) {
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
        EnHintnuts_SetupTalk(this);
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
        EnHintnuts_SetupLeave(this, play);
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
            EnHintnuts_SetupBurrow(this);
        } else {
            EnHintnuts_HitByScrubProjectile1(this, play);
            EnHintnuts_HitByScrubProjectile2(this);
        }
    } else if (play->actorCtx.unk_02 != 0) {
        EnHintnuts_HitByScrubProjectile1(this, play);
        EnHintnuts_HitByScrubProjectile2(this);
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

void EnHintnuts_ApplyNetState(EnHintnuts* this, s16 stateIndex) {
    // States 0-6 are safely applied via Setup helpers. States 9
    // (Freeze) and 10 (BeginFreeze) are mostly safe — SetupFreeze
    // mutates sPuzzleCounter only conditionally (if it equals -3),
    // and the visual effects (color filter, freeze animation) are
    // what the user wants to see synced. Apply them directly to fix
    // residual #4 — the "blue stunned scrub" visual sync gap.
    //
    // Re-entry guard for state 3 (ThrowNut) — residual #2: if local
    // is already in ThrowNut, don't restart the animation. The vanilla
    // SetupThrowScrubProjectile calls Animation_PlayOnce(...) which
    // resets curFrame to 0. If host's per-packet sync re-applies
    // ThrowNut every packet (e.g. during sustained mismatch with
    // local cycle), the animation never reaches frame 6 where the
    // En_Nutsball projectile spawns — net result: animation plays
    // repeatedly with zero projectile spawns past the first.
    // Caller's curState check should already prevent this in normal
    // operation, but the explicit re-entry guard is belt-and-suspenders.
    switch (stateIndex) {
        case 0: EnHintnuts_SetupWait(this);                    break;
        case 1: EnHintnuts_SetupLookAround(this);              break;
        case 2: EnHintnuts_SetupStand(this);                   break;
        case 3:
            if (this->actionFunc != EnHintnuts_ThrowNut) {
                EnHintnuts_SetupThrowScrubProjectile(this);
            }
            break;
        case 4: EnHintnuts_SetupBurrow(this);                  break;
        // 5 (BeginRun) — no public Setup; entered via the Hit-By-
        //                Projectile flow only on local collision. Skip.
        case 6: EnHintnuts_SetupRun(this);                     break;
        case 7: EnHintnuts_SetupTalk(this);                    break;
        // 8 (Leave) — locally-driven only. SetupLeave needs a PlayState
        //             which we don't have here, and it spawns a recovery
        //             heart (suppressed via the drop guard either way).
        //             Local Talk→Leave transition fires naturally on
        //             dialog close. Skip.
        // 9 (Freeze) — visual stun for puzzle-wrong scrub. SetupFreeze
        //              plays freeze anim + color filter. The
        //              sPuzzleCounter mutation in SetupFreeze is
        //              conditional (only fires if local sPuzzleCounter
        //              == -3); on a peer with a different counter
        //              value the mutation is a no-op.
        case 9: EnHintnuts_SetupFreeze(this);                  break;
        // 10 (BeginFreeze) — no public Setup; just plays anim then
        //                    transitions to Freeze. Apply Freeze
        //                    directly so the visual is consistent.
        case 10: EnHintnuts_SetupFreeze(this);                 break;
        default: break;
    }
}

// =============================================================================
// Anchor multiplayer — receive-side death cycle (KB-16 / SetupDyingNet).
// Mirrors EnSw_SetupDyingNet / EnGoma_SetupDyingNet pattern. Called from
// HandlePacket_EnemyDefeated when the host (or any client) reports this
// actor's defeat — instead of the receiver firing Actor_Kill instantly,
// route through SetupLeave so the natural Run-and-Leave animation plays
// end-to-end on the peer.
//
// Death state-machine path on host:
//   `EnHintnuts_HitByScrubProjectile2` (z_en_hintnuts.c:172) on third
//   correct puzzle hit (params 1-3 with sPuzzleCounter == 2 — the
//   "winning" scrub) → `actionFunc = EnHintnuts_BeginRun` →
//   `EnHintnuts_BeginRun` (line 348) → `EnHintnuts_SetupRun` → Run
//   actionFunc (line 376) → on player approach + talk: SetupTalk →
//   on dialog close: SetupLeave (line 210) → Leave actionFunc (line 461)
//   → `Actor_Kill` after Run-off animation completes.
//
// We bypass the BeginRun/Run/Talk states and jump straight to
// `SetupLeave`, because:
//   1. By the time ENEMY_DEFEATED arrives, the host's actor has finished
//      the dialog and is already in Leave (which fires Actor_Kill at
//      animation tail).
//   2. Run/Talk are sPuzzleCounter-driven and per-client; replicating
//      them on the receiver would re-fire the heart drop or re-prompt
//      the dialog. SetupLeave with the existing Anchor_ShouldSuppress-
//      HintnutsDrop guard is the visually-correct receive-side action.
//
// Freeze-state guard: if peer's local actor is in Freeze (state 9 — wrong
// puzzle hit), let local cycle finish. The Freeze actionFunc at line 497
// fires Actor_Kill when sPuzzleCounter reaches 3, so it self-terminates.
// We don't kick into Leave from Freeze — that would visually flip a
// frozen-blue scrub to a running animation mid-sink.
//
// SetupLeave fires GameInteractor_OnEnemyDefeat indirectly via the
// natural Actor_Kill at Leave's tail (which is gated against echo by
// the existing sentDefeatThisScene dedup). No extra GI suppression
// needed here.
void EnHintnuts_SetupDyingNet(EnHintnuts* this, PlayState* play) {
    // Freeze guard — local sPuzzleCounter==3 path terminates Freeze
    // naturally; SetupLeave from Freeze state isn't a tested transition.
    if (this->actionFunc == EnHintnuts_Freeze ||
        this->actionFunc == EnHintnuts_BeginFreeze) {
        return;
    }
    EnHintnuts_SetupLeave(this, play);
}
