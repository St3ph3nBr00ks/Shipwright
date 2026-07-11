// Anchor.h MUST be first: pulls libultraship + z64.h + variables.h
// with C++ linkage (Pitfall 40). Overlay-specific headers wrapped in
// extern "C" below get their C-linkage decls without re-processing
// libultraship's template + std::shared_ptr contents.

#include "soh/Network/Anchor/Anchor.h"
#include "DialogChoiceVoteInlineDots.h"

#include <cmath>
#include <cstdint>

extern "C" {
#include "functions.h"
#include "macros.h"
}

// Inline choice-vote dots — v3 (small anti-aliased circle textures).
//
// See Analysis/inline_choice_vote_ui_2026-07-10.md for evolving design.
//
// V3 revisions (user field-test feedback 2026-07-11, post-v2):
//
//   * Two v2 bugs corrected in v3:
//       (a) v2 dots were SQUARES — gDPFillRectangle draws only rectangles.
//           v3 returns to an anti-aliased IA8 circle texture.
//       (b) v2 dots rendered ~50-100 px too high on-screen. Root cause:
//           gDPFillRectangle in FILL cycle mode bypasses the message
//           pipeline's ortho projection; the arrow (via gSPTextureRectangle)
//           and my dots (via gDPFillRectangle) used mismatched coord
//           systems on libultraship's PC render path.
//
//   * v3 uses gSPTextureRectangle with an IA8 mask — the SAME rendering
//     primitive as the vanilla arrow (z_message_PAL.c:738) and vanilla
//     ocarina notes (:4311). Guaranteed to share the surrounding message
//     pipeline's coord system.
//
//   * v3 inherits the ambient combiner (gDPSetCombineLERP set by
//     Message_DrawTextboxIcon at line 725) instead of switching to
//     gDPSetCombineMode. This avoids v1's cycle-type-mismatch corruption
//     (the "checkered JPG" bug).
//
//   * v3 sets env color to black explicitly. The inherited LERP is
//     `(PRIM - ENV) * TEXEL + ENV`; env=black makes soft edges fade to
//     transparent black instead of to whatever env color the arrow's
//     flash animation happened to leave behind.

namespace {

// ---- Geometry constants (all N64 ortho px) -------------------------
//
// Space available for dots: from just-right-of-arrow (R_TEXT_CHOICE_XPOS
// + arrow_width + gap) to just-left-of-text (choice text indented +32
// from R_TEXT_INIT_XPOS per z_message_PAL.c:1392-1395). Arrow width is
// 16 ortho px (sCharTexSize per z_message_PAL.c:735).

constexpr int kDotSize            = 8;   // dot rendered as 8x8 ortho (from 8x8 IA8 mask, 1:1 scaling)
constexpr int kDotSpacing         = 9;   // centre-to-centre spacing (1 px gap)
constexpr int kDotLeftGapFromArrow = 18; // arrow is 16 wide + 2 px gap
constexpr int kMaxVoteDotsPerChoice = 8; // Q6 cap

// Arrow spans ortho Y=[R_TEXT_CHOICE_YPOS(n) .. +16]; visual mid-height
// at +8. Dot at 8x8 wants its own mid-height at +8, so top-left Y offset
// is +4 (dot spans y+4..y+12 relative to arrow's y..y+16).
constexpr int kDotYOffset         = 4;

// Choice text left edge X: R_TEXT_INIT_XPOS + 32 (per vanilla newline
// handler). Right boundary of dot row is `textStart - 2` (2 px gap
// before text).
constexpr int kChoiceTextIndent      = 32;
constexpr int kDotRightMarginToText  = 2;

// ---- 8x8 IA8 circle mask (I=constant 0xFF, A=antialiased edge) -----
//
// Lazy-initialised at first call. I byte constant so prim color drives
// dot color fully via the inherited combiner. A byte: soft circle
// (inner radius 3, outer 3.5).

constexpr int  kMaskDim   = 8;
constexpr int  kMaskBytes = kMaskDim * kMaskDim * 2;  // IA8 = 2 bytes/pixel

alignas(8) uint8_t sDotMaskIA8[kMaskBytes];
bool sMaskInitialized = false;

void InitDotMask() {
    if (sMaskInitialized) return;
    const float cx = static_cast<float>(kMaskDim) / 2.0f - 0.5f;
    const float cy = cx;
    const float rInner = 3.0f;
    const float rOuter = 3.5f;
    for (int y = 0; y < kMaskDim; ++y) {
        for (int x = 0; x < kMaskDim; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            uint8_t a = 0;
            if (d < rInner) {
                a = 0xFF;
            } else if (d < rOuter) {
                a = static_cast<uint8_t>(
                    255.0f * (rOuter - d) / (rOuter - rInner));
            }
            const int idx = (y * kMaskDim + x) * 2;
            sDotMaskIA8[idx + 0] = 0xFF;  // I: constant, color from prim
            sDotMaskIA8[idx + 1] = a;     // A: antialiased mask
        }
    }
    sMaskInitialized = true;
}

// ---- Gate: should we render inline dots this frame? -----------------

bool ShouldRender(PlayState* play) {
    if (play == nullptr) return false;
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return false;
    const auto& state = Anchor::Instance->dialogChoiceVoteState;
    if (!state.active) return false;
    if (state.numChoices < 2) return false;

    // Q2 / Q10: gate on local textbox showing the exact textId the
    // vote is about (any synced dialogue with a matching choice
    // textbox, cutscene or otherwise — no Play_InCsMode requirement).
    if (play->msgCtx.textId != state.textId) return false;

    // Also confirm the local textbox is actually in a choice-type
    // end state. The splice already gates on this at the C call site,
    // but re-check defensively.
    const uint8_t et = play->msgCtx.textboxEndType;
    if (et != TEXTBOX_ENDTYPE_2_CHOICE && et != TEXTBOX_ENDTYPE_3_CHOICE) {
        return false;
    }
    return true;
}

// ---- Per-choice voter enumeration -----------------------------------

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

// ---- Gfx emission --------------------------------------------------
//
// Inherits the ambient combiner set by Message_DrawTextboxIcon
// (gDPSetCombineLERP at z_message_PAL.c:725). Under that combiner:
//
//   color_out = (PRIM - ENV) * TEXEL0.I + ENV
//   alpha_out = TEXEL0.A * PRIM.A
//
// With ENV = (0,0,0,255) and PRIM = peer_color, the dot renders in
// peer color where the mask is fully opaque and fades to transparent
// black at the anti-aliased edge. Framebuffer blends via ambient
// render mode.

void EmitDotRectangle(Gfx** pgfx, int x, int y,
                      uint8_t r, uint8_t g, uint8_t b) {
    Gfx* gfx = *pgfx;
    gDPPipeSync(gfx++);
    gDPSetPrimColor(gfx++, 0, 0, r, g, b, 0xFF);
    // gSPTextureRectangle takes 10.2 fixed-point coordinates (<< 2).
    // dsdx = dtdy = 1024 (1<<10) = 1:1 texel:pixel mapping.
    gSPTextureRectangle(
        gfx++,
        x << 2, y << 2,
        (x + kDotSize) << 2, (y + kDotSize) << 2,
        G_TX_RENDERTILE,
        0, 0,
        1 << 10, 1 << 10);
    *pgfx = gfx;
}

}  // namespace

