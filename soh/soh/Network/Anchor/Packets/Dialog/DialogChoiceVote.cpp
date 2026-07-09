// DIALOG_CHOICE_* packet family — MP choice-textbox vote system.
//
// Landed 2026-07-09 on feature/dialog-choice-vote. Solves the case where
// vanilla NPC / cutscene choice-textboxes allow the local player to
// unilaterally pick which option all peers proceed with, causing a
// desync where each client's `msgCtx.choiceIndex` diverges.
//
// Design: plurality vote with 10 s countdown starting on FIRST vote
// (does not restart on subsequent votes). First-vote-wins tiebreaker.
// Early exit when every same-scene same-team peer has voted. See
// Analysis/dialog_choice_vote_design_v2_2026-07-09.md for full design
// + wire-format spec.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/RecentlyConsumedVotes.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/CutsceneCatchup.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace {

// Vote-skip host resolution — cutscene originator when a cutscene is
// active, room-host otherwise. Duplicates the pattern from
// CutsceneTextAdvance.cpp `ResolveVoteSkipHostClientId` because that
// function is file-scope-static there. Same logic — safe extraction
// into a shared helper in a future DRY pass.
uint32_t ResolveDialogChoiceHostClientId() {
    if (Anchor::Instance == nullptr) return 0;
    if (!Anchor::Instance->cutsceneStartActive.empty() &&
        !Anchor::Instance->cutsceneOriginatorByKindKey.empty()) {
        const auto it = Anchor::Instance->cutsceneOriginatorByKindKey.begin();
        if (it != Anchor::Instance->cutsceneOriginatorByKindKey.end() &&
            it->second != 0) {
            return it->second;
        }
    }
    if (gPlayState == nullptr) return 0;
    return ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
}

bool AmDialogChoiceHost() {
    if (Anchor::Instance == nullptr) return false;
    const uint32_t host = ResolveDialogChoiceHostClientId();
    return host != 0 && host == Anchor::Instance->ownClientId;
}

// Count same-scene same-timeline same-team peers who could vote (plus
// self). Same logic as CountInSceneTeamSize in CutsceneTextAdvance.cpp.
size_t CountInSceneTeamSize() {
    if (gPlayState == nullptr) return 0;
    if (!Anchor::Instance) return 1;
    int16_t  scene    = (int16_t)gPlayState->sceneNum;
    uint8_t  timeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    std::string ownTeamId =
        CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    size_t count = 1;
    for (auto& [cid, client] : Anchor::Instance->clients) {
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

// Straggler-drop buffer (shared pattern with vote-skip host). Each
// consumer instantiates its own — dedup domains don't cross.
RecentlyConsumedVotes::Buffer sRecentlyConsumedChoiceVotesBuffer;

// Choice-vote timer default. User-locked: 10 s countdown from first
// vote, does NOT restart on subsequent votes.
constexpr int kDialogChoiceCountdownMs = 10000;

// Cooldown window (post-resolution) — drop stale votes for the same
// textId that arrive within this window after resolution. Mirrors
// vote-skip Fix J.b behavior.
constexpr int kDialogChoiceResolvedCooldownMs = 750;

// Argmax with first-vote-wins tiebreaker. Given the current tally +
// firstVoteChoiceIndex sentinel, return the winning choice.
uint8_t ComputeWinningChoice(
    const std::unordered_map<uint32_t, uint8_t>& votes,
    uint8_t numChoices,
    uint8_t firstVoteChoiceIndex) {
    if (votes.empty()) return firstVoteChoiceIndex;  // safety
    // Tally per-choice counts.
    std::vector<uint16_t> tally(numChoices, 0);
    for (const auto& [cid, choice] : votes) {
        if (choice < numChoices) tally[choice]++;
    }
    // Find max.
    uint16_t maxCount = 0;
    for (auto n : tally) if (n > maxCount) maxCount = n;
    // Tiebreak: if firstVoteChoiceIndex is among the max, use it.
    if (firstVoteChoiceIndex < numChoices &&
        tally[firstVoteChoiceIndex] == maxCount) {
        return firstVoteChoiceIndex;
    }
    // Otherwise argmax by first-index-with-max-count.
    for (uint8_t i = 0; i < numChoices; i++) {
        if (tally[i] == maxCount) return i;
    }
    return firstVoteChoiceIndex;  // unreachable
}

}  // namespace

// ---- SendPacket_DialogChoiceVote ------------------------------------

void Anchor::SendPacket_DialogChoiceVote(uint16_t textId,
                                          uint8_t choiceIndex,
                                          uint8_t numChoices) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    nlohmann::json payload;
    payload["type"]         = DIALOG_CHOICE_VOTE;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["textId"]       = (int)textId;
    payload["choiceIndex"]  = (int)choiceIndex;
    payload["numChoices"]   = (int)numChoices;
    PacketTimeline::SetTimelineField(payload);

    const uint32_t target = ResolveDialogChoiceHostClientId();
    payload["targetClientId"] = target;

    SPDLOG_INFO("[DialogChoiceVote] Sending textId=0x{:04X} choice={} "
                "numChoices={} target={}",
                (unsigned)textId, (int)choiceIndex, (int)numChoices,
                target);

    SendJsonToRemote(payload);

    // Fix V pattern — host self-invokes HandlePacket to count its own
    // vote synchronously, in case relay loopback stalls. Straggler-drop
    // (§below) prevents the loopback duplicate from re-counting.
    if (AmDialogChoiceHost()) {
        nlohmann::json selfPayload = payload;
        selfPayload["clientId"] = ownClientId;
        HandlePacket_DialogChoiceVote(selfPayload);
    }
}

