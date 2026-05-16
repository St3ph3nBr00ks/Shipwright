/**
 * Invader — implementation (v1 step 15a scaffold).
 *
 * Anchor_TickInvaderActor is a no-op for v1 (combat AI blocked on
 * #208). The draw-context flag (sCurrentlyDrawingInvader) is the
 * one piece of real logic — used by the VB_APPLY_TUNIC_COLOR hook
 * to apply hostile-black tint instead of inheriting the previous
 * draw's env color.
 *
 * Future combat AI lives here, mirroring AIFollowerNPC/FollowerNPC.cpp's
 * structure (state dispatch, G-guards, recovery harness). See plan §0.5
 * for the "clone, modify, then extract once the second consumer is
 * functional" sequencing.
 */

#include "Invader.h"

#include <libultraship/libultraship.h>

namespace {
// Set during EnInvader_Draw's Player_DrawImpl call; cleared after.
// Read by the VB_APPLY_TUNIC_COLOR hook to know which actor's tunic
// is being rendered, so it can apply the black-tint override. File-
// scope static rather than class member because actor draws don't
// nest — the gfx context is single-threaded and each draw completes
// before the next starts.
static Actor* sCurrentlyDrawingInvader = nullptr;
}  // namespace

extern "C" void Anchor_TickInvaderActor(Actor* invader, PlayState* play) {
    // v1: no-op. Combat AI state machine (post-#208) populates this.
    (void)invader;
    (void)play;
}

extern "C" void Anchor_InvaderDrawBegin(Actor* invader) {
    sCurrentlyDrawingInvader = invader;
}

extern "C" void Anchor_InvaderDrawEnd(void) {
    sCurrentlyDrawingInvader = nullptr;
}

extern "C" Actor* Anchor_GetCurrentlyDrawingInvader(void) {
    return sCurrentlyDrawingInvader;
}
