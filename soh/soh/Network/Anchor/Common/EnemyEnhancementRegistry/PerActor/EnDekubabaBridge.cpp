/**
 * EnDekubabaBridge — extern "C" bridge from z_en_dekubaba.c to
 * EnDekubabaDescriptor (Feature A: acid vomit, tracker #308).
 *
 * Hook points in z_en_dekubaba.c:
 *   - EnDekubaba_DecideLunge: before SetupPrepareLunge, calls
 *     Anchor_Enhance_EnDekubaba_MaybeAcidLunge. If returns 1,
 *     caller instead invokes EnDekubaba_SetupAcidVomit.
 *   - EnDekubaba_AcidVomit (new actionFunc): per-frame calls
 *     Anchor_Enhance_EnDekubaba_OnAcidVomitTick with current
 *     frame index.
 *   - EnDekubaba_Recover: at attack-cycle exit calls
 *     Anchor_Enhance_EnDekubaba_OnAttackComplete.
 *   - EnDekubaba_SetupPrunedSomersault / SetupShrinkDie: OnDeath.
 *   - EnDekubaba_Destroy: OnActorDestroy.
 *
 * All shims are cheap no-ops when descriptor isn't registered
 * (impossible post-ShipInit — defensive).
 *
 * Also includes the CVar-gated audio boost shim mirroring the
 * Karebaba V8 pattern — 6 vanilla Audio_PlayActorSound2 sites in
 * z_en_dekubaba.c will call through this to get the +100% boost
 * when the AcidVomit CVar is on.
 */

// Pitfall 40 — Anchor.h FIRST.
#include "soh/Network/Anchor/Anchor.h"

#include "EnDekubabaDescriptor.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementRegistry.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementAudio.h"

namespace {

AnchorEnemyEnhancement::EnDekubabaDescriptor* GetDekubabaDescriptor() {
    auto& reg = AnchorEnemyEnhancement::EnhancementRegistry::Instance();
    return const_cast<AnchorEnemyEnhancement::EnDekubabaDescriptor*>(
        static_cast<const AnchorEnemyEnhancement::EnDekubabaDescriptor*>(
            reg.Find(ACTOR_EN_DEKUBABA)));
}

}  // namespace

// --- Feature A bridge shims ------------------------------------------

extern "C" int Anchor_Enhance_EnDekubaba_MaybeAcidLunge(EnDekubaba* actor,
                                                         PlayState* play) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->OnHostMaybeAcidLunge(actor, play) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnDekubaba_ApplyPeerAcidActiveFlag(
    EnDekubaba* actor, int active) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveAcidActiveFlag(actor, active != 0);
}

extern "C" void Anchor_Enhance_EnDekubaba_ApplyPeerAcidChargedFlag(
    EnDekubaba* actor, int charged) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveAcidChargedFlag(actor, charged != 0);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnAcidVomitTick(EnDekubaba* actor,
                                                            PlayState* play,
                                                            int frame) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnAcidVomitTick(actor, play, frame);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnAttackComplete(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnAttackComplete(actor);
}

extern "C" int Anchor_Enhance_EnDekubaba_IsCurrentAttackAcid(EnDekubaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsCurrentAttackAcid(actor) ? 1 : 0;
}

extern "C" int Anchor_Enhance_EnDekubaba_IsAcidCharged(EnDekubaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsAcidCharged(actor) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnDekubaba_OnDeath(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnDeath(actor);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnActorDestroy(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnActorDestroy(actor);
}

// --- Feature B (#309) — detach + pursue bridge shims -----------------

extern "C" int Anchor_Enhance_EnDekubaba_MaybeDetach(EnDekubaba* actor,
                                                      PlayState* play) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->OnHostMaybeDetach(actor, play) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnDekubaba_ApplyPeerDetachActiveFlag(
    EnDekubaba* actor, int active) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveDetachActiveFlag(actor, active != 0);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnDetachedSquirmTick(
    EnDekubaba* actor, PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnDetachedSquirmTick(actor, play);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnDetachedDyingTick(
    EnDekubaba* actor, PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnDetachedDyingTick(actor, play);
}

extern "C" int Anchor_Enhance_EnDekubaba_IsDetached(EnDekubaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsDetached(actor) ? 1 : 0;
}

// --- Feature C (#318) — seed spawn bridge shims ---------------------

extern "C" int Anchor_Enhance_EnDekubaba_MaybeSeedFire(EnDekubaba* actor,
                                                        PlayState* play) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->OnHostMaybeSeedFire(actor, play) ? 1 : 0;
}

extern "C" void Anchor_Enhance_EnDekubaba_ApplyPeerSeedActiveFlag(
    EnDekubaba* actor, int active) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveSeedActiveFlag(actor, active != 0);
}

extern "C" void Anchor_Enhance_EnDekubaba_ApplyPeerSeedLandingPos(
    EnDekubaba* actor, float x, float y, float z) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnPeerReceiveSeedLandingPos(actor, x, y, z);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnSeedTelegraphTick(
    EnDekubaba* actor, PlayState* play, int frame) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnSeedTelegraphTick(actor, play, frame);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnSeedFireTick(
    EnDekubaba* actor, PlayState* play, int frame) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnSeedFireTick(actor, play, frame);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnSeedLanded(
    EnDekubaba* parent, PlayState* play, float x, float y, float z) {
    if (parent == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnSeedLanded(parent, play, x, y, z);
}

extern "C" void Anchor_Enhance_EnDekubaba_OnActorInit(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return;
    desc->OnActorInit(actor);
}

extern "C" int Anchor_Enhance_EnDekubaba_IsSeedActive(EnDekubaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsSeedActive(actor) ? 1 : 0;
}

extern "C" int Anchor_Enhance_EnDekubaba_IsSpawnedByEnhancement(
    EnDekubaba* actor) {
    if (actor == nullptr) return 0;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0;
    return desc->IsSpawnedByEnhancement(actor) ? 1 : 0;
}

extern "C" float Anchor_Enhance_EnDekubaba_GetSeedLandingX(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0.0f;
    return desc->GetSeedLandingX(actor);
}
extern "C" float Anchor_Enhance_EnDekubaba_GetSeedLandingY(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0.0f;
    return desc->GetSeedLandingY(actor);
}
extern "C" float Anchor_Enhance_EnDekubaba_GetSeedLandingZ(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto* desc = GetDekubabaDescriptor();
    if (desc == nullptr) return 0.0f;
    return desc->GetSeedLandingZ(actor);
}

// --- CVar-gated audio boost shim (mirrors Karebaba V8) --------------
//
// Called from z_en_dekubaba.c at Audio_PlayActorSound2 sites so
// vanilla Dekubaba SFX get +100% volume when the AcidVomit CVar
// is on (aesthetic parity with Karebaba's boosted enhancement).
// CVar off = vanilla behavior unchanged.
extern "C" void Anchor_Enhance_EnDekubaba_PlayActorSfx(Actor* actor,
                                                        u16 sfxId) {
    AnchorEnemyEnhancement::EnhancementAudio::PlayCVarGatedActorSfx(
        actor, sfxId,
        AnchorEnemyEnhancement::EnDekubabaDescriptor::AcidVomitCVarName(),
        &AnchorEnemyEnhancement::EnhancementAudio::kDoubledVolScale);
}
