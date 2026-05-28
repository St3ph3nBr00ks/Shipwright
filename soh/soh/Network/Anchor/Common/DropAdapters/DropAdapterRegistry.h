#pragma once

// DropAdapterRegistry — owns the per-mechanism `DropAdapter` instances.
//
// Inheritance-based registry (one adapter instance per mechanism) with
// explicit `Register()` calls during Anchor init. Same shape as the
// AIDirector descriptor pattern documented as the canonical "per-X
// registry" model in `Claude/session_state.md` Architecture Decisions.
//
// Why explicit registration (not auto-discovery via static-init or
// macros): the registration order is grep-able and re-orderable; an
// adapter can be temporarily disabled during a bisect by commenting
// out one line; no link-order surprises that bite macro-based
// registries.
//
// Plan: Claude/Plans/synced_claimable_drop_design.md §5.1.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"
#include <memory>
#include <vector>

namespace SyncedClaimableDrop {

class DropAdapter;

class DropAdapterRegistry {
public:
    static DropAdapterRegistry& Instance();

    // Register an adapter. Takes ownership. Returns the raw pointer so
    // the caller can stash it for direct dispatch (each mechanism
    // dispatches to its own adapter directly — the registry exists
    // mainly to own the adapters' lifetimes and provide debug
    // enumeration).
    //
    // Typical usage from Anchor init:
    //
    //   auto* groundDrop = DropAdapterRegistry::Instance().Register(
    //       std::make_unique<GroundDropAdapter>());
    //   // groundDrop is the singleton instance for the mechanism;
    //   // mechanism-specific spawn callers reference it directly.
    DropAdapter* Register(std::unique_ptr<DropAdapter> adapter);

    // Read-only iteration (debug UI, log enumeration, smoke tests).
    const std::vector<std::unique_ptr<DropAdapter>>& All() const { return adapters_; }
    size_t Size() const { return adapters_.size(); }

    // Clear all registrations. Called only by tests; production code
    // registers once at Anchor init and never tears down.
    void Clear() { adapters_.clear(); }

private:
    DropAdapterRegistry() = default;
    DropAdapterRegistry(const DropAdapterRegistry&) = delete;
    DropAdapterRegistry& operator=(const DropAdapterRegistry&) = delete;

    std::vector<std::unique_ptr<DropAdapter>> adapters_;
};

}  // namespace SyncedClaimableDrop
