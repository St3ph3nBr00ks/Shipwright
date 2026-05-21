/**
 * DirectorDebugWindow — ImGui panel surfacing AI Director state for
 * field-test diagnostics.
 *
 * Visibility CVar: CVAR_WINDOW("AIDirectorDebug"). Toggle via console
 * (`set CVAR_WINDOW("AIDirectorDebug") 1`) or via a menu entry once
 * one is added to SohMenuEnhancements (deferred to avoid clobbering
 * parallel-agent menu work).
 *
 * Subclasses Ship::GuiWindow per the project pattern (matches
 * AnchorRoomWindow). Always-instantiated; rendering is gated by the
 * visibility CVar via Ship's GUI manager.
 *
 * Sections (plan §6):
 *   - Header: global-host status, tick rate, descriptor count.
 *   - Per-descriptor collapsing blocks: id, enabled, live count,
 *     cooldown remaining, descriptor-private debug snapshot line.
 *   - Session view snapshot: per-player rows (clientId, scene/room,
 *     pos, cutscene/invuln/follower flags).
 *   - TestDescriptor controls: CVar toggle + force-spawn button.
 *
 * Read-only from the network's perspective — opening the panel on a
 * non-host client doesn't trigger any host-side work. Non-host
 * clients see whatever state the most recent DIRECTOR_STATE_SYNC
 * carried (may be stale; intentional — panel is primarily a
 * host-side diagnostic).
 */

#pragma once

#include <libultraship/libultraship.h>

namespace AnchorDirector {

class DirectorDebugWindow : public Ship::GuiWindow {
public:
    using GuiWindow::GuiWindow;

    void InitElement()   override {}
    void UpdateElement() override {}
    void DrawElement()   override;

private:
    void DrawHeader();
    void DrawDescriptors();
    void DrawSessionView();
};

}  // namespace AnchorDirector
