/**
 * Invader — NPC Invader Director-side tick driver and rendering hooks.
 *
 * Step 15a — scaffold: actor + draw-context flag.
 * Step 15b — Phase B equipment swap: Anchor_InvaderDrawBegin/End now
 *   ALSO save the local Player's modelGroup + hand/sheath DLists and
 *   force PLAYER_MODELGROUP_SWORD_AND_SHIELD so the Invader appears
 *   permanently armed independent of Link's actual equipment. End()
 *   restores Player's saved state. (Agent 1)
 * Step 15c — Phase 2 locomotion: Anchor_TickInvaderActor implements
 *   IDLE / FOLLOW / STUCK state machine cloned from NPC Follower's
 *   same-named states. Direct yaw + speedXZ; no substrate path
 *   consumption in v1. Includes G18 (cutscene freeze) + G10 (leash
 *   teleport) safety guards. (Agent 2 reconstructed)
 * Step 15d — Phase 3 combat: ATTACK / BLOCK / ENGAGE /
 *   RANGED_ATTACK / STANDBY states cloned from NPC Follower Stage 4
 *   ahead of #208 — the canonical combat shape will be revisited
 *   once #208 documents the design contract. Combat target
 *   selection delegates to PickHostileTargetForInvader (Agent 4).
 *   (Agent 3)
 * Step 15e — Phase 4 animation parity: adds SWIMMING + LEDGE_HOIST
 *   states (cloned from NPC Follower's same-named handlers), kJump /
 *   kRunJump airborne anim selection in FOLLOW (auto-fires on
 *   walked-off-edge), and a 3-anim fidget rotation in IDLE
 *   (kFidgetLookA / kFidgetWarmB / kFidgetStretchD cycle modulo 3
 *   after ~6s of sustained kWait). Full CLIMBING state intentionally
 *   left out — v1 Invader does not pursue into vertical spaces;
 *   slot 2 reserved for future. Draw/sheathe transitions documented
 *   as a gap: OoT has no standalone "draw sword" anim, equipment
 *   swap is instant via Phase B (Anchor_InvaderDrawBegin/End). See
 *   Invader.cpp comment near InvaderAnim enum.
 *
 * Draw-context flag pattern: same shape as the NPC Follower's
 * Anchor_FollowerNpcDrawBegin/End. See FollowerNPC.cpp:422-526 for
 * the canonical equipment-swap pattern. The NPC Follower's
 * bow/slingshot quirk-fix (Player_HoldsSlingshot heldItemAction
 * override) is intentionally omitted here — v1 Invader has no
 * ranged state needing the slingshot variant.
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
// Implements the full state machine: IDLE/FOLLOW/STUCK locomotion
// (Agent 2) + ATTACK/BLOCK/ENGAGE/RANGED_ATTACK/STANDBY combat
// (Agent 3) + G18/G10 safety guards. Skipped on peer replicas.
// TODO post-#208: revisit state shape against canonical follower
// design pass.
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
