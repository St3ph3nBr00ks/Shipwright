#include "PushableActorState.h"

// Include Anchor.h FIRST so libultraship C++ template headers (Resource.h,
// nlohmann json) are processed with C++ linkage. Their include guards then
// prevent re-processing inside the extern "C" block below when the overlay
// headers transitively reference them (Pitfall 6 — extern "C" wrapping
// templates is a hard error). Pattern mirrors ActorSyncHelpers.cpp.
#include "soh/Network/Anchor/Anchor.h"

extern "C" {
#include "src/overlays/actors/ovl_Obj_Oshihiki/z_obj_oshihiki.h"
#include "src/overlays/actors/ovl_Bg_Spot15_Rrbox/z_bg_spot15_rrbox.h"
}

extern "C" int Anchor_IsActorMidPush(Actor* actor) {
    if (actor == NULL) {
        return 0;
    }
    switch (actor->id) {
        case ACTOR_OBJ_OSHIHIKI: {
            ObjOshihiki* block = (ObjOshihiki*)actor;
            return (block->stateFlags & (PUSHBLOCK_PUSH | PUSHBLOCK_FALL)) ? 1 : 0;
        }
        case ACTOR_BG_SPOT15_RRBOX: {
            // Lon Lon milk crate. No stateFlags enum; mid-push is encoded as:
            //   unk_178 > 0         — active push slide (z_bg_spot15_rrbox.c:267)
            //   actor.gravity < 0   — falling after push past an edge (set to
            //                          -1.0f in func_808B4380, z_bg_spot15_rrbox.c:307)
            // Both fields are zeroed in the Idle action func, so this exactly
            // matches PUSHBLOCK_PUSH | PUSHBLOCK_FALL semantics for OBJ_OSHIHIKI.
            BgSpot15Rrbox* crate = (BgSpot15Rrbox*)actor;
            if (crate->unk_178 > 0.001f) {
                return 1;
            }
            if (crate->dyna.actor.gravity < 0.0f) {
                return 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}