// ---- HandlePacket_DialogChoiceVote (host-side tally) ----------------

void Anchor::HandlePacket_DialogChoiceVote(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    if (!AmDialogChoiceHost()) return;

    const int16_t  sceneNum    = (int16_t)payload.value("sceneNum", -1);
    const uint16_t textId      = (uint16_t)payload.value("textId", 0);
    const uint8_t  choiceIndex = (uint8_t)payload.value("choiceIndex", 0);
    const uint8_t  numChoices  = (uint8_t)payload.value("numChoices", 2);

    if (gPlayState->sceneNum != sceneNum) return;

    const uint32_t senderId = payload.value("clientId", (uint32_t)0);
    if (senderId == 0) return;

    // Straggler-drop dedup — Fix V loopback duplicate.
    const auto now = std::chrono::steady_clock::now();
    if (sRecentlyConsumedChoiceVotesBuffer.ShouldDrop(textId, senderId, now)) {
        SPDLOG_INFO("[DialogChoiceVote] Straggler-vote drop: sender={} "
                    "textId=0x{:04X} (Fix V loopback duplicate)",
                    senderId, (unsigned)textId);
        return;
    }

    // Post-resolution cooldown — drop stale votes for a textId that
    // has already been resolved. Prevents late-arriving votes from
    // starting a fresh cycle for the same choice.
    if (dialogChoiceLastResolvedTextId == textId) {
        const auto sinceResolvedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - dialogChoiceLastResolvedAt).count();
        if (sinceResolvedMs < kDialogChoiceResolvedCooldownMs) {
            SPDLOG_INFO("[DialogChoiceVote] Post-resolution drop: sender={} "
                        "textId=0x{:04X} arrived {} ms after resolution",
                        senderId, (unsigned)textId,
                        (long long)sinceResolvedMs);
            return;
        }
    }

    auto& state = dialogChoiceVoteState;
    const bool newTextbox = !state.active || state.textId != textId ||
                            state.sceneNum != sceneNum;
    if (newTextbox) {
        // Fresh vote cycle.
        state.active            = true;
        state.textId            = textId;
        state.sceneNum          = sceneNum;
        state.numChoices        = numChoices;
        state.votesByClient.clear();
        state.voteOrderByClient.clear();
        state.firstVoteChoiceIndex = choiceIndex;  // first-vote-wins sentinel
        state.countdownEndsAt   = now + std::chrono::milliseconds(
            kDialogChoiceCountdownMs);
        state.countdownStarted  = true;
        SPDLOG_INFO("[DialogChoiceVote] New vote cycle textId=0x{:04X} "
                    "numChoices={} firstVoter={} firstChoice={}",
                    (unsigned)textId, (int)numChoices, senderId,
                    (int)choiceIndex);
    }

    // Insert / overwrite vote. Track first-time-vote for insertion
    // order (tiebreaker). Re-votes DON'T restart the timer — that's
    // the user-locked design (§1).
    const bool firstTimeVote = (state.votesByClient.find(senderId) ==
                                 state.votesByClient.end());
    state.votesByClient[senderId] = choiceIndex;
    if (firstTimeVote) {
        state.voteOrderByClient.push_back(senderId);
    }

    // Record for straggler-drop.
    sRecentlyConsumedChoiceVotesBuffer.Record(textId, senderId, now);

    SPDLOG_INFO("[DialogChoiceVote] Vote received clientId={} textId=0x{:04X} "
                "choice={} tally={} of {} team members",
                senderId, (unsigned)textId, (int)choiceIndex,
                (int)state.votesByClient.size(),
                (int)CountInSceneTeamSize());

    // Broadcast updated state.
    SendPacket_DialogChoiceVoteState();

    // All-voted early exit (§1 Q2 — user-locked yes).
    const size_t teamSize = CountInSceneTeamSize();
    if (state.votesByClient.size() >= teamSize) {
        // Resolve immediately via TickDialogChoiceVote's shared path.
        // Set countdown to now so the tick fires next call.
        state.countdownEndsAt = now;
        TickDialogChoiceVote();
    }
}

