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
#include "soh/Network/Anchor/Bridge/AnchorMessageBridge.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <chrono>
#include <cstdint>
#include <string>

extern "C" {
#include "z64.h"
#include "functions.h"  // Message_GetState
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace {

// Choice-vote branch (2026-07-09) — invoked at TOP of
// Anchor_ShouldAdvanceCutsceneTextLocal when Message_GetState returns
// TEXT_STATE_CHOICE. Handles all choice-textbox advance decisions
// separately from the yes/no vote-skip logic below.
//
// Return values match the outer function:
//   1 — local message system advances THIS frame (msgCtx.choiceIndex
//       has been set to the winning value if applicable).
//   0 — local press was captured as a vote (or peer is waiting for
//       vote resolution). Do not advance.
//
// See Analysis/dialog_choice_vote_design_v2_2026-07-09.md §5 for the
// decision tree.
int HandleChoiceVoteAdvance(int wasLocalPressDetected,
                             unsigned currentTextId) {
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return wasLocalPressDetected ? 1 : 0;

    const uint16_t textId = (uint16_t)currentTextId;

    // (A) Late-join catchup replay: if the leader already resolved this
    // textId's choice and shipped it in the catchup delta, apply
    // immediately.
    auto lateJoinIt = anchor->dialogChoiceLateJoinResolutions.find(textId);
    if (lateJoinIt != anchor->dialogChoiceLateJoinResolutions.end()) {
        AnchorMessageBridge::SetChoiceIndex(lateJoinIt->second);
        SPDLOG_INFO("[DialogChoiceVote] Late-join replay: textId=0x{:04X} "
                    "winner={} (from catchup ledger)",
                    (unsigned)textId, (int)lateJoinIt->second);
        anchor->dialogChoiceLateJoinResolutions.erase(lateJoinIt);
        return 1;
    }

    // (B) Solo mode — no same-scene team peers ALSO IN CUTSCENE STATE,
    // so vanilla behavior: local A press advances immediately.
    //
    // Fix L.1 (2026-07-10) — filter also by client.csCtxState. Without
    // this, when a peer is in the same scene but NOT in the same
    // cutscene (e.g., Fix J-alt opt-in come-back: initiator in cutscene,
    // peer in gameplay), the peer would incorrectly count as "someone
    // to vote with" — the vote flow would then send DIALOG_CHOICE_VOTE
    // packets that leak the vote UI onto the peer's screen AND make
    // the initiator wait for a vote from a peer who can't cast one.
    // See Analysis/dialog_choice_vote_scope_leaks_to_non_cutscene_peers_
    // 2026-07-10.md Fix L.
    //
    // client.csCtxState is broadcast per PLAYER_UPDATE (~60Hz) per
    // AnchorClient.h:148. Defaults to CS_STATE_IDLE (0) for pre-update
    // peers — safe (treated as not-in-cutscene, matching design
    // intent).
    bool hasPeer = false;
    if (gPlayState != nullptr) {
        int16_t myScene = (int16_t)gPlayState->sceneNum;
        uint8_t myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
        std::string myTeamId =
            CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        for (auto& [cid, client] : anchor->clients) {
            if (client.self) continue;
            if (!client.online) continue;
            if (!client.isSaveLoaded) continue;
            if (client.sceneNum != myScene) continue;
            if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
            if (client.teamId != myTeamId) continue;
            if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;
            hasPeer = true;
            break;
        }
    }
    if (!hasPeer) {
        return wasLocalPressDetected ? 1 : 0;
    }

    // (C) Pending-apply from resolution broadcast — advance now.
    if (anchor->dialogChoicePendingApplyTextId == textId) {
        AnchorMessageBridge::SetChoiceIndex(
            anchor->dialogChoicePendingApplyChoiceIndex);
        SPDLOG_INFO("[DialogChoiceVote] Pending-apply consumed: textId=0x{:04X} "
                    "winner={}",
                    (unsigned)textId,
                    (int)anchor->dialogChoicePendingApplyChoiceIndex);
        anchor->dialogChoicePendingApplyTextId = 0;
        anchor->dialogChoicePendingApplyChoiceIndex = 0;
        return 1;
    }

    // (D) Local A press → send vote.
    if (wasLocalPressDetected) {
        const uint8_t localChoice = AnchorMessageBridge::GetChoiceIndex();
        const uint8_t numChoices  = AnchorMessageBridge::GetChoiceNumOptions();
        if (numChoices >= 2) {
            anchor->SendPacket_DialogChoiceVote(textId, localChoice,
                                                 numChoices);
        }
    }

    // (E) Waiting for vote resolution — do not advance.
    return 0;
}

}  // namespace

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

    // Choice-vote branch (2026-07-09) — takes priority over the
    // yes/no advance-skip logic below when the local message system is
    // at TEXT_STATE_CHOICE. Per §2.1 audit, all NPC + cutscene choice
    // paths route through Message_ShouldAdvance/Silent, so this
    // single-branch insertion covers all choice-textbox advance
    // decisions.
    if (gPlayState != nullptr &&
        Message_GetState(&gPlayState->msgCtx) == TEXT_STATE_CHOICE) {
        s_diagLastPressed = wasLocalPressDetected;
        return HandleChoiceVoteAdvance(wasLocalPressDetected, currentTextId);
    }

    // Consume the broadcast flag if it matches the current textbox.
    // Edge case: matched broadcast for a previous textId stays
    // consumed when we move to a new textId — the new textId's
    // vote count starts fresh.
    // Design E v5 — consumed-count decrement on return-1 path.
    // Replaces the earlier boolean flag which lost broadcasts on
    // back-to-back arrivals (log 646).
    if (Anchor::Instance->cutsceneTextAdvancePendingCount > 0 &&
        Anchor::Instance->cutsceneTextAdvancePendingTextId == (uint16_t)currentTextId) {
        Anchor::Instance->cutsceneTextAdvancePendingCount--;
        // Shadow update handled by R1's Anchor_OnMessageBufPosAdvanced
        // hook in z_message_PAL.c AWAIT_NEXT case — no need for the
        // redundant write that lived here.
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
        Anchor::Instance->cutsceneTextAdvancePendingTextId != (uint16_t)currentTextId) {
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
                // Shadow update handled by R1's Anchor_OnMessage-
                // BufPosAdvanced hook in z_message_PAL.c AWAIT_NEXT
                // case.
                s_diagLastPressed = wasLocalPressDetected;
                return 1;
            }
            if (idleThresholdMs > 0 &&
                nowMs - s_soloIdleStartMs >= idleThresholdMs) {
                SPDLOG_INFO("[CutsceneText] Solo idle auto-advance after {} ms (textId=0x{:04X})",
                            (long long)idleThresholdMs, (unsigned)currentTextId);
                s_soloIdleStartMs = nowMs;  // prevent immediate re-fire next frame
                // Shadow update handled by R1's hook.
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
