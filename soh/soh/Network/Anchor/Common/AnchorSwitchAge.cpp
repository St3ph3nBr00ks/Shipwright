#include "AnchorSwitchAge.h"
#include "SceneMultiplayerConfig.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace AnchorSwitchAge {

Result SwitchAgeAndTeleport(s32 targetLinkAge,
                            s16 targetSceneNum,
                            s32 targetEntrance,
                            s8  targetRoom,
                            Vec3f targetPos,
                            s16 targetYaw) {
    if (gPlayState == nullptr) {
        return Result::RefusedNoPlayState;
    }

    // Q 4.B.6 — TimeTravel CVar gate. When the enhancement is disabled,
    // the player has explicitly opted out of mid-game age switching.
    // Cross-timeline teleport refuses without any side effects.
    if (CVarGetInteger(CVAR_ENHANCEMENT("TimeTravel"), TIME_TRAVEL_OOT) == TIME_TRAVEL_DISABLED) {
        SPDLOG_INFO("[AnchorSwitchAge] Refused: CVAR_ENHANCEMENT(TimeTravel) == DISABLED");
        return Result::RefusedTimeTravelDisabled;
    }

    // Q 4.B.7 — castle-exit override. Some scenes have age-mismatched
    // geometry that drops the player through the floor when respawning at
    // an exact pos/yaw saved by a peer at the other age. The §5 override
    // table flags those scenes; on a hit, we discard the supplied
    // pos/yaw/room and replace the entrance with a stable outdoor spawn.
    auto overrides = SceneMultiplayerConfig::GetSceneOverrides(targetSceneNum, /*roomNum=*/-1);
    bool useExitPath = overrides.forceCastleExitPath;

    if (useExitPath) {
        SPDLOG_INFO("[AnchorSwitchAge] Q 4.B.7 castle-exit override applied for scene {} ({})",
                    targetSceneNum, overrides.displayName ? overrides.displayName : "?");
        targetEntrance = ENTR_CASTLE_GROUNDS_SOUTH_EXIT;
        // Drop the exact-position respawn — let OoT use the entrance's
        // canonical spawn pos/yaw/room via the normal scene-load path.
        // gSaveContext.respawnFlag stays 0 below for this branch.
    }

    // Adapt SwitchAge() pattern from soh/Enhancements/mods.cpp:20-66.
    if (!useExitPath) {
        gSaveContext.respawnFlag = 1;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = targetEntrance;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex     = targetRoom;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].pos           = targetPos;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw           = targetYaw;
        gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams  = 0xDFF;
    } else {
        // Castle-exit path: clean entrance load. Reset any stale respawn
        // data that a prior teleport may have left so OoT doesn't honor
        // a now-incorrect mid-castle position when the entrance loads.
        gSaveContext.respawnFlag = 0;
    }

    gPlayState->nextEntranceIndex   = targetEntrance;
    gPlayState->transitionTrigger   = TRANS_TRIGGER_START;
    gPlayState->transitionType      = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;

    // The age switch — gPlayState->linkAgeOnLoad propagates to
    // gSaveContext.linkAge on the next scene init.
    gPlayState->linkAgeOnLoad = targetLinkAge;

    // One-shot void-damage suppression for the transition frame, mirror of
    // the SwitchAge() / TeleportTo handler pattern. Without this, mid-air
    // pos values from the peer would void-out the player on landing.
    static HOOK_ID hookId = 0;
    hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
        *should = false;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
    });

    SPDLOG_INFO("[AnchorSwitchAge] Cross-timeline teleport: targetLinkAge={} sceneNum={} entrance=0x{:04X} useExitPath={}",
                targetLinkAge, (int)targetSceneNum, (u16)targetEntrance, useExitPath);

    return Result::Accepted;
}

}  // namespace AnchorSwitchAge
