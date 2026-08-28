// AnchorMessageBridge — single ownership boundary for Anchor writes to
// gPlayState->msgCtx.
//
// Purpose (refactor R2, 2026-07-09): before this file existed, msgCtx
// writes from Anchor code were scattered across:
//   - Packets/Cutscene/CutsceneCatchup.cpp fast-forward loop (Fix L.2:
//     clear msgMode/textId/msgLength per accelerated tick)
//   - Packets/Cutscene/CutsceneCatchup.cpp post-fast-forward (Design E:
//     force-write msgBufPos after Message_StartTextbox)
//   - Packets/Cutscene/CutsceneTextAdvance.cpp TryConsumePendingSubText-
//     Advance (Fix S/T: replicate vanilla's AWAIT_NEXT → NEXT_MSG
//     transition — msgMode, textUnskippable, msgBufPos)
//   - Bridge/CutsceneBridge.cpp — 3 shadow-update sites where the
//     bridge captures msgBufPos+1 into leaderMsgBufPosLastSubStart
//
// Adding a new msgCtx field to sync required finding and modifying
// every one of these sites. Discovering a new advance path (like
// Design E v3 → v4 solo-mode discovery) required a shadow-update audit
// across CutsceneBridge.cpp.
//
// This bridge centralizes the write path behind intent-named APIs.
// Adding a new field is now a 1-file change. Adding a new advance
// path is a 0-Anchor-file change if the path goes through R1's
// vanilla hook.
//
// Threading: all functions run on the game thread. gPlayState->msgCtx
// is game-thread state. No cross-thread synchronization required.
//
// See Analysis/cutscene_catchup_deep_review_2026-07-09.md §6 R2.

#pragma once

#include <cstdint>

namespace AnchorMessageBridge {

// ---- Fast-forward tick clearing (Fix L.2) ---------------------------
//
// During the peer's silent fast-forward, we call func_800645A0
// (vanilla cutscene tick) up to 10 times per real frame. Vanilla's
// Cutscene_Command_Textbox rewinds csCtx.frames when Message_GetState
// returns a non-terminal state (z_demo.c:1667-1669). To prevent the
// rewind and let csFrames race to the leader's target, we clear the
// message state before each accelerated tick so Message_GetState
// returns TEXT_STATE_NONE (which IS in the rewind-exception list).
//
// Called from CutsceneCatchup.cpp fast-forward loop.
void ClearMessageStateForFastForwardTick();

// ---- Peer catchup landing (Design E) --------------------------------
//
// After the fast-forward completes and Message_StartTextbox reloads
// the current textbox (Fix N), the peer's msgBufPos is at 0 (sub 1).
// Vanilla will run through TEXT_START → BOX_GROWING → STARTING →
// NEXT_MSG → Message_Decode naturally over ~10 frames. When
// NEXT_MSG's Decode runs, it reads from msgBufPos — so force-writing
// msgBufPos here lands the peer at leader's sub-textbox position.
//
// Design E v2 lesson: msgMode is intentionally NOT overridden.
// Preserving TEXT_START (set by Message_StartTextbox) lets vanilla's
// setup states run to establish R_TEXTBOX_Y_TARGET and grow
// textboxColorAlphaCurrent. Overriding msgMode caused Bug 1
// (no dark background) and Bug 2 (text at top of screen) in log 641.
//
// Called from CutsceneCatchup.cpp post-fast-forward Fix N block.
void JumpToLeaderSubTextboxPosition(uint16_t msgBufPos);

// ---- Sub-textbox chain advance (Fix S / Fix T) ----------------------
//
// Replicates vanilla's AWAIT_NEXT → NEXT_MSG transition at
// z_message_PAL.c:4651-4657:
//
//   case MSGMODE_TEXT_AWAIT_NEXT:
//       if (Message_ShouldAdvance(play)) {
//           msgCtx->msgMode = MSGMODE_TEXT_NEXT_MSG;
//           msgCtx->textUnskippable = false;
//           msgCtx->msgBufPos++;
//       }
//       break;
//
// Called by TryConsumePendingSubTextAdvance in CutsceneTextAdvance.cpp
// when the pending-count is > 0 and the msgMode is AWAIT_NEXT.
//
// Returns true iff the transition was applied (msgMode was
// AWAIT_NEXT and expectedTextId matched msgCtx.textId).
//
// Side effect: updates the leader-side shadow field
// leaderMsgBufPosLastSubStart to the new msgBufPos, since this is
// also a msgBufPos++ event that the shadow must track.
bool TryForceSubTextboxAdvance(uint16_t expectedTextId);

// ---- Vanilla msgBufPos++ hook (R1) ----------------------------------
//
// Called from z_message_PAL.c AWAIT_NEXT case AFTER vanilla's own
// msgBufPos++, and from CutsceneBridge.cpp solo-mode return-1 paths
// where vanilla's line 4655 is about to fire.
//
// Updates leaderMsgBufPosLastSubStart shadow field. This single hook
// replaces the 3 scattered CutsceneBridge return-1 site updates.
// Adding a new advance path in vanilla or bridge automatically gets
// shadow tracking as long as the path funnels through this hook.
//
// `newMsgBufPos` is the value AFTER vanilla's ++ (i.e., the start of
// the sub-textbox about to be decoded).
extern "C" void Anchor_OnMessageBufPosAdvanced(unsigned newMsgBufPos,
                                                unsigned currentTextId);

// ---- Textbox open — shadow reset ------------------------------------
//
// Called from the per-frame leader tracker when msgCtx.textId
// transitions. Message_StartTextbox has reset msgBufPos=0, so first
// sub starts at 0.
void ResetShadowForNewTextbox();

// ---- Shadow read accessor -------------------------------------------
//
// Returns leaderMsgBufPosLastSubStart. Called from
// SnapshotCamera in Common/CutsceneCatchup.cpp when the ledger
// captures the leader's current sub-textbox start position for the
// peer to decode from.
uint16_t GetLeaderMsgBufPosLastSubStart();

// ---- Choice-index accessors (2026-07-09) ----------------------------
//
// Read/write msgCtx.choiceIndex behind a bridge boundary. Called from
// the DIALOG_CHOICE_* system to:
//   - Read the local player's current cursor position at vote time
//     (SendPacket_DialogChoiceVote captures + broadcasts this).
//   - Write the resolved winning choice at pending-apply consume time
//     (Anchor_ShouldAdvanceCutsceneTextLocal sets msgCtx.choiceIndex
//     before returning true so the actor's Talk handler dispatches on
//     the plurality-voted choice).
//
// Only meaningful when Message_GetState() == TEXT_STATE_CHOICE. Reads
// return 0 and writes are no-ops when gPlayState is null.
uint8_t GetChoiceIndex();
void    SetChoiceIndex(uint8_t choiceIndex);

// Returns msgCtx.textboxEndType. Used by the bridge to compute
// numChoices at vote time (endType 2_CHOICE → 2, 3_CHOICE → 3).
uint8_t GetTextboxEndType();

// Convenience — returns the number of choices in the current choice
// textbox based on textboxEndType. Returns 0 if not a choice textbox.
uint8_t GetChoiceNumOptions();

}  // namespace AnchorMessageBridge

