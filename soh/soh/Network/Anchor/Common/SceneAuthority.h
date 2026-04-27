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

// True when the local client is the effective host for a specific
// (sceneNum, timeline). Phase 1 stub: same answer as IsEffectiveHost().
bool IsSceneHost(int16_t sceneNum, uint8_t timeline);

// Shortcut: IsSceneHost(gPlayState->sceneNum, gSaveContext.linkAge).
bool IsMyCurrentSceneHost();

}  // namespace SceneAuthority
