#include "GroundDropAdapter.h"
#include "DropAdapterRegistry.h"
#include "../SyncedClaimableDrop.h"

#include <memory>

namespace SyncedClaimableDrop {

GroundDropAdapter* GroundDropAdapter::GetInstance() {
    // Lazy-init singleton with self-registration on first access.
    // Function-local static => initialized once per process, thread-
    // safely (C++11 guarantee). The lambda registers with the global
    // DropAdapterRegistry and returns the raw pointer the registry's
    // unique_ptr now owns.
    static GroundDropAdapter* instance = []() -> GroundDropAdapter* {
        auto adapter = std::make_unique<GroundDropAdapter>();
        GroundDropAdapter* raw = adapter.get();
        DropAdapterRegistry::Instance().Register(std::move(adapter));
        return raw;
    }();
    return instance;
}

void GroundDropAdapter::RegisterSpawn(uint32_t dropId,
                                       uint32_t authorityClientId,
                                       int16_t  itemType,
                                       Vec3f    pos,
                                       int16_t  sceneNum,
                                       int8_t   roomNum,
                                       uint8_t  linkAge,
                                       uint32_t killerClientId,
                                       int64_t  spawnTimeMs,
                                       Actor*   actor) {
    if (dropId == 0 || actor == nullptr) {
        return;
    }

    Registry& registry = Registry::Instance();

    // Find-or-allocate. Both host's broadcast site and peer's receive
    // site call this; whichever runs first allocates, the second
    // dedupes. AllocateDrop returns the existing entry on duplicate
    // (logged at DEBUG level).
    Drop* drop = registry.Find(dropId);
    if (drop == nullptr) {
        drop = registry.AllocateDrop(dropId, authorityClientId, itemType, pos,
                                     sceneNum, roomNum, linkAge,
                                     killerClientId, spawnTimeMs);
    }
    if (drop == nullptr) {
        // AllocateDrop only returns null for dropId==0 (already checked).
        // Defensive guard.
        return;
    }

    // Assign adapter on first registration. Don't overwrite — another
    // adapter may have already claimed this drop (shouldn't happen for
    // ground drops, but defensive).
    if (drop->adapter == nullptr) {
        drop->adapter = this;
    }

    registry.RegisterVisualRep(dropId, actor);
}

}  // namespace SyncedClaimableDrop
