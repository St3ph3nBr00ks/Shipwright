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

// Pillar A Phase 2 — per-(sceneNum, timeline) authority.
//
// Election rule: among all online clients whose `client.sceneNum` matches
// the requested sceneNum AND whose `client.linkAge & 0x1` matches the
// requested timeline, the lowest clientId wins. This gives each scene+
// timeline its own scene host independent of the global effective host,
// so a peer alone in a room becomes that room's authority and runs its
// state machines (ENEMY_STATE broadcast, ENEMY_SPAWN, etc.) without
// needing the global host to be in the same room.
//
// Empty-scene fallback: when no online client is in (sceneNum, timeline),
// returns GetEffectiveHostClientId() (Phase 1 global host). This keeps
// non-actor-context queries — e.g. dispatching a packet for a sceneNum
// no one's currently in — answerable without crashing the call site.
//
// Known limitation: scene-transition window (~100ms) where local view
// of clients map disagrees with peer's view. Both clients converge as
// UPDATE_CLIENT_STATE broadcasts arrive. Authority briefly flickers;
// state-sync re-converges naturally.

// Returns the clientId currently authoritative for (sceneNum, timeline).
uint32_t GetSceneHostClientId(int16_t sceneNum, uint8_t timeline);

// True when the local client is the scene host for (sceneNum, timeline).
bool IsSceneHost(int16_t sceneNum, uint8_t timeline);

// Shortcut: IsSceneHost(gPlayState->sceneNum, gSaveContext.linkAge & 0x1).
// Use only from actor-context call sites where gPlayState is valid.
bool IsMyCurrentSceneHost();

}  // namespace SceneAuthority
