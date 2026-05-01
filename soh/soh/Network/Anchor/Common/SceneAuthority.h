#pragma once

// Scene authority API (Pillar A).
//
// Two layers of host:
//
//   Effective host (Phase 1, global):
//     - If roomState.ownerClientId is online → that client is effective host.
//     - Else lowest-online clientId is effective host.
//     Used for non-actor concerns: room admin UI, whole-room bookkeeping
//     like ClearAllDefeatBroadcasts, fallback when the room scope is empty.
//     Election runs on connect, on AllClientState updates, and on
//     UpdateClientState `online` flag flips.
//
//   Room host (Phase 2, per-(sceneNum, roomNum, timeline)):
//     Lowest-online clientId currently in that scope. Used for actor-
//     context decisions (ENEMY_STATE broadcast, ENEMY_SPAWN, OnEnemyDefeat,
//     ShouldActorUpdate's targeting-field patch, etc.). Election is purely
//     a function of the local clients-map snapshot — no handoff packet —
//     so all clients converge on the same answer.
//
// Call-site rule: code that previously checked
//   `roomState.ownerClientId == ownClientId` for actor-context decisions
//   → `IsMyCurrentRoomHost()` (or `IsRoomHost(scene, room, timeline)`).
// Code checking the same condition for non-actor concerns (admin UI, etc.)
//   → `IsEffectiveHost()`.

#include <cstdint>

namespace SceneAuthority {

// Returns the clientId currently considered effective host.
uint32_t GetEffectiveHostClientId();

// True when the local client is the effective host of the room.
bool IsEffectiveHost();

// Pillar A Phase 2 — per-(sceneNum, roomNum, timeline) "room host"
// authority for actor-context decisions.
//
// Election rule: among all online clients whose `client.sceneNum` matches
// the requested sceneNum AND `client.curRoomNum` matches roomNum AND
// `client.linkAge & 0x1` matches timeline, the lowest clientId wins.
//
// Why room-level (not scene-level): OoT scenes can have multiple rooms
// loaded one-at-a-time (Inside Deku Tree, dungeons generally). Room
// transitions unload the previous room's actors and load the new room's,
// so a client in room A has no actor-list visibility of room B's
// enemies. Authority must follow the actor-list visibility — otherwise
// a client alone in a room can't run its state machines because some
// other client in a different room of the same scene "wins" the
// election but has no actors to drive.
//
// "Room host" vs "effective host": the room host is the per-room
// election above (used everywhere actor-context decisions happen —
// ENEMY_STATE broadcast, ENEMY_SPAWN, OnEnemyDefeat, etc.). The
// effective host (`IsEffectiveHost` above) is the global room-owner
// election from Phase 1, used for non-actor concerns: room admin UI,
// whole-room bookkeeping like ClearAllDefeatBroadcasts, and the
// fallback when an actor's room has no online occupant.
//
// Empty-room fallback: when no online client is in (sceneNum, roomNum,
// timeline), returns GetEffectiveHostClientId() (Phase 1 global host).
//
// Known limitation: room-transition window (~100ms) where local view
// of clients map disagrees with peers'. Both sides converge as
// UPDATE_CLIENT_STATE broadcasts arrive. Authority briefly flickers;
// state-sync re-converges naturally.

// Returns the clientId currently authoritative for the (sceneNum,
// roomNum, timeline) tuple.
uint32_t GetRoomHostClientId(int16_t sceneNum, int8_t roomNum, uint8_t timeline);

// True when the local client is the room host for the supplied tuple.
bool IsRoomHost(int16_t sceneNum, int8_t roomNum, uint8_t timeline);

// Shortcut: IsRoomHost(gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num,
// gSaveContext.linkAge & 0x1). Use only from actor-context call sites
// where gPlayState is valid.
bool IsMyCurrentRoomHost();

}  // namespace SceneAuthority
