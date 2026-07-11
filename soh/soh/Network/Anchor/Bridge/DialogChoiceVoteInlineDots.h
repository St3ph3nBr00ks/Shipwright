#pragma once

// Inline choice-vote dots — Path A ortho-native renderer.
//
// User design (2026-07-10): render one per-voter dot to the RIGHT of
// each choice line inside a vanilla multi-choice textbox, using the
// same N64 Gfx display-list projection as the vanilla ▶ cursor arrow.
// Replaces the earlier ImGui bottom-right panel display.
//
// Anchor's C++ side owns the vote tally state
// (`Anchor::dialogChoiceVoteState`). Vanilla decomp's C side owns the
// textbox rendering. This function bridges them from the
// `z_message_PAL.c` `Message_DrawTextboxIcon` splice site, inside the
// `TEXTBOX_ENDTYPE_{2,3}_CHOICE` branches.
//
// Design decisions logged in
// Claude/Analysis/inline_choice_vote_ui_2026-07-10.md.
//
// Header intentionally forward-declares its two struct/union types via
// their existing typedef names in z64.h — so the header can be
// included from ANY translation unit without pulling libultraship /
// nlohmann into `extern "C"` scope (Pitfall 40). Consumers that need
// the full definitions include z64.h themselves.

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;
union Gfx;

// Emit Gfx commands to render inline vote-dots for the currently
// displayed multi-choice textbox. Safe to call unconditionally; the
// function internally checks: (a) Anchor connected, (b)
// `dialogChoiceVoteState.active`, (c) local textbox matches the
// active vote's textId, (d) local textbox's textboxEndType is
// TEXTBOX_ENDTYPE_2_CHOICE or _3_CHOICE. Any check failure -> no-op
// (no Gfx emitted).
//
// Splice site MUST be inside the choice-type branches of
// `Message_DrawTextboxIcon`'s caller so the ortho registers
// R_TEXT_CHOICE_YPOS(n) are populated to their per-choice values.
//
// Emits (per voter):
//   - Brightness-adaptive backer circle at 20 px diameter
//   - Peer-color dot at 16 px diameter
//
// Bounded to `kMaxVoteDotsPerChoice = 8` dots per choice (user Q6).
void Anchor_DrawInlineChoiceVoteDots(struct PlayState* play, union Gfx** gfx);

#ifdef __cplusplus
}
#endif
