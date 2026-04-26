#pragma once

// Multiplayer-aware time-control gate (Pillar G).
//
// Single gate function that owns the "does world time advance this frame"
// decision. Routes 7 existing call sites through it.
//
// Phase 1 STUB: returns the legacy answer for every context, preserving
// behaviour. Pillar G.i implementation
// (Claude/Plans/pillar_g_time_control.md) flips PauseMenu to return true
// in multiplayer; subsequent §4.G.ii work fills in cutscene/text-box/ocarina rules.

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
