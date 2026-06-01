#pragma once

// Refactor B.6 — TransientGate RAII helpers for Begin/End shim pairs.
//
// The Anchor bridge layer exposes six `extern "C"` Begin/End shim
// pairs (ItemDrop, ItemDropForKiller, ItemDropLocalOnly,
// HostingPeerEnvActorDrop, NetworkItemDropSpawn,
// NetworkEnvActorDestroy). C-code callers (z_en_item00.c,
// z_shot_sun.c) use them as the public API; the shims themselves
// own the flag/depth-counter semantics + any deferred cleanup
// (broadcast drain at depth==0, etc.).
//
// This header provides RAII wrappers for C++ call sites only. Each
// scoped guard constructs by calling the matching Begin and
// destructs by calling End — guaranteeing End fires on early
// return, exception unwinding, or break/continue out of nested
// scopes. The extern "C" shims remain in place; this is purely a
// C++-side ergonomic improvement.
//
// Per Plans/B.6_design_review.md (DR-3):
//   - 3 simple-bool pairs: ItemDropLocalOnly, HostingPeerEnvActorDrop,
//     NetworkEnvActorDestroy
//   - 3 depth-counter pairs: ItemDrop, ItemDropForKiller (shares End
//     with ItemDrop), NetworkItemDropSpawn
//
// Why wrap the shims rather than the underlying flags directly:
// the depth-counter pairs do non-trivial work at depth==0 (drain
// `g_pendingItemDropBroadcasts`, clear killer/spawnTime state).
// Wrapping the shims keeps that logic in one place. The simple-bool
// pairs could in principle bind the flag directly, but using the
// same shim-wrapping pattern across all six keeps the API uniform.

#include <cstdint>
#include <string>

#include <libultraship/libultraship.h>  // pre-load C++ bridge headers
                                        // (mirror of ActorSyncHelpers.h pattern)

extern "C" {
#include "z64.h"  // Actor*
}

// Forward-declare the extern "C" shims so callers only need to
// `#include "TransientGate.h"` to get the RAII wrappers.
extern "C" void Anchor_BeginItemDrop(Actor* fromActor);
extern "C" void Anchor_EndItemDrop(void);
extern "C" void Anchor_BeginItemDropForKiller(uint32_t killerClientId);
extern "C" void Anchor_BeginItemDropLocalOnly(void);
extern "C" void Anchor_EndItemDropLocalOnly(void);
extern "C" void Anchor_BeginHostingPeerEnvActorDrop(void);
extern "C" void Anchor_EndHostingPeerEnvActorDrop(void);
extern "C" void Anchor_BeginNetworkEnvActorDestroy(void);
extern "C" void Anchor_EndNetworkEnvActorDestroy(void);

// C++-linkage receive helpers (declared in Bridge/ItemDropBridgeState.h
// but redeclared here so the RAII wrapper compiles without that
// header).
void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                      int64_t spawnTimeMs,
                                      const std::string& killerTeamId);
void Anchor_EndNetworkItemDropSpawn(void);

namespace AnchorBridge {

// ----------------------------------------------------------------------------
// Simple-bool guards (no depth counter; Begin/End just toggle a flag).
// ----------------------------------------------------------------------------

class ScopedItemDropLocalOnly {
public:
    ScopedItemDropLocalOnly()  { Anchor_BeginItemDropLocalOnly(); }
    ~ScopedItemDropLocalOnly() { Anchor_EndItemDropLocalOnly(); }
    ScopedItemDropLocalOnly(const ScopedItemDropLocalOnly&)            = delete;
    ScopedItemDropLocalOnly& operator=(const ScopedItemDropLocalOnly&) = delete;
};

class ScopedHostingPeerEnvActorDrop {
public:
    ScopedHostingPeerEnvActorDrop()  { Anchor_BeginHostingPeerEnvActorDrop(); }
    ~ScopedHostingPeerEnvActorDrop() { Anchor_EndHostingPeerEnvActorDrop(); }
    ScopedHostingPeerEnvActorDrop(const ScopedHostingPeerEnvActorDrop&)            = delete;
    ScopedHostingPeerEnvActorDrop& operator=(const ScopedHostingPeerEnvActorDrop&) = delete;
};

class ScopedNetworkEnvActorDestroy {
public:
    ScopedNetworkEnvActorDestroy()  { Anchor_BeginNetworkEnvActorDestroy(); }
    ~ScopedNetworkEnvActorDestroy() { Anchor_EndNetworkEnvActorDestroy(); }
    ScopedNetworkEnvActorDestroy(const ScopedNetworkEnvActorDestroy&)            = delete;
    ScopedNetworkEnvActorDestroy& operator=(const ScopedNetworkEnvActorDestroy&) = delete;
};

// ----------------------------------------------------------------------------
// Depth-counter guards. Begin increments g_pendingItemDropDepth and
// records state on the OUTERMOST call; End decrements and drains
// state (deferred broadcast queue, killer/spawnTime clear) at
// depth==0. Nesting is real — Item_DropCollectibleRandom's inner
// loop calls Item_DropCollectible recursively (z_en_item00.c:
// 1797/1799), and the EnvActorDrop handler nests
// HostingPeerEnvActorDrop inside ItemDropForKiller.
// ----------------------------------------------------------------------------

class ScopedItemDrop {
public:
    explicit ScopedItemDrop(Actor* fromActor = nullptr) {
        Anchor_BeginItemDrop(fromActor);
    }
    ~ScopedItemDrop() { Anchor_EndItemDrop(); }
    ScopedItemDrop(const ScopedItemDrop&)            = delete;
    ScopedItemDrop& operator=(const ScopedItemDrop&) = delete;
};

// Shares End with ScopedItemDrop (intentional — see ItemDropBridge.cpp
// comment block above Anchor_BeginItemDropForKiller).
class ScopedItemDropForKiller {
public:
    explicit ScopedItemDropForKiller(uint32_t killerClientId) {
        Anchor_BeginItemDropForKiller(killerClientId);
    }
    ~ScopedItemDropForKiller() { Anchor_EndItemDrop(); }
    ScopedItemDropForKiller(const ScopedItemDropForKiller&)            = delete;
    ScopedItemDropForKiller& operator=(const ScopedItemDropForKiller&) = delete;
};

class ScopedNetworkItemDropSpawn {
public:
    ScopedNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                int64_t spawnTimeMs,
                                const std::string& killerTeamId) {
        Anchor_BeginNetworkItemDropSpawn(netId, killerClientId, spawnTimeMs, killerTeamId);
    }
    ~ScopedNetworkItemDropSpawn() { Anchor_EndNetworkItemDropSpawn(); }
    ScopedNetworkItemDropSpawn(const ScopedNetworkItemDropSpawn&)            = delete;
    ScopedNetworkItemDropSpawn& operator=(const ScopedNetworkItemDropSpawn&) = delete;
};

}  // namespace AnchorBridge
