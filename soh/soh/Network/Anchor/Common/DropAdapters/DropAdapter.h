#pragma once

// DropAdapter — virtual interface for per-mechanism drop handling.
//
// One subclass per vanilla drop mechanism (Item_DropCollectible,
// Actor_OfferGetItemNearby, func_8083E4C4 modal phantom, etc.). Each
// adapter owns the mechanism-specific spawn entry point AND the
// dismissal behaviour for visual representations of drops it owns.
//
// The spawn entry point is mechanism-specific (different signatures
// per mechanism) and is NOT part of the virtual interface — callers
// invoke `GroundDropAdapter::Drop(...)` etc. directly. The virtual
// interface is the small set of operations that the
// SyncedClaimableDrop registry needs to perform uniformly across
// adapters (currently just visual-rep dismissal).
//
// Plan: Claude/Plans/synced_claimable_drop_design.md §5.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"  // Actor
}

namespace SyncedClaimableDrop {

struct Drop;  // forward decl from SyncedClaimableDrop.h

class DropAdapter {
public:
    virtual ~DropAdapter() = default;

    // Diagnostic name for logs and debug UI. Stable identifier — must
    // match across host and peer clients so log correlation works.
    // Example values: "GroundDrop", "ModalOffer", "ModalPhantom".
    virtual const char* Name() const = 0;

    // Dismiss a visual representation actor. Called by the registry
    // when a drop transitions to Resolved (for non-winner clients) or
    // when the drop is otherwise terminated (timer expire, scene
    // exit). Default implementation calls Actor_Kill — sufficient for
    // most mechanisms whose visual rep is a self-contained EN_ITEM00.
    // Override when a mechanism needs a more nuanced teardown (e.g. a
    // modal-offering actor that should advance to its own kill state
    // via the state machine rather than be force-killed mid-animation,
    // to play the death animation cleanly).
    //
    // MUST be idempotent. The actor may already be dead (`update ==
    // nullptr`), partially torn down, or scrubbed from the visualReps
    // vector by OnActorDestroy between the resolve broadcast arriving
    // and this fire. Implementations should null-check + status-check
    // the actor before acting.
    virtual void DismissVisualRep(Drop& drop, Actor* actor);
};

}  // namespace SyncedClaimableDrop
