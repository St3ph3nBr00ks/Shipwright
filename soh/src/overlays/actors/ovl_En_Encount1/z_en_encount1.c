#include "z_en_encount1.h"
#include "vt.h"
#include "overlays/actors/ovl_En_Tite/z_en_tite.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include <libultraship/log/luslog.h>

// SoH multiplayer: Hyrule Field Stalchild spawner consults the synced
// player list to round-robin spawn positions across local Link + every
// in-timeline DummyPlayer. Wrapper lives in HookHandlers.cpp.
extern int Anchor_GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount);

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_LOCK_ON_DISABLED)

void EnEncount1_Init(Actor* thisx, PlayState* play);
void EnEncount1_Update(Actor* thisx, PlayState* play);

void EnEncount1_SpawnLeevers(EnEncount1* this, PlayState* play);
void EnEncount1_SpawnTektites(EnEncount1* this, PlayState* play);
void EnEncount1_SpawnStalchildOrWolfos(EnEncount1* this, PlayState* play);

static s16 sLeeverAngles[] = { 0x0000, 0x2710, 0x7148, 0x8EB8, 0xD8F0 };
static f32 sLeeverDists[] = { 200.0f, 170.0f, 120.0f, 120.0f, 170.0f };

const ActorInit En_Encount1_InitVars = {
    ACTOR_EN_ENCOUNT1,
    ACTORCAT_PROP,
    FLAGS,
    OBJECT_GAMEPLAY_KEEP,
    sizeof(EnEncount1),
    (ActorFunc)EnEncount1_Init,
    NULL,
    (ActorFunc)EnEncount1_Update,
    NULL,
    NULL,
};

void EnEncount1_Init(Actor* thisx, PlayState* play) {
    s32 pad;
    EnEncount1* this = (EnEncount1*)thisx;
    f32 spawnRange;

    if (this->actor.params <= 0) {
        osSyncPrintf("\n\n");
        // "Input error death!"
        osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 入力エラーデッス！ ☆☆☆☆☆ \n" VT_RST);
        osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 入力エラーデッス！ ☆☆☆☆☆ \n" VT_RST);
        osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 入力エラーデッス！ ☆☆☆☆☆ \n" VT_RST);
        osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 入力エラーデッス！ ☆☆☆☆☆ \n" VT_RST);
        osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 入力エラーデッス！ ☆☆☆☆☆ \n" VT_RST);
        osSyncPrintf("\n\n");
        Actor_Kill(&this->actor);
        return;
    }

    this->spawnType = (this->actor.params >> 0xB) & 0x1F;
    this->maxCurSpawns = (this->actor.params >> 6) & 0x1F;
    this->maxTotalSpawns = this->actor.params & 0x3F;
    this->curNumSpawn = this->totalNumSpawn = 0;
    spawnRange = 120.0f + (40.0f * this->actor.world.rot.z);
    this->spawnRange = spawnRange;

    osSyncPrintf("\n\n");
    // "It's an enemy spawner!"
    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 敵発生ゾーンでた！ ☆☆☆☆☆ %x\n" VT_RST, this->actor.params);
    // "Type"
    osSyncPrintf(VT_FGCOL(YELLOW) "☆☆☆☆☆ 種類\t\t   ☆☆☆☆☆ %d\n" VT_RST, this->spawnType);
    // "Maximum number of simultaneous spawns"
    osSyncPrintf(VT_FGCOL(PURPLE) "☆☆☆☆☆ 最大同時発生数     ☆☆☆☆☆ %d\n" VT_RST, this->maxCurSpawns);
    // "Maximum number of spawns"
    osSyncPrintf(VT_FGCOL(CYAN) "☆☆☆☆☆ 最大発生数  \t   ☆☆☆☆☆ %d\n" VT_RST, this->maxTotalSpawns);
    // "Spawn check range"
    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生チェック範囲   ☆☆☆☆☆ %f\n" VT_RST, this->spawnRange);
    osSyncPrintf("\n\n");

    this->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    switch (this->spawnType) {
        case SPAWNER_LEEVER:
            this->timer = 30;
            this->maxCurSpawns = 5;
            if (play->sceneNum == SCENE_HAUNTED_WASTELAND) { // Haunted Wasteland
                this->reduceLeevers = true;
                this->maxCurSpawns = 3;
            }
            this->updateFunc = EnEncount1_SpawnLeevers;
            break;
        case SPAWNER_TEKTITE:
            this->maxCurSpawns = 2;
            this->updateFunc = EnEncount1_SpawnTektites;
            break;
        case SPAWNER_STALCHILDREN:
        case SPAWNER_WOLFOS:
            if (play->sceneNum == SCENE_HYRULE_FIELD) { // Hyrule Field
                this->maxTotalSpawns = 10000;
            }
            this->updateFunc = EnEncount1_SpawnStalchildOrWolfos;
            break;
    }
}

