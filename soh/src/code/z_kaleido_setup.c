#include "global.h"
#include "soh/Network/Anchor/Common/GameTimeControllerBridge.h"

s16 sKaleidoSetupKscpPos0[] = { PAUSE_QUEST, PAUSE_EQUIP, PAUSE_ITEM, PAUSE_MAP };
f32 sKaleidoSetupEyeX0[] = { 0.0f, 64.0f, 0.0f, -64.0f };
f32 sKaleidoSetupEyeZ0[] = { -64.0f, 0.0f, 64.0f, 0.0f };

s16 sKaleidoSetupKscpPos1[] = { PAUSE_MAP, PAUSE_QUEST, PAUSE_EQUIP, PAUSE_ITEM };
f32 sKaleidoSetupEyeX1[] = { -64.0f, 0.0f, 64.0f, 0.0f };
f32 sKaleidoSetupEyeZ1[] = { 0.0f, -64.0f, 0.0f, 64.0f };

void KaleidoSetup_Update(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    Input* input = &play->state.input[0];

    // Anchor multiplayer #192 diagnostic — when BTN_START is pressed but
    // the pause-menu gate below rejects, log every gate input so we can
    // identify which flag is stuck. The post-Goma cutscene chain leaves
    // one of these fields in a non-IDLE state and silently swallows the
    // press; user workaround is a scene change. Diagnostic stays in
    // place until #192's root cause is identified, then can be deleted.
    // No per-frame spam — only logs on the press edge.
    if (CHECK_BTN_ALL(input->press.button, BTN_START)) {
        Player* player = GET_PLAYER(play);
        s32 gateOk = (pauseCtx->state == 0 && pauseCtx->debugState == 0 &&
                       play->gameOverCtx.state == GAMEOVER_INACTIVE &&
                       play->transitionTrigger == TRANS_TRIGGER_OFF &&
                       play->transitionMode == TRANS_MODE_OFF &&
                       gSaveContext.cutsceneIndex < 0xFFF0 &&
                       gSaveContext.nextCutsceneIndex < 0xFFF0 &&
                       !Play_InCsMode(play) &&
                       play->shootingGalleryStatus <= 1 &&
                       gSaveContext.magicState != MAGIC_STATE_STEP_CAPACITY &&
                       gSaveContext.magicState != MAGIC_STATE_FILL &&
                       (play->sceneNum != SCENE_BOMBCHU_BOWLING_ALLEY ||
                        !Flags_GetSwitch(play, 0x38)));
        LUSLOG_INFO("[PauseAttempt] %s pauseState=%d debugState=%d gameOver=%d "
                    "transTrigger=%d transMode=%d cs=0x%04X nextCs=0x%04X "
                    "inCsMode=%d shootGalStat=%d magic=%d csCtxState=%d "
                    "stateFlags1=0x%08X csAction=%d",
                    gateOk ? "OK" : "BLOCKED",
                    pauseCtx->state, pauseCtx->debugState,
                    play->gameOverCtx.state,
                    play->transitionTrigger, play->transitionMode,
                    (unsigned)gSaveContext.cutsceneIndex,
                    (unsigned)gSaveContext.nextCutsceneIndex,
                    Play_InCsMode(play),
                    play->shootingGalleryStatus,
                    gSaveContext.magicState,
                    play->csCtx.state,
                    player ? player->stateFlags1 : 0,
                    player ? player->csAction : 0);
    }

    if (pauseCtx->state == 0 && pauseCtx->debugState == 0 && play->gameOverCtx.state == GAMEOVER_INACTIVE &&
        play->transitionTrigger == TRANS_TRIGGER_OFF && play->transitionMode == TRANS_MODE_OFF &&
        gSaveContext.cutsceneIndex < 0xFFF0 && gSaveContext.nextCutsceneIndex < 0xFFF0 && !Play_InCsMode(play) &&
        play->shootingGalleryStatus <= 1 && gSaveContext.magicState != MAGIC_STATE_STEP_CAPACITY &&
        gSaveContext.magicState != MAGIC_STATE_FILL &&
        (play->sceneNum != SCENE_BOMBCHU_BOWLING_ALLEY || !Flags_GetSwitch(play, 0x38))) {

        if (CHECK_BTN_ALL(input->cur.button, BTN_L) && CHECK_BTN_ALL(input->press.button, BTN_CUP)) {
            if (BREG(0)) {
                pauseCtx->debugState = 3;
            }
        } else if (CHECK_BTN_ALL(input->press.button, BTN_START)) {

            gSaveContext.unk_13EE = gSaveContext.unk_13EA;

            if (CHECK_BTN_ALL(input->cur.button, BTN_L))
                CVarSetInteger(CVAR_GENERAL("PauseMenuAnimatedLinkTriforce"), 1);
            else
                CVarSetInteger(CVAR_GENERAL("PauseMenuAnimatedLinkTriforce"), 0);

            WREG(16) = -175;
            WREG(17) = 155;

            pauseCtx->unk_1EA = 0;
            pauseCtx->unk_1E4 = 1;

            if (ZREG(48) == 0) {
                pauseCtx->eye.x = sKaleidoSetupEyeX0[pauseCtx->pageIndex];
                pauseCtx->eye.z = sKaleidoSetupEyeZ0[pauseCtx->pageIndex];
                pauseCtx->pageIndex = sKaleidoSetupKscpPos0[pauseCtx->pageIndex];
            } else {
                pauseCtx->eye.x = sKaleidoSetupEyeX1[pauseCtx->pageIndex];
                pauseCtx->eye.z = sKaleidoSetupEyeZ1[pauseCtx->pageIndex];
                pauseCtx->pageIndex = sKaleidoSetupKscpPos1[pauseCtx->pageIndex];
            }

            pauseCtx->mode = (u16)(pauseCtx->pageIndex * 2) + 1;
            pauseCtx->state = 1;

            osSyncPrintf("Ｍｏｄｅ=%d  eye.x=%f,  eye.z=%f  kscp_pos=%d\n", pauseCtx->mode, pauseCtx->eye.x,
                         pauseCtx->eye.z, pauseCtx->pageIndex);
        }

        if (pauseCtx->state == 1) {
            WREG(2) = -6240;
            // #182 Phase 2.5 follow-up: vanilla bumps R_UPDATE_RATE from 3
            // to 2 when pause opens (60/2=30fps logic vs 60/3=20fps), to
            // make the rotating-Link / camera-zoom kaleido animation
            // smoother while world is frozen. With live-world rendering
            // on, the world keeps simulating — bumping R_UPDATE_RATE
            // makes ALL world simulation 50% faster (Goroiwa rolls
            // faster, scrolling textures advance faster, animation
            // plays faster). Skip the bump when live-world is on so
            // the simulation stays at vanilla 20fps logic.
            if (!Anchor_PauseLiveWorldRendering()) {
                R_UPDATE_RATE = 2;
            }

            if (ShrinkWindow_GetVal()) {
                ShrinkWindow_SetVal(0);
            }

            func_800F64E0(1);
        }
    }
}

