#include "SceneMultiplayerConfig.h"

namespace SceneMultiplayerConfig {

// Sparse override map. Most scenes have no entry (defaults apply).
// Whole-scene entries use roomNum = -1; per-room entries use specific roomNum.
//
// Initial seed entries (2026-04-25):
// - Teleport-problematic scenes (retired from RequestTeleport.cpp:70-78)
// - Q 4.B.7 cross-timeline castle-exit destinations
// - Cutscene rooms — TBD during First Dungeon Demo (#167) playtest;
//   placeholder commented out below.
static const std::unordered_map<SceneRoomKey, SceneMultiplayerOverrides, SceneRoomKeyHash>
kSceneMultiplayerOverrides = {
    // Retired from RequestTeleport.cpp:70-78
    {{SCENE_GROTTOS, -1},
     {.disableTeleportTo = true, .displayName = "grottos (ambiguous entrance)"}},
    {{SCENE_MARKET_DAY, -1},
     {.disableTeleportTo = true, .displayName = "Castle Town Market"}},
    {{SCENE_MARKET_NIGHT, -1},
     {.disableTeleportTo = true, .displayName = "Castle Town Market (night)"}},
    {{SCENE_MARKET_RUINS, -1},
     {.disableTeleportTo = true, .displayName = "Castle Town Ruins"}},
    {{SCENE_MARKET_ENTRANCE_DAY, -1},
     {.disableTeleportTo = true, .displayName = "Market Entrance"}},
    {{SCENE_MARKET_ENTRANCE_NIGHT, -1},
     {.disableTeleportTo = true, .displayName = "Market Entrance (night)"}},
    {{SCENE_MARKET_ENTRANCE_RUINS, -1},
     {.disableTeleportTo = true, .displayName = "Market Entrance (ruins)"}},
    {{SCENE_TEMPLE_OF_TIME_EXTERIOR_DAY, -1},
     {.disableTeleportTo = true, .displayName = "Temple of Time Exterior"}},
    {{SCENE_TEMPLE_OF_TIME_EXTERIOR_NIGHT, -1},
     {.disableTeleportTo = true, .displayName = "ToT Exterior (night)"}},
    {{SCENE_TEMPLE_OF_TIME_EXTERIOR_RUINS, -1},
     {.disableTeleportTo = true, .displayName = "ToT Exterior (ruins)"}},
    {{SCENE_BACK_ALLEY_DAY, -1},
     {.disableTeleportTo = true, .displayName = "Back Alley (day)"}},
    {{SCENE_BACK_ALLEY_NIGHT, -1},
     {.disableTeleportTo = true, .displayName = "Back Alley (night)"}},

    // Q 4.B.7 — cross-timeline castle-exit override
    {{SCENE_HYRULE_CASTLE, -1},
     {.forceCastleExitPath = true, .displayName = "Hyrule Castle Grounds (adult)"}},
    {{SCENE_OUTSIDE_GANONS_CASTLE, -1},
     {.forceCastleExitPath = true, .displayName = "Outside Ganon's Castle (child)"}},

    // Q H — cutscene rooms with PvP disabled. Curated during demo playtest.
    // {{SCENE_DEKU_TREE, GOHMA_ROOM_NUM},
    //  {.disablePvP = true, .displayName = "Gohma boss arena"}},
    // {{SCENE_TEMPLE_OF_TIME, PEDESTAL_ROOM_NUM},
    //  {.disablePvP = true, .displayName = "Master Sword pedestal"}},
};

SceneMultiplayerOverrides GetSceneOverrides(int16_t sceneNum, int8_t roomNum) {
    if (auto it = kSceneMultiplayerOverrides.find({sceneNum, roomNum});
        it != kSceneMultiplayerOverrides.end()) return it->second;
    if (auto it = kSceneMultiplayerOverrides.find({sceneNum, (int8_t)-1});
        it != kSceneMultiplayerOverrides.end()) return it->second;
    return {};  // all defaults
}

bool ShouldDisablePvP(PlayState* play) {
    if (play == nullptr) return false;

    // Table check
    if (GetSceneOverrides(play->sceneNum, (int8_t)play->roomCtx.curRoom.num).disablePvP) {
        return true;
    }

    // Property: any active cutscene auto-disables PvP
    if (play->csCtx.state != CS_STATE_IDLE) return true;

    // Property: during any scene transition wipe
    if (play->transitionMode != TRANS_MODE_OFF) return true;

    return false;
}

}  // namespace SceneMultiplayerConfig
