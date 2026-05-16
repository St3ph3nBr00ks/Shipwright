/**
 * Invader — AI Invader Director-side tick driver and rendering hooks.
 *
 * v1 (step 15a) — scaffold + draw-context flag.
 * v1.5 (step 15d) — combat state machine (ATTACK / BLOCK / ENGAGE /
 * RANGED_ATTACK / STANDBY). Cloned from NPC Follower Stage 4 ahead
 * of #208 to support parallel agent work; the canonical combat
 * shape will be revisited once #208 documents the design contract.
 *
 * The locomotion states (IDLE / FOLLOW / STUCK) and non-combat anim
 * pipeline are owned by Agent 2. Target picking will be replaced by
 * Agent 4 (multi-player picker that respects director-side state);
 * for now the combat layer uses Anchor_GetNearestPlayerActor as a
 * placeholder hostile-target source.
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