void KaleidoSetup_Init(PlayState* play) {
    PauseContext* pauseCtx = &play->pauseCtx;
    u64 temp = 0; // Necessary to match

    pauseCtx->state = 0;
    pauseCtx->debugState = 0;
    pauseCtx->alpha = 0;
    pauseCtx->unk_1EA = 0;
    pauseCtx->unk_1E4 = 0;
    pauseCtx->mode = 0;
    pauseCtx->pageIndex = PAUSE_ITEM;

    pauseCtx->unk_1F4 = 160.0f;
    pauseCtx->unk_1F8 = 160.0f;
    pauseCtx->unk_1FC = 160.0f;
    pauseCtx->unk_200 = 160.0f;
    pauseCtx->eye.z = 64.0f;
    pauseCtx->unk_1F0 = 936.0f;
    pauseCtx->eye.x = pauseCtx->eye.y = 0.0f;
    pauseCtx->unk_204 = -314.0f;

    pauseCtx->cursorPoint[PAUSE_ITEM] = 0;
    pauseCtx->cursorPoint[PAUSE_MAP] = VREG(30) + 3;
    pauseCtx->cursorPoint[PAUSE_QUEST] = 0;
    pauseCtx->cursorPoint[PAUSE_EQUIP] = 1;
    pauseCtx->cursorPoint[PAUSE_WORLD_MAP] = 10;

    pauseCtx->cursorX[PAUSE_ITEM] = 0;
    pauseCtx->cursorY[PAUSE_ITEM] = 0;
    pauseCtx->cursorX[PAUSE_MAP] = 0;
    pauseCtx->cursorY[PAUSE_MAP] = 0;
    pauseCtx->cursorX[PAUSE_QUEST] = temp;
    pauseCtx->cursorY[PAUSE_QUEST] = temp;
    pauseCtx->cursorX[PAUSE_EQUIP] = 1;
    pauseCtx->cursorY[PAUSE_EQUIP] = 0;

    pauseCtx->cursorItem[PAUSE_ITEM] = PAUSE_ITEM_NONE;
    pauseCtx->cursorItem[PAUSE_MAP] = VREG(30) + 3;
    pauseCtx->cursorItem[PAUSE_QUEST] = PAUSE_ITEM_NONE;
    pauseCtx->cursorItem[PAUSE_EQUIP] = ITEM_SWORD_KOKIRI;

    pauseCtx->cursorSlot[PAUSE_ITEM] = 0;
    pauseCtx->cursorSlot[PAUSE_MAP] = VREG(30) + 3;
    pauseCtx->cursorSlot[PAUSE_QUEST] = 0;
    pauseCtx->cursorSlot[PAUSE_EQUIP] = pauseCtx->cursorPoint[PAUSE_EQUIP];

    pauseCtx->infoPanelOffsetY = -40;
    pauseCtx->nameDisplayTimer = 0;
    pauseCtx->nameColorSet = 0;
    pauseCtx->cursorColorSet = 4;
    pauseCtx->ocarinaSongIdx = -1;
    pauseCtx->cursorSpecialPos = 0;

    pauseCtx->randoQuestMode = 0;

    View_Init(&pauseCtx->view, play->state.gfxCtx);
}

void KaleidoSetup_Destroy(PlayState* play) {
}
