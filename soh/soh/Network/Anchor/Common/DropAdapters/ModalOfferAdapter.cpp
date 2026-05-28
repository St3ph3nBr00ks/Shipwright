#include "ModalOfferAdapter.h"
#include "DropAdapterRegistry.h"
#include "../SyncedClaimableDrop.h"
#include "../SceneAuthority.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <chrono>
#include <memory>

extern "C" {
#include "variables.h"  // gSaveContext
#include "functions.h"  // Actor_OfferGetItemNearby
#include "z64.h"
extern PlayState* gPlayState;
}

namespace SyncedClaimableDrop {

ModalOfferAdapter* ModalOfferAdapter::GetInstance() {
    static ModalOfferAdapter* instance = []() -> ModalOfferAdapter* {
        auto adapter = std::make_unique<ModalOfferAdapter>();
        ModalOfferAdapter* raw = adapter.get();
        DropAdapterRegistry::Instance().Register(std::move(adapter));
        return raw;
    }();
    return instance;
}

int16_t ModalOfferAdapter::GetItemTypeForGI(int32_t getItemId) {
    switch (getItemId) {
        // Sticks / nuts (Karebaba, Dekubaba stem, Mad Scrub flower).
        case GI_STICKS_1:      return ITEM00_STICK;
        case GI_STICKS_5:      return ITEM00_STICK;
        case GI_STICKS_10:     return ITEM00_STICK;
        case GI_NUTS_5:        return ITEM00_NUTS;
        case GI_NUTS_10:       return ITEM00_NUTS;
        case GI_NUTS_5_2:      return ITEM00_NUTS;
        // Hearts.
        case GI_HEART:         return ITEM00_HEART;
        // Rupees.
        case GI_RUPEE_GREEN:   return ITEM00_RUPEE_GREEN;
        case GI_RUPEE_BLUE:    return ITEM00_RUPEE_BLUE;
        case GI_RUPEE_RED:     return ITEM00_RUPEE_RED;
        case GI_RUPEE_PURPLE:  return ITEM00_RUPEE_PURPLE;
        case GI_RUPEE_GOLD:    return ITEM00_RUPEE_ORANGE;
        // Magic.
        case GI_MAGIC_SMALL:   return ITEM00_MAGIC_SMALL;
        case GI_MAGIC_LARGE:   return ITEM00_MAGIC_LARGE;
        // Ammo.
        case GI_BOMBS_5:       return ITEM00_BOMBS_A;
        case GI_BOMBS_10:      return ITEM00_BOMBS_A;
        case GI_BOMBS_20:      return ITEM00_BOMBS_A;
        case GI_BOMBS_30:      return ITEM00_BOMBS_A;
        case GI_ARROWS_SMALL:  return ITEM00_ARROWS_SMALL;
        case GI_ARROWS_MEDIUM: return ITEM00_ARROWS_MEDIUM;
        case GI_ARROWS_LARGE:  return ITEM00_ARROWS_LARGE;
        case GI_SEEDS_5:       return ITEM00_SEEDS;
        case GI_SEEDS_30:      return ITEM00_SEEDS;
        case GI_BOMBCHUS_10:   return ITEM00_BOMBCHU;
        case GI_BOMBCHUS_20:   return ITEM00_BOMBCHU;
        default:
            return 0;
    }
}

void ModalOfferAdapter::OfferGetItemNearby(Actor* offerer, PlayState* play, int32_t getItemId) {
    if (offerer == nullptr || play == nullptr) return;

    // Disconnected: vanilla behaviour, no abstraction.
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        Actor_OfferGetItemNearby(offerer, play, getItemId);
        return;
    }

    // Phase 3 C-hybrid (item_drop_behavior_spec.md §1 Q1): connected
    // sessions SUPPRESS the vanilla offer on BOTH host and peer. The
    // actual stick pickup is handled by an EN_ITEM00 STICK that the
    // offering actor's setup function spawns at the head's landing
    // position via Item_DropCollectible. That EN_ITEM00 goes through
    // the standard ground-drop pickup pipeline (Layer 1 / race A /
    // ITEM_COLLECTED), which both clients can drive — no
    // modal-offer-specific arbitration needed.
    //
    // The decorative offering actor (Dekubaba head in DeadStickDrop,
    // Karebaba in DeadItemDrop) remains visible for the vanilla
    // "head with stick on the ground" look until either someone
    // picks up the EN_ITEM00 STICK (dismissed via
    // ITEM_COLLECTED.associatedActorNetId — see Phase 3.4) or the
    // actor's existing 200-frame timer expires naturally.
    //
    // Earlier design (host runs vanilla offer + allocates Drop +
    // broadcasts MODAL_OFFER_CLAIMED on host's pickup) is fully
    // superseded by this approach. The MODAL_OFFER_CLAIMED packet
    // and the host-side modal-phantom-detection branch in
    // HookHandlers.cpp become dead code paths under this design.
    return;
}

}  // namespace SyncedClaimableDrop

// extern "C" bridge — used by C decomp actor files via forward decl.
extern "C" void Anchor_OfferGetItemNearby(Actor* offerer, PlayState* play, int32_t getItemId) {
    SyncedClaimableDrop::ModalOfferAdapter::GetInstance()->OfferGetItemNearby(offerer, play, getItemId);
}

// Phase 3 C-hybrid helper for SetupDeadStickDrop / SetupDeadItemDrop
// in z_en_dekubaba.c / z_en_karebaba.c.
//
// WHEN CONNECTED:
//   Spawn an EN_ITEM00 STICK at the offering actor's landing
//   position. This is the interactive pickup collider AND the
//   visible stick for the player to walk to.
//
// WHEN DISCONNECTED (solo):
//   Helper is a no-op. Vanilla Anchor_OfferGetItemNearby in the
//   DeadStickDrop / DeadItemDrop actionFunc remains unsuppressed
//   and handles pickup. The vanilla decorative head stays visible
//   for the full 200-frame timer (or until vanilla Actor_HasParent
//   triggers Actor_Kill on pickup).
//
// Position note (log 297 regression fix): the earlier
// invisible-decorative design relied on the Dekubaba head's
// gDekuBabaStickDropDL render at the actor's world.pos for the
// visible cue. But SetupDeadStickDrop sets shape.rot.x -= 0x4000
// + shape.yOffset = 1000 + scale = 0.03, then the head is drawn
// with Matrix_Translate(0, 0, 200, APPLY) on top — the visible
// head ends up ~30+ world units offset from world.pos. The
// invisible EN_ITEM00 collider stayed at world.pos so the player
// could see the head but never reach the 30u xzDist pickup
// radius. Making the EN_ITEM00 visible (no draw=NULL) puts the
// pickable visual at the same XZ as the collider; the Dekubaba
// head decoration remains but is now redundant rather than
// load-bearing for pickup.
extern "C" void Anchor_SpawnSyncedStickDrop(Actor* offerer, PlayState* play) {
    if (offerer == nullptr || play == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    (void)Item_DropCollectible(play, &offerer->world.pos, ITEM00_STICK);
}
