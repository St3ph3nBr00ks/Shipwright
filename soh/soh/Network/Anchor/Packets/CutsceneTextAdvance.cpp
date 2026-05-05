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

// Count team members currently in the same scene + same timeline as
// the local client. The all-pressed advance condition needs the
// expected team size to compare against `pressedClientIds.size()`.
//
// "Online + saveLoaded + same scene + same timeline" matches the
// receive scope for cutscene-text packets. Includes self.
size_t CountInSceneTeamSize() {
    if (gPlayState == nullptr) return 0;
    if (!::Anchor::Instance) return 1;
    int16_t  scene    = (int16_t)gPlayState->sceneNum;
    uint8_t  timeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    std::string ownTeamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    size_t count = 1;  // self
    for (auto& [cid, client] : ::Anchor::Instance->clients) {
        if (client.self) continue;
        if (!client.online) continue;
        if (!client.isSaveLoaded) continue;
        if (client.sceneNum != scene) continue;
        if ((uint8_t)(client.linkAge & 0x1) != timeline) continue;
        if (client.teamId != ownTeamId) continue;
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

    // All-pressed check (includes host, who votes via local press too).
    size_t teamSize = CountInSceneTeamSize();
    if (state.pressedClientIds.size() >= teamSize) {
        SendPacket_CutsceneTextAdvanced(textId, "all_pressed");
        // Also locally consume — host's own Message_ShouldAdvance picks it up.
        cutsceneTextAdvanceConsumed = true;
        cutsceneTextAdvanceConsumedTextId = textId;
        state.active = false;
        state.pressedClientIds.clear();
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
        SendPacket_CutsceneTextAdvanced(state.textId, "timer");
        cutsceneTextAdvanceConsumed = true;
        cutsceneTextAdvanceConsumedTextId = state.textId;
        state.active = false;
        state.pressedClientIds.clear();
    }
}
