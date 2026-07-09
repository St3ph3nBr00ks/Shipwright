#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * CUTSCENE_TEXT_ADVANCE / CUTSCENE_TEXT_ADVANCED — voting-skip for
 * cutscene-internal textboxes.
 *
 * Vanilla `Message_ShouldAdvance` returns true when the local player
 * presses A/B/CUP. In MP that means each client's local press only
 * advances their own copy — clients drift out of sync during multi-page
 * cutscene dialogue (post-Goma "Great Deku Tree dies" sequence has 13
 * boxes; without this fix, players had to verbally coordinate).
 *
 * Design (from #191 user direction):
 *
 *   - First press from any client starts a countdown (default 5s).
 *   - Each additional press from a NEW client subtracts 1s from the
 *     countdown.
 *   - Advance fires when the countdown reaches 0 OR every team member
 *     in scene has pressed.
 *   - When advance fires, host broadcasts CUTSCENE_TEXT_ADVANCED.
 *   - Receivers set a one-shot flag that the next
 *     `Message_ShouldAdvance` call returns true for.
 *
 * Tunable via CVars (defaults match the user-direction spec):
 *   gAnchor.CutsceneAdvance.Enabled            (default 1)
 *   gAnchor.CutsceneAdvance.InitialDurationMs  (default 5000)
 *   gAnchor.CutsceneAdvance.PerPressReductionMs (default 1000)
 *
 * Distinct from #164 (CUTSCENE_START / CUTSCENE_END) which covers
 * cutscene boundaries; this packet covers progression *within* an
 * already-running cutscene.
 */

namespace {

constexpr int kDefaultInitialDurationMs   = 5000;
constexpr int kDefaultPerPressReductionMs = 1000;
// MP dialogue hard deadline — matches solo vanilla auto-advance so
// dialogue never stays open longer than the solo experience. Vote-
// skip runs as an inner-bound (min against this) so late presses
// don't extend the wait. Design: Claude/Analysis/cutscene_ff_reset_
// and_mp_dialogue_deadline_2026-07-08.md §2.
constexpr int kDefaultHardDeadlineMs      = 10000;

int GetCutsceneInitialDurationMs() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.InitialDurationMs"),
                          kDefaultInitialDurationMs);
}

int GetCutscenePerPressReductionMs() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.PerPressReductionMs"),
                          kDefaultPerPressReductionMs);
}

int GetCutsceneHardDeadlineMs() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.HardDeadlineMs"),
                          kDefaultHardDeadlineMs);
}

// Fix S / Fix T helper — replicate vanilla's AWAIT_NEXT sub-textbox
// advance transition directly. Called from top-of-tick (Fix S: catches
// flags set from network-thread packet handlers) and inline from the
// game-thread hard_deadline flag-setter (Fix T: same-tick race with
// top-of-tick check because the setter runs AFTER the check).
//
// Vanilla's mechanism to advance a multi-part cutscene textbox
// (delimited by MESSAGE_BOX_BREAK control codes) is
// z_message_PAL.c:4651-4657 :
//
//   case MSGMODE_TEXT_AWAIT_NEXT:
//       if (Message_ShouldAdvance(play)) {
//           msgCtx->msgMode = MSGMODE_TEXT_NEXT_MSG;
//           msgCtx->textUnskippable = false;
//           msgCtx->msgBufPos++;
//       }
//       break;
//
// SoH routes Message_ShouldAdvance through
// Anchor_ShouldAdvanceCutsceneTextLocal (CutsceneBridge.cpp), which
// returns 1 when the vote-skip TEXT_ADVANCED broadcast has been
// consumed. That should trigger the vanilla transition — but logs
// 638 + 639 show the vanilla transition silently does not stick on
// the host. This helper explicitly performs the transition when
// msgMode is AWAIT_NEXT, then clears the consume flag so vanilla's
// later Message_ShouldAdvance poll (if it fires) does not
// double-advance msgBufPos.
//
// Race-safety: always called from the game thread (either at the top
// of TickCutsceneTextAdvance, or inline from the hard_deadline
// branch of the same tick). Message_Update also runs on the game
// thread later in Play_Update (z_play.c:1295) — deterministic
// ordering, no torn writes to msgCtx.
//
// Idempotent — msgMode moves out of AWAIT_NEXT after the first fire.
// The clear of cutsceneTextAdvanceConsumed is what makes the helper
// safe to call at multiple sites within the same tick; subsequent
// calls no-op on the same broadcast.
void ApplyForcedSubTextAdvanceIfInAwaitNext(const char* siteTag) {
    if (gPlayState == nullptr) return;
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return;
    if (!anchor->cutsceneTextAdvanceConsumed) return;
    if ((uint16_t)gPlayState->msgCtx.textId !=
            anchor->cutsceneTextAdvanceConsumedTextId) return;
    if (gPlayState->msgCtx.msgMode != MSGMODE_TEXT_AWAIT_NEXT) return;
    SPDLOG_INFO("[CutsceneTextAdvance] Fix S/T ({}) — force AWAIT_NEXT→"
                "NEXT_MSG for textId=0x{:04X} msgBufPos={}",
                siteTag,
                (unsigned)gPlayState->msgCtx.textId,
                (int)gPlayState->msgCtx.msgBufPos);
    gPlayState->msgCtx.msgMode = MSGMODE_TEXT_NEXT_MSG;
    gPlayState->msgCtx.textUnskippable = false;
    gPlayState->msgCtx.msgBufPos++;
    // Design E v3 shadow — after msgBufPos++, this is the START of
    // the sub-textbox that Message_Decode is about to process. Save
    // it so the leader's SnapshotCamera captures a position peer's
    // Decode can start from cleanly. See Anchor.h
    // leaderMsgBufPosLastSubStart doc for full rationale.
    anchor->leaderMsgBufPosLastSubStart =
        (uint16_t)gPlayState->msgCtx.msgBufPos;
    anchor->cutsceneTextAdvanceConsumed = false;
}

