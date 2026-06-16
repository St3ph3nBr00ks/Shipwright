#pragma once

#include <libultraship/libultraship.h>

// Shared forward-only bidirectional time-of-day reconciliation. Used by
// both HandlePacket_UpdateClientState (existing scene-change-driven sync)
// and HandlePacket_TimeSync (issue #63 — periodic + edge-triggered sync).
//
// Single source of truth for the reconcile rule.
//
// SEMANTIC MODEL: vanilla dayTime is a circular u16 clock (z_kankyo.c
// :957-959 natural overflow at 0xFFFF -> 0x0000; nightFlag thresholds
// at z_kankyo.c:974-978 span the wrap boundary). "Ahead" means "forward
// on the circular clock by < half a day" — NOT "numerically greater".
//
// Implemented via MODULAR FORWARD DISTANCE: received is ahead iff
// (received - local) & 0xFFFF is in (0, 0x8000). This treats the
// wraparound correctly:
//
//   local = 0xFF80 (pre-wrap, late evening), received = 0x0064 (post-
//   wrap, early morning). Linear comparison 0x0064 > 0xFF80 = false
//   would wrongly suppress the apply. Modular: (0x0064 - 0xFF80) &
//   0xFFFF = 0x00E4 (228 forward) < 0x8000 -> apply. Correct.
//
//   local = 0x1700 (post-wrap, early morning), received = 0xFE3C
//   (pre-wrap, late evening, e.g. from a peer frozen in Lon Lon
//   Ranch). Linear 0xFE3C > 0x1700 = true would yank local backwards
//   ~15 hours. Modular: (0xFE3C - 0x1700) & 0xFFFF = 0xE73C (~59196
//   forward) > 0x8000 -> don't apply. Correct.
//
// See Claude/Analysis/time_sync_wraparound_bug_analysis_2026-06-16.md
// for the full why-chain (log 535 field-test revealed the linear-
// comparison bug — P1 in Hyrule Field wrapped, P2 frozen in Lon Lon
// Ranch kept yanking P1 back; morning never arrived for ~3 minutes).
//
// On apply, also writes gSaveContext.skyboxTime so the visual sky
// snaps to the synced time without the 1-2 frame catch-up lag through
// vanilla z_kankyo.c:967-969 (which only fires when dayTime > skyboxTime,
// so an equal write is a next-frame no-op).

namespace AnchorTimeOfDay {

// Forward-only bidirectional reconciliation. Returns true iff the apply
// happened (i.e., received time was meaningfully ahead of local).
//
// On apply, writes BOTH gSaveContext.dayTime AND gSaveContext.skyboxTime
// to receivedDayTime — eliminates the 1-frame visual lag where the skybox
// would otherwise take vanilla's catch-up cycle (z_kankyo.c:966-970) to
// match the new dayTime. Without this, a sun-to-midnight sync would briefly
// show the sun hanging in the daytime position before fading to night.
//
// logTag identifies the call site (e.g. "UpdateClientState", "TimeSync
// periodic", "TimeSync cs_start") for SPDLOG diagnostics. Pass nullptr or
// an empty string to suppress the log line.
bool ApplyIfAhead(u16 receivedDayTime, s32 receivedNightFlag,
                  const char* logTag);

}  // namespace AnchorTimeOfDay
