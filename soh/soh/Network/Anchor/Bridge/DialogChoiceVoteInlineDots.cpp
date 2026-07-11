// Anchor.h MUST be first: pulls libultraship + z64.h + variables.h
// with C++ linkage (Pitfall 40). Overlay-specific headers wrapped in
// extern "C" below get their C-linkage decls without re-processing
// libultraship's template + std::shared_ptr contents.

#include "soh/Network/Anchor/Anchor.h"
#include "DialogChoiceVoteInlineDots.h"

#include <cstdint>

extern "C" {
#include "functions.h"
#include "macros.h"
}

// Inline choice-vote dots — v2 (small solid squares left of choice text).
//
// See Analysis/inline_choice_vote_ui_2026-07-10.md for original design.
// V2 revisions (user field-test feedback 2026-07-11):
//
//   * Position moved from RIGHT side of textbox to the ~30 ortho-px gap
//     between the vanilla ▶ arrow (at R_TEXT_CHOICE_XPOS) and the choice
//     text (indented +32 from textbox left per z_message_PAL.c:1392-1395).
//     Reason: choice text can be long enough to reach the right edge,
//     but the arrow-to-text gap is always present and roomy.
//
//   * Rendering switched from IA8 texture + gSPTextureRectangle to
//     gDPFillRectangle in FILL cycle mode. Reason: the surrounding
//     Message_DrawTextboxIcon (z_message_PAL.c:631) leaves the RCP in
//     2-cycle mode via gDPSetCombineLERP (line 725). My earlier v1 code
//     set a 1-cycle combiner without explicitly setting cycle type,
//     producing corrupted texture output ("checkered red/white JPG-like
//     pattern" per user screenshot). gDPFillRectangle needs FILL cycle,
//     which we set explicitly — no texture load, no combiner ambiguity.
//
//   * Backer removed for inline dots. The vanilla textbox background
//     (dark grey/black semitransparent) provides adequate contrast for
//     peer colors, matching the user's mockup 2026-07-11.
//     Vote-skip HUD backer (added earlier this session for Q3) STAYS —
//     that renders over ImGui-side variable backgrounds and still needs
//     the readability guarantee.
//
//   * Dots are small solid squares (kDotSize ortho px), visually reading
//     as dots at typical screen scale (~2-3x ortho).

