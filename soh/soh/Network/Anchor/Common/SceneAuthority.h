#pragma once

// Scene authority API (Pillar A).
//
// Phase 1 STUB: returns the legacy answer (`roomState.ownerClientId == ownClientId`)
// to preserve behaviour. Full migration logic in
// Claude/Plans/anchor_host_migration_plan.md fills in the real
// election rule when implementation begins.
//
// Call-site rule: every code path that previously checked
//   `roomState.ownerClientId == ownClientId`
// should be migrated to call `IsEffectiveHost()` instead. The migration
// is mechanical because the stub returns the same answer today.

#include <cstdint>

namespace SceneAuthority {

// Returns the clientId currently considered effective host.
// Phase 1 stub: returns roomState.ownerClientId unconditionally.
uint32_t GetEffectiveHostClientId();

// True when the local client is the effective host of the room.
bool IsEffectiveHost();

// True when the local client is the effective host for a specific
// (sceneNum, timeline). Phase 1 stub: same answer as IsEffectiveHost().
bool IsSceneHost(int16_t sceneNum, uint8_t timeline);

// Shortcut: IsSceneHost(gPlayState->sceneNum, gSaveContext.linkAge).
bool IsMyCurrentSceneHost();

}  // namespace SceneAuthority
