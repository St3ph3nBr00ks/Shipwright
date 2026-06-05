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
#include <stdint.h>
extern "C" {
#else
#include <stdbool.h>
#include <stdint.h>
#endif

// Mirrors GameTimeController::TimeContext (in GameTimeController.h).
#define ANCHOR_TIME_CTX_PAUSE_MENU       0
#define ANCHOR_TIME_CTX_TEXT_BOX         1
#define ANCHOR_TIME_CTX_ITEM_GET         2
#define ANCHOR_TIME_CTX_CUTSCENE         3
#define ANCHOR_TIME_CTX_OCARINA          4
#define ANCHOR_TIME_CTX_SCENE_TRANSITION 5
#define ANCHOR_TIME_CTX_GAME_OVER        6  // Pillar G.iii (#239)

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
// CVar: gRemote.Anchor.PauseLiveWorld (integer, default 0).
bool Anchor_PauseLiveWorldRendering(void);

// Returns true ~30% of the frames it is called, false the other ~70%, using
// a fractional-carry counter (0.3 per call; an extra tick is requested when
// the carry crosses 1.0, then 1.0 is subtracted).
//
// Used by the kaleido update path to drive an extra `KaleidoScope_Update`
// tick on those frames so pause-menu animations animate at ~26fps while
// the world tick stays at 20fps. Only meaningful when
// `Anchor_PauseLiveWorldRendering()` is true — single-player and the
// vanilla full-freeze pause path don't need acceleration.
//
// Caller is expected to invoke this exactly once per kaleido update tick.
// Internal carry resets to 0 when the pause menu closes (state == 0) so
// the rate stays predictable across pause sessions.
bool Anchor_PauseMenuShouldExtraTick(void);

// Pillar G.ii — item-get presentation mode for a getItem entry.
//
// Mirrors GameTimeController::ItemPresentationMode (in GameTimeController.h).
// Macros and the C++ enum MUST stay numerically aligned. If a new
// presentation mode is added, update:
//   - GameTimeController.h (ItemPresentationMode enum)
//   - GameTimeController.cpp (GetItemPresentationMode logic)
//   - this file (ANCHOR_ITEM_PRESENTATION_* macro)
//   - z_player.c func_8083A434 (call-site switch)
//
// Default in single-player AND when multiplayer is off: VANILLA. Default
// in multiplayer for non-iconic items: NOTIFICATION_ONLY.
#define ANCHOR_ITEM_PRESENTATION_VANILLA            0
#define ANCHOR_ITEM_PRESENTATION_NOTIFICATION_ONLY  1

// Returns the presentation mode for a given GI_* item-get id. C callers
// use the return value to branch the item-get sequence: VANILLA → run
// the legacy freeze cutscene; NOTIFICATION_ONLY → route through the
// silent-give path (Path 1: force skipItemCutscene=true so vanilla
// runs func_8083E4C4 + emit Anchor_EmitItemGetToast after; Path 2:
// inline chest-open visuals + Item_Give + Anchor_EmitItemGetToast).
int Anchor_GetItemPresentationMode(int16_t getItemId);

// Pillar G.ii — emit a corner Notification toast for an item-get that
// was routed through the silent (non-cutscene) path. NO-OP in single-
// player so vanilla FastDrops users don't suddenly see toasts. NO
// inventory write — caller is responsible (either via the existing
// func_8083E4C4 silent-give path in Path 1, or via inline Item_Give
// in Path 2 chest-open path).
//
// `getItemId` reserved for future per-id branching (unused today).
// `itemId` is the ITEM_* inventory id used for the icon + name lookup.
void Anchor_EmitItemGetToast(int16_t getItemId, uint8_t itemId);

// Pillar G.ii — composite predicate: Anchor is connected AND the
// Non-Blocking Item Pickups toggle is on (host-authoritative read).
// True means the user-visible vanilla FastChests setting is
// effectively overridden for non-iconic chests (Path 2 inline gate
// forces fast box-kick regardless). UI surfaces consult this to
// disable widgets that would be misleading.
bool Anchor_IsPillarGiiActive(void);

#ifdef __cplusplus
}
#endif

#endif  // ANCHOR_GAME_TIME_CONTROLLER_BRIDGE_H