// ---- Forced-dialog classifier (GH #339, 2026-08-28) -----------------
//
// Returns non-zero when the current textbox is part of a "forced"
// dialog context that all players are pulled into together — a signal
// the multiplayer vote-skip / auto-advance system should engage.
// Returns zero for voluntary player-initiated NPC dialog (Hintnut,
// business scrub, Kokiri kid, shopkeepers, hint stones, etc.) where
// each player reads locally at their own pace.
//
// Two runtime signals combine to define "forced":
//   1. play->csCtx.state != CS_STATE_IDLE
//      — a scripted cutscene command sequence is running. Covers sage
//        medallion awards, boss intros, Zelda courtyard, story trigs.
//   2. player->csAction != 0 && player->cv.haltActorsDuringCsAction
//      — Player_SetCsActionWithHaltedActors was invoked (z_actor.c:1489).
//        Covers Kaepora Gaebora (En_Owl), Impa speeches, Dark Link
//        (En_Torch2 line 621), story-driven NPCs that halt the world.
//        Two-part gate: csAction != 0 excludes the union alias
//        (cv.slidingDoorBgCamIndex) from firing during sliding-door
//        room transitions.
//
// Replaces the earlier Play_InCsMode-based gate at
// CutsceneTextAdvance.cpp:178 + z_message_PAL.c:198/241, which fired
// for ANY talk because Player_SetupTalk unconditionally sets
// PLAYER_STATE1_IN_CUTSCENE — arming vote-skip for every NPC dialog
// including voluntary ones.
//
// See GH #339 for the observed hint-nut re-open cycle and design
// tradeoff between narrow (csCtx.state only) and composite (both
// signals). Composite classifier preserves vote-skip for Owl / Impa /
// sage class while excluding the routine NPC surface.
//
// Declared at file scope (outside AnchorMessageBridge namespace) so
// C++ callers see it unqualified — same pattern as
// Anchor_ShouldAdvanceCutsceneTextLocal (Bridge/CutsceneBridge.cpp)
// and Anchor_ShouldAutoAdvanceNpcDialog (this TU's .cpp).
extern "C" int Anchor_IsForcedDialogContext(void);
