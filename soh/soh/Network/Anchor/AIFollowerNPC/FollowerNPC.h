#pragma once

/*
 * FollowerNPC.h — public API to the C actor body.
 *
 * Plan: Plans/npc_follower_plan.md.
 *
 * z_en_follower.c (the C actor body) calls into our C++ tick via the
 * extern "C" wrapper declared here. State machine + locomotion live
 * in the .cpp file alongside the Phase 2 spawn helper.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Forward decls so the header is C-includable without pulling in
// z64.h.
struct Actor;
struct PlayState;

// Per-tick state machine driver. Called from EnFollower_Update for
// the local owner's NPC AND for peer replicas (the function gates
// internally — peer replicas skip AI, just apply STATE packets in
// their Update).
//
// Phase 4 contract:
//   - Determines whether `npc` is our local NPC or a peer replica.
//   - If local: runs IDLE / FOLLOW state machine, drives locomotion
//     via Actor_MoveXZGravity.
//   - If peer replica: no-op (pos comes from STATE packet apply).
void Anchor_TickFollowerNpcActor(struct Actor* npc, struct PlayState* play);

// Draw-context flag (color-bug fix, mirrors Anchor_PauseLinkDrawBegin
// / End). Set around EnFollower_Draw's Player_DrawImpl call so the
// VB_APPLY_TUNIC_COLOR hook in HookHandlers.cpp can apply the OWNER's
// color instead of inheriting whatever GPU env color the previous
// DummyPlayer draw left behind.
//
// Same root cause as the pause-menu bug — Player_DrawImpl's `data`
// param is the local player Player* (so equipment-draw resolves
// correctly), which doesn't match the npc Actor* in the
// VB_APPLY_TUNIC_COLOR check. Without this sentinel, the hook can't
// distinguish "drawing the NPC" from "drawing the local player" and
// applies the wrong color.
//
// `Anchor_GetCurrentlyDrawingFollowerNpc` returns the npc Actor*
// pointer being drawn (NULL outside the begin/end pair). The hook
// uses this to look up the owner via Anchor::FindFollowerNpcOwner.
void   Anchor_FollowerNpcDrawBegin(struct Actor* npc);
void   Anchor_FollowerNpcDrawEnd(void);
struct Actor* Anchor_GetCurrentlyDrawingFollowerNpc(void);

#ifdef __cplusplus
}
#endif
