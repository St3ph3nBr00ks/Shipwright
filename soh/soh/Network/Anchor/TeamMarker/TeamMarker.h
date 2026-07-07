#pragma once

/*
 * TeamMarker.h — public API for the SoH Team Marker.
 *
 * Plan: Plans/team_marker_plan.md.
 * Tracker: St3ph3nBr00ks/Shipwright#219.
 *
 * The Team Marker is a through-walls fairy indicator drawn over each
 * same-team peer's head. All state lives in TeamMarker.cpp as file-
 * statics (per-peer marker map + sidecar owner-lookup map + draw-context
 * flag) — Anchor.h stays untouched.
 *
 * Consumers:
 *   - z_en_team_marker.c calls Anchor_TeamMarkerDrawBegin/End around the
 *     fairy DL emit so the draw callback can look up the owner's Anchor
 *     colour + name for tinting / nameplate.
 *   - ShipInit hook registers the OnGameFrameUpdate reconciliation tick.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;

// Draw-context flag pair. Set around the fairy DL emit in
// EnTeamMarker_Draw. Anchor_GetCurrentlyDrawingTeamMarker returns the
// marker Actor* between Begin and End; nullptr otherwise. Mirrors
// Anchor_FollowerNpcDrawBegin/End (NPC Follower colour-fix pattern).
void   Anchor_TeamMarkerDrawBegin(struct Actor* marker);
void   Anchor_TeamMarkerDrawEnd(void);
struct Actor* Anchor_GetCurrentlyDrawingTeamMarker(void);

// Sidecar owner-lookup. `params` is s16 and cannot hold the full
// uint32_t Anchor clientId; the marker's owning clientId is stored in
// a file-static map at spawn time and looked up here at draw time.
// Returns 0 if `marker` is unknown.
unsigned int Anchor_GetTeamMarkerOwnerClientId(struct Actor* marker);

#ifdef __cplusplus
}
#endif
