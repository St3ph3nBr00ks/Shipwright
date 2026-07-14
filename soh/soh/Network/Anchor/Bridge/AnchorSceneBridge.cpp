// AnchorSceneBridge — implementation.
//
// See header for design intent (variant C landing, 2026-07-09).

#include "soh/Network/Anchor/Bridge/AnchorSceneBridge.h"

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

namespace AnchorSceneBridge {

bool IsCurrentRoomFullyLoaded() {
    if (gPlayState == nullptr) return false;
    if (gPlayState->roomCtx.status != 0) return false;
    if (gPlayState->roomCtx.curRoom.segment == nullptr) return false;
    return true;
}

}  // namespace AnchorSceneBridge
