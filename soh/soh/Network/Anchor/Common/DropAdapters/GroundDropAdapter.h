#pragma once

// GroundDropAdapter — first concrete DropAdapter (Plan B step 3).
//
// Covers vanilla mechanisms:
//   #1 — Item_DropCollectible (bounce-and-rest EN_ITEM00).
//   #2 — Item_DropCollectibleRandom (decomposes into N #1 events).
//   #6 — Direct Actor_Spawn(EN_ITEM00) outside the wrapped paths
//        (En_Hintnuts heart and similar one-off spawns).
//
// Visual representation: the EN_ITEM00 actor itself.
// Dismissal: default Actor_Kill (inherited from DropAdapter base).
//
// Integration:
//   - Host: in OnActorSpawn(ACTOR_EN_ITEM00), after the existing
//     SendPacket_ItemDropSync broadcast, call RegisterSpawn() to
//     populate the SyncedClaimableDrop registry.
//   - Peer: in HandlePacket_ItemDropSync, after the local
//     Item_DropCollectible returns successfully, call RegisterSpawn()
//     with the host-assigned dropId.
//
// In this step, the registry is populated but not yet consumed —
// adapter dismissal is dormant until a subsequent step wires
// ITEM_COLLECTED to transition Drops to Resolved and dispatch
// dismissal through the adapter.
//
// Plan: Claude/Plans/synced_claimable_drop_design.md §5.2.

#include "DropAdapter.h"

namespace SyncedClaimableDrop {

class GroundDropAdapter : public DropAdapter {
public:
    // Lazy singleton — registered with DropAdapterRegistry on first
    // access. Subsequent calls return the same instance. Caller stashes
    // the returned pointer for direct dispatch.
    static GroundDropAdapter* GetInstance();

    const char* Name() const override { return "GroundDrop"; }
    // DismissVisualRep: base class default (Actor_Kill) is sufficient.

    // Called after an EN_ITEM00 has been spawned locally (host or
    // peer) and the ItemDropNetId extension has been stamped. Looks
    // up or allocates the Drop entry, sets its adapter pointer, and
    // registers the actor as a visual rep. Idempotent — safe to call
    // multiple times for the same (dropId, actor) pair.
    void RegisterSpawn(uint32_t dropId,
                       uint32_t authorityClientId,
                       int16_t  itemType,
                       Vec3f    pos,
                       int16_t  sceneNum,
                       int8_t   roomNum,
                       uint8_t  linkAge,
                       uint32_t killerClientId,
                       int64_t  spawnTimeMs,
                       Actor*   actor);
};

}  // namespace SyncedClaimableDrop