// True when the local client is in a cutscene text state — the
// vanilla textbox is loaded and displayed as part of an ongoing
// cutscene. Used by the host to detect textbox-open edges and start
// the hard deadline timer.
//
// Predicates:
//   - Play_InCsMode: cutscene active (csCtx.state != IDLE OR
//     player linkAction non-null).
//   - msgCtx.msgMode != MSGMODE_NONE: message subsystem has an
//     active textbox loaded / displaying.
//   - msgCtx.textId != 0: sanity check that textId is a valid
//     dialogue reference.
bool IsHostInCutsceneTextState() {
    if (gPlayState == nullptr) return false;
    if (!Play_InCsMode(gPlayState)) return false;
    if (gPlayState->msgCtx.msgMode == MSGMODE_NONE) return false;
    if (gPlayState->msgCtx.textId == 0) return false;
    return true;
}

// Diag 1 helper — snapshot the vanilla state that drives the vote-state
// transition. Called from the host tick and from HandlePacket sites so
// every state.active transition is paired with a "why" log line the log
// 625 investigation identified as missing. Gated on
// gEnhancements.Anchor.CutsceneLateJoinDiag so production logs stay clean.
// See Analysis/cutscene_ff_reset_and_mp_dialogue_deadline_2026-07-08.md
// Bug 1 "same textId reopen" hypothesis.
void LogVoteStateTransitionReason(const char* reason,
                                   bool priorActive, bool newActive,
                                   uint16_t priorTextId, uint16_t newTextId) {
    if (CVarGetInteger(CVAR_ENHANCEMENT("Anchor.CutsceneLateJoinDiag"), 0) == 0) {
        return;
    }
    if (gPlayState == nullptr) return;
    const int  msgMode      = (int)gPlayState->msgCtx.msgMode;
    const unsigned msgTextId = (unsigned)gPlayState->msgCtx.textId;
    const int  csState      = (int)gPlayState->csCtx.state;
    const int  csFrames     = (int)gPlayState->csCtx.frames;
    const bool inCsMode     = Play_InCsMode(gPlayState);
    const bool inTextState  = IsHostInCutsceneTextState();
    // Diag D1 — msgLength + Message_GetState result. Vanilla's
    // Message_GetState (z_message_PAL.c:3116-3149) checks msgLength
    // FIRST for TEXT_STATE_NONE; if non-zero, msgMode determines the
    // returned state. Includes both `msgLength` (raw field) and the
    // computed `Message_GetState` result (which is what
    // Cutscene_Command_Textbox reads at z_demo.c:1665). Distinguishes
    // "message state fully clear (msgLength=0 → TEXT_STATE_NONE)"
    // from "msgMode zeroed but msgLength retained → TEXT_STATE_DONE_
    // FADING". Log 632 Fix L insufficiency was invisible without
    // this diagnostic.
    const int msgLength = (int)gPlayState->msgCtx.msgLength;
    const int msgGetState = (int)Message_GetState(&gPlayState->msgCtx);
    SPDLOG_INFO("[VoteState.diag] reason={} active {}→{} textId 0x{:04X}→0x{:04X} "
                "| Play_InCsMode={} inTextState={} msgMode={} msgTextId=0x{:04X} "
                "msgLen={} Message_GetState={} csState={} csFrames={}",
                reason, (int)priorActive, (int)newActive,
                (unsigned)priorTextId, (unsigned)newTextId,
                (int)inCsMode, (int)inTextState, msgMode, msgTextId,
                msgLength, msgGetState, csState, csFrames);
}