// ---- TickDialogChoiceVote (host-side timer expiry) ------------------

void Anchor::TickDialogChoiceVote() {
    if (gPlayState == nullptr) return;
    auto& state = dialogChoiceVoteState;
    if (!state.active) return;
    if (!AmDialogChoiceHost()) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < state.countdownEndsAt) return;

    // Resolve.
    const size_t teamSize = CountInSceneTeamSize();
    const bool   allVoted = state.votesByClient.size() >= teamSize;
    const uint8_t winning = ComputeWinningChoice(
        state.votesByClient, state.numChoices, state.firstVoteChoiceIndex);
    const uint16_t resolvedTextId = state.textId;
    const char* reason = allVoted ? "all_voted" : "timer_expired";

    SPDLOG_INFO("[DialogChoiceVote] Resolving textId=0x{:04X} winner={} "
                "reason={} votes={}/{} tiebreaker-first={}",
                (unsigned)resolvedTextId, (int)winning, reason,
                (int)state.votesByClient.size(), (int)teamSize,
                (int)state.firstVoteChoiceIndex);

    // Ledger record for late-join replay (§4 catchup integration).
    ::CutsceneCatchup::RecordDialogChoiceResolution(resolvedTextId, winning);

    // Broadcast resolution.
    SendPacket_DialogChoiceApplied(resolvedTextId, winning, reason);

    // Self-consume — set pending-apply so our own Anchor_Should-
    // AdvanceCutsceneTextLocal call fires the advance next tick.
    dialogChoicePendingApplyTextId = resolvedTextId;
    dialogChoicePendingApplyChoiceIndex = winning;

    // Clear state + arm post-resolution cooldown.
    state.active = false;
    state.votesByClient.clear();
    state.voteOrderByClient.clear();
    state.countdownStarted = false;
    dialogChoiceLastResolvedTextId = resolvedTextId;
    dialogChoiceLastResolvedAt = now;

    // Broadcast one final state = inactive.
    SendPacket_DialogChoiceVoteState();
}

// ---- SendPacket_DialogChoiceVoteState (host-side broadcast) ---------

void Anchor::SendPacket_DialogChoiceVoteState() {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!AmDialogChoiceHost()) return;

    auto& state = dialogChoiceVoteState;
    const auto now = std::chrono::steady_clock::now();

    int64_t msRemaining = 0;
    if (state.active) {
        const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                                state.countdownEndsAt - now).count();
        msRemaining = delta > 0 ? delta : 0;
    }

    nlohmann::json votes = nlohmann::json::array();
    for (const auto& [cid, choice] : state.votesByClient) {
        nlohmann::json v;
        v["clientId"]    = cid;
        v["choiceIndex"] = (int)choice;
        votes.push_back(v);
    }

    const uint64_t seq = ++dialogChoiceVoteStateSequence;

    nlohmann::json payload;
    payload["type"]              = DIALOG_CHOICE_VOTE_STATE;
    payload["seq"]               = seq;
    payload["sceneNum"]          = (int)state.sceneNum;
    payload["textId"]            = (int)state.textId;
    payload["numChoices"]        = (int)state.numChoices;
    payload["active"]            = state.active;
    payload["countdownStarted"]  = state.countdownStarted;
    payload["msRemaining"]       = msRemaining;
    payload["votes"]             = votes;
    payload["targetTeamId"]      = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"),
                                                  "default");
    payload["quiet"]             = true;
    PacketTimeline::SetTimelineField(payload);

    SendJsonToRemote(payload);
}