void EnEncount1_SpawnLeevers(EnEncount1* this, PlayState* play) {
    Player* player = GET_PLAYER(play);
    s32 floorType;
    f32 spawnDist;
    s32 spawnParams;
    s16 spawnLimit;
    s16 spawnAngle;
    Vec3f spawnPos;
    CollisionPoly* floorPoly;
    s32 bgId;
    f32 floorY;
    EnReeba* leever;

    this->outOfRangeTimer = 0;
    spawnPos = this->actor.world.pos;

    if ((this->timer == 0) && (play->csCtx.state == CS_STATE_IDLE) && (this->curNumSpawn <= this->maxCurSpawns) &&
        (this->curNumSpawn < 5)) {
        floorType = func_80041D4C(&play->colCtx, player->actor.floorPoly, player->actor.floorBgId);
        if ((floorType != 4) && (floorType != 7) && (floorType != 12)) {
            this->numLeeverSpawns = 0;
        } else if (!(this->reduceLeevers && (this->actor.xzDistToPlayer > 1300.0f))) {
            spawnLimit = 5;
            if (this->reduceLeevers) {
                spawnLimit = 3;
            }
            while ((this->curNumSpawn < this->maxCurSpawns) && (this->curNumSpawn < spawnLimit) && (this->timer == 0)) {
                spawnDist = sLeeverDists[this->leeverIndex];
                spawnAngle = sLeeverAngles[this->leeverIndex] + player->actor.shape.rot.y;
                spawnParams = LEEVER_SMALL;

                if ((this->killCount >= 10) && (this->bigLeever == NULL)) {
                    this->killCount = this->numLeeverSpawns = 0;
                    spawnAngle = sLeeverAngles[0];
                    spawnDist = sLeeverDists[2];
                    spawnParams = LEEVER_BIG;
                }

                spawnPos.x = player->actor.world.pos.x + Math_SinS(spawnAngle) * spawnDist;
                spawnPos.y = player->actor.floorHeight + 120.0f;
                spawnPos.z = player->actor.world.pos.z + Math_CosS(spawnAngle) * spawnDist;

                floorY = BgCheck_EntityRaycastFloor4(&play->colCtx, &floorPoly, &bgId, &this->actor, &spawnPos);
                if (floorY <= BGCHECK_Y_MIN) {
                    break;
                }
                spawnPos.y = floorY;

                leever = (EnReeba*)Actor_SpawnAsChild(&play->actorCtx, &this->actor, play, ACTOR_EN_REEBA, spawnPos.x,
                                                      spawnPos.y, spawnPos.z, 0, 0, 0, spawnParams);

                if (leever != NULL) {
                    this->curNumSpawn++;
                    leever->aimType = this->leeverIndex++;
                    if (this->leeverIndex >= 5) {
                        this->leeverIndex = 0;
                    }
                    this->numLeeverSpawns++;
                    if (this->numLeeverSpawns >= 12) {
                        this->timer = 150;
                        this->numLeeverSpawns = 0;
                    }
                    if (spawnParams != LEEVER_SMALL) {
                        this->timer = 300;
                        this->bigLeever = leever;
                    }
                    if (!this->reduceLeevers) {
                        this->maxCurSpawns = (s16)Rand_ZeroFloat(3.99f) + 2;
                    } else {
                        this->maxCurSpawns = (s16)Rand_ZeroFloat(2.99f) + 1;
                    }
                } else {
                    // "Cannot spawn!"
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    break;
                }
            }
            int32_t modifiedSpawnRate = CVarGetInteger(CVAR_ENHANCEMENT("LeeverSpawnRate"), 0);
            if (modifiedSpawnRate) {
                this->timer = 20 * modifiedSpawnRate;
            }
        }
    }
}

