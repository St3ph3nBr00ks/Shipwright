#include "DropAdapterRegistry.h"
#include "DropAdapter.h"

namespace SyncedClaimableDrop {

DropAdapterRegistry& DropAdapterRegistry::Instance() {
    static DropAdapterRegistry instance;
    return instance;
}

DropAdapter* DropAdapterRegistry::Register(std::unique_ptr<DropAdapter> adapter) {
    if (adapter == nullptr) {
        SPDLOG_WARN("[SyncedClaimableDrop] Register: null adapter rejected");
        return nullptr;
    }
    DropAdapter* raw = adapter.get();
    SPDLOG_INFO("[SyncedClaimableDrop] Registered DropAdapter '{}'", adapter->Name());
    adapters_.push_back(std::move(adapter));
    return raw;
}

}  // namespace SyncedClaimableDrop
