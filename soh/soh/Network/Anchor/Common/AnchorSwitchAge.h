#pragma once

// Pillar B Phase 5 — cross-timeline teleport (Q 4.B.4 + Q 4.B.6 + Q 4.B.7).
//
// Adapts SoH's existing SwitchAge() pattern (soh/Enhancements/mods.cpp:20)
// with a custom destination so REQUEST_TELEPORT / TELEPORT_TO can carry a
// requester across timelines: the requester switches gPlayState->linkAgeOnLoad
// to the target's linkAge while triggering the scene transition.
//
// Gates applied:
//   Q 4.B.6 — refuse if CVAR_ENHANCEMENT("TimeTravel") == TIME_TRAVEL_DISABLED.
//   Q 4.B.7 — when SceneMultiplayerConfig::GetSceneOverrides flags the
//             destination as forceCastleExitPath (Hyrule Castle / Outside
//             Ganon's Castle), the entrance is rerouted to
//             ENTR_CASTLE_GROUNDS_SOUTH_EXIT and the requester's exact
//             position/yaw is discarded — landing the player at a stable
//             outdoor spawn rather than mid-castle where age-mismatched
//             collision can drop them through the floor.
//   Q 4.B.8 — confirm dialog skipped in v1; CVar
//             CVAR_REMOTE_ANCHOR("SkipCrossTimelineConfirm") is reserved
//             for the future polish pass when the dialog lands.
//   Q 4.B.9 — AI Follower follows automatically because the local follower
//             shares gSaveContext.linkAge with the leader (see Pillar D
//             "shared inventory with leader" decision in design doc).
//             No explicit hook needed.
//
// Plan: Claude/Plans/pillar_b_timeline_implementation.md.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers

extern "C" {
#include "z64.h"
}

namespace AnchorSwitchAge {

enum class Result {
    Accepted,                    // teleport in flight; transition trigger armed
    RefusedTimeTravelDisabled,   // CVAR_ENHANCEMENT("TimeTravel") == DISABLED
    RefusedNoPlayState,          // gPlayState == nullptr (defensive)
};

// Switch local linkAge to targetLinkAge AND teleport to the supplied
// destination as a single atomic transition. Caller must already have
// verified IsSaveLoaded() and that targetLinkAge differs from local.
//
// Returns Accepted on success (transition is armed and will fire on the
// next frame). Otherwise no game state has been mutated.
Result SwitchAgeAndTeleport(s32 targetLinkAge,
                            s16 targetSceneNum,
                            s32 targetEntrance,
                            s8  targetRoom,
                            Vec3f targetPos,
                            s16 targetYaw);

}  // namespace AnchorSwitchAge
