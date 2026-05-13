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

#ifdef __cplusplus
}
#endif
