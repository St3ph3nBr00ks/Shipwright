#ifndef ANCHOR_PAUSE_LINK_BUFFER_H
#define ANCHOR_PAUSE_LINK_BUFFER_H

// #182 Phase 2.5 follow-up: separate object-bank buffer for the pause-menu
// rotating-Link preview when the live-world rendering feature is on.
//
// Vanilla pause-Link DMAs gameplay_keep (~14 KB) and the player's Link
// object (~70-150 KB) into `play->objectCtx.spaceStart + 0x30`, which IS
// the world's object bank. With the world frozen during vanilla pause
// (mode-3 backdrop short-circuit), this overwrite is harmless. Live-world
// rendering keeps the world drawing every frame, so the world's object
// bank must stay intact and the pause-Link DMA destination must be a
// separate buffer.
//
// Returns a 64-byte-aligned pointer suitable for use as the `segment`
// argument of `func_80091738`. The buffer is large enough for the worst-
// case gameplay_keep + Link object combined.

#ifdef __cplusplus
#include <cstdbool>
extern "C" {
#else
#include <stdbool.h>
#endif

void* Anchor_GetPauseLinkBuffer(void);

// Pause-Link draw context flag. Set by KaleidoScope_DrawPlayerWork
// around its Player_DrawPause call. Read by the VB_APPLY_TUNIC_COLOR
// hook (HookHandlers.cpp) so it can apply the local own-color CVar
// regardless of the `data = &playerSwordAndShield` stack pointer
// that doesn't match `myPlayer` or any `client.player`. Without this,
// the hook falls through with no override and the GPU env color from
// the previous DummyPlayer draw (the last remote player) leaks onto
// the local pause-Link.
void Anchor_PauseLinkDrawBegin(void);
void Anchor_PauseLinkDrawEnd(void);
bool Anchor_IsDrawingPauseLink(void);

#ifdef __cplusplus
}
#endif

#endif  // ANCHOR_PAUSE_LINK_BUFFER_H
