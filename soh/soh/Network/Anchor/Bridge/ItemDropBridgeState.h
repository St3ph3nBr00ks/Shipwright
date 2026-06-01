#pragma once

// Refactor A.8 — ItemDrop bridge shared internal state.
//
// File-scope statics relocated from HookHandlers.cpp on 2026-06-01.
// Declared `extern` here so both ItemDropBridge.cpp (shim definitions)
// and HookHandlers.cpp (OnActorSpawn(EN_ITEM00) hook + the
// ItemDropSync receive-side spawning helpers) can read/write
// directly. DR-5 §"Shared dependencies" called out that a pure-
// accessor-function pattern would require ~10+ getter/setter pairs
// for this domain — too much surface. The shared-header pattern
// keeps the contract narrow and grep-friendly: every reference to
// these symbols still uses the original `g_pendingItem*` /
// `g_isSpawning*` names, but ownership of the storage moves to
// ItemDropBridge.cpp.
//
// Internal-to-ItemDrop helpers (the receive-spawn Begin/End used by
// ItemDropSync.cpp + ItemDropSnapshot.cpp) are also declared here
// rather than in a separate Bridge/ItemDropBridge.h — the consumers
// previously forward-declared them inline; this header centralises
// the contract.

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"  // Actor*
}

// PendingItemDropBroadcast — deferred-broadcast record pushed by
// OnActorSpawn(EN_ITEM00) and drained by Anchor_EndItemDrop. See
// the comment block above the declarations in ItemDropBridge.cpp
// for the full nut-trajectory-desync rationale (log 281).
struct PendingItemDropBroadcast {
    Actor*   actor;
    uint32_t itemNetId;
    int16_t  resolvedType;
    uint32_t killerClientId;
    int64_t  spawnTimeMs;
    bool     invisibleDecorative;  // Phase 3 C-hybrid stick-drop tag
};

// Cross-bridge shared state. All definitions live in
// Bridge/ItemDropBridge.cpp; the `extern` declarations here let
// HookHandlers.cpp's hooks reference them without each adding a
// local forward-decl.
extern uint32_t    g_pendingItemDropKillerClientId;
extern std::string g_pendingItemDropKillerTeamId;
extern int64_t     g_pendingItemDropSpawnTimeMs;
extern int         g_pendingItemDropDepth;
extern bool        g_isSpawningNetworkItemDrop;
extern uint32_t    g_pendingNetworkItemDropNetId;
extern bool        g_pendingItemDropInvisibleDecorative;
extern bool        g_isLocalOnlyItemDrop;
extern std::vector<PendingItemDropBroadcast> g_pendingItemDropBroadcasts;

// Receive-side helpers (C++ linkage). Used by HandlePacket_ItemDropSync
// + HandlePacket_ItemDropSnapshot to bracket the local Actor_Spawn so
// the OnActorSpawn(EN_ITEM00) hook stamps the extension with the
// host's broadcast netId/killer/spawnTimeMs and suppresses
// re-broadcast.
void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                       int64_t spawnTimeMs,
                                       const std::string& killerTeamId);
void Anchor_EndNetworkItemDropSpawn(void);
