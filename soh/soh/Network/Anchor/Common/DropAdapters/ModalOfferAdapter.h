#pragma once

// ModalOfferAdapter — second concrete DropAdapter (Plan B step 4).
//
// Covers vanilla mechanism #4: Actor_OfferGetItemNearby (the "press A
// to receive" modal offer). Used by Karebaba's stick offer, Dekubaba's
// stem stick offer, and any NPC that offers an item via the modal
// mechanism.
//
// Visual representation: the OFFERING ACTOR itself (the Karebaba /
// Dekubaba / NPC), not an EN_ITEM00. The actor is alive in its
// "DeadItemDrop" / "DeadStickDrop" / equivalent state, visually
// displaying the offered item. When the modal completes (host's
// player accepts), vanilla transitions the actor through its kill
// state (SetupDead etc.) and the actor's death sync propagates to
// peers via the existing ENEMY_DEFEATED pipeline.
//
// Authority model: HOST-ONLY MODAL.
//   - Disconnected: call vanilla; behaviour unchanged from
//     single-player.
//   - Host: call vanilla. Allocate a Drop in the registry (no
//     broadcast yet — step 4 keeps the registry host-local for
//     modal-offer drops; a future step adds the wire field
//     `offererEnemyNetId` so peer can register its mirror actor).
//   - Peer: SUPPRESS vanilla offer entirely. Peer's offering actor
//     sits in its modal-offering state until existing actor-death
//     sync (ENEMY_DEFEATED → SetupDyingNet for Karebaba, similar
//     for Dekubaba) kills it.
//
// Why host-only modal: vanilla Actor_OfferGetItemNearby is a
// per-player mechanism. Without suppression, both clients run their
// own modal locally and each credit themselves (and each cross-credit
// the OTHER via the existing GIVE_ITEM cross-broadcast), producing
// 4 items per kill instead of the intended 2 (cooperative) or 1
// (competitive). Field log 2026-05-25 (#193) documented this case.
//
// DismissVisualRep: base class default (Actor_Kill) for now. The
// offering actor's natural death-state transition (SetupDead etc.)
// may be preferable to direct Actor_Kill — it preserves death anims
// and regrowth timers — but per-actor knowledge is needed to know
// which field/transition to drive. Step 4 accepts the default;
// polish step can add per-actor dismiss callbacks if visual jank
// surfaces.
//
// Plan: Claude/Plans/synced_claimable_drop_design.md §5.4, §10.2.

#include "DropAdapter.h"

namespace SyncedClaimableDrop {

class ModalOfferAdapter : public DropAdapter {
public:
    static ModalOfferAdapter* GetInstance();

    const char* Name() const override { return "ModalOffer"; }
    // DismissVisualRep: base class default (Actor_Kill) for v1.

    // The shared entry point — called from the extern "C" wrapper
    // `Anchor_OfferGetItemNearby` (which the actor .c files invoke
    // in place of vanilla Actor_OfferGetItemNearby). Decides the
    // host/peer/disconnected behaviour.
    void OfferGetItemNearby(Actor* offerer, PlayState* play, int32_t getItemId);

    // Map vanilla GI_* (GetItem ID, passed to Actor_OfferGetItemNearby)
    // to the corresponding ITEM00_* drop type. Used when populating
    // the Drop record (itemType field) so downstream consumers see a
    // consistent type code regardless of whether the drop came in via
    // ground-drop or modal-offer.
    //
    // Returns 0 if the GI_* is not in the mapping table. Not all
    // GI_* values map to ITEM00_*; modal offers for progression items
    // (sword, ocarina, etc.) return 0 and the caller can choose to
    // skip Drop allocation if desired.
    static int16_t GetItemTypeForGI(int32_t getItemId);
};

}  // namespace SyncedClaimableDrop

// C-callable wrapper. Each actor's .c file forward-declares this and
// calls it in place of vanilla `Actor_OfferGetItemNearby`. The wrapper
// dispatches to ModalOfferAdapter::GetInstance()->OfferGetItemNearby().
#ifdef __cplusplus
extern "C" {
#endif
void Anchor_OfferGetItemNearby(struct Actor* offerer, struct PlayState* play, int32_t getItemId);
#ifdef __cplusplus
}
#endif
