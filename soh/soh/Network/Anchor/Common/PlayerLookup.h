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