// ---- Public API: splice entry point ---------------------------------

extern "C" void Anchor_DrawInlineChoiceVoteDots(PlayState* play, Gfx** pgfx) {
    if (pgfx == nullptr || *pgfx == nullptr) return;
    if (!ShouldRender(play)) return;

    InitDotMask();

    const auto& state = Anchor::Instance->dialogChoiceVoteState;

    // Dot row X range: [dotRowLeft .. dotRowRight] in ortho px.
    const int arrowX      = R_TEXT_CHOICE_XPOS;
    const int dotRowLeft  = arrowX + kDotLeftGapFromArrow;
    const int dotRowRight = R_TEXT_INIT_XPOS + kChoiceTextIndent
                            - kDotRightMarginToText;
    if (dotRowLeft >= dotRowRight) return;

    Gfx* gfx = *pgfx;

    // Establish env color for the ambient combiner LERP. Load the mask
    // texture once (per invocation) — TMEM stays populated across the
    // subsequent per-dot gSPTextureRectangle calls.
    gDPPipeSync(gfx++);
    gDPSetEnvColor(gfx++, 0, 0, 0, 0xFF);
    gDPLoadTextureBlock(gfx++,
        reinterpret_cast<uintptr_t>(sDotMaskIA8),
        G_IM_FMT_IA, G_IM_SIZ_8b,
        kMaskDim, kMaskDim, 0,
        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
        G_TX_NOMASK, G_TX_NOMASK,
        G_TX_NOLOD, G_TX_NOLOD);

    *pgfx = gfx;

    for (uint8_t choiceIdx = 0; choiceIdx < state.numChoices; ++choiceIdx) {
        VoterColor voters[kMaxVoteDotsPerChoice];
        const int nVoters = GatherVotersForChoice(
            *Anchor::Instance, choiceIdx, voters, kMaxVoteDotsPerChoice);
        if (nVoters == 0) continue;

        const int y = R_TEXT_CHOICE_YPOS(choiceIdx) + kDotYOffset;

        for (int i = 0; i < nVoters; ++i) {
            const int x = dotRowLeft + i * kDotSpacing;
            if (x + kDotSize > dotRowRight) break;
            EmitDotRectangle(pgfx, x, y,
                             voters[i].r, voters[i].g, voters[i].b);
        }
    }
}
