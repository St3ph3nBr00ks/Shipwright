#pragma once

// Per-actor state extract for ACTOR_EN_DEKUBABA.
//
// Mirrors EnKarebabaState — dedicated struct nested into EnemyNetId
// for Dekubaba-specific wire fields that don't belong on the generic
// per-enemy state. See EnemyNetId.h for the nesting.
//
// Fields:
//   - netAcidActive : wire-received flag indicating host's current
//     attack cycle is an acid vomit. Peer's ApplyPeerAcidActiveFlag
//     is called BEFORE ApplyNetState so the local AcidVomit path
//     picks up the right visuals + spawn behavior.
//   - netAcidCharged: wire-received flag indicating host's charge
//     state is Ready (telegraph should render). Peer's
//     ApplyPeerAcidChargedFlag is called BEFORE ApplyNetState.
//
// Both flags mirror the Karebaba pattern (karebaba.netEnhancedSpin
// + karebaba.netCharged) for architectural consistency.

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"  // s16
}

namespace EnemySync {

struct EnDekubabaState {
    // Pillar 5 (GH #308) — acid vomit enhancement flags from the
    // most recent host broadcast. Peer's HookHandlers forwards these
    // to the EnDekubabaDescriptor via
    //   Anchor_Enhance_EnDekubaba_ApplyPeerAcidActiveFlag
    //   Anchor_Enhance_EnDekubaba_ApplyPeerAcidChargedFlag
    // BEFORE calling EnDekubaba_ApplyNetState, so peer's local
    // ApplyNetState (which routes to the new SetupAcidVomit for
    // state index 14) sees the right visuals + telegraph state.
    bool netAcidActive  = false;
    bool netAcidCharged = false;

    // Pillar 5 (GH #309) — detach + pursue enhancement flag. Sticky
    // per actor life; peer's HookHandlers forwards to descriptor via
    //   Anchor_Enhance_EnDekubaba_ApplyPeerDetachActiveFlag
    // so peer's Draw-side leaf-bundle hide + SetupDetachedSquirm
    // path see the right state.
    bool netDetachActive = false;

    // Pillar 5 (GH #318) — seed spawn active flag + landing target.
    // Applied per-tick to descriptor via
    //   Anchor_Enhance_EnDekubaba_ApplyPeerSeedActiveFlag
    //   Anchor_Enhance_EnDekubaba_ApplyPeerSeedLandingPos
    // so peer's SetupSeedTelegraph → SeedFire → projectile spawn
    // sequence mirrors host's landing coord exactly.
    bool netSeedActive        = false;
    float netSeedLandingX     = 0.0f;
    float netSeedLandingY     = 0.0f;
    float netSeedLandingZ     = 0.0f;
};

}  // namespace EnemySync
