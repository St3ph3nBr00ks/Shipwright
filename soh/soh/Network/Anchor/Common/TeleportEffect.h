// TeleportEffect — vanilla-parity sparkle burst helper.
//
// Purpose (variant D landing, 2026-07-09): first landed as the visual
// polish for the cutscene late-join case, but designed as a reusable
// module for every teleport site in the codebase. Session_state Q about
// applying this to Anchor player teleport, AI Player Follower, NPC
// Follower, AI Invader — all should consume this module and pass the
// appropriate theme.
//
// Uses vanilla EffectSsKiraKira_SpawnDispersed (z_actor.c:2453 pattern
// from Farore's Wind respawn) — the primitive underneath the
// DEMOKANKYO_SPARKLES actor, without the actor lifecycle baggage.
// Sparkles animate independently for their configured life duration, so
// a burst at a departure position lingers naturally after the source
// actor teleports away.
//
// Threading: game-thread only. Depends on gPlayState-scoped effect
// system.
//
// See Analysis/vote_skip_straggler_bug_2026-07-09.md sibling doc for the
// analysis pattern; teleport-effect analysis in
// session_state.md's "⭐ Team Marker" section (color polish precedent
// for peer-color-driven effects).

#pragma once

#include <cstdint>

struct Vec3f;
struct PlayState;

namespace TeleportEffect {

// Spawn a burst of sparkle particles centered on `centerX/Y/Z`.
//
// Sparkle inner color = (primR, primG, primB); outer glow = (envR,
// envG, envB). For the peer-color use case, callers pass the peer's
// AnchorClient.color values as primR/G/B and a darkened variant as
// envR/G/B (60% intensity).
//
// count = number of particles (recommended 15-20). life = frames alive
// per particle (recommended 30 = ~0.5 s at 60 fps). scale = 1000 matches
// vanilla Farore's Wind respawn.
//
// Effect drifts upward with slight gravity; ~40u radial spread from
// center. Placement is randomized within a small volume so the
// burst reads as a magical event, not a static texture.
void SpawnSparkleBurst(PlayState* play,
                       float centerX, float centerY, float centerZ,
                       uint8_t primR, uint8_t primG, uint8_t primB,
                       uint8_t envR, uint8_t envG, uint8_t envB,
                       int count = 20,
                       int life = 30);

}  // namespace TeleportEffect

// C-linkage shim for calling from C decomp translation units.
extern "C" void Anchor_SpawnTeleportSparkles(
    PlayState* play,
    float x, float y, float z,
    uint8_t primR, uint8_t primG, uint8_t primB,
    uint8_t envR, uint8_t envG, uint8_t envB);
