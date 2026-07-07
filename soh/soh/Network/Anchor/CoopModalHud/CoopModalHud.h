#ifndef ANCHOR_COOP_MODAL_HUD_H
#define ANCHOR_COOP_MODAL_HUD_H
#ifdef __cplusplus

#include <libultraship/libultraship.h>

// Co-op Modal HUD — Phase 1 (voting-skip only).
//
// Renders a small ImGui overlay above the vanilla textbox area when
// a cutscene-internal textbox voting-skip session is active. Reads
// state directly from `Anchor::Instance->cutsceneTextAdvanceState`;
// zero new API on the Anchor side. Mirrors the `Notification::Window`
// registration pattern — instantiated + registered from
// `SohGui::Create()`.
//
// Design + rationale: Claude/Plans/coop_modal_hud_plan.md.
// Widget spec (dot row + numeric countdown text): plan §5.1.
//
// Phase 2+ scenarios (choice-lock banner, item-get awaiting-peer,
// cutscene-skip vote) plug into the same Draw() dispatch by adding
// `else if` branches per plan §5.3.

namespace CoopModalHud {

class Window final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override {}
    void DrawElement() override {}
    void Draw() override;
    void UpdateElement() override {}
};

}  // namespace CoopModalHud

#endif  // __cplusplus
#endif  // ANCHOR_COOP_MODAL_HUD_H
