#pragma once

// Synced Claimable Drop — Plan B core abstraction (#193 Plan B).
//
// A `Drop` is a logical drop event in the world, identified by a unique
// `dropId`. A single logical drop may be rendered by multiple actors —
// the ground EN_ITEM00 on each client, the modal-offering actor (e.g.
// Karebaba in DeadItemDrop state), the modal-completion phantom, etc.
// Each renderer is a "visual representation" attached to the drop via
// `Registry::RegisterVisualRep`.
//
// When the drop transitions to Claimed (host arbitration grants claim
// to a winner) or Dismissed (timer / scene exit), all visual reps on
// all clients are dismissed by their adapters.
//
// v1 scope: data structures + registry only. Adapters (one per vanilla
// drop mechanism) land in subsequent commits.
//
// Plan: Claude/Plans/synced_claimable_drop_design.md.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"
#include <cstdint>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"  // Actor*, Vec3f
}

namespace SyncedClaimableDrop {

enum class DropState {
    Available,   // Drop is in the world, pickup-eligible.
    Claiming,    // Local pickup attempt in flight; awaiting host arbitration.
    Claimed,     // Host has granted claim to claimerClientId; Item_Give pending.
    Consumed,    // Item_Give has fired on the winner; pickup complete.
    Dismissed,   // No pickup occurred (timer expired, scene exit, etc.).
};

// One per logical drop event. Replicated across all clients with matching
// `dropId`. Visual reps are local actors; the vector contents differ per
// client even though `dropId` matches.
struct Drop {
    uint32_t dropId           = 0;
    uint32_t authorityClientId = 0;  // Owning client — usually room host at spawn.
    int16_t  itemType         = 0;   // ITEM00_* enum (post-resolution).
    Vec3f    spawnPos         = { 0.0f, 0.0f, 0.0f };
    int16_t  sceneNum         = -1;
    int8_t   roomNum          = -1;
    uint8_t  linkAge          = 0;   // Pillar B timeline bit.

    DropState state           = DropState::Available;
    uint32_t  claimerClientId = 0;   // 0 = unclaimed.
    int64_t   spawnTimeMs     = 0;   // Host's monotonic ms at spawn (killer-window).
    uint32_t  killerClientId  = 0;   // Client whose action caused the drop.

    std::vector<Actor*> visualReps;  // Local actor pointers — populated by adapters.
};

// Per-client registry. Stateful singleton; lifetime matches Anchor's.
//
// Thread-model: single-threaded game thread. No locking.
class Registry {
public:
    static Registry& Instance();

    // Allocate or look up by dropId. If `dropId` is 0 returns nullptr.
    // Returns nullptr if a drop with this id already exists (idempotent
    // network-receive path should call Find() first, then AllocateDrop()
    // only on miss).
    //
    // Caller fills in additional fields as needed after allocation.
    Drop* AllocateDrop(uint32_t dropId, uint32_t authorityClientId,
                       int16_t itemType, Vec3f spawnPos,
                       int16_t sceneNum, int8_t roomNum, uint8_t linkAge,
                       uint32_t killerClientId, int64_t spawnTimeMs);

    // Lookup. Returns nullptr if not found.
    Drop* Find(uint32_t dropId);

    // Visual representation management. Idempotent — duplicate registers
    // are no-ops. UnregisterFromAllDrops scrubs an actor from every
    // drop's vector — call from OnActorDestroy.
    void RegisterVisualRep(uint32_t dropId, Actor* actor);
    void UnregisterFromAllDrops(Actor* actor);

    // State transition. Plain setter for v1; adapters may override the
    // dispatch in later commits (e.g. arm a backstop timer on Claimed).
    void TransitionTo(Drop& drop, DropState newState);

    // Remove a drop entry entirely. After this call, Find(dropId) returns
    // nullptr. The drop's `visualReps` are NOT killed automatically —
    // caller is responsible for any Actor_Kill before removal.
    void RemoveDrop(uint32_t dropId);

    // Clear all drops. Called on Anchor disconnect.
    void Clear();

    // Read-only iteration support (for late-join snapshot generation, etc.).
    const std::unordered_map<uint32_t, Drop>& GetAllDrops() const { return drops_; }
    size_t Size() const { return drops_.size(); }

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    std::unordered_map<uint32_t, Drop> drops_;
};

}  // namespace SyncedClaimableDrop
