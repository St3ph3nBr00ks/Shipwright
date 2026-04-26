#pragma once

// Scene/room multiplayer overrides (§5).
//
// Sparse static map keyed by (sceneNum, roomNum). Most scenes have no
// entry and inherit defaults (everything allowed). Powers PvP-disable
// in cutscene rooms (Q H), teleport-exclusion list retirement
// (RequestTeleport.cpp:70-78), and Q 4.B.7 cross-timeline castle-exit
// override.
//
// See Claude/Plans/scene_multiplayer_overrides.md.

#include <unordered_map>
#include <utility>

extern "C" {
#include "z64.h"
}

namespace Anchor {

struct SceneMultiplayerOverrides {
    bool disableMultiplayer  = false;  // no sync at all (title, credits)
    bool disablePvP          = false;  // suppress player-vs-player damage
    bool disableTeleportTo   = false;  // opponents can't teleport here
    bool forceCastleExitPath = false;  // Q 4.B.7 cross-timeline routing
    const char* displayName  = nullptr;  // human-readable diagnostic tag
};

using SceneRoomKey = std::pair<int16_t, int8_t>;

struct SceneRoomKeyHash {
    size_t operator()(const SceneRoomKey& k) const noexcept {
        return std::hash<int32_t>()((int32_t)k.first << 8 | (uint8_t)k.second);
    }
};

// Lookup: per-room first, then whole-scene (roomNum = -1), then defaults.
SceneMultiplayerOverrides GetSceneOverrides(int16_t sceneNum, int8_t roomNum);

// Property-aware wrapper for PvP gating.
// Combines table lookup with property-based fallbacks:
// - any active cutscene (csCtx.state != CS_STATE_IDLE) → disable PvP
// - any scene transition (transitionMode != TRANS_MODE_OFF) → disable PvP
bool ShouldDisablePvP(PlayState* play);

}  // namespace Anchor
