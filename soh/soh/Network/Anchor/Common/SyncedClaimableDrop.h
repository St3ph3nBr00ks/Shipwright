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

// Forward decl — full interface lives in DropAdapters/DropAdapter.h.
// Each Drop is owned by exactly one adapter, which defines how to
// dismiss the drop's visual representations when it resolves.
class DropAdapter;

enum class DropState {
    Available,   // Pickup-eligible.
                 //   Host:  vanilla pickup allowed.
                 //   Peer:  vanilla pickup → send ITEM_PICKUP_REQUEST,
                 //          transition to Claiming.
    Claiming,    // Peer claim in flight; vanilla pickup suppressed
                 // locally until host's grant arrives.
    Resolved,    // Terminal. Inspect `claimerClientId`:
                 //   != 0 → that client won; vanilla pickup runs (or
                 //          ran) on that client.
                 //   == 0 → drop dismissed without pickup (timer
                 //          expired, scene exit, etc.).
                 // Visual reps cleaned up by adapters either way.
                 //
                 // Earlier drafts split this into Claimed / Consumed /
                 // Dismissed; the gap between Claimed and Consumed
                 // (vanilla pickup mid-animation vs done) was
                 // observed by no consumer, and Claimed-vs-Dismissed
                 // collapsed to a single check on claimerClientId.
                 // Consolidated to keep the state set minimal.
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

    // The adapter that owns this drop. Set by the calling site after
    // AllocateDrop returns. Drives dismissal semantics on Resolved
    // transition. Nullable: a drop with no adapter falls back to a
    // direct Actor_Kill on every visual rep (defensive default).
    DropAdapter* adapter = nullptr;
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
