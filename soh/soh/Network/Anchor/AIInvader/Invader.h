/**
 * Invader — AI Invader Director-side tick driver and rendering hooks.
 *
 * Step 15c (Phase 2) — locomotion + anim infrastructure + G-guards.
 * Anchor_TickInvaderActor implements IDLE / FOLLOW / STUCK state
 * machine cloned from NPC Follower's same-named states. NO combat
 * states (ATTACK/BLOCK/ENGAGE/RANGED_ATTACK/STANDBY) — those are
 * Agent 3's scope. NO substrate path consumption — direct yaw +
 * speedXZ only (Phase 3+ will add substrate path consumption).
 *
 * The file-scope draw-context flag (Anchor_InvaderDrawBegin/End +
 * Anchor_GetCurrentlyDrawingInvader) supports the
 * VB_APPLY_TUNIC_COLOR hook's black-tint override for the
 * hostile-Link visual (owned by Agent 1; we don't touch it here).
 *
 * Combat AI lands here post-#208 (follower state-machine formal
 * design pass); see Plans/ai_invader_plan.md §2.2.
 *
 * Parallels soh/soh/Network/Anchor/AIFollowerNPC/FollowerNPC.h.
 */

#pragma once

extern "C" {
#include "z64.h"  // Actor*, PlayState*
}

namespace AnchorInvader {

}  // namespace AnchorInvader

extern "C" {

// Per-frame tick called from EnInvader_Update via extern wrapper.
// v1 is a no-op; combat AI fills this in post-#208.
void Anchor_TickInvaderActor(Actor* invader, PlayState* play);

// Draw-context flag — set/cleared by EnInvader_Draw around its
// Player_DrawImpl call. The VB_APPLY_TUNIC_COLOR hook in
// HookHandlers.cpp reads Anchor_GetCurrentlyDrawingInvader() to
// detect Invader draws and override the tunic color with the
// hostile-black tint.
//
// Same draw-context flag shape as the NPC Follower's
// Anchor_FollowerNpcDrawBegin/End. See FollowerNPC.cpp:422 for the
// canonical pattern.
void   Anchor_InvaderDrawBegin(Actor* invader);
void   Anchor_InvaderDrawEnd(void);
Actor* Anchor_GetCurrentlyDrawingInvader(void);

}  // extern "C"