// ---- HandlePacket_DialogChoiceVoteState (peer-side mirror) ----------

void Anchor::HandlePacket_DialogChoiceVoteState(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    // Vote-skip host doesn't consume its own broadcast — it authored it.
    if (AmDialogChoiceHost()) return;

    // Session-monotonic seq (Pitfall 43).
    if (payload.contains("seq") && payload["seq"].is_number_unsigned()) {
        const uint64_t seq = payload["seq"].get<uint64_t>();
        if (seq <= peerLastAppliedDialogChoiceStateSeq) return;
        peerLastAppliedDialogChoiceStateSeq = seq;
    }

    auto& state = dialogChoiceVoteState;
    state.sceneNum         = (int16_t)payload.value("sceneNum", -1);
    state.textId           = (uint16_t)payload.value("textId", 0);
    state.numChoices       = (uint8_t)payload.value("numChoices", 2);
    state.active           = payload.value("active", false);
    state.countdownStarted = payload.value("countdownStarted", false);

    const int64_t msRemaining = payload.value("msRemaining", (int64_t)0);
    if (state.active && msRemaining > 0) {
        state.countdownEndsAt = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(msRemaining);
    }

    state.votesByClient.clear();
    state.voteOrderByClient.clear();
    if (payload.contains("votes") && payload["votes"].is_array()) {
        for (const auto& v : payload["votes"]) {
            if (v.contains("clientId") && v.contains("choiceIndex") &&
                v["clientId"].is_number_unsigned() &&
                v["choiceIndex"].is_number()) {
                const uint32_t cid = v["clientId"].get<uint32_t>();
                const uint8_t  ch  = (uint8_t)v["choiceIndex"].get<int>();
                state.votesByClient[cid] = ch;
                state.voteOrderByClient.push_back(cid);
            }
        }
    }
}

// ---- SendPacket_DialogChoiceApplied (host-side resolution) ----------

void Anchor::SendPacket_DialogChoiceApplied(uint16_t textId,
                                             uint8_t winningChoiceIndex,
                                             const char* reason) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    const uint64_t seq = ++dialogChoiceAppliedSequence;
    nlohmann::json payload;
    payload["type"]                = DIALOG_CHOICE_APPLIED;
    payload["seq"]                 = seq;
    payload["sceneNum"]            = (int)gPlayState->sceneNum;
    payload["textId"]              = (int)textId;
    payload["winningChoiceIndex"]  = (int)winningChoiceIndex;
    payload["reason"]              = std::string(reason ? reason : "unknown");
    payload["targetTeamId"]        = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"),
                                                    "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[DialogChoiceApplied] Broadcasting textId=0x{:04X} winner={} "
                "reason={}",
                (unsigned)textId, (int)winningChoiceIndex,
                reason ? reason : "unknown");

    SendJsonToRemote(payload);
}

// ---- HandlePacket_DialogChoiceApplied (peer-side pending-apply) -----

void Anchor::HandlePacket_DialogChoiceApplied(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    // Session-monotonic seq.
    if (payload.contains("seq") && payload["seq"].is_number_unsigned()) {
        const uint64_t seq = payload["seq"].get<uint64_t>();
        if (seq <= peerLastAppliedDialogChoiceAppliedSeq) return;
        peerLastAppliedDialogChoiceAppliedSeq = seq;
    }

    const int16_t  sceneNum = (int16_t)payload.value("sceneNum", -1);
    const uint16_t textId   = (uint16_t)payload.value("textId", 0);
    const uint8_t  winning  = (uint8_t)payload.value("winningChoiceIndex", 0);

    if (gPlayState->sceneNum != sceneNum) return;

    SPDLOG_INFO("[DialogChoiceApplied] Received textId=0x{:04X} winner={} "
                "reason={}",
                (unsigned)textId, (int)winning,
                payload.value("reason", std::string("unknown")).c_str());

    // Set pending-apply. Consumed by Anchor_ShouldAdvanceCutscene-
    // TextLocal on the next TEXT_STATE_CHOICE call for this textId.
    dialogChoicePendingApplyTextId = textId;
    dialogChoicePendingApplyChoiceIndex = winning;

    // Clear local vote-state mirror so HUD hides immediately.
    dialogChoiceVoteState.active = false;
    dialogChoiceVoteState.votesByClient.clear();
    dialogChoiceVoteState.voteOrderByClient.clear();
    dialogChoiceVoteState.countdownStarted = false;
}
