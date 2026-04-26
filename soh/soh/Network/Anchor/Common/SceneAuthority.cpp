#include "SceneAuthority.h"
#include "soh/Network/Anchor/Anchor.h"

extern "C" {
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace Anchor::SceneAuthority {

uint32_t GetEffectiveHostClientId() {
    // Phase 1 stub: returns the legacy answer (room owner) unconditionally.
    // Phase 1-real (Plans/anchor_host_migration_plan.md) replaces this with
    // the election rule "ownerClientId if online, else lowest online clientId."
    if (!::Anchor::Instance) return 0;
    return ::Anchor::Instance->roomState.ownerClientId;
}

bool IsEffectiveHost() {
    if (!::Anchor::Instance) return false;
    return ::Anchor::Instance->roomState.ownerClientId == ::Anchor::Instance->ownClientId;
}

bool IsSceneHost(int16_t /*sceneNum*/, uint8_t /*timeline*/) {
    // Phase 1 stub: same answer as IsEffectiveHost(). Phase 2 of the migration
    // plan keys this by (sceneNum, timeline) per Pillar A + Pillar B.
    return IsEffectiveHost();
}

bool IsMyCurrentSceneHost() {
    if (!gPlayState) return IsEffectiveHost();
    return IsSceneHost(gPlayState->sceneNum, (uint8_t)gSaveContext.linkAge);
}

}  // namespace Anchor::SceneAuthority
