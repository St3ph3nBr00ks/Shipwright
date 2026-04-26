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
