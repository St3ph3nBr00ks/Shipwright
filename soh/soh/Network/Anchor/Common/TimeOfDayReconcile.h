#pragma once

#include <libultraship/libultraship.h>

// Shared forward-only bidirectional time-of-day reconciliation. Used by
// both HandlePacket_UpdateClientState (existing scene-change-driven sync)
// and HandlePacket_TimeSync (issue #63 — periodic + edge-triggered sync).
//
// Single source of truth for the reconcile rule. If the rule needs to
// change (e.g., add wraparound handling), it changes in one place.
//
// See Claude/Analysis/time_of_day_sync_implementation_analysis_2026-06-16.md
// for the full design rationale including:
//   - Why forward-only bidirectional (no host authority)
//   - Why both dayTime AND skyboxTime get written on apply (skybox visual
//     immediacy — Pitfall: vanilla auto-catch-up at z_kankyo.c:967-969 only
//     fires when dayTime > skyboxTime, so setting them equal is safe)
//   - Why wraparound (u16 0xFFFF -> 0x0000) is out of scope (vanilla never
//     wraps in normal play; existing UpdateClientState sync has lived in
//     production without wrap-related reports)

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
