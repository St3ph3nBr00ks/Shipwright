// Anchor.h MUST be first: pulls libultraship + z64.h + variables.h
// with C++ linkage (Pitfall 40). Overlay-specific headers wrapped in
// extern "C" below get their C-linkage decls without re-processing
// libultraship's template + std::shared_ptr contents.

#include "soh/Network/Anchor/Anchor.h"
#include "DialogChoiceVoteInlineDots.h"

#include <cmath>
#include <cstdint>

extern "C" {
// z64.h + variables.h + regs.h already pulled by Anchor.h with C++
// linkage — their include guards no-op them here. Only functions.h
// and macros.h need explicit re-inclusion for their C-linkage macros
// (`gSPTextureRectangle` alias per Pitfall 17, R_TEXT_CHOICE_YPOS,
// R_TEXTBOX_X, R_TEXTBOX_WIDTH, TEXTBOX_ENDTYPE_2_CHOICE constants).
#include "functions.h"
#include "macros.h"
}

// Inline choice-vote dots — Path A implementation.
//
// See DialogChoiceVoteInlineDots.h + Analysis/inline_choice_vote_ui_2026-07-10.md
// for design rationale.
//
// Renders one per-voter dot on the RIGHT side of each choice line
// inside a vanilla multi-choice textbox. Dots share the arrow
// cursor's N64 ortho projection so widescreen / letterbox handling
// is automatic. Called as a splice from
// `Message_DrawTextboxIcon`'s TEXTBOX_ENDTYPE_{2,3}_CHOICE caller
// site inside `z_message_PAL.c`.

