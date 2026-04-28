#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/resource/type/Skeleton.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

static void UpdateCustomSkeleton() {
    if (!GameInteractor::IsSaveLoaded(true) || gPlayState == NULL) {
        return;
    }

    SOH::SkeletonPatcher::UpdateCustomSkeletons();
}

// KB-19 fix b — gate the bake on the SkelAnime that just initialised actually
// belonging to the local player. SoH fires OnLinkSkeletonInit from
// SkelAnime_InitLink, which Anchor's DummyPlayer_Init also drives while
// gSaveContext.linkAge is temporarily swapped to the remote player's age (see
// the swap windows in DummyPlayer.cpp lines 75-95 / 221-225 / 331-390). Without
// this gate, every remote-DummyPlayer skelAnime initialisation re-bakes the
// LOCAL player's Link mid-swap; the bake reads gSaveContext.linkAge in places
// like Skeleton.cpp:2182's face-texture path, producing a wrong-age skeleton
// for our own Link and visible vertex distortion.
//
// Fix b plumbs SkelAnime* through the OnLinkSkeletonInit hook so we can drop
// any call that is not for our own player skelAnime. If a future upstream merge
// finds plumbing the parameter through the hook table too invasive, the
// alternative fix a is: keep the no-arg hook signature, and instead guard
// inside SkeletonPatcher::UpdateTunicSkeletons on
// `gSaveContext.linkAge == skel.<age field>`. That is cheaper to apply but
// couples the cosmetic path to a global that can be transiently wrong; fix b
// is preferred because the hook source already knows the skelAnime identity.
static void UpdateCustomSkeletonsForLinkInit(SkelAnime* skelAnime) {
    if (!SOH::SkeletonPatcher::IsLocalPlayerSkelAnime(skelAnime)) {
        return;
    }
    // KB-19 follow-up #2 — targeted bake of the SkelAnime that just
    // initialised. Avoids redundant bakes of the other two local entries
    // (player->skelAnime, player->upperSkelAnime, pauseCtx->playerSkelAnime)
    // on every pause-open / equipment-change.
    SOH::SkeletonPatcher::UpdateCustomSkeletons(skelAnime);
}

static void RegisterCustomSkeletons() {
    COND_HOOK(OnAssetAltChange, true, UpdateCustomSkeleton);
    COND_HOOK(OnLinkSkeletonInit, true, UpdateCustomSkeletonsForLinkInit);
    // Disambiguate the no-arg overload — Q1 #2 (commit f39b39017) added
    // UpdateCustomSkeletons(SkelAnime*), so the bare function-name reference
    // here is overload-ambiguous (C2664) without the cast.
    COND_HOOK(OnLinkEquipmentChange, true, static_cast<void (*)()>(&SOH::SkeletonPatcher::UpdateCustomSkeletons));
}

static RegisterShipInitFunc initFunc(RegisterCustomSkeletons);
