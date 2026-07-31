/**
 * EnKarebabaBridge — extern "C" call sites for the vanilla
 * En_Karebaba actor.
 *
 * z_en_karebaba.c calls into these hook points via plain-`extern`
 * forward declarations (Pitfall 7 — C-linkage is implicit in .c
 * files). Each hook looks up EnKarebabaDescriptor in the
 * enhancement registry and dispatches to the appropriate method.
 * All hooks are cheap no-ops when no descriptor is registered.
 *
 * Hooks:
 *   Anchor_Enhance_EnKarebaba_OnHostSetupSpin — called from
 *     EnKarebaba_SetupSpin. Host rolls RNG; returns 1 if the
 *     spin will be enhanced (caller stores flag for outgoing
 *     ENEMY_STATE payload).
 *   Anchor_Enhance_EnKarebaba_ApplyPeerEnhancedFlag — called from
 *     HookHandlers BEFORE EnKarebaba_ApplyNetState on peer.
 *     Records the flag from network before local SetupSpin fires.
 *   Anchor_Enhance_EnKarebaba_OnSpinTick — called from
 *     EnKarebaba_Spin per-frame body. Descriptor applies head-
 *     scale + spawns geyser on frame 1.
 *   Anchor_Enhance_EnKarebaba_OnSpinExit — called from
 *     EnKarebaba_SetupUpright (spin exit path). Descriptor resets
 *     actor.scale + clears enhancement flag.
 *   Anchor_Enhance_EnKarebaba_IsCurrentSpinEnhanced — called from
 *     EnemyState.cpp send-side. Returns 1 if outgoing payload
 *     should include the enhanced flag.
 *   Anchor_Enhance_EnKarebaba_OnActorDestroy — called from
 *     EnKarebaba_Destroy. Descriptor removes per-actor state entry.
 *
 * See `Claude/Plans/en_karebaba_enhanced_plan.md` for the full
 * design.
 */

// Pitfall 40 — Anchor.h FIRST.
#include "soh/Network/Anchor/Anchor.h"

#include "EnKarebabaDescriptor.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementRegistry.h"

namespace {

// Look up the registered EnKarebabaDescriptor. Returns nullptr when
// no descriptor is registered (impossible post-ShipInit but checked
// defensively).
AnchorEnemyEnhancement::EnKarebabaDescriptor* GetKarebabaDescriptor() {
    auto& reg = AnchorEnemyEnhancement::EnhancementRegistry::Instance();
    return const_cast<AnchorEnemyEnhancement::EnKarebabaDescriptor*>(
        static_cast<const AnchorEnemyEnhancement::EnKarebabaDescriptor*>(
            reg.Find(ACTOR_EN_KAREBABA)));
}

}  // namespace

// --- Public extern "C" bridge API called from z_en_karebaba.c ---------

extern "C" int Anchor_Enhance_EnKarebaba_OnHostSetupSpin(EnKarebaba* actor,
                                                          PlayState* play) {
    if (actor == nullptr) return 0;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->OnHostSetupSpin(actor, play) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnKarebaba_ApplyPeerEnhancedFlag(EnKarebaba* actor,
                                                                  int enhanced) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveEnhancedSpinFlag(actor, enhanced != 0);
}

extern "C" void Anchor_Enhance_EnKarebaba_OnSpinTick(EnKarebaba* actor,
                                                       PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnSpinTick(actor, play);
}

extern "C" void Anchor_Enhance_EnKarebaba_OnSpinExit(EnKarebaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnSpinExit(actor);
}

extern "C" int Anchor_Enhance_EnKarebaba_IsCurrentSpinEnhanced(EnKarebaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsCurrentSpinEnhanced(actor) ? 1 : 0;
}

// V6 — telegraph render + charge state wire-sync hooks.

extern "C" void Anchor_Enhance_EnKarebaba_OnUprightTick(EnKarebaba* actor,
                                                          PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnUprightTick(actor, play);
}

extern "C" void Anchor_Enhance_EnKarebaba_OnDeath(EnKarebaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnDeath(actor);
}

extern "C" int Anchor_Enhance_EnKarebaba_IsCharged(EnKarebaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsCharged(actor) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnKarebaba_ApplyPeerChargedFlag(EnKarebaba* actor,
                                                                 int charged) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveChargedFlag(actor, charged != 0);
}

extern "C" void Anchor_Enhance_EnKarebaba_OnActorDestroy(EnKarebaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetKarebabaDescriptor();
    if (desc == nullptr) return;
    desc->OnActorDestroy(actor);
}