// Count team members currently in the same scene + same timeline +
// also in cutscene state as the local client. The all-pressed advance
// condition needs the expected vote count — peers NOT in cutscene
// (e.g. P2 in Kokiri Forest while P1 is in the Great Deku Tree
// opening cutscene) can't vote because they have no textbox; counting
// them blocks the all-pressed condition until the timer elapses.
//
// "Online + saveLoaded + same scene + same timeline + csCtxState !=
// CS_STATE_IDLE" matches the actual cutscene-participant scope.
// Includes self (the host's local cutscene state is non-IDLE
// whenever this function is called from the per-textbox vote tally).
//
// Multi-player dialogue redesign (#191 follow-up): the previous
// version counted ALL team members in scene, including peers not
// participating in the cutscene — those peers never voted, so the
// all-pressed condition only fired when teamSize was exactly the
// number of voters. Now teamSize equals the voter set, so all-
// pressed fires as soon as every cutscene-participant has voted.
size_t CountInSceneTeamSize() {
    if (gPlayState == nullptr) return 0;
    if (!::Anchor::Instance) return 1;
    int16_t  scene    = (int16_t)gPlayState->sceneNum;
    uint8_t  timeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    std::string ownTeamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    size_t count = 1;  // self (always in cutscene when tallying)
    for (auto& [cid, client] : ::Anchor::Instance->clients) {
        if (client.self) continue;
        if (!client.online) continue;
        if (!client.isSaveLoaded) continue;
        if (client.sceneNum != scene) continue;
        if ((uint8_t)(client.linkAge & 0x1) != timeline) continue;
        if (client.teamId != ownTeamId) continue;
        if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;
        count++;
    }
    return count;
}

// Fix E → J.b — file-scope suppression state shared between
// TickCutsceneTextAdvance (timer-elapsed path) and
// HandlePacket_CutsceneTextAdvance (all-pressed path). Both forced-
// advance paths arm this so the open-edge branch in the tick
// suppresses reopen for the same textId during vanilla's message-
// pipeline teardown window.
//
// Fix E (superseded by J.b): pure-time cooldown of 750 ms. Log 630
// showed vanilla msgMode stuck at 52 for >800 ms, causing the reopen
// to fire just past the cooldown (Bug 4). See
// Analysis/cutscene_savecontext_double_broadcast_2026-07-08.md Bug 4.
//
// Fix J.b: semantic-gate replacement. Suppression is lifted only when
// vanilla's `msgCtx.msgMode` has reached `MSGMODE_NONE` at least once
// since the forced advance was armed (the semantic signal that vanilla
// has actually processed the advance and closed its textbox), OR when
// `curTextId` differs from the advanced textId (a genuinely new
// textbox). A safety-net timeout of 5 s catches pathological cases
// where vanilla never returns to NONE — well beyond any legitimate
// vanilla close window (~30-100 ms in normal cutscenes).
uint16_t sLastForcedAdvanceTextId = 0;
bool sLastForcedAdvanceMsgModeReachedNone = false;
std::chrono::steady_clock::time_point sLastForcedAdvanceAt =
    std::chrono::steady_clock::time_point::min();
constexpr int kForcedAdvanceMaxWaitMs = 5000;

// Fix O — resolve vote-skip authority host for the current state.
// When a cutscene is active locally (any entry in cutsceneStartActive),
// authority is the cutscene ORIGINATOR (client that broadcast
// CUTSCENE_START). Otherwise fall back to room-host of local scene/
// room. Log 634 Bug 10: P1 and P2 landed in different rooms during
// deku_tree_intro; each became their own room-host, both broadcast
// vote-state, both ignored each other's broadcasts — no sync. Using
// the originator (a single clientId across all peers) collapses this
// to a single authoritative host. See Analysis/cutscene_room_desync_
// and_vote_scope_2026-07-08.md Bug 10.
uint32_t ResolveVoteSkipHostClientId() {
    if (Anchor::Instance == nullptr) return 0;
    if (!Anchor::Instance->cutsceneStartActive.empty() &&
        !Anchor::Instance->cutsceneOriginatorByKindKey.empty()) {
        // v1 assumes single-cutscene sessions; use first entry.
        const auto it = Anchor::Instance->cutsceneOriginatorByKindKey.begin();
        if (it != Anchor::Instance->cutsceneOriginatorByKindKey.end() &&
            it->second != 0) {
            return it->second;
        }
    }
    // No active cutscene (or originator unknown) — fall back to
    // room-host authority for non-cutscene textboxes.
    if (gPlayState == nullptr) return 0;
    return ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
}

// Fix O — am I the vote-skip authority? Reuses ResolveVoteSkipHostClientId.
bool AmVoteSkipHost() {
    if (Anchor::Instance == nullptr) return false;
    const uint32_t host = ResolveVoteSkipHostClientId();
    return host != 0 && host == Anchor::Instance->ownClientId;
}

}  // namespace

