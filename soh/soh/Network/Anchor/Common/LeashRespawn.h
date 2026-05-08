/**
 * LeashRespawn — host-only enemy rehydration on scene re-entry.
 *
 * Plan §10. When a player re-enters a scene that was previously
 * vacated (so synced enemies despawned via vanilla cleanup), the
 * host overrides the spawn position with the last-known
 * authoritative position from the registry — IF a player was last
 * seen near that enemy.
 *
 * Enemy-only by intent. Players own their own actor lifecycle, AI
 * Followers respawn via their state machine, and waypoint actors
 * are scene-baked. The "respawn into scene with overridden
 * position" surface is unique to host-authoritative synced enemies,
 * so the override map only takes EnemyNetId-tracked actors.
 *
 * Default-off: gated by gEnhancements.Nav.Enabled AND
 * gEnhancements.Nav.LeashRespawn. NavTraits.eligibleForLeashRespawn
 * gates per-actor (AI Follower opts out by default; bosses opt out
 * via kBossDefaults).
 *
 * Host-only: a non-host has no authority to override actor positions.
 * The OnSceneSpawnActors hook gates on `SceneAuthority::IsMyCurrentRoomHost`
 * before any override fires. Migration corner case (ungraceful host
 * disconnect mid-session): registry lives on the original host and
 * is lost on migration; new host falls back to vanilla home.pos.
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §10
 *   - GitHub #206 Phase 1 commit 7
 */

#pragma once

#include <cstdint>

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

// Record the last-known authoritative position for `netId`. Called
// by host-side ENEMY_STATE send paths (or any other authority pump
// that broadcasts a synced enemy's position). The registry persists
// across scene transitions on the host until the host disconnects
// or the game exits.
void RecordKnownPosition(uint32_t netId, const Vec3f& pos);

// Drop the registry entry for `netId`. Called when a synced enemy
// is killed — the rehydration of a confirmed-dead enemy is undesired.
void ForgetKnownPosition(uint32_t netId);

// Drop every registry entry. Called from OnExitGame so the next
// session starts clean.
void ClearAllKnownPositions();

// Scene-spawn callback. Walks the newly-spawned synced-enemy actor
// list; for each actor whose netId is in the registry AND whose
// NavTraits.eligibleForLeashRespawn is true AND a recent player
// trail waypoint sits within ~600u of the registered position,
// override actor->world.pos with the registered position.
//
// Host-only — non-host call is a no-op.
void OnSceneEnter(int16_t sceneNum, PlayState* play);

// True when the master Nav CVar is on AND Nav.LeashRespawn is on.
bool IsLeashRespawnEnabled();

// ShipInit registration. Hooks OnSceneSpawnActors and OnExitGame.
void RegisterLeashRespawn();

} // namespace AnchorNav
