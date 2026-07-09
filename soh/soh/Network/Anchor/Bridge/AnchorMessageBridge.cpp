// AnchorMessageBridge — implementation.
//
// See header for design intent (R2 refactor 2026-07-09).

#include "soh/Network/Anchor/Bridge/AnchorMessageBridge.h"
#include "soh/Network/Anchor/Anchor.h"

#include <libultraship/libultraship.h>  // SPDLOG

extern "C" {
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace AnchorMessageBridge {

void ClearMessageStateForFastForwardTick() {
    if (gPlayState == nullptr) return;
    gPlayState->msgCtx.msgMode   = MSGMODE_NONE;
    gPlayState->msgCtx.textId    = 0;
    gPlayState->msgCtx.msgLength = 0;
}

void JumpToLeaderSubTextboxPosition(uint16_t msgBufPos) {
    if (gPlayState == nullptr) return;
    // Preserve msgMode at whatever Message_StartTextbox set (typically
    // MSGMODE_TEXT_START). Only advance msgBufPos. Design E v2
    // rationale: vanilla's setup states (TEXT_START → BOX_GROWING ×
    // 8 → STARTING → NEXT_MSG) must run naturally to set
    // R_TEXTBOX_Y_TARGET + grow textboxColorAlphaCurrent. Overriding
    // msgMode caused visual bugs in log 641.
    gPlayState->msgCtx.msgBufPos = msgBufPos;
}

bool TryForceSubTextboxAdvance(uint16_t expectedTextId) {
    if (gPlayState == nullptr) return false;
    if (gPlayState->msgCtx.textId != expectedTextId) return false;
    if (gPlayState->msgCtx.msgMode != MSGMODE_TEXT_AWAIT_NEXT) return false;

    // Replicate vanilla's z_message_PAL.c:4653-4655 transition.
    gPlayState->msgCtx.msgMode = MSGMODE_TEXT_NEXT_MSG;
    gPlayState->msgCtx.textUnskippable = false;
    gPlayState->msgCtx.msgBufPos++;

    // Route shadow update through the same hook vanilla's own path
    // uses. Single ownership boundary — R1.
    Anchor_OnMessageBufPosAdvanced((unsigned)gPlayState->msgCtx.msgBufPos,
                                    (unsigned)gPlayState->msgCtx.textId);
    return true;
}

extern "C" void Anchor_OnMessageBufPosAdvanced(unsigned newMsgBufPos,
                                                unsigned currentTextId) {
    // Called from vanilla z_message_PAL.c AWAIT_NEXT case AFTER
    // vanilla's own msgBufPos++, AND from CutsceneBridge solo-mode
    // return-1 paths where vanilla's line 4655 is about to fire.
    //
    // Update shadow only if the textId matches what the shadow was
    // tracking. Called in cutscene mode by both paths but the hook is
    // safe to invoke from vanilla NPC dialog too — the anchor may not
    // be tracking a shadow in NPC contexts, in which case this write
    // is harmless (the field is only READ by SnapshotCamera during
    // leader-side cutscene state capture).
    if (Anchor::Instance == nullptr) return;
    (void)currentTextId;  // unused — shadow is textId-agnostic; the
                          // textId-change detection in TickCutscene-
                          // Catchup resets it separately.
    Anchor::Instance->leaderMsgBufPosLastSubStart =
        (uint16_t)newMsgBufPos;
}

void ResetShadowForNewTextbox() {
    if (Anchor::Instance == nullptr) return;
    Anchor::Instance->leaderMsgBufPosLastSubStart = 0;
}

uint16_t GetLeaderMsgBufPosLastSubStart() {
    if (Anchor::Instance == nullptr) return 0;
    return Anchor::Instance->leaderMsgBufPosLastSubStart;
}

uint8_t GetChoiceIndex() {
    if (gPlayState == nullptr) return 0;
    return (uint8_t)gPlayState->msgCtx.choiceIndex;
}

void SetChoiceIndex(uint8_t choiceIndex) {
    if (gPlayState == nullptr) return;
    gPlayState->msgCtx.choiceIndex = choiceIndex;
}

uint8_t GetTextboxEndType() {
    if (gPlayState == nullptr) return 0;
    return (uint8_t)gPlayState->msgCtx.textboxEndType;
}

uint8_t GetChoiceNumOptions() {
    if (gPlayState == nullptr) return 0;
    const uint8_t endType = (uint8_t)gPlayState->msgCtx.textboxEndType;
    if (endType == TEXTBOX_ENDTYPE_2_CHOICE) return 2;
    if (endType == TEXTBOX_ENDTYPE_3_CHOICE) return 3;
    return 0;
}

}  // namespace AnchorMessageBridge