namespace {

// ---- Geometry constants (all N64 ortho px) -------------------------
//
// Textbox width defaults ~256 ortho px per R_TEXTBOX_WIDTH_TARGET;
// dots are sized to fit inside the right margin without overlapping
// vanilla text (choice text is indented +32 from left edge per
// z_message_PAL.c:1392-1395, so right margin is ~24 px wide by default).

constexpr int  kDotSize          = 16;  // dot texture size (16x16 IA8)
constexpr int  kBackerSize       = 20;  // backer render size (scaled from same texture)
constexpr int  kDotRightMargin   = 6;   // gap between textbox right edge and rightmost dot
constexpr int  kDotSpacing       = 18;  // center-to-center horizontal spacing
constexpr int  kDotVerticalOffset = 0;  // Y adjustment relative to arrow Y
constexpr int  kMaxVoteDotsPerChoice = 8; // Q6 cap; extra voters truncated

// ---- 16x16 IA8 circle mask (I=constant 0xFF, A=antialiased edge) ---
//
// Lazy-initialised at first call. Mask has:
//   - inner radius 6: full opacity
//   - 6..7: linear alpha ramp to 0 (soft anti-aliased edge)
//   - beyond 7: transparent
// Intensity byte constant so prim color drives dot color entirely.

constexpr int  kMaskDim = 16;
constexpr int  kMaskBytes = kMaskDim * kMaskDim * 2;  // IA8 = 2 bytes/pixel

alignas(8) uint8_t sDotMaskIA8[kMaskBytes];
bool sMaskInitialized = false;

void InitDotMask() {
    if (sMaskInitialized) return;
    const float cx = 7.5f;
    const float cy = 7.5f;
    const float rInner = 6.0f;
    const float rOuter = 7.0f;
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

// ---- Brightness-adaptive backer color -------------------------------
//
// Rec. 601 luma: (299*R + 587*G + 114*B) / 1000. Above 127 → dark
// backer; else light backer. Mirrors Team Marker's nametag pattern
// (see session_state.md → ⭐ Team Marker).

void GetBackerColor(uint8_t peerR, uint8_t peerG, uint8_t peerB,
                    uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    const int luma = (299 * peerR + 587 * peerG + 114 * peerB) / 1000;
    if (luma > 127) {
        outR = 0; outG = 0; outB = 0;
    } else {
        outR = 255; outG = 255; outB = 255;
    }
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
    // textbox, cutscene or otherwise — no Play_InCsMode requirement
    // per user 2026-07-10 answer to Q2).
    if (play->msgCtx.textId != state.textId) return false;

    // Also confirm the local textbox is actually in a choice-type
    // end state. The splice already gates on this at the C call site
    // (TEXTBOX_ENDTYPE_2_CHOICE / _3_CHOICE branches), but re-check
    // defensively so the query API is safe from any call site.
    const uint8_t et = play->msgCtx.textboxEndType;
    if (et != TEXTBOX_ENDTYPE_2_CHOICE && et != TEXTBOX_ENDTYPE_3_CHOICE) {
        return false;
    }
    return true;
}

// ---- Per-choice voter enumeration -----------------------------------
//
// Returns up to `kMaxVoteDotsPerChoice` voters (in insertion order)
// who chose `choiceIdx`. Colors are the voters' Anchor colors.

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

// Emit a colored circle at (x, y) with the given color and destination
// size. Uses G_CC_MODULATEIA_PRIM combiner (matches vanilla message-
// icon rendering — see z_message_PAL.c:902,1141). Assumes texture is
// already loaded via LoadDotTexture().
void EmitCircle(Gfx** pgfx, int x, int y, int size,
                uint8_t r, uint8_t g, uint8_t b) {
    Gfx* gfx = *pgfx;
    gDPPipeSync(gfx++);
    gDPSetPrimColor(gfx++, 0, 0, r, g, b, 0xFF);
    // Scale factor: dsdx = kMaskDim * (1<<10) / size. Guard against
    // size=0 which shouldn't happen but would be a divide-by-zero.
    const int scale = (size > 0) ? ((kMaskDim << 10) / size) : (1 << 10);
    gSPTextureRectangle(
        gfx++,
        x << 2, y << 2,
        (x + size) << 2, (y + size) << 2,
        G_TX_RENDERTILE,
        0, 0,
        scale, scale);
    *pgfx = gfx;
}

// Load the shared IA8 mask into TMEM tile 0 and establish the
// combiner mode we need (G_CC_MODULATEIA_PRIM matches vanilla message-
// icon rendering at z_message_PAL.c:902,1141). Called once per splice
// invocation before any dot rendering.
//
// Deliberately leaves cycle type / render mode / texture LUT / persp
// alone — they inherit from the surrounding message-render pipeline
// that just finished drawing the arrow. Setting them here would risk
// disrupting subsequent frame passes. Vanilla `Message_DrawTextboxIcon`
// (z_message_PAL.c:631) sets combiner-only via `gDPSetCombineLERP` and
// relies on inherited state for the rest — same principle applied here.
void LoadDotTexture(Gfx** pgfx) {
    Gfx* gfx = *pgfx;
    gDPPipeSync(gfx++);
    gDPSetCombineMode(gfx++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPLoadTextureBlock(gfx++,
        reinterpret_cast<uintptr_t>(sDotMaskIA8),
        G_IM_FMT_IA, G_IM_SIZ_8b,
        kMaskDim, kMaskDim, 0,
        G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
        G_TX_NOMASK, G_TX_NOMASK,
        G_TX_NOLOD, G_TX_NOLOD);
    *pgfx = gfx;
}

}  // namespace

// ---- Public API: splice entry point ---------------------------------

extern "C" void Anchor_DrawInlineChoiceVoteDots(PlayState* play, Gfx** pgfx) {
    if (pgfx == nullptr || *pgfx == nullptr) return;
    if (!ShouldRender(play)) return;

    InitDotMask();

    const auto& state = Anchor::Instance->dialogChoiceVoteState;
    const int textboxRight = R_TEXTBOX_X + R_TEXTBOX_WIDTH;

    LoadDotTexture(pgfx);

    // Per-choice: enumerate voters, render backer + dot for each.
    for (uint8_t choiceIdx = 0; choiceIdx < state.numChoices; ++choiceIdx) {
        VoterColor voters[kMaxVoteDotsPerChoice];
        const int nVoters = GatherVotersForChoice(
            *Anchor::Instance, choiceIdx, voters, kMaxVoteDotsPerChoice);
        if (nVoters == 0) continue;

        // Choice line ortho Y — arrow renders at this Y too.
        const int arrowY = R_TEXT_CHOICE_YPOS(choiceIdx);
        const int rowY   = arrowY + kDotVerticalOffset;

        // Rightmost dot's left-edge X (dots layout right-to-left so
        // the first-voter dot is nearest the choice text; later
        // voters extend rightward toward the textbox edge).
        const int firstDotLeftX = textboxRight - kDotRightMargin - kDotSize;

        for (int i = 0; i < nVoters; ++i) {
            const int dotX = firstDotLeftX - i * kDotSpacing;
            if (dotX < R_TEXTBOX_X) break;  // out of textbox — silently truncate

            // Backer: brightness-adaptive, drawn 2 px larger radius
            // (kBackerSize is 20 vs kDotSize 16 = +2 radius each side).
            const int backerX = dotX - (kBackerSize - kDotSize) / 2;
            const int backerY = rowY - (kBackerSize - kDotSize) / 2;
            uint8_t br, bg, bb;
            GetBackerColor(voters[i].r, voters[i].g, voters[i].b, br, bg, bb);
            EmitCircle(pgfx, backerX, backerY, kBackerSize, br, bg, bb);

            // Dot: voter color.
            EmitCircle(pgfx, dotX, rowY, kDotSize,
                       voters[i].r, voters[i].g, voters[i].b);
        }
    }
}