void Anchor::SendPacket_CutsceneTextAdvance(uint16_t textId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    nlohmann::json payload;
    payload["type"]        = CUTSCENE_TEXT_ADVANCE;
    payload["sceneNum"]    = (int)gPlayState->sceneNum;
    payload["textId"]      = (int)textId;
    PacketTimeline::SetTimelineField(payload);

    // Fix O.1 — route to the cutscene originator when a cutscene is
    // active; otherwise room-host of local scene/room. Handles the
    // case where peers landed in different rooms during a shared
    // cutscene (log 634 Bug 10).
    const uint32_t target = ResolveVoteSkipHostClientId();
    payload["targetClientId"] = target;

    SPDLOG_INFO("[CutsceneTextAdvance] Sending textId=0x{:04X} sceneNum={} target={}",
                (unsigned)textId, (int)gPlayState->sceneNum, target);

    SendJsonToRemote(payload);

    // Fix V — host self-counts its own vote without waiting for the
    // relay loopback packet. When the local client IS the vote-skip
    // host, the packet we just sent will be routed by the relay via
    // targetClientId=self back to us. If the network is healthy, that
    // packet arrives ~10-50ms later and HandlePacket_CutsceneText-
    // Advance tallies our vote. But if the incoming TCP direction
    // stalls (log 646: P1 rx dropped to 0.0 pps for 10s before formal
    // TCP disconnect at 17:47:00.222), our own vote never comes back,
    // vote-skip stalls until hard_deadline fires.
    //
    // Fix V: synthesize an equivalent payload representing "self
    // voted" and invoke HandlePacket_CutsceneTextAdvance directly.
    // HandlePacket's dedup (line 377: state.pressedClientIds.count
    // (senderId)) makes this idempotent — when the actual loopback
    // packet arrives later (if it ever does), the second call
    // returns early without double-counting.
    //
    // Only fires when we are the vote-skip host; peer-side clients
    // still rely on the host receiving their vote packet via the
    // normal relay path. See Analysis/log_646_vote_skip_regression_
    // 2026-07-08.md §7 Fix V.
    if (AmVoteSkipHost()) {
        nlohmann::json selfPayload = payload;
        // HandlePacket reads clientId as the sender identifier. The
        // relay stamps this on incoming packets, but for our direct
        // self-invoke we must set it explicitly.
        selfPayload["clientId"] = ownClientId;
        HandlePacket_CutsceneTextAdvance(selfPayload);
    }
}

void Anchor::SendPacket_CutsceneTextAdvanced(uint16_t textId, const char* reason) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    nlohmann::json payload;
    payload["type"]         = CUTSCENE_TEXT_ADVANCED;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["textId"]       = (int)textId;
    payload["reason"]       = std::string(reason ? reason : "unknown");
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);
    SPDLOG_INFO("[CutsceneTextAdvanced] Broadcasting textId=0x{:04X} reason={}",
                (unsigned)textId, reason ? reason : "unknown");
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneTextAdvance(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    int16_t sceneNum = (int16_t)payload.value("sceneNum", -1);
    uint16_t textId  = (uint16_t)payload.value("textId", 0);

    // Fix O.2 — host-side gate. Only the vote-skip authority
    // accumulates votes: cutscene originator when a cutscene is
    // active, room host otherwise.
    if (!AmVoteSkipHost()) {
        return;
    }

    if (gPlayState->sceneNum != sceneNum) return;

    uint32_t senderId = payload.value("clientId", (uint32_t)0);
    if (senderId == 0) return;

    auto& state = cutsceneTextAdvanceState;

    // New textbox edge — reset state. Also seed the hard-deadline
    // countdown here in case the vote packet arrives BEFORE
    // TickCutsceneTextAdvance's textbox-open detection has run (rare
    // race — peer presses within one game-thread tick of the textbox
    // appearing). Idempotent with the tick's own activation.
    auto nowForNewTextbox = std::chrono::steady_clock::now();
    if (!state.active || state.textId != textId || state.sceneNum != sceneNum) {
        state.active           = true;
        state.textId           = textId;
        state.sceneNum         = sceneNum;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        state.countdownEndsAt  = nowForNewTextbox + std::chrono::milliseconds(
            GetCutsceneHardDeadlineMs());
    }

    // Multi-press dedup — same client tapping repeatedly only counts once.
    if (state.pressedClientIds.count(senderId)) {
        return;
    }
    state.pressedClientIds.insert(senderId);

    auto now = std::chrono::steady_clock::now();
    if (!state.countdownStarted) {
        state.countdownStarted = true;
        // Vote-skip is an inner-bound clamped against the hard
        // deadline. If the hard-deadline countdown already has less
        // time left than the vote-skip default, keep the shorter
        // value — vote-skip never extends the wait beyond the
        // 10s auto-advance. See design analysis §2.4.
        auto voteSkipEndsAt = now + std::chrono::milliseconds(
            GetCutsceneInitialDurationMs());
        if (state.countdownEndsAt > voteSkipEndsAt) {
            state.countdownEndsAt = voteSkipEndsAt;
        }
    } else {
        state.countdownEndsAt -= std::chrono::milliseconds(GetCutscenePerPressReductionMs());
        // Never shrink below now — additional presses can't fire the
        // advance retroactively.
        if (state.countdownEndsAt < now) {
            state.countdownEndsAt = now;
        }
    }

    SPDLOG_INFO("[CutsceneTextAdvance] Vote received clientId={} textId=0x{:04X} "
                "votes={}/{}",
                senderId, (unsigned)textId,
                (int)state.pressedClientIds.size(),
                (int)CountInSceneTeamSize());

    // Broadcast the new state to peers so their CoopModalHud renders
    // the same dot row + countdown as the host. Fires on every vote-
    // received edge so peers see other clients' dots fill in real time.
    SendPacket_CutsceneTextVoteState();

    // All-pressed check (includes host, who votes via local press too).
    size_t teamSize = CountInSceneTeamSize();
    if (state.pressedClientIds.size() >= teamSize) {
        // Clear state FIRST so the state-cleared broadcast goes out
        // BEFORE CUTSCENE_TEXT_ADVANCED (TCP preserves send order →
        // peers process the HUD-hide before the local textbox
        // advance). Eliminates the visible-race window where peers'
        // HUD stayed shown across the advance because the state clear
        // arrived after ADVANCED.
        cutsceneTextAdvanceConsumed = true;
        cutsceneTextAdvanceConsumedTextId = textId;
        state.active = false;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        // Fix E → J.b — arm the reopen-suppression (see file-scope
        // declaration and TickCutsceneTextAdvance's Fix J.b block for
        // rationale). Reset the msgMode-reached-NONE flag so the tick
        // detects vanilla actually closing the textbox before allowing
        // any reopen for the same textId.
        sLastForcedAdvanceTextId              = textId;
        sLastForcedAdvanceAt                  = std::chrono::steady_clock::now();
        sLastForcedAdvanceMsgModeReachedNone  = false;
        LogVoteStateTransitionReason(
            "handle_all_pressed",
            /*priorActive=*/true, /*newActive=*/false, textId, textId);
        SendPacket_CutsceneTextVoteState();
        SendPacket_CutsceneTextAdvanced(textId, "all_pressed");
    }
}

