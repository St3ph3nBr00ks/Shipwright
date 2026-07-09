// Refactor A.8 — Cutscene extern "C" shim bridge.
//
// Single C-linkage hook for Message_ShouldAdvance (#191 — peer-voted
// cutscene text advance + solo idle auto-advance). Moved from
// HookHandlers.cpp on 2026-06-01 per Plans/A.8_design_review.md (DR-5)
// Stage 3.8.
//
// Domain: Cutscene-internal textbox advance arbitration. Function-
// local statics (`s_soloIdleStartMs`, `s_soloIdleLastTextId`) move
// with the function — they retain their per-Anchor-state semantics
// because no other TU references them. DR-5 §"Shared dependencies"
// suggested promoting them to file scope, but the code-as-written
// uses function-local statics; the function-local form is
// functionally equivalent (init-once, persists across calls) and
// keeps the scope tighter.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <chrono>
#include <cstdint>
#include <string>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

// #191 — Anchor-aware override for Message_ShouldAdvance during a
// cutscene-internal textbox. C-callable from z_message_PAL.c.
//
// Returns 1 when the local message system should advance THIS frame
// (i.e., return true from Message_ShouldAdvance):
//   - The vote-completion broadcast was just received for the current
//     textId (one-shot — consumed on read).
//
// Returns 0 when the local press should NOT immediately advance the
// textbox; it has been forwarded to the host as a vote and the actual
// advance fires when host's CUTSCENE_TEXT_ADVANCED arrives.
//
// `wasLocalPressDetected` is the OR of (BTN_A | BTN_B | BTN_CUP) press
// computed by the caller — when true, we send a CUTSCENE_TEXT_ADVANCE
// vote packet to the host. When the local press is for a different
// textId than the most recent broadcast, the broadcast flag clears so
// future presses for the new textId follow the vote pattern.
//
// `currentTextId` is the textbox the caller wants to advance.
//
// Single-player or disconnected: returns wasLocalPressDetected
// unchanged — vanilla parity.
extern "C" int Anchor_ShouldAdvanceCutsceneTextLocal(int wasLocalPressDetected,
                                                     unsigned currentTextId) {
    // Diagnostic (2026-07-07). Gated on gRemote.Anchor.CutsceneBridgeDiag
    // (default 0). Answers the question "why did the first textbox in
    // the DT intro not use vote-skip?" Three signatures to watch for:
    //
    //   1. `NEW textId` fires with wasLocalPress=0 for the first
    //      textbox but the log NEVER shows a subsequent line for that
    //      textId with wasLocalPress=1 → user pressed A but the vanilla
    //      Message_ShouldAdvance path bypassed the bridge (Cause B/C:
    //      cutscene script auto-advance or a different advance path).
    //
    //   2. `NEW textId` fires but `peerInCutscene=0` on the first
    //      textbox → PLAYER_UPDATE hadn't propagated the peer's
    //      csCtxState yet (Cause A: timing race). Subsequent textboxes
    //      show peerInCutscene=1 as PLAYER_UPDATE catches up.
    //
    //   3. `NEW textId` never fires for the very first textbox in the
    //      cutscene → the message system uses a different textId
    //      encoding or the bridge invocation is guarded upstream in
    //      z_message_PAL.c (Cause B/C, deeper investigation needed).
    const bool diagEnabled =
        CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneBridgeDiag"), 0) != 0;
    static uint16_t s_diagLastTextId  = 0xFFFF;
    static int      s_diagLastPressed = 0;
    if (diagEnabled && (uint16_t)currentTextId != s_diagLastTextId) {
        SPDLOG_INFO("[CutsceneBridge.diag] NEW textId=0x{:04X} wasLocalPress={} "
                    "anchorConnected={}",
                    (unsigned)currentTextId, wasLocalPressDetected,
                    Anchor::Instance ? (int)Anchor::Instance->isConnected : -1);
        s_diagLastTextId = (uint16_t)currentTextId;
    }

    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        if (diagEnabled && wasLocalPressDetected && !s_diagLastPressed) {
            SPDLOG_INFO("[CutsceneBridge.diag] textId=0x{:04X} press: Anchor OFFLINE "
                        "→ solo (vanilla)", (unsigned)currentTextId);
        }
        s_diagLastPressed = wasLocalPressDetected;
        return wasLocalPressDetected ? 1 : 0;
    }
    // s_diagLastPressed used below at return points — track across the
    // rest of the function.

    // Consume the broadcast flag if it matches the current textbox.
    // Edge case: matched broadcast for a previous textId stays
    // consumed when we move to a new textId — the new textId's
    // vote count starts fresh.
    // Design E v5 — consumed-count decrement on return-1 path.
    // Replaces the earlier boolean flag which lost broadcasts on
    // back-to-back arrivals (log 646).
    if (Anchor::Instance->cutsceneTextAdvancePendingCount > 0 &&
        Anchor::Instance->cutsceneTextAdvanceConsumedTextId == (uint16_t)currentTextId) {
        Anchor::Instance->cutsceneTextAdvancePendingCount--;
        // Design E v4 — shadow update on the consumed-flag return-1
        // path. Vanilla's AWAIT_NEXT case will do msgBufPos++ after
        // this return. Capture the post-++ value now. Defense-in-
        // depth: if Fix S/T helper already ran this frame (before
        // Message_Update), it already updated shadow — this write
        // is redundant but harmless. If helper did NOT run (rare
        // hook-ordering path), this write is load-bearing. See
        // Analysis/design_e_v3_shadow_alone_mode_gap_2026-07-08.md.
        if (gPlayState != nullptr &&
            gPlayState->msgCtx.msgMode == MSGMODE_TEXT_AWAIT_NEXT) {
            Anchor::Instance->leaderMsgBufPosLastSubStart =
                (uint16_t)(gPlayState->msgCtx.msgBufPos + 1);
        }
        if (diagEnabled) {
            SPDLOG_INFO("[CutsceneBridge.diag] textId=0x{:04X} advance-broadcast "
                        "consumed → local advance (pending remaining={})",
                        (unsigned)currentTextId,
                        (int)Anchor::Instance->cutsceneTextAdvancePendingCount);
        }
        s_diagLastPressed = wasLocalPressDetected;
        return 1;
    }
    // Stale-textId drop: pending count is for a prior textbox we've
    // already moved past. Reset so it doesn't confuse the next open.
    if (Anchor::Instance->cutsceneTextAdvancePendingCount > 0 &&
        Anchor::Instance->cutsceneTextAdvanceConsumedTextId != (uint16_t)currentTextId) {
        Anchor::Instance->cutsceneTextAdvancePendingCount = 0;
    }

    // Multi-player dialogue redesign (#191 follow-up) —
    // "alone in cutscene" detection.
    //
    // Vote-and-countdown semantics only make sense when MULTIPLE team
    // members share the same cutscene (e.g. the post-Goma sequence
    // both clients see). When the local player is the only client in
    // a cutscene state — typical for NPC dialog and per-player
    // scripted scenes like the Great Deku Tree opening cutscene where
    // only the player who triggered it sees the textbox — the
    // countdown becomes pure friction (the timer must elapse before
    // the local player's button press takes effect).
    //
    // Walk online team members in the same scene + timeline. If none
    // is in cutscene state with us, treat as solo and return the
    // vanilla input directly. Otherwise route through the voting
    // flow.
    //
    // The csCtxState field on AnchorClient is updated from PLAYER_UPDATE
    // (60 pps), so the detection picks up peer transitions into/out of
    // the cutscene within ~16 ms. Pre-update peers default to
    // CS_STATE_IDLE — safe (treated as not-in-cutscene).
    if (gPlayState != nullptr) {
        bool peerInCutscene = false;
        int16_t myScene    = (int16_t)gPlayState->sceneNum;
        uint8_t myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
        std::string myTeamId =
            CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        int diagPeerCount = 0;
        int diagPeerInCutsceneCount = 0;
        for (auto& [cid, client] : Anchor::Instance->clients) {
            if (client.self) continue;
            if (!client.online) continue;
            if (!client.isSaveLoaded) continue;
            if (client.sceneNum != myScene) continue;
            if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
            if (client.teamId != myTeamId) continue;
            diagPeerCount++;
            if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;
            diagPeerInCutsceneCount++;
            peerInCutscene = true;
            break;
        }
        if (diagEnabled && wasLocalPressDetected && !s_diagLastPressed) {
            SPDLOG_INFO("[CutsceneBridge.diag] textId=0x{:04X} press: "
                        "peersInScene={} peersInCutscene={} → branch={}",
                        (unsigned)currentTextId, diagPeerCount,
                        diagPeerInCutsceneCount,
                        peerInCutscene ? "vote" : "solo");
        }
        if (!peerInCutscene) {
            // Solo cutscene — vanilla parity for input.
            //
            // Solo idle auto-advance (#191 follow-up): if no input
            // for `kSoloDialogIdleAdvanceMs` (default 10s, tunable
            // via gAnchor.SoloDialogIdleAutoAdvanceMs), force-advance
            // so AFK / accessibility players don't get stuck on a
            // single textbox indefinitely. Resets on textId edge
            // (new textbox starts fresh) and on input (player is
            // engaged again).
            //
            // Function-local statics suffice since this is per-Anchor
            // state on the game thread (Message_ShouldAdvance is
            // called from the game tick).
            static int64_t  s_soloIdleStartMs    = 0;
            static uint16_t s_soloIdleLastTextId = 0;

            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t idleThresholdMs = (int64_t)CVarGetInteger(
                CVAR_REMOTE_ANCHOR("SoloDialogIdleAutoAdvanceMs"), 10000);

            if (s_soloIdleLastTextId != (uint16_t)currentTextId) {
                s_soloIdleLastTextId = (uint16_t)currentTextId;
                s_soloIdleStartMs = nowMs;
            }
            if (wasLocalPressDetected) {
                s_soloIdleStartMs = nowMs;
                // Design E v4 — shadow update on solo-mode local
                // press return-1. Vanilla's AWAIT_NEXT case will do
                // msgBufPos++ after this return. Without this, the
                // shadow stays at its last-known value (typically 0
                // for a fresh textbox) even though vanilla advances
                // msgBufPos through multiple subs. Log 644 root
                // cause: leader user pressed A four times in solo
                // mode (peer late-joining but not yet in cs_state),
                // shadow stayed at 0 for 10 s until first
                // hard_deadline. See Analysis/design_e_v3_shadow_
                // alone_mode_gap_2026-07-08.md.
                if (gPlayState != nullptr &&
                    gPlayState->msgCtx.msgMode == MSGMODE_TEXT_AWAIT_NEXT) {
                    Anchor::Instance->leaderMsgBufPosLastSubStart =
                        (uint16_t)(gPlayState->msgCtx.msgBufPos + 1);
                }
                s_diagLastPressed = wasLocalPressDetected;
                return 1;
            }
            if (idleThresholdMs > 0 &&
                nowMs - s_soloIdleStartMs >= idleThresholdMs) {
                SPDLOG_INFO("[CutsceneText] Solo idle auto-advance after {} ms (textId=0x{:04X})",
                            (long long)idleThresholdMs, (unsigned)currentTextId);
                s_soloIdleStartMs = nowMs;  // prevent immediate re-fire next frame
                // Design E v4 — same shadow update as the solo-local
                // press path above. Solo idle auto-advance also
                // fires vanilla's msgBufPos++ via the AWAIT_NEXT
                // case return.
                if (gPlayState != nullptr &&
                    gPlayState->msgCtx.msgMode == MSGMODE_TEXT_AWAIT_NEXT) {
                    Anchor::Instance->leaderMsgBufPosLastSubStart =
                        (uint16_t)(gPlayState->msgCtx.msgBufPos + 1);
                }
                s_diagLastPressed = wasLocalPressDetected;
                return 1;
            }
            s_diagLastPressed = wasLocalPressDetected;
            return 0;
        }
    }

    // Local press → forward to host as a vote.
    if (wasLocalPressDetected) {
        Anchor::Instance->SendPacket_CutsceneTextAdvance((uint16_t)currentTextId);
    }

    s_diagLastPressed = wasLocalPressDetected;

    // Don't immediately advance — wait for the host's broadcast.
    return 0;
}
