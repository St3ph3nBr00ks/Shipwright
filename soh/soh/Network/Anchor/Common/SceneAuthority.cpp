#include "SceneAuthority.h"
#include "soh/Network/Anchor/Anchor.h"

extern "C" {
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace SceneAuthority {

uint32_t GetEffectiveHostClientId() {
    // Pillar A Phase 1 — read the cached value maintained by
    // Anchor::RecomputeEffectiveHost(). The real election rule lives there.
    if (!::Anchor::Instance) return 0;
    return ::Anchor::Instance->effectiveHostClientId;
}

bool IsEffectiveHost() {
    if (!::Anchor::Instance) return false;
    return ::Anchor::Instance->effectiveHostClientId == ::Anchor::Instance->ownClientId;
}

bool IsSceneHost(int16_t /*sceneNum*/, uint8_t /*timeline*/) {
    // Phase 1: same answer as IsEffectiveHost(). Phase 2 keys this by
    // (sceneNum, timeline) per Pillar A + Pillar B migration plan.
    return IsEffectiveHost();
}

bool IsMyCurrentSceneHost() {
    if (!gPlayState) return IsEffectiveHost();
    return IsSceneHost(gPlayState->sceneNum, (uint8_t)gSaveContext.linkAge);
}

}  // namespace SceneAuthority