void Anchor::HandlePacket_CutsceneTextAdvanced(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    int16_t sceneNum = (int16_t)payload.value("sceneNum", -1);
    uint16_t textId  = (uint16_t)payload.value("textId", 0);
    if (gPlayState->sceneNum != sceneNum) return;

    SPDLOG_INFO("[CutsceneTextAdvanced] Received textId=0x{:04X} reason={}",
                (unsigned)textId,
                payload.value("reason", std::string("unknown")).c_str());

    const bool priorActive = cutsceneTextAdvanceState.active;
    const uint16_t priorTextId = cutsceneTextAdvanceState.textId;
    cutsceneTextAdvanceConsumed = true;
    cutsceneTextAdvanceConsumedTextId = textId;

    // Belt-and-suspenders — clear the local vote-state mirror so the
    // HUD hides immediately when the advance fires. The host's send
    // order (CUTSCENE_TEXT_VOTE_STATE cleared FIRST, then
    // CUTSCENE_TEXT_ADVANCED) already handles this over the wire, but
    // clearing here defends against packet reorder / drop scenarios
    // and any future path that fires ADVANCED without a preceding
    // state broadcast. Idempotent on the host — state is already
    // false there when this runs.
    cutsceneTextAdvanceState.active           = false;
    cutsceneTextAdvanceState.pressedClientIds.clear();
    cutsceneTextAdvanceState.countdownStarted = false;
    if (priorActive) {
        LogVoteStateTransitionReason(
            "peer_recv_advanced",
            priorActive, /*newActive=*/false, priorTextId, textId);
    }
}

