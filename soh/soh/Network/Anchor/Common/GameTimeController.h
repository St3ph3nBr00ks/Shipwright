#pragma once

// Multiplayer-aware time-control gate (Pillar G).
//
// Single gate function that owns the "does world time advance this frame"
// decision. Routes 7 candidate call sites through it.
//
// Pillar G.i (LANDED): PauseMenu returns true when multiplayer is active
// (so the local pause menu does not freeze the world). All other contexts
// still return the legacy single-player answer; §4.G.ii (text-box / ocarina /
// cutscene rules in PvP) is future work tracked in
// Claude/Plans/pillar_g_time_control.md.
//
// C call sites use the C bridge in GameTimeControllerBridge.h.

namespace GameTimeController {

enum class TimeContext {
    PauseMenu,
    TextBox,
    ItemGet,
    Cutscene,
    Ocarina,
    SceneTransition,
};

// Returns true = advance world time; false = hold world frozen.
// Phase 1 stub: returns legacy answer per context.
bool ShouldAdvanceWorldTime(TimeContext context);

}  // namespace GameTimeController
