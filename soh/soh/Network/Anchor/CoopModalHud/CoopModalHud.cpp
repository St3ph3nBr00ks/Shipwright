#include "CoopModalHud.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/libultraship.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "variables.h"
extern PlayState* gPlayState;
}

// Co-op Modal HUD — Phase 1 implementation.
//
// Draw() consults Anchor::Instance->cutsceneTextAdvanceState directly.
// While `active` is true, renders vote dots + numeric countdown text
// above the vanilla textbox region. Design in
// Claude/Plans/coop_modal_hud_plan.md §5.1.
//
// Positioning: fixed-screen anchor, 25% up from bottom edge (plan
// §4.3(a) recommendation). Simpler than reading msgCtx.textBoxType and
// projecting N64 coords; reassess after field-test if the widget
// visually clashes with any textbox position.

namespace CoopModalHud {

namespace {

// ---- Colors (widget palette) ----------------------------------------

constexpr ImVec4 kBgColor       = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
constexpr ImVec4 kBorderColor   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
constexpr ImVec4 kPromptColor   = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
constexpr ImVec4 kTimerNormal   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
// Integer-timer thresholds: yellow when display reads "1s", red at "0s".
constexpr ImVec4 kTimerWarn     = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
constexpr ImVec4 kTimerCritical = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);

// Dot geometry (plan §5.1: ~16px diameter, ~6px spacing).
constexpr float kDotRadius     = 8.0f;
constexpr float kDotSpacing    = 22.0f;  // center-to-center
constexpr float kDotToTextGap  = 20.0f;
constexpr float kPadding       = 12.0f;

// Bottom-right anchor — fixed pixel margin from the viewport bottom-right
// corner. Widget pivots at (1.0, 1.0) so its bottom-right sits at
// (vp.right - margin, vp.bottom - margin). Works across all aspect
// ratios; positions predictably regardless of the vanilla textbox
// height/position. See §5.1 note in coop_modal_hud_plan.md — bottom-
// right of viewport is the KISS approximation of "bottom-right of
// dialogue box"; a future refinement can read msgCtx.textPosX/Y for
// pixel-accurate dialog-box-relative placement.
constexpr float kBottomRightMargin = 24.0f;

// ---- Peer enumeration -----------------------------------------------

// One dot to render — self-inclusive. `voted` reflects whether this
// clientId appears in `cutsceneTextAdvanceState.pressedClientIds`.
// Color from `client.color` (Color_RGB8 → ImColor).
struct DotEntry {
    uint32_t  clientId;
    ImU32     color;
    bool      voted;
};

ImU32 ColorFromRgb8(const Color_RGB8& c, float alpha) {
    return IM_COL32(c.r, c.g, c.b, (int)(alpha * 255.0f));
}

// Collect all peers (including self) that are eligible to vote:
// online + saveLoaded + same scene + same timeline + same teamId +
// csCtxState != IDLE. Mirrors the host-side voter set defined in
// CountInSceneTeamSize (Packets/Cutscene/CutsceneTextAdvance.cpp).
//
// Self is always included when Draw() is invoked — Draw() only runs
// when cutsceneTextAdvanceState.active is true, and active implies
// the local client is in a cutscene text state.
std::vector<DotEntry> CollectVoters(const Anchor& anchor) {
    std::vector<DotEntry> out;
    if (gPlayState == nullptr) return out;

    const int16_t myScene    = (int16_t)gPlayState->sceneNum;
    const uint8_t myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    const std::string myTeamId =
        CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    const auto& pressed = anchor.cutsceneTextAdvanceState.pressedClientIds;

    // Self entry first.
    {
        Color_RGB8 selfColor = { 255, 255, 255 };
        auto selfIt = anchor.clients.find(anchor.ownClientId);
        if (selfIt != anchor.clients.end()) {
            selfColor = selfIt->second.color;
        }
        DotEntry e;
        e.clientId = anchor.ownClientId;
        e.color    = ColorFromRgb8(selfColor, 1.0f);
        e.voted    = pressed.count(anchor.ownClientId) > 0;
        out.push_back(e);
    }

    for (const auto& [cid, client] : anchor.clients) {
        if (client.self) continue;
        if (!client.online) continue;
        if (!client.isSaveLoaded) continue;
        if (client.sceneNum != myScene) continue;
        if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
        if (client.teamId != myTeamId) continue;
        if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;

        DotEntry e;
        e.clientId = cid;
        e.color    = ColorFromRgb8(client.color, 1.0f);
        e.voted    = pressed.count(cid) > 0;
        out.push_back(e);
    }
    return out;
}

// ---- Time helpers ---------------------------------------------------

float RemainingSecondsFrom(
    const std::chrono::steady_clock::time_point& endsAt) {
    auto now = std::chrono::steady_clock::now();
    if (endsAt <= now) return 0.0f;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  endsAt - now).count();
    return (float)ms / 1000.0f;
}

// Integer-timer color palette. The widget displays whole seconds only
// (per user design direction 2026-07-07 — matches OoT's text-driven
// aesthetic + gives players a precise integer countdown). Color cues:
//   >= 2s remaining     → white  (kTimerNormal)
//   1s remaining        → yellow (kTimerWarn)
//   0s remaining (last  → red    (kTimerCritical)
//     second before advance)
ImVec4 TimerColorForInteger(int wholeSec) {
    if (wholeSec <= 0) return kTimerCritical;
    if (wholeSec <= 1) return kTimerWarn;
    return kTimerNormal;
}

// ---- Dot drawing ----------------------------------------------------
//
// Filled circle if voted; outlined circle if pending. Both use the
// player's Anchor color. Ordering: filled-vs-outline is the shape
// distinction that keeps the widget color-blindness-friendly (plan
// §8.5).

