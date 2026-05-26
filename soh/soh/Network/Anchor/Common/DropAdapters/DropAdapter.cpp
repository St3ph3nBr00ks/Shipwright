#include "DropAdapter.h"
#include "../SyncedClaimableDrop.h"  // Drop struct

extern "C" {
#include "functions.h"  // Actor_Kill
}

namespace SyncedClaimableDrop {

void DropAdapter::DismissVisualRep(Drop& drop, Actor* actor) {
    (void)drop;
    if (actor == nullptr) return;
    if (actor->update == nullptr) return;  // Already killed; idempotent no-op.
    Actor_Kill(actor);
}

}  // namespace SyncedClaimableDrop
