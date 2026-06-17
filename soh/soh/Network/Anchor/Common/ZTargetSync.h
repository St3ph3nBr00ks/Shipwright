#pragma once

// Multi-player Z-target query helper. Pattern 4 / #277 follow-up.
//
// Vanilla `Actor_IsTargeted(play, actor)` reads the LOCAL Player's
// Z-target context and `actor->isTargeted` (a per-actor field set by
// the local target-context manager in z_actor.c). Both are host-local:
// they reflect ONLY the local Player's lock-on. When a peer Z-targets
// an enemy on host's machine, host's `actor->isTargeted` stays false
// and enemy AI gates that check `Actor_IsTargeted` fail (Stalfos's
// "commit to SlashDown when player is locked on" branch is the
// surfaced case — see Analysis/peer_player_state_deep_analysis
// _2026-06-16.md §3.1.1).
//
// This helper returns true if EITHER the local Player has Z-locked
// onto `actor`, OR any peer has Z-locked onto `actor` (per the
// `focusActorNetId` field added to PLAYER_UPDATE in the same change).
//
// Use this in enemy AI as a drop-in replacement for `Actor_IsTargeted`
// when the gate is an AI-decision gate that should respect peer
// targeting. Do NOT use for local-only purposes (UI hints, local SFX
// positioning) — keep `Actor_IsTargeted` for those.
//
// `extern "C"` for direct callability from C decomp TUs (per Pitfall 7
// the .c caller's forward-decl uses plain `extern` syntax).

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

extern "C" bool Anchor_IsActorTargetedByAnyPlayer(Actor* actor, PlayState* play);
