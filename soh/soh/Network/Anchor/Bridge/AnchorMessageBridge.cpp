// AnchorMessageBridge — implementation.
//
// See header for design intent (R2 refactor 2026-07-09).

#include "soh/Network/Anchor/Bridge/AnchorMessageBridge.h"
#include "soh/Network/Anchor/Anchor.h"

#include <chrono>  // Anchor_ShouldAutoAdvanceNpcDialog local timer
#include <libultraship/libultraship.h>  // SPDLOG
#include <libultraship/bridge/consolevariablebridge.h>  // [Diag] gEnhancements.PendingBugsDiag CVar gate

extern "C" {
#include "z64.h"
#include "macros.h"
#include "message_data_fmt.h"  // MESSAGE_BOX_BREAK / MESSAGE_TEXTID / etc.
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
    // [Diag] pending-bugs 2026-07-15 — cutscene bug 3 (catchup fail).
    // Log the msgBufPos apply so we can see whether the Fix R RESPONSE
    // is (a) actually reaching this code path and (b) writing to
    // msgBufPos with correct values. See
    // Claude/Analysis/deku_tree_cutscene_bugs_log715_2026-07-15.md Bug 3.
    if (CVarGetInteger("gEnhancements.PendingBugsDiag", 0)) {
        SPDLOG_INFO("[MessageBridge.diag] JumpToLeaderSubTextboxPosition: "
                    "leader.msgBufPos={} peer.before.msgBufPos={} peer.msgMode={} peer.msgTextId=0x{:04X}",
                    (int)msgBufPos,
                    (int)gPlayState->msgCtx.msgBufPos,
                    (int)gPlayState->msgCtx.msgMode,
                    (int)gPlayState->msgCtx.textId);
    }
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
    if (gPlayState == nullptr) return;
    if (newMsgBufPos == 0) return;
    (void)currentTextId;  // unused — shadow is textId-agnostic; the
                          // textId-change detection in TickCutscene-
                          // Catchup resets it separately.

    // Fix M (log 668) — validate that the msgBufPos++ that fired this
    // hook was a SUB-BOUNDARY crossing (past a BOX_BREAK or
    // BOX_BREAK_DELAYED control code). If it was a textbox-transition
    // crossing (TEXTID / EVENT / END), the new msgBufPos lands in the
    // MIDDLE of a multi-byte control structure — that position is
    // garbage as a decode start point. Skip shadow update; the
    // textId-change tracker in TickCutsceneCatchup will reset the
    // shadow to 0 when the new textbox opens (typically within 1-2
    // frames). Leaving the shadow at its previous value is a strict
    // improvement over overwriting it with an interior byte position.
    //
    // Rationale: log 668 captured a snapshot mid-transition where
    // shadow=209 pointed at a middle byte of a TEXTID → 0x1016
    // structure. Peer's Message_Decode from position 209 read the
    // interior byte as a control code, exited with empty/spurious
    // content, and vanilla auto-advanced peer to textbox 0x1016 —
    // running ahead of leader who was still on 0x1015. See
    // Analysis/design_e_v2_msgbufpos_overshoot_2026-07-10.md.
    const Font* font = &gPlayState->msgCtx.font;
    const uint8_t justConsumed = font->msgBuf[newMsgBufPos - 1];
    if (justConsumed != MESSAGE_BOX_BREAK &&
        justConsumed != MESSAGE_BOX_BREAK_DELAYED) {
        SPDLOG_DEBUG("[Anchor] Shadow update skipped — justConsumed=0x{:02X} "
                     "at msgBufPos-1={} (not a sub-boundary control code)",
                     (unsigned)justConsumed, newMsgBufPos - 1);
        return;
    }

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

// C-linkage entry for the local-timer NPC dialog auto-advance, called
// from Message_ShouldAdvance / _Silent in z_message_PAL.c. Returns 1
// when the current textbox has been open for longer than the
// MpNpcDialogAutoAdvanceMs CVar (default 10000). Zero when not
// connected, when the CVar disables it, when the textbox is a choice
// or persistent (never auto-close those), or when the timer hasn't
// elapsed yet.
//
// Rationale (2026-07-16 user request):
// "I do not want an individual player to be able to hold-up dialogue
// for all other players." Quest-critical NPC dialogs produce team-wide
// state via SET_FLAG / GIVE_ITEM. AFK-in-dialog would soft-lock team
// progress until the initiating player advances. This local timer
// forces advance when the local textbox has been idle for >= N ms,
// preventing the softlock. Choice textboxes are left alone — those
// need explicit selection, and the existing DIALOG_CHOICE_VOTE
// mechanism handles cutscene-mode multi-client choices separately.
//
// The timer is LOCAL per client (no wire coordination) because
// regular NPC dialog is inherently per-player: each client is in
// their own dialog (if any). Different clients may have different
// textIds open simultaneously; a shared single-textId state (as in
// CutsceneTextAdvanceState) doesn't scale here.
//
// Solo/SP unaffected: gated on Anchor::Instance->isConnected.
extern "C" int Anchor_ShouldAutoAdvanceNpcDialog(unsigned currentTextId) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) {
        return 0;
    }
    if (gPlayState == nullptr) return 0;
    if (currentTextId == 0) return 0;
    if (!CVarGetInteger("gEnhancements.MpNpcDialogAutoAdvance", 1)) {
        return 0;
    }
    // Skip choice / persistent — those need explicit selection.
    const uint8_t endType = (uint8_t)gPlayState->msgCtx.textboxEndType;
    if (endType == TEXTBOX_ENDTYPE_2_CHOICE ||
        endType == TEXTBOX_ENDTYPE_3_CHOICE ||
        endType == TEXTBOX_ENDTYPE_PERSISTENT) {
        return 0;
    }

    static uint16_t sTrackedTextId = 0;
    static std::chrono::steady_clock::time_point sTextOpenedAt =
        std::chrono::steady_clock::time_point::min();
    const auto now = std::chrono::steady_clock::now();

    if ((uint16_t)currentTextId != sTrackedTextId) {
        sTrackedTextId = (uint16_t)currentTextId;
        sTextOpenedAt  = now;
        return 0;
    }
    if (sTextOpenedAt == std::chrono::steady_clock::time_point::min()) {
        sTextOpenedAt = now;
        return 0;
    }

    const int timeoutMs = CVarGetInteger("gEnhancements.MpNpcDialogAutoAdvanceMs", 10000);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - sTextOpenedAt).count();
    if (elapsedMs >= timeoutMs) {
        // Reset so the timer doesn't re-fire every frame until the
        // vanilla advance actually happens. Vanilla will move to the
        // next textId (or clear textId to 0), which resets the tracker
        // above naturally. Belt-and-suspenders: shift the opening time
        // forward by 1s so if vanilla doesn't advance instantly, we
        // don't spam-advance every frame.
        sTextOpenedAt = now - std::chrono::milliseconds(timeoutMs - 1000);
        SPDLOG_INFO("[NpcDialogAutoAdvance] textId=0x{:04X} elapsed={}ms — "
                    "auto-advancing (MP softlock prevention)",
                    (unsigned)currentTextId, (long long)elapsedMs);
        return 1;
    }
    return 0;
}

// -------- Forced-dialog classifier (GH #339) --------
//
// See header for the design rationale + why-chain. Two OR'd signals:
//   (1) csCtx.state != CS_STATE_IDLE  — pure cutscene commands
//   (2) csAction != 0 && cv.haltActorsDuringCsAction  — halted-actors
//       cutscene mode (Player_SetCsActionWithHaltedActors, z_actor.c:1489)
//
// The two-part check on signal (2) is load-bearing: `cv` is a union
// with `slidingDoorBgCamIndex`, which is written during sliding-door
// room transitions with a non-zero BgCamIndex value. Gating on
// csAction != 0 filters that alias out — sliding doors don't set
// csAction, so a stray non-zero cv value there can't false-positive.
extern "C" int Anchor_IsForcedDialogContext(void) {
    if (gPlayState == nullptr) return 0;
    if (gPlayState->csCtx.state != CS_STATE_IDLE) return 1;
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && player->csAction != 0 &&
        player->cv.haltActorsDuringCsAction) {
        return 1;
    }
    return 0;
}
