#ifndef ANCHOR_GAME_TIME_CONTROLLER_BRIDGE_H
#define ANCHOR_GAME_TIME_CONTROLLER_BRIDGE_H

// C bridge for GameTimeController::ShouldAdvanceWorldTime.
//
// The C++ gate (GameTimeController.h) is the source of truth. C translation
// units (z_play.c, z_actor.c, ...) call Anchor_ShouldAdvanceWorldTime with
// one of the ANCHOR_TIME_CTX_* macros below.
//
// Macros and the C++ enum (GameTimeController::TimeContext) MUST stay
// numerically aligned. If a new context is added, update:
//   - GameTimeController.h (TimeContext enum)
//   - GameTimeController.cpp (LegacyAdvanceWorldTimeRule switch)
//   - this file (ANCHOR_TIME_CTX_* macro)
// Keep declaration order identical so static_cast<int> round-trips cleanly.

#ifdef __cplusplus
#include <stdbool.h>
extern "C" {
#else
#include <stdbool.h>
#endif

// Mirrors GameTimeController::TimeContext (in GameTimeController.h).
#define ANCHOR_TIME_CTX_PAUSE_MENU       0
#define ANCHOR_TIME_CTX_TEXT_BOX         1
#define ANCHOR_TIME_CTX_ITEM_GET         2
#define ANCHOR_TIME_CTX_CUTSCENE         3
#define ANCHOR_TIME_CTX_OCARINA          4
#define ANCHOR_TIME_CTX_SCENE_TRANSITION 5

// Returns true if world time should advance this frame for the given context.
// Phase 1 / Pillar G.i: PauseMenu in multiplayer always returns true; every
// other context returns the legacy single-player rule.
bool Anchor_ShouldAdvanceWorldTime(int contextEnum);

#ifdef __cplusplus
}
#endif

#endif  // ANCHOR_GAME_TIME_CONTROLLER_BRIDGE_H