void EnEncount1_SpawnTektites(EnEncount1* this, PlayState* play) {
    Player* player = GET_PLAYER(play);
    s32 bgId;
    CollisionPoly* floorPoly;
    Vec3f spawnPos;
    f32 floorY;

    if (this->timer == 0) {
        this->timer = 10;
        if ((fabsf(player->actor.world.pos.y - this->actor.world.pos.y) > 100.0f) ||
            (this->actor.xzDistToPlayer > this->spawnRange)) {
            this->outOfRangeTimer++;
        } else {
            this->outOfRangeTimer = 0;
            if ((this->curNumSpawn < this->maxCurSpawns) && (this->totalNumSpawn < this->maxTotalSpawns)) {
                spawnPos.x = this->actor.world.pos.x + Rand_CenteredFloat(50.0f);
                spawnPos.y = this->actor.world.pos.y + 120.0f;
                spawnPos.z = this->actor.world.pos.z + Rand_CenteredFloat(50.0f);
                floorY = BgCheck_EntityRaycastFloor4(&play->colCtx, &floorPoly, &bgId, &this->actor, &spawnPos);
                if (floorY <= BGCHECK_Y_MIN) {
                    return;
                }
                spawnPos.y = floorY;
                if (Actor_SpawnAsChild(&play->actorCtx, &this->actor, play, ACTOR_EN_TITE, spawnPos.x, spawnPos.y,
                                       spawnPos.z, 0, 0, 0, TEKTITE_RED) != NULL) { // Red tektite
                    this->curNumSpawn++;
                    this->totalNumSpawn++;
                } else {
                    // "Cannot spawn!"
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                }
            }
        }
    }
}

