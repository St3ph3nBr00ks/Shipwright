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

int GetCutsceneInitialDurationMs() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.InitialDurationMs"),
                          kDefaultInitialDurationMs);
}

int GetCutscenePerPressReductionMs() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.PerPressReductionMs"),
                          kDefaultPerPressReductionMs);
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

}  // namespace

void Anchor::SendPacket_CutsceneTextAdvance(uint16_t textId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    nlohmann::json payload;
    payload["type"]        = CUTSCENE_TEXT_ADVANCE;
    payload["sceneNum"]    = (int)gPlayState->sceneNum;
    payload["textId"]      = (int)textId;
    PacketTimeline::SetTimelineField(payload);

    // Routed to the effective scene host of (sceneNum, my-roomNum, timeline).
    const uint32_t target = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    payload["targetClientId"] = target;

    SPDLOG_INFO("[CutsceneTextAdvance] Sending textId=0x{:04X} sceneNum={} target={}",
                (unsigned)textId, (int)gPlayState->sceneNum, target);

    SendJsonToRemote(payload);
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

    // Host-side gate. Only the room host accumulates votes.
    if (!::SceneAuthority::IsRoomHost(sceneNum,
                                       (int8_t)gPlayState->roomCtx.curRoom.num,
                                       (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    if (gPlayState->sceneNum != sceneNum) return;

    uint32_t senderId = payload.value("clientId", (uint32_t)0);
    if (senderId == 0) return;

    auto& state = cutsceneTextAdvanceState;

    // New textbox edge — reset state.
    if (!state.active || state.textId != textId || state.sceneNum != sceneNum) {
        state.active           = true;
        state.textId           = textId;
        state.sceneNum         = sceneNum;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
    }

    // Multi-press dedup — same client tapping repeatedly only counts once.
    if (state.pressedClientIds.count(senderId)) {
        return;
    }
    state.pressedClientIds.insert(senderId);

    auto now = std::chrono::steady_clock::now();
    if (!state.countdownStarted) {
        state.countdownStarted = true;
        state.countdownEndsAt  = now + std::chrono::milliseconds(GetCutsceneInitialDurationMs());
    } else {
        state.countdownEndsAt -= std::chrono::milliseconds(GetCutscenePerPressReductionMs());
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
}

void Anchor::TickCutsceneTextAdvance() {
    if (!isConnected) return;
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    auto& state = cutsceneTextAdvanceState;
    if (!state.active || !state.countdownStarted) return;

    // Only the host runs the countdown.
    if (!::SceneAuthority::IsRoomHost(state.sceneNum,
                                       (int8_t)gPlayState->roomCtx.curRoom.num,
                                       (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now >= state.countdownEndsAt) {
        SPDLOG_INFO("[CutsceneTextAdvance] Timer elapsed for textId=0x{:04X}, broadcasting",
                    (unsigned)state.textId);
        // Same order flip as the all-pressed branch above — state
        // cleared broadcast goes out BEFORE CUTSCENE_TEXT_ADVANCED so
        // peers hide their HUD in the same frame as the local advance
        // instead of one packet-arrival later.
        uint16_t clearedTextId = state.textId;
        cutsceneTextAdvanceConsumed = true;
        cutsceneTextAdvanceConsumedTextId = state.textId;
        state.active = false;
        state.pressedClientIds.clear();
        state.countdownStarted = false;
        SendPacket_CutsceneTextVoteState();
        SendPacket_CutsceneTextAdvanced(clearedTextId, "timer");
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

    // ms remaining as a signed clamp — 0 when countdown not started or
    // has already elapsed. Peers use this to derive their own local
    // endsAt so the display counts down in local time.
    int64_t msRemaining = 0;
    if (state.countdownStarted) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                         state.countdownEndsAt - now).count();
        msRemaining = delta > 0 ? delta : 0;
    }

    nlohmann::json votedIds = nlohmann::json::array();
    for (uint32_t cid : state.pressedClientIds) {
        votedIds.push_back(cid);
    }

    nlohmann::json payload;
    payload["type"]              = CUTSCENE_TEXT_VOTE_STATE;
    payload["sceneNum"]          = (int)state.sceneNum;
    payload["textId"]            = (int)state.textId;
    payload["active"]            = state.active;
    payload["countdownStarted"]  = state.countdownStarted;
    payload["msRemaining"]       = msRemaining;
    payload["pressedClientIds"]  = votedIds;
    payload["targetTeamId"]      = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);
    payload["quiet"]             = true;  // relay logging noise-reducer

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneTextVoteState(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("CutsceneAdvance.Enabled"), 1)) return;

    // Only peers apply — the host's authoritative state comes from its
    // local vote-tally flow, not a wire echo. Belt-and-suspenders since
    // the relay wouldn't route the packet back to sender in normal
    // flow, but a self-broadcast bug would be silently absorbed.
    if (::SceneAuthority::IsRoomHost((int16_t)gPlayState->sceneNum,
                                      (int8_t)gPlayState->roomCtx.curRoom.num,
                                      (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    auto& state = cutsceneTextAdvanceState;
    state.sceneNum         = (int16_t)payload.value("sceneNum", -1);
    state.textId           = (uint16_t)payload.value("textId", 0);
    state.active           = payload.value("active", false);
    state.countdownStarted = payload.value("countdownStarted", false);

    // Convert wire msRemaining → local endsAt.
    int64_t msRemaining = payload.value("msRemaining", (int64_t)0);
    if (state.countdownStarted && msRemaining > 0) {
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