namespace {

// ---- Geometry constants (all N64 ortho px) -------------------------
//
// Space available for dots: from just-right-of-arrow (R_TEXT_CHOICE_XPOS
// + arrow_width + gap) to just-left-of-text (choice text is indented +32
// from textbox left). Arrow width is 16 ortho px (sCharTexSize per
// z_message_PAL.c:735). So the usable strip is roughly:
//
//   [R_TEXT_CHOICE_XPOS + 18]  ..  [textbox_left + 32 - 2]
//
// The strip is typically ~28-32 ortho px wide. 6 dots at 4 px + 5 px
// centre-to-centre spacing fit in 29 px = 4 + 5*5 = 4+25 = 29. 8 dots
// need 4 + 7*5 = 39 which overflows; the render loop truncates at the
// point where a dot would cross the choice-text start X.

constexpr int kDotSize            = 4;   // per-dot square (ortho px)
constexpr int kDotSpacing         = 5;   // centre-to-centre spacing
constexpr int kDotLeftGapFromArrow = 18; // arrow is 16 wide + 2 px gap
constexpr int kMaxVoteDotsPerChoice = 8; // Q6 cap (may truncate visually earlier)

// Vertical: y-align dot centre with arrow centre. Arrow renders as a
// 16-px texture at Y = R_TEXT_CHOICE_YPOS(choiceIdx), so arrow's visual
// centre is at Y = R_TEXT_CHOICE_YPOS(choiceIdx) + 8. Dot centre at
// Y = choice_y + kDotYOffset - kDotSize/2 gives dot top-left at
// choice_y + kDotYOffset - kDotSize/2. Empirically visually aligned at
// +6 for a 4-px dot (dot spans y=6..10, arrow spans y=0..16).
constexpr int kDotYOffset         = 6;

// ---- Choice-text left-edge X in ortho space ------------------------
//
// From z_message_PAL.c:1392-1395: when numChoices >= 1, the newline
// handler indents choice text by +32 from R_TEXT_INIT_XPOS (which is
// the standard textbox text-start X). The arrow renders at
// R_TEXT_CHOICE_XPOS which sits in that indent. The right boundary
// for our dot row is `textStart - kDotRightMarginToText`.
//
// R_TEXT_INIT_XPOS = XREG(65) per regs.h. Adding +32 gives the choice
// text left edge. Subtract a small margin (2 ortho px) to leave a
// visual gap before the choice glyphs.
constexpr int kChoiceTextIndent      = 32;
constexpr int kDotRightMarginToText  = 2;

// ---- Gate: should we render inline dots this frame? -----------------

bool ShouldRender(PlayState* play) {
    if (play == nullptr) return false;
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return false;
    const auto& state = Anchor::Instance->dialogChoiceVoteState;
    if (!state.active) return false;
    if (state.numChoices < 2) return false;

    // Q2 / Q10: gate on local textbox showing the exact textId the
    // vote is about (any synced dialogue with a matching choice
    // textbox, cutscene or otherwise — no Play_InCsMode requirement
    // per user answer to Q2).
    if (play->msgCtx.textId != state.textId) return false;

    // Also confirm the local textbox is actually in a choice-type
    // end state. The splice already gates on this at the C call site
    // (TEXTBOX_ENDTYPE_2_CHOICE / _3_CHOICE branches), but re-check
    // defensively so this query is safe from any call site.
    const uint8_t et = play->msgCtx.textboxEndType;
    if (et != TEXTBOX_ENDTYPE_2_CHOICE && et != TEXTBOX_ENDTYPE_3_CHOICE) {
        return false;
    }
    return true;
}

// ---- Per-choice voter enumeration -----------------------------------
//
// Returns up to `kMaxVoteDotsPerChoice` voters (in insertion order) who
// chose `choiceIdx`. Colors are the voters' Anchor colors.

struct VoterColor {
    uint8_t r, g, b;
};

int GatherVotersForChoice(const Anchor& anchor, uint8_t choiceIdx,
                          VoterColor* out, int maxCount) {
    int n = 0;
    const auto& state = anchor.dialogChoiceVoteState;
    for (uint32_t voterId : state.voteOrderByClient) {
        if (n >= maxCount) break;
        auto voteIt = state.votesByClient.find(voterId);
        if (voteIt == state.votesByClient.end()) continue;
        if (voteIt->second != choiceIdx) continue;

        Color_RGB8 c = { 255, 255, 255 };
        auto clientIt = anchor.clients.find(voterId);
        if (clientIt != anchor.clients.end()) {
            c = clientIt->second.color;
        }
        out[n].r = c.r;
        out[n].g = c.g;
        out[n].b = c.b;
        ++n;
    }
    return n;
}

// ---- Gfx emission helpers ------------------------------------------
//
// FILL-cycle rendering: pack the RGB into RGBA5551 twice (high + low
// halves of a u32), set fill color, emit `gDPFillRectangle(x, y, x+w,
// y+h)`. See z_message_PAL.c:4394-4406 for the canonical vanilla
// example (debug variable-change visualisation). Cycle type MUST be
// FILL before gDPFillRectangle, and the render mode should be
// (G_RM_NOOP, G_RM_NOOP2) to prevent the blender from mangling the
// fill.

void SetupFillCycle(Gfx** pgfx) {
    Gfx* gfx = *pgfx;
    gDPPipeSync(gfx++);
    gDPSetCycleType(gfx++, G_CYC_FILL);
    gDPSetRenderMode(gfx++, G_RM_NOOP, G_RM_NOOP2);
    *pgfx = gfx;
}

void RestoreOneCycle(Gfx** pgfx) {
    Gfx* gfx = *pgfx;
    gDPPipeSync(gfx++);
    gDPSetCycleType(gfx++, G_CYC_1CYCLE);
    *pgfx = gfx;
}

void EmitFilledDot(Gfx** pgfx, int x, int y, int w, int h,
                   uint8_t r, uint8_t g, uint8_t b) {
    Gfx* gfx = *pgfx;
    const uint32_t packed = GPACK_RGBA5551(r, g, b, 1);
    gDPSetFillColor(gfx++, (packed << 16) | packed);
    // gDPFillRectangle takes inclusive coords in ortho pixel space
    // (no <<2 shift; that's for gSPTextureRectangle only).
    gDPFillRectangle(gfx++, x, y, x + w - 1, y + h - 1);
    *pgfx = gfx;
}

}  // namespace

// ---- Public API: splice entry point ---------------------------------

extern "C" void Anchor_DrawInlineChoiceVoteDots(PlayState* play, Gfx** pgfx) {
    if (pgfx == nullptr || *pgfx == nullptr) return;
    if (!ShouldRender(play)) return;

    const auto& state = Anchor::Instance->dialogChoiceVoteState;

    // Dot row X range: [dotRowLeft .. dotRowRight] in ortho px.
    // Left = just right of the vanilla arrow icon.
    // Right = just left of the choice text (which is indented +32 from
    //         textbox left).
    const int arrowX     = R_TEXT_CHOICE_XPOS;
    const int dotRowLeft = arrowX + kDotLeftGapFromArrow;
    const int dotRowRight = R_TEXT_INIT_XPOS + kChoiceTextIndent
                            - kDotRightMarginToText;

    if (dotRowLeft >= dotRowRight) return; // no room — shouldn't happen

    SetupFillCycle(pgfx);

    for (uint8_t choiceIdx = 0; choiceIdx < state.numChoices; ++choiceIdx) {
        VoterColor voters[kMaxVoteDotsPerChoice];
        const int nVoters = GatherVotersForChoice(
            *Anchor::Instance, choiceIdx, voters, kMaxVoteDotsPerChoice);
        if (nVoters == 0) continue;

        // Y-align dot with arrow's visual centre.
        const int y = R_TEXT_CHOICE_YPOS(choiceIdx) + kDotYOffset;

        for (int i = 0; i < nVoters; ++i) {
            const int x = dotRowLeft + i * kDotSpacing;
            if (x + kDotSize > dotRowRight) break; // would overlap choice text
            EmitFilledDot(pgfx, x, y, kDotSize, kDotSize,
                          voters[i].r, voters[i].g, voters[i].b);
        }
    }

    RestoreOneCycle(pgfx);
}