void EnEncount1_SpawnStalchildOrWolfos(EnEncount1* this, PlayState* play) {
    Player* localPlayer = GET_PLAYER(play);
    Player* player = localPlayer;
    f32 spawnDist;
    s16 spawnAngle;
    s16 spawnId;
    s16 spawnParams;
    s16 kcOver10;
    s16 tempmod;
    Vec3f spawnPos;
    CollisionPoly* floorPoly;
    s32 bgId;
    f32 floorY;

    // SoH multiplayer — Hyrule Field Stalchild spawner: per-player burst.
    // For each in-timeline player (local Link + DummyPlayers), spawn up
    // to 2 Stalchildren in a dedicated burst around that player's
    // position + facing. Total maxCurSpawns scales as 2 * numFieldPlayers
    // (each player owns 2 of the global slots). Other scenes (dungeons
    // that use En_Encount1) keep vanilla local-Link-only behavior —
    // out-of-Hyrule-Field gameplay isn't a multi-player scenario for
    // this spawner.
    //
    // Replaced an earlier round-robin cursor approach (single static
    // cursor advancing one position per spawn iteration) — that left
    // distribution sensitive to vanilla's "2 spawns per cycle" cap,
    // which fired before the cap-update path could lift it (logs 219:
    // 4 stalchildren clustered around host even with 2 players in
    // scene). Per-player burst is order-deterministic and decouples
    // distribution from cooldown timing.
    Actor* fieldPlayers[8];
    int    numFieldPlayers = 0;
    if (play->sceneNum == SCENE_HYRULE_FIELD) {
        numFieldPlayers = Anchor_GetSyncedPlayerActors(play, fieldPlayers, 8);
        if (numFieldPlayers <= 0) {
            // Anchor disabled / not yet connected — fall back to local
            // Link only so single-player and pre-handshake behavior match
            // vanilla.
            fieldPlayers[0]  = &localPlayer->actor;
            numFieldPlayers  = 1;
        }
        // Always-on cap update (vanilla only set maxCurSpawns inside
        // the timer==60 cooldown branch — that fires once per cycle and
        // missed the FIRST spawn cycle entirely, so the first burst was
        // capped at vanilla 2 even with multiple players present, logs
        // 219). Set it here, every call, so a player joining mid-game
        // takes effect on the next spawn opportunity instead of the
        // following one.
        this->maxCurSpawns = (s16)(2 * numFieldPlayers);
    }

    if (play->sceneNum != SCENE_HYRULE_FIELD) {
        if ((fabsf(player->actor.world.pos.y - this->actor.world.pos.y) > 100.0f) ||
            (this->actor.xzDistToPlayer > this->spawnRange)) {
            this->outOfRangeTimer++;
            return;
        }
    } else if (IS_DAY || (Player_GetMask(play) == PLAYER_MASK_BUNNY)) {
        this->killCount = 0;
        return;
    }

    this->outOfRangeTimer = 0;
    spawnPos = this->actor.world.pos;
    // In authentic gameplay, the game checks how many Stalchildren were spawned and only spawns new ones
    // when the old ones are despawned and a timer is reached.
    // With Enemy Randomizer on, this will keep spawning enemies based on the timer and the total amount of existing
    // enemies because it's much more difficult tracking how many enemies specifically spawned by this spawner have
    // been spawned and/or killed.
    int8_t enemyCount = play->actorCtx.actorLists[ACTORCAT_ENEMY].length;
    if ((this->curNumSpawn < this->maxCurSpawns && this->totalNumSpawn < this->maxTotalSpawns) ||
        (CVarGetInteger(CVAR_ENHANCEMENT("RandomizedEnemies"), 0) && enemyCount < 15)) {

        // SoH multiplayer / vanilla: gating + cooldown is shared across
        // all players. If host's local Link is airborne/in-water, pause
        // spawning entirely; if cooldown timer is non-zero, decrement
        // and exit. These run ONCE before iterating players (vanilla
        // ran them inside the spawn loop, but the per-iteration
        // semantics produced single-player behavior — see logs 219).
        if (play->sceneNum == SCENE_HYRULE_FIELD) {
            if ((localPlayer->floorSfxOffset == 0) || (localPlayer->actor.floorBgId != BGCHECK_SCENE) ||
                !(localPlayer->actor.bgCheckFlags & 1) || (localPlayer->stateFlags1 & PLAYER_STATE1_IN_WATER)) {
                this->fieldSpawnTimer = 60;
                return;
            }
            if (this->fieldSpawnTimer != 0) {
                this->fieldSpawnTimer--;
                return;
            }
        }

        // Per-player burst: visit each in-timeline player and try to
        // give them up to 2 Stalchildren. With per-player budgeting,
        // P1 alone in field gets 2 around them; P1+P2 each get 2 (4
        // total); each kill opens a slot for that same player on the
        // next cycle.
        const int kPerPlayerBudget = 2;
        const int numIterPlayers = (play->sceneNum == SCENE_HYRULE_FIELD) ? numFieldPlayers : 1;
        for (int playerIdx = 0; playerIdx < numIterPlayers; playerIdx++) {
            Actor* targetPlayer = (play->sceneNum == SCENE_HYRULE_FIELD) ? fieldPlayers[playerIdx] : &localPlayer->actor;
            int spawnedThisPlayer = 0;
            while (spawnedThisPlayer < kPerPlayerBudget &&
                   ((this->curNumSpawn < this->maxCurSpawns && this->totalNumSpawn < this->maxTotalSpawns) ||
                    (CVarGetInteger(CVAR_ENHANCEMENT("RandomizedEnemies"), 0) && enemyCount < 15))) {

                if (play->sceneNum == SCENE_HYRULE_FIELD) {
                    spawnDist = Rand_CenteredFloat(40.0f) + 200.0f;
                    spawnAngle = targetPlayer->shape.rot.y;
                    // Vanilla: second stalchild for a player spawns
                    // 100u BEHIND (opposite facing) instead of ahead.
                    if (spawnedThisPlayer != 0) {
                        spawnAngle = -spawnAngle;
                        spawnDist = Rand_CenteredFloat(40.0f) + 100.0f;
                    }
                    spawnPos.x =
                        targetPlayer->world.pos.x + (Math_SinS(spawnAngle) * spawnDist) + Rand_CenteredFloat(40.0f);
                    // Local Link's floorHeight as raycast start
                    // (DummyPlayer doesn't carry a reliable floorHeight;
                    // Hyrule Field is mostly flat so the proxy is safe —
                    // raycast finds the actual floor at spawn xz).
                    spawnPos.y = localPlayer->actor.floorHeight + 120.0f;
                    spawnPos.z =
                        targetPlayer->world.pos.z + (Math_CosS(spawnAngle) * spawnDist) + Rand_CenteredFloat(40.0f);
                    floorY = BgCheck_EntityRaycastFloor4(&play->colCtx, &floorPoly, &bgId, &this->actor, &spawnPos);
                    if (floorY <= BGCHECK_Y_MIN) {
                        // Raycast failed — Hyrule Field uses dynamic
                        // collision regions tied to the camera/scene
                        // origin, so positions far from this spawner's
                        // location may not have an active collision
                        // mesh. Fall back to the target player's own
                        // world.pos.y; for DummyPlayer this is peer's
                        // broadcasted ground position (peer's local Link
                        // already passed Actor_UpdateBgCheckInfo before
                        // sending), and for local Link it's the actual
                        // floor. Without this fallback, far-away
                        // DummyPlayers got zero spawns while host got 4
                        // per cycle (logs 221).
                        LUSLOG_INFO("[En_Encount1] Stalchild raycast miss for targetIdx=%d/%d "
                                    "spawnPos=(%.0f,%.0f,%.0f) — falling back to target Y=%.0f",
                                    playerIdx, numIterPlayers,
                                    spawnPos.x, spawnPos.y, spawnPos.z,
                                    targetPlayer->world.pos.y);
                        floorY = targetPlayer->world.pos.y;
                    }
                    if ((localPlayer->actor.yDistToWater != BGCHECK_Y_MIN) &&
                        (floorY < (localPlayer->actor.world.pos.y +
                                   localPlayer->actor.yDistToWater *
                                       (CVarGetInteger(CVAR_ENHANCEMENT("EnemySpawnsOverWaterboxes"), 0) ? 1 : -1)))) {
                        break;
                    }
                    spawnPos.y = floorY;
                }
                if (this->spawnType == SPAWNER_WOLFOS) {
                    spawnId = ACTOR_EN_WF;
                    spawnParams = (0xFF << 8) | 0x00;
                } else {
                    spawnId = ACTOR_EN_SKB;
                    spawnParams = 0;

                    kcOver10 = this->killCount / 10;
                    if (kcOver10 > 0) {
                        tempmod = this->killCount % 10;
                        if (tempmod == 0) {
                            spawnParams = kcOver10 * 5;
                        }
                    }
                    this->killCount++;
                }

                if (!GameInteractor_Should(VB_ENCOUNT1_SPAWN_STALCHILD_OR_WOLFOS, true, this, play, spawnId, spawnPos,
                                           spawnParams)) {
                    spawnedThisPlayer++;  // counted as attempted; advance to avoid infinite loop
                    continue;
                }

                if (Actor_SpawnAsChild(&play->actorCtx, &this->actor, play, spawnId, spawnPos.x, spawnPos.y, spawnPos.z,
                                       0, 0, 0, spawnParams) != NULL) {
                    this->curNumSpawn++;
                    spawnedThisPlayer++;
                    LUSLOG_INFO("[En_Encount1] Stalchild spawn: targetIdx=%d/%d targetPos=(%.0f,%.0f,%.0f) "
                                "spawnPos=(%.0f,%.0f,%.0f) curNumSpawn=%d maxCurSpawns=%d",
                                playerIdx, numIterPlayers,
                                targetPlayer->world.pos.x, targetPlayer->world.pos.y, targetPlayer->world.pos.z,
                                spawnPos.x, spawnPos.y, spawnPos.z,
                                (int)this->curNumSpawn, (int)this->maxCurSpawns);
                    if (this->curNumSpawn >= this->maxCurSpawns) {
                        this->fieldSpawnTimer = 100;
                    }
                    if (play->sceneNum != SCENE_HYRULE_FIELD) {
                        this->totalNumSpawn++;
                    }
                } else {
                    // "Cannot spawn!"
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    osSyncPrintf(VT_FGCOL(GREEN) "☆☆☆☆☆ 発生できません！ ☆☆☆☆☆\n" VT_RST);
                    break;  // skip this player; spawn slot exhausted or alloc failed
                }
            }
        }
    }
}

void EnEncount1_Update(Actor* thisx, PlayState* play) {
    s32 pad;
    EnEncount1* this = (EnEncount1*)thisx;

    if (this->timer != 0) {
        this->timer--;
    }

    this->updateFunc(this, play);

    if (BREG(0) != 0) {
        if (this->outOfRangeTimer != 0) {
            if ((this->outOfRangeTimer & 1) == 0) {
                DebugDisplay_AddObject(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z,
                                       this->actor.world.rot.x, this->actor.world.rot.y, this->actor.world.rot.z, 1.0f,
                                       1.0f, 1.0f, 120, 120, 120, 255, 4, play->state.gfxCtx);
            }
        } else {
            DebugDisplay_AddObject(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z,
                                   this->actor.world.rot.x, this->actor.world.rot.y, this->actor.world.rot.z, 1.0f,
                                   1.0f, 1.0f, 255, 0, 255, 255, 4, play->state.gfxCtx);
        }
    }
}
