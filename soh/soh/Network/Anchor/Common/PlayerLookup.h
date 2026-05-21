#pragma once

// Player-lookup helper shared by enemy sync, AI follower, and any
// future feature that needs "who's the nearest player to this enemy".
// Extracted from HookHandlers.cpp in #173 Phase 1.
//
// See Claude/Plans/anchor_code_decoupling.md.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

// Returns the Actor* of the nearest player-type actor to `enemy`.
// Considers the local player and all live DummyPlayer actors.
// DummyPlayers that are out-of-scene are already at (-9999,-9999,-9999) by
// DummyPlayer_Update, so they are naturally excluded by the distance comparison.
Actor* FindNearestPlayerActor(Actor* enemy, PlayState* play);

// Fills `outActors[0..min(maxCount, returned)-1]` with the local Link's
// Actor* followed by each in-timeline DummyPlayer Actor* present in
// the actor list. Cross-timeline DummyPlayers are filtered out (Pillar
// B Phase 3) so callers spawning enemies / picking targets don't see
// invisible peers from the other timeline. Returns the total count.
//
// Out-of-scene DummyPlayers are excluded by their (-9999,-9999,-9999)
// position — callers that want to gate by scene presence should
// distance-filter on the returned actors.
int GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount);

// Multi-player hostile-target picker for NPC Invader.
//
// Walks the FULL session — local Player + every in-timeline DummyPlayer
// in the actor list + the local NPC Follower (when targetable) — and
// returns the closest valid candidate by XZ distance from the invader.
//
// Per-candidate validity gates (any failure → skip):
//   - For the local Player: gPlayState->sceneNum equals the invader's
//     scene (always true since the local Player IS in our scene), save
//     loaded (Anchor::Instance->IsSaveLoaded() / gSaveContext fallback),
//     not in cutscene (gPlayState->csCtx.state == CS_STATE_IDLE),
//     and gPlayState->sceneNum is NOT in IsSceneFlaggedNoInvaders.
//   - For DummyPlayers: same-timeline filter (Pillar B Phase 3),
//     plus the owning peer's AnchorClient must be online + isSaveLoaded
//     + csCtxState == CS_STATE_IDLE + sceneNum == local scene + scene
//     not blacklisted. Out-of-scene DummyPlayers (at -9999) are
//     naturally excluded by sceneNum mismatch since DummyPlayer_Update
//     parks them outside the live actor coordinate range.
//   - For the NPC Follower: IsFollowerNpcTargetable() must be true,
//     update pointer non-null, same-scene as invader, and the invader
//     is in a non-blacklisted scene.
//
// Scoring: closest by 2D (XZ) distance squared. Future phases can
// extend with LOS / aggro / sticky-target on top of this — keep the
// signature stable.
//
// Returns nullptr when no valid candidate exists. Callers should
// treat nullptr as "no target — hold IDLE".
//
// In a single-player session the function collapses to "local Player
// is the only candidate" with the same gating; in that case it returns
// the local Player when it passes the gates, otherwise nullptr.
//
// This is the multi-player replacement for Anchor_GetNearestPlayerActor
// when the consumer is the NPC Invader — that legacy helper is used by
// vanilla-enemy targeting and does NOT apply session-level validity
// gates (cutscene / save-loaded / scene blacklist). Other consumers
// of Anchor_GetNearestPlayerActor remain unchanged; this helper is
// the new entry point for the Invader actor's target-acquisition path.
Actor* PickHostileTargetForInvader(Actor* invader, PlayState* play);
