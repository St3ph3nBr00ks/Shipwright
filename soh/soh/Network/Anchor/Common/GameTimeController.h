#pragma once

#include <cstdint>  // int16_t for GetItemPresentationMode signature

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
    GameOver,         // Pillar G.iii (#239) — death cycle on the dying client.
};

// Returns true = advance world time; false = hold world frozen.
// Phase 1 stub: returns legacy answer per context.
bool ShouldAdvanceWorldTime(TimeContext context);

// Pillar G.ii — item-get presentation policy.
//
// How the standard item-get sequence (z_player.c func_8083A434) should
// be presented to the player. Each item can be routed independently
// via GetItemPresentationMode(getItemId). New modes can be added
// without changing the gate function signature or the call site shape.
enum class ItemPresentationMode {
    // Full vanilla freeze + camera pan + Link's hands-up pose + textbox.
    // Default in single-player AND for allowlisted "iconic" items in
    // multiplayer. Use for items whose narrative moment justifies the
    // gameplay interruption.
    Vanilla = 0,

    // No freeze, no animation, no camera pan. Inventory write happens
    // immediately. A Notification toast (icon + item name + brief
    // description) appears in the corner for ~6s and fades. Player
    // retains full control throughout. Default in multiplayer for
    // non-iconic items.
    NotificationOnly = 1,

    // RESERVED for future modes — examples:
    //   AnimNoFreeze     = 2,  // Link plays pickup anim, world keeps ticking
    //   SilentGive       = 3,  // no UI at all (e.g. randomizer junk-flood)
    //   ToastWithAnim    = 4,  // notification + brief Link gesture, no camera
    // Add the matching case to the call-site switch in z_player.c
    // (func_8083A434) when adding a new mode.
};

// Returns the presentation mode for this item-get. Decision logic
// today: master CVar off → Vanilla; multiplayer inactive → Vanilla;
// item in iconic allowlist → Vanilla; everything else →
// NotificationOnly. Centralised here so future randomizer / per-context
// policy changes touch one function.
ItemPresentationMode GetItemPresentationMode(int16_t getItemId);

// Master kill switch for the non-blocking item-get path. When false,
// the gate ALWAYS returns Vanilla regardless of item id or MP state.
// Lets the user revert to legacy behavior at runtime if a sync
// regression surfaces during field test.
//
// CVar: gEnhancements.Anchor.NonBlockingItemGet (default 1).
bool IsNonBlockingItemGetEnabled();

// Master kill switch for the non-blocking text-box path. When true (the
// default in MP), ShouldAdvanceWorldTime(TextBox) returns true even
// while an NPC dialog is open — the day/night clock keeps advancing on
// the reading client so peers don't see the world freeze whenever
// someone talks to an NPC. Sibling to IsNonBlockingItemGetEnabled();
// same host-authoritative read pattern via AnchorCVarSync.
//
// CVar: gEnhancements.Anchor.NonBlockingTextBox (default 1).
bool IsNonBlockingTextBoxEnabled();

}  // namespace GameTimeController