void Anchor::TickCutsceneTextAdvance() {
    if (!isConnected) return;
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    // Fix S / Fix T — force sub-textbox chain advance when TEXT_ADVANCED
    // consumed. Runs at top of tick for peer paths (network-thread flag
    // set → picked up next game frame). Fix T ALSO invokes the same
    // helper inline at the game-thread hard_deadline flag setter below,
    // because within the same tick TickCutsceneTextAdvance's own
    // hard_deadline branch sets the flag AFTER this top-of-tick check
    // already ran → Message_Update fires later that same frame,
    // CutsceneBridge consumes the flag first, and Fix S at next frame's
    // top sees an empty flag. Log 639 showed P1 (host) stall + Solo idle
    // fallback while P2 (peer) advanced correctly via this top-of-tick
    // path. See detailed rationale on the helper below.
    ApplyForcedSubTextAdvanceIfInAwaitNext("top_of_tick");

    auto& state = cutsceneTextAdvanceState;
    auto now = std::chrono::steady_clock::now();

    // Fix O.2 — only the vote-skip authority tracks + broadcasts
    // textbox state. Cutscene originator when active, room host
    // otherwise.
    const int16_t mySceneNum = (int16_t)gPlayState->sceneNum;
    if (!AmVoteSkipHost()) return;

    // ---- Textbox open/close edge detection (MP hard deadline) -------
    //
    // Detect when the host's local msgCtx transitions to a NEW
    // cutscene textbox and start the 10s hard deadline. Also detect
    // textbox-close so state deactivates cleanly. Peers see this via
    // the CUTSCENE_TEXT_VOTE_STATE broadcast (state.countdownEndsAt is
    // converted to msRemaining on wire, peer recomputes local endsAt).
    //
    // This runs on the same tick even when nobody has voted — the
    // hard deadline is the auto-advance timer that MATCHES SOLO
    // BEHAVIOR (10s per textbox). Vote-skip is an inner-bound that
    // lets players advance faster.
    const bool inTextState = IsHostInCutsceneTextState();
    const uint16_t curTextId = inTextState
        ? (uint16_t)gPlayState->msgCtx.textId : (uint16_t)0;

    // Fix B — debounce inTextState==false transitions. Vanilla msgMode
    // briefly returns to MSGMODE_NONE for a frame or two between adjacent
    // cutscene textboxes (post-textbox → next-textbox-load window). Without
    // debounce, state.active rapidly toggles false → true on every gap,
    // which triggers redundant CUTSCENE_TEXT_VOTE_STATE broadcasts and
    // (per log 624) an ImGui multi-viewport quirk that spawns a second SoH
    // window. Track when inTextState was last true; only close state.active
    // if it's been continuously false for kInTextStateFalseDebounceMs.
    // See Analysis/cutscene_ff_reset_and_mp_dialogue_deadline_2026-07-08.md
    // Finding 2.
    static std::chrono::steady_clock::time_point sLastInTextStateTrueAt =
        std::chrono::steady_clock::time_point::min();
    constexpr int kInTextStateFalseDebounceMs = 300;
    if (inTextState) {
        sLastInTextStateTrueAt = now;
    }
    const bool debouncedInTextState =
        inTextState ||
        (sLastInTextStateTrueAt != std::chrono::steady_clock::time_point::min() &&
         std::chrono::duration_cast<std::chrono::milliseconds>(
             now - sLastInTextStateTrueAt).count() < kInTextStateFalseDebounceMs);

    // Fix E → J.b — post-advance reopen suppression. When the timer-
    // elapsed or all-pressed path fires an advance, vanilla takes
    // several frames to transition `msgMode` from DISPLAYING (53) /
    // stuck sub-states (e.g. 52 in log 630) through ADVANCING → CLOSING
    // (54) → NONE (0). During that window, IsHostInCutsceneTextState()
    // still returns true AND msgCtx.textId still holds the pre-
    // advance textId — so the open-edge branch below would re-open
    // state.active for the SAME textId we just advanced past.
    //
    // Fix E (pure 750 ms cooldown, superseded): worked for the common
    // case but log 630 revealed vanilla stuck at msgMode=52 for
    // ~800 ms, so the reopen fired just past cooldown (Bug 4).
    //
    // Fix J.b: gate reopen on vanilla's ACTUAL state, not wall-clock:
    //   - Suppress while sLastForcedAdvanceTextId == curTextId AND
    //     vanilla has NOT yet reached MSGMODE_NONE since the arm.
    //   - Different curTextId (vanilla progressed to next textbox) →
    //     suppression lifted immediately.
    //   - Safety-net timeout of kForcedAdvanceMaxWaitMs guards against
    //     a pathological vanilla state where msgMode never returns to
    //     NONE — otherwise the HUD would be locked out forever.
    //
    // The msgMode-reached-NONE flag is updated every tick below.
    if (gPlayState != nullptr &&
        gPlayState->msgCtx.msgMode == MSGMODE_NONE &&
        !sLastForcedAdvanceMsgModeReachedNone &&
        sLastForcedAdvanceTextId != 0) {
        sLastForcedAdvanceMsgModeReachedNone = true;
        if (CVarGetInteger(CVAR_ENHANCEMENT("Anchor.CutsceneLateJoinDiag"), 0) != 0) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - sLastForcedAdvanceAt).count();
            SPDLOG_INFO("[VoteState.diag] msgMode reached NONE {}ms after "
                        "forced advance of textId=0x{:04X} — reopen "
                        "suppression lifted",
                        (long long)ms, (unsigned)sLastForcedAdvanceTextId);
        }
    }
    const bool inForcedAdvanceCooldown =
        sLastForcedAdvanceTextId == curTextId &&
        sLastForcedAdvanceTextId != 0 &&
        !sLastForcedAdvanceMsgModeReachedNone &&
        sLastForcedAdvanceAt != std::chrono::steady_clock::time_point::min() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - sLastForcedAdvanceAt).count() < kForcedAdvanceMaxWaitMs;

    if (inTextState && !inForcedAdvanceCooldown &&
        (!state.active || state.textId != curTextId)) {
        // Textbox-open edge — new dialog just appeared on host.
        const bool priorActive = state.active;
        const uint16_t priorTextId = state.textId;
        state.active = true;
        state.textId = curTextId;
        state.sceneNum = mySceneNum;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        state.countdownEndsAt = now + std::chrono::milliseconds(
            GetCutsceneHardDeadlineMs());
        SPDLOG_INFO("[CutsceneTextAdvance] Textbox opened textId=0x{:04X} — "
                    "hard deadline {}ms",
                    (unsigned)curTextId, GetCutsceneHardDeadlineMs());
        LogVoteStateTransitionReason(
            (!priorActive) ? "tick_open_edge_from_inactive" : "tick_textid_change",
            priorActive, /*newActive=*/true, priorTextId, curTextId);
        SendPacket_CutsceneTextVoteState();
    } else if (!debouncedInTextState && state.active) {
        // Textbox-close edge — textbox was advanced or cutscene ended.
        // Clear state so HUD hides and the next open is a fresh cycle.
        // Only fires after the debounce window elapses so brief msgMode
        // gaps between adjacent textboxes don't trigger spurious closes.
        const uint16_t priorTextId = state.textId;
        state.active = false;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        LogVoteStateTransitionReason(
            "tick_debounced_close",
            /*priorActive=*/true, /*newActive=*/false, priorTextId, priorTextId);
        SendPacket_CutsceneTextVoteState();
    }

    // ---- Countdown expiry ------------------------------------------
    //
    // Fires whenever state.countdownEndsAt is reached, regardless of
    // whether the countdown was started by a vote press or the hard
    // deadline. Vote-press-triggered countdown is clamped against the
    // hard deadline in HandlePacket_CutsceneTextAdvance so it can't
    // extend past the hard limit.
    if (state.active && now >= state.countdownEndsAt) {
        SPDLOG_INFO("[CutsceneTextAdvance] Timer elapsed for textId=0x{:04X}, "
                    "broadcasting (reason={})",
                    (unsigned)state.textId,
                    state.countdownStarted ? "vote_timer" : "hard_deadline");
        // Same order flip as the all-pressed branch — state cleared
        // broadcast goes out BEFORE CUTSCENE_TEXT_ADVANCED so peers
        // hide their HUD in the same frame as the local advance
        // instead of one packet-arrival later.
        uint16_t clearedTextId = state.textId;
        const bool wasVoteTimer = state.countdownStarted;
        cutsceneTextAdvanceConsumed = true;
        cutsceneTextAdvanceConsumedTextId = state.textId;
        state.active = false;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        // Fix E → J.b — arm the reopen-suppression so the next few
        // ticks don't re-open state.active for the same textId while
        // vanilla's message pipeline is still tearing down the
        // advanced textbox. Reset the msgMode-reached-NONE flag so
        // the tick detects vanilla actually closing the textbox
        // before allowing any reopen for the same textId.
        sLastForcedAdvanceTextId              = clearedTextId;
        sLastForcedAdvanceAt                  = now;
        sLastForcedAdvanceMsgModeReachedNone  = false;
        LogVoteStateTransitionReason(
            wasVoteTimer ? "tick_timer_elapsed_vote" : "tick_timer_elapsed_hard_deadline",
            /*priorActive=*/true, /*newActive=*/false, clearedTextId, clearedTextId);
        SendPacket_CutsceneTextVoteState();
        SendPacket_CutsceneTextAdvanced(clearedTextId,
                                        wasVoteTimer ? "timer" : "hard_deadline");
        // Fix T — inline invocation of the sub-textbox force-advance
        // helper for the host game-thread hard_deadline path. The
        // top-of-tick Fix S check already ran earlier this tick with
        // the flag still false; without this inline call the flag
        // gets consumed by CutsceneBridge during Message_Update later
        // this same frame, and the next tick's top-of-tick Fix S sees
        // an empty flag. Log 639 P1 showed exactly this pattern:
        // "advance-broadcast consumed → local advance" fired but the
        // vanilla AWAIT_NEXT→NEXT_MSG transition never stuck,
        // leaving P1 stalled until the 10 s "Solo idle auto-advance"
        // fallback fired. See Analysis/cutscene_fix_t_host_timing_-
        // 2026-07-08.md (to be written) if this pattern recurs.
        ApplyForcedSubTextAdvanceIfInAwaitNext(
            wasVoteTimer ? "hard_deadline_vote" : "hard_deadline_timer");
    }
}

