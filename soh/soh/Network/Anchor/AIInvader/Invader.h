/**
 * Invader — AI Invader Director-side tick driver and rendering hooks.
 *
 * v1 (steps 15a + 15b) — Anchor_TickInvaderActor is a no-op; the
 * Begin/End pair around EnInvader_Draw's Player_DrawImpl call does
 * two things:
 *   (1) sets/clears the file-scope draw-context flag for the
 *       VB_APPLY_TUNIC_COLOR hook's hostile-black tint override
 *       (Anchor_GetCurrentlyDrawingInvader);
 *   (2) Phase B equipment swap — temporarily overrides the local
 *       Player's modelGroup + hand/sheath DLists to
 *       PLAYER_MODELGROUP_SWORD_AND_SHIELD so the Invader appears
 *       permanently armed, independent of Link's actual equipment.
 *       End() restores Player's saved state.
 *
 * Combat AI lands here post-#208 (follower state-machine formal
 * design pass); see Plans/ai_invader_plan.md §2.2.
 *
 * Parallels soh/soh/Network/Anchor/AIFollowerNPC/FollowerNPC.h —
 * the Invader will grow into a similarly-shaped state machine once
 * #208 lands and the cloning contract is documented.
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

// Draw-context flag + Phase B equipment swap. Begin/End bracket the
// Player_DrawImpl call inside EnInvader_Draw. Begin() sets the flag
// (so VB_APPLY_TUNIC_COLOR can apply the hostile-black tint via
// Anchor_GetCurrentlyDrawingInvader) and saves the local Player's
// model state, then forces PLAYER_MODELGROUP_SWORD_AND_SHIELD so the
// Invader appears armed regardless of Link's equipment. End() clears
// the flag and restores Player's saved state.
//
// Same draw-context-flag + equipment-swap shape as the NPC Follower's
// Anchor_FollowerNpcDrawBegin/End. See FollowerNPC.cpp:422-526 for
// the canonical pattern. The NPC Follower's bow/slingshot quirk-fix
// (Player_HoldsSlingshot heldItemAction override) is intentionally
// omitted here — v1 Invader has no ranged state.
void   Anchor_InvaderDrawBegin(Actor* invader);
void   Anchor_InvaderDrawEnd(void);
Actor* Anchor_GetCurrentlyDrawingInvader(void);

}  // extern "C"
