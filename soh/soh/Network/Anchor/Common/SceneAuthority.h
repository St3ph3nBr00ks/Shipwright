#pragma once

// Scene authority API (Pillar A).
//
// Phase 1 (active): pure (a) effective-host election with no migrate-back.
// Election rule lives in Anchor::RecomputeEffectiveHost():
//   - If roomState.ownerClientId is online → that client is effective host.
//   - Else lowest-online clientId is effective host.
// The election runs on connect, on AllClientState updates, and on
// UpdateClientState `online` flag flips.
//
// Phase 2 (planned): per-(sceneNum, timeline) authority with handoff.
// `IsSceneHost`/`IsMyCurrentSceneHost` are stubs today and return the
// global answer; they will key by scene/timeline once Phase 2 lands.
//
// Call-site rule: every code path that previously checked
//   `roomState.ownerClientId == ownClientId`
// should call `IsEffectiveHost()` instead so it picks up handoff.

#include <cstdint>

namespace SceneAuthority {

// Returns the clientId currently considered effective host.
uint32_t GetEffectiveHostClientId();

// True when the local client is the effective host of the room.
bool IsEffectiveHost();

// Pillar A Phase 2 — per-(sceneNum, roomNum, timeline) authority.
//
// Election rule: among all online clients whose `client.sceneNum` matches
// the requested sceneNum AND `client.curRoomNum` matches roomNum AND
// `client.linkAge & 0x1` matches timeline, the lowest clientId wins.
//
// Why roomNum is part of the key: OoT scenes can have multiple rooms
// loaded one-at-a-time (Inside Deku Tree, dungeons generally). Room
// transitions unload the previous room's actors and load the new room's,
// so a client in room A doesn't have actor-list visibility of room B's
// enemies. Authority must follow the actor-list visibility — otherwise
// a client alone in a room can't run its state machines because some
// other client in a different room of the same scene "wins" the
// election but has no actors to drive.
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
uint32_t GetSceneHostClientId(int16_t sceneNum, int8_t roomNum, uint8_t timeline);

// True when the local client is the scene host for the supplied tuple.
bool IsSceneHost(int16_t sceneNum, int8_t roomNum, uint8_t timeline);

// Shortcut: IsSceneHost(gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num,
// gSaveContext.linkAge & 0x1). Use only from actor-context call sites
// where gPlayState is valid.
bool IsMyCurrentSceneHost();

}  // namespace SceneAuthority