void RenderDot(ImDrawList* dl, ImVec2 center, ImU32 color, bool voted) {
    if (voted) {
        dl->AddCircleFilled(center, kDotRadius, color, 24);
    } else {
        // Outline only (2px thick).
        dl->AddCircle(center, kDotRadius, color, 24, 2.0f);
    }
}

// ---- Voting-skip widget --------------------------------------------

void DrawVotingSkipWidget(const Anchor& anchor) {
    const auto& state = anchor.cutsceneTextAdvanceState;

    std::vector<DotEntry> voters = CollectVoters(anchor);
    if (voters.size() < 2) {
        // Solo — nothing to display (matches the "alone in cutscene"
        // short-circuit in CutsceneBridge.cpp). Belt-and-suspenders vs.
        // the plan §8.3 defensive check.
        return;
    }

    // Countdown / prompt string.
    std::string rightLabel;
    ImVec4 rightColor = kPromptColor;
    if (state.countdownStarted) {
        float rem = RemainingSecondsFrom(state.countdownEndsAt);
        // Ceiling of remaining seconds so the display transitions
        // 5→4→3→2→1→0 (users expect "1s" to mean "up to 1 second
        // remaining"). Ceiling avoids the awkward case where the timer
        // reads "0s" for a full second before the actual advance.
        int wholeSec = (int)std::ceil(rem);
        if (wholeSec < 0) wholeSec = 0;
        char buf[16];
        // Bare integer — no "s" suffix per user design direction
        // 2026-07-07. Matches OoT's clean text-driven aesthetic
        // (e.g. rupee counter, timer icons — no unit label).
        std::snprintf(buf, sizeof(buf), "%d", wholeSec);
        rightLabel = buf;
        rightColor = TimerColorForInteger(wholeSec);
    } else {
        rightLabel = "Press A to skip";
    }

    // ---- Position + geometry --------------------------------------
    auto vp = ImGui::GetMainViewport();
    const float dotRowWidth =
        (voters.size() > 0)
            ? (kDotSpacing * (voters.size() - 1)) + (kDotRadius * 2.0f)
            : 0.0f;

    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBgColor);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorderColor);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(kPadding, kPadding));

    // Font scale — modest bump over default so text is legible over
    // vanilla dialogue. Reuses the same CVar as Notification so users
    // who tune notification size get consistent HUD sizing.
    const float fontScale =
        CVarGetFloat(CVAR_SETTING("Notifications.Size"), 1.8f);

    ImGui::Begin(
        "AnchorCoopHudVotingSkip", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings);

    ImGui::SetWindowFontScale(fontScale);

    // Layout row: dots on the left, label on the right, gap between.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // Dot centers.
    const float dotBaseY = cursor.y + kDotRadius;
    for (size_t i = 0; i < voters.size(); i++) {
        const auto& v = voters[i];
        ImVec2 center(cursor.x + kDotRadius + i * kDotSpacing, dotBaseY);
        RenderDot(dl, center, v.color, v.voted);
    }

    // Advance cursor past the dot row so the label sits to its right.
    ImGui::Dummy(ImVec2(dotRowWidth, kDotRadius * 2.0f));
    ImGui::SameLine(0.0f, kDotToTextGap);
    // Nudge vertical to keep the text roughly baseline-aligned with
    // dot centers.
    ImGui::TextColored(rightColor, "%s", rightLabel.c_str());

    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace

void Window::Draw() {
    if (!IsVisible()) return;
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isConnected) {
        return;
    }
    const auto& anchor = *::Anchor::Instance;

    // Durable render-side observer for the vote-skip subsystem.
    // Logs when the HUD's Draw observes `state.active` flipping. Paired
    // with the `[VoteState SEND]` / `[VoteState RECV] APPLY|DROP` lines
    // in Packets/Cutscene/CutsceneTextAdvance.cpp — together they
    // reconstruct "packet sent → packet applied → HUD reacted" for any
    // future field-test triage of the vote-skip system. Every-frame
    // logging would spam; edge-triggered only.
    const bool currActive = anchor.cutsceneTextAdvanceState.active;
    static bool s_lastActiveLogged = false;
    if (currActive != s_lastActiveLogged) {
        SPDLOG_INFO("[CoopModalHud.Draw] state.active {}→{} textId=0x{:04X} "
                    "votes={} countdownStarted={}",
                    (int)s_lastActiveLogged, (int)currActive,
                    (unsigned)anchor.cutsceneTextAdvanceState.textId,
                    (int)anchor.cutsceneTextAdvanceState.pressedClientIds.size(),
                    (int)anchor.cutsceneTextAdvanceState.countdownStarted);
        s_lastActiveLogged = currActive;
    }

    // Phase 1 dispatch. Phase 2/3/4 scenarios plug in as sibling
    // `else if` branches per plan §5.3 (priority: voting-skip wins
    // when multiple scenarios coexist).
    if (currActive) {
        // Bottom-right of viewport — pivot (1.0, 1.0) so the widget's
        // bottom-right corner sits at (vp.right - margin, vp.bottom -
        // margin). Position is aspect-ratio agnostic; approximates
        // "bottom-right of dialogue box" in the common case where the
        // vanilla textbox spans most of the bottom third of the frame.
        auto vp = ImGui::GetMainViewport();
        const float anchorX = vp->Pos.x + vp->Size.x - kBottomRightMargin;
        const float anchorY = vp->Pos.y + vp->Size.y - kBottomRightMargin;
        ImGui::SetNextWindowPos(ImVec2(anchorX, anchorY),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        DrawVotingSkipWidget(anchor);
    }
}

}  // namespace CoopModalHud