// ---------------------------------------------------------------------
// CUTSCENE_TEXT_VOTE_STATE — host → all-clients state broadcast.
//
// Sent every time the host mutates its local cutsceneTextAdvanceState
// (activation, new vote received, or explicit clear). Peers apply the
// payload to their local state so CoopModalHud has the same data on
// every client — filling dots as peers vote and displaying the same
// countdown text.
//
// Wire fields:
//   sceneNum         — scene scope
//   textId           — active textbox id (0 when active=false)
//   active           — host's state.active
//   countdownStarted — host's state.countdownStarted
//   msRemaining      — ms remaining on countdown at send time; peer
//                      converts to its local endsAt = now + msRemaining
//                      (small drift from packet latency acceptable at
//                       ~5s countdown scale)
//   pressedClientIds — array of client IDs that have voted
// ---------------------------------------------------------------------

void Anchor::SendPacket_CutsceneTextVoteState() {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    auto& state = cutsceneTextAdvanceState;

    // ms remaining as a signed clamp — 0 only when the countdown has
    // already elapsed. Peers use this to derive their own local endsAt
    // so the display counts down in local time.
    //
    // Fix C — calculate whenever `active` is true, not just when
    // `countdownStarted`. Both the hard-deadline countdown (started at
    // textbox-open) and the vote-skip countdown (started at first press)
    // write into state.countdownEndsAt, but only the vote-skip path sets
    // countdownStarted=true. Gating on countdownStarted broadcast
    // msRemaining=0 while the hard deadline was running, which peers
    // interpreted as "no time left" (or ignored via
    // countdownStarted=false), producing the log-624 flicker on peer HUDs.
    // See analysis Finding 3.
    int64_t msRemaining = 0;
    if (state.active) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                         state.countdownEndsAt - now).count();
        msRemaining = delta > 0 ? delta : 0;
    }

    nlohmann::json votedIds = nlohmann::json::array();
    for (uint32_t cid : state.pressedClientIds) {
        votedIds.push_back(cid);
    }

    // Monotonic seq — increments on every send to defend against relay-
    // side reordering (goroutine race in Anchor/anchor_git/room.go:56).
    // Session-monotonic (not per-cycle) so cross-cycle stale packets are
    // also rejected. Reset in OnSceneSpawnActors.
    const uint64_t seq = ++voteStateSequence;

    nlohmann::json payload;
    payload["type"]              = CUTSCENE_TEXT_VOTE_STATE;
    payload["seq"]               = seq;
    payload["sceneNum"]          = (int)state.sceneNum;
    payload["textId"]            = (int)state.textId;
    payload["active"]            = state.active;
    payload["countdownStarted"]  = state.countdownStarted;
    payload["msRemaining"]       = msRemaining;
    payload["pressedClientIds"]  = votedIds;
    payload["targetTeamId"]      = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);
    payload["quiet"]             = true;  // relay logging noise-reducer

    SPDLOG_INFO("[VoteState SEND] seq={} active={} textId={} votes={} msRemaining={}",
                seq, state.active, state.textId, votedIds.size(), msRemaining);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneTextVoteState(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    // Fix O.2 — only peers apply. Vote-skip authority (cutscene
    // originator when active, room host otherwise) owns its own state
    // and does not consume its own broadcasts.
    if (AmVoteSkipHost()) {
        return;
    }

    // Fix Q — receive-side gate. Peer applies VoteState only when
    // locally participating in a cutscene. Otherwise the HUD would
    // render "Press A to skip" for a cutscene the peer isn't in
    // (log 635 Bug 11: peer received VoteState 17 s before joining
    // the cutscene, HUD opened with Play_InCsMode=0 csState=0). See
    // Analysis/cutscene_hud_leak_and_latency_2026-07-08.md.
    if (!Play_InCsMode(gPlayState)) {
        return;
    }

    // Monotonic seq — reject packets older than the last one we applied.
    // Defends against relay-side reordering (goroutine race in
    // Anchor/anchor_git/room.go:56). Session-monotonic on the host, so
    // any cross-cycle late arrivals are also caught. Missing/absent seq
    // (e.g., older-build sender) applies without check for compat.
    if (payload.contains("seq") && payload["seq"].is_number_unsigned()) {
        const uint64_t seq = payload["seq"].get<uint64_t>();
        if (seq <= peerLastAppliedVoteStateSeq) {
            SPDLOG_INFO("[VoteState RECV] DROP stale seq={} lastApplied={} "
                        "(active={} textId={})",
                        seq, peerLastAppliedVoteStateSeq,
                        payload.value("active", false),
                        payload.value("textId", 0));
            return;
        }
        peerLastAppliedVoteStateSeq = seq;
        SPDLOG_INFO("[VoteState RECV] APPLY seq={} active={} textId={} votes={}",
                    seq, payload.value("active", false),
                    payload.value("textId", 0),
                    payload.contains("pressedClientIds")
                        ? payload["pressedClientIds"].size() : 0);
    }

    auto& state = cutsceneTextAdvanceState;
    const bool priorActive = state.active;
    const uint16_t priorTextId = state.textId;
    state.sceneNum         = (int16_t)payload.value("sceneNum", -1);
    state.textId           = (uint16_t)payload.value("textId", 0);
    state.active           = payload.value("active", false);
    state.countdownStarted = payload.value("countdownStarted", false);
    if (priorActive != state.active || priorTextId != state.textId) {
        LogVoteStateTransitionReason(
            "peer_recv_vote_state",
            priorActive, state.active, priorTextId, state.textId);
    }

    // Convert wire msRemaining → local endsAt.
    // Fix C — apply whenever active + msRemaining > 0. Sender's send-side
    // Fix C already broadcasts msRemaining for the hard-deadline countdown
    // (which has countdownStarted=false). Gating this on countdownStarted
    // would drop the hard-deadline timer on peers → HUD countdown display
    // never appears until first vote press.
    int64_t msRemaining = payload.value("msRemaining", (int64_t)0);
    if (state.active && msRemaining > 0) {
        state.countdownEndsAt = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(msRemaining);
    }

    // Replace the voter set from the wire payload.
    state.pressedClientIds.clear();
    if (payload.contains("pressedClientIds") &&
        payload["pressedClientIds"].is_array()) {
        for (const auto& id : payload["pressedClientIds"]) {
            if (id.is_number_unsigned()) {
                state.pressedClientIds.insert(id.get<uint32_t>());
            }
        }
    }
}
