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

    // Peer: SUPPRESS the vanilla offer entirely. The offering actor
    // stays alive in its modal-offering state until existing actor-
    // death sync propagates host's modal completion (ENEMY_DEFEATED
    // for Karebaba/Dekubaba → SetupDyingNet on peer).
    if (!::SceneAuthority::IsMyCurrentRoomHost()) {
        return;
    }

    // Host: vanilla offer runs; also register the offering actor in
    // the Drop registry so future dismissal can route through this
    // adapter. Step 4 keeps the registration host-local — no broadcast
    // — because the existing actor-death sync handles peer-side
    // dismissal for the v1 set of consumers (Karebaba, Dekubaba). A
    // future step extends ITEM_DROP_SYNC with an `offererEnemyNetId`
    // field so peer can register its mirror actor and dismissal routes
    // through this adapter uniformly across mechanisms.
    Actor_OfferGetItemNearby(offerer, play, getItemId);

    // Need an EnemyNetId on the offerer to allocate a unique dropId.
    // Karebaba / Dekubaba are synced enemies, so they should have one
    // by the time the modal offer fires. If absent (unusual actor that
    // calls Actor_OfferGetItemNearby but isn't in the sync allowlist),
    // skip Drop allocation — vanilla path still ran above.
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(offerer);
    if (ext == nullptr || ext->netId == 0) {
        return;
    }

    // Reuse the offerer's EnemyNetId.netId as the dropId. Each Karebaba
    // / Dekubaba has a unique netId, and an actor offers at most one
    // modal item over its lifetime — so the netId is unique across the
    // session for this purpose.
    uint32_t dropId = ext->netId;
    int16_t  itemType = GetItemTypeForGI(getItemId);
    int64_t  nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    Registry& registry = Registry::Instance();
    Drop* drop = registry.Find(dropId);
    if (drop == nullptr) {
        drop = registry.AllocateDrop(
            dropId,
            Anchor::Instance->ownClientId,
            itemType,
            offerer->world.pos,
            (int16_t)gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num,
            (uint8_t)(gSaveContext.linkAge & 0x1),
            /*killerClientId=*/ Anchor::Instance->ownClientId,
            nowMs);
    }
    if (drop == nullptr) return;
    if (drop->adapter == nullptr) drop->adapter = this;

    registry.RegisterVisualRep(dropId, offerer);
}

}  // namespace SyncedClaimableDrop

// extern "C" bridge — used by C decomp actor files via forward decl.
extern "C" void Anchor_OfferGetItemNearby(Actor* offerer, PlayState* play, int32_t getItemId) {
    SyncedClaimableDrop::ModalOfferAdapter::GetInstance()->OfferGetItemNearby(offerer, play, getItemId);
}
