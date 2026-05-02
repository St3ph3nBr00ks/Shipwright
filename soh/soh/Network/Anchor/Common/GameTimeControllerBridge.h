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

// Returns true iff the pause menu is *actively freezing the world* this frame.
//
// Inverse of Anchor_ShouldAdvanceWorldTime(ANCHOR_TIME_CTX_PAUSE_MENU). Wraps
// the inversion so call sites match their original semantics line-for-line:
//
//     if (play->pauseCtx.state != 0)        ← legacy: "pause is freezing"
//     if (Anchor_PauseMenuFreezesWorld())   ← MP-aware: same semantics + MP false
//
// In multiplayer, returns false even when the pause menu UI is open, so any
// gameplay subsystem migrated to this predicate skips its freeze. Currently
// consumed by game.c (rendering gate at line 341). Future routings will
// retire other direct pauseCtx.state reads as they're identified.
bool Anchor_PauseMenuFreezesWorld(void);

// Returns true iff the live-world pause-menu rendering feature is active
// this frame: multiplayer is on (Anchor enabled), the pause menu is up,
// AND the gAnchor.PauseLiveWorld CVar is set.
//
// When this returns true, the rendering pipeline should:
//   - skip pause-Link init (func_80091738 DMA stomp + gSegments[4]/[6]
//     override) so the world's object bank stays intact;
//   - skip the rotating-Link draw in the equipment screen;
//   - skip the mode-3 captured-frame backdrop (z_play.c:1504-1513);
//   - allow DummyPlayer_Draw to run during pause (peer visibility).
//
// All four call sites gate on this single predicate so the CVar acts as
// an atomic enable/disable for the whole feature. Default-off ships the
// current safe behaviour (DummyPlayer hidden, captured-frame shown,
// pause-Link rotates).
//
// CVar: gAnchor.PauseLiveWorld (integer, default 0).
bool Anchor_PauseLiveWorldRendering(void);

#ifdef __cplusplus
}
#endif

#endif  // ANCHOR_GAME_TIME_CONTROLLER_BRIDGE_H
