/**
 * EnDekubabaDescriptor — Pillar 5 enhancement pilot for the vanilla
 * Deku Baba (ACTOR_EN_DEKUBABA).
 *
 * Three features (all default-off, independent CVars):
 *   - Feature A: acid-vomit projectile (#308)
 *   - Feature B: stem detach + ground pursue (#309) [DEFERRED]
 *   - Feature C: seed-spawn projectile (#318) [DEFERRED]
 *
 * This descriptor covers Feature A only in v1. B and C hook points
 * are stubbed but return no-op; adding them layers on top of A's
 * ChargeStateMachine + wire-sync infrastructure without disturbing
 * the A path.
 *
 * Design source:
 *   Claude/Plans/en_dekubaba_enhanced_plan.md
 *   Claude/Analysis/en_sw_mp_sync_audit_2026-07-31.md
 *
 * Dekubaba source audit (2026-07-31):
 *   14 vanilla state indices (0-13 in GetStateIndex / ApplyNetState).
 *   Attack cycle: DecideLunge → PrepareLunge (8f) → Lunge → PullBack →
 *   Recover → (loop back to DecideLunge, or Retract if Link far).
 *   Feature A inserts state 14 = AcidVomit at the DecideLunge decision
 *   point: on acid-fire, transition to a new actionFunc that plays a
 *   telegraph + spawns EN_DEKUBABA_ACID projectile + transitions to
 *   PullBack. Vanilla Lunge is skipped.
 *
 * MP sync: extend ENEMY_STATE Dekubaba payload with two booleans:
 *   dekubabaAcidActive  — this attack cycle uses acid (like Karebaba's
 *                         karebabaEnhancedSpin)
 *   dekubabaAcidCharged — telegraph should render (like Karebaba's
 *                         karebabaCharged)
 * Charge/cooldown state machine runs host-only per sync-rule 1
 * (host is sole RNG decider); peer receives the two booleans and
 * mirrors visuals only.
 *
 * Feature B / C notes: when they land, add
 *   dekubabaDetachActive  (bool)
 *   dekubabaSeedActive    (bool) + seedLandingPos (Vec3f)
 * to the same payload.
 */

#pragma once

#include "../EnemyEnhancementDescriptor.h"

extern "C" {
#include "z64.h"
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
}

namespace AnchorEnemyEnhancement {

class EnDekubabaDescriptor : public EnemyEnhancementDescriptor {
public:
    EnDekubabaDescriptor() = default;
    ~EnDekubabaDescriptor() override = default;

    int16_t     ActorId()   const override { return ACTOR_EN_DEKUBABA; }
    const char* ActorName() const override { return "Deku Baba (En_Dekubaba)"; }

    // Descriptor exists purely to hold per-actor state + dispatch
    // decorated attack decisions. Doesn't consume the shared registry
    // capabilities (nav / gravity / active-aggro / replace-combat) —
    // Dekubaba is stem-anchored and doesn't consume the nav mesh.
    CapabilityFlags Capabilities() const override { return {}; }

    // CVars — host-authoritative via EnforcedCVarRegistry.
    static const char* AcidVomitCVarName() {
        return "gEnhancements.Dekubaba.AcidVomit";
    }
    // Future — declared now so hook points can reference the strings.
    static const char* DetachAndPursueCVarName() {
        return "gEnhancements.Dekubaba.DetachAndPursue";
    }
    static const char* SeedSpawnCVarName() {
        return "gEnhancements.Dekubaba.SeedSpawn";
    }

    // ---- Feature A API (called from EnDekubabaBridge extern "C" shims)

    // Called from EnDekubaba_DecideLunge on the HOST at the moment
    // vanilla would transition to PrepareLunge. Rolls the ChargeState
    // machine, applies range gate (Link within acid-usable distance,
    // outside melee lunge range but within acid arc range). Returns
    // true iff caller should skip SetupPrepareLunge and instead call
    // SetupAcidVomit — the acid fire supersedes the vanilla lunge.
    bool OnHostMaybeAcidLunge(EnDekubaba* actor, PlayState* play);

    // Called from HookHandlers on the PEER before EnDekubaba_ApplyNetState.
    // Records the wire-received acid-active flag so peer's local
    // SetupAcidVomit path picks up the right visuals.
    void OnPeerReceiveAcidActiveFlag(EnDekubaba* actor, bool active);

    // Called from HookHandlers on the PEER before EnDekubaba_ApplyNetState.
    // Mirror of Karebaba's netCharged. Drives the pre-fire telegraph
    // render during PrepareLunge / equivalent.
    void OnPeerReceiveAcidChargedFlag(EnDekubaba* actor, bool charged);

    // Called from the new EnDekubaba_AcidVomit actionFunc per-frame.
    // Renders telegraph particles (mouth spit) and (at fire frame)
    // spawns the EN_DEKUBABA_ACID projectile. `spinFrames` is the
    // current progress through the acid attack cycle (0..kAcidTotalFrames).
    void OnAcidVomitTick(EnDekubaba* actor, PlayState* play, int frame);

    // Called from EnDekubaba_Recover at attack-cycle end regardless
    // of which attack fired. Advances all charge counters + resets
    // per-attack flags. This is the shared attack-complete hook per
    // plan §Feature-interaction "Counter-advancement rules".
    void OnAttackComplete(EnDekubaba* actor);

    // Query — send-side. Included in ENEMY_STATE payload when host
    // is broadcasting this Dekubaba's state.
    bool IsCurrentAttackAcid(EnDekubaba* actor);
    bool IsAcidCharged(EnDekubaba* actor);

    // Reset on death — matches Karebaba OnDeath. Wipes charge state
    // + per-attack flags. Dekubaba's Dying → Regrow → Init cycle
    // preserves the Actor* pointer so per-actor state map entry
    // stays; contents get zeroed.
    void OnDeath(EnDekubaba* actor);

    // Cleanup on actor destroy — removes per-actor state entry.
    void OnActorDestroy(EnDekubaba* actor);

    // ---- Feature B API (detach + pursue, #309) --------------------

    // Called from EnDekubaba_Recover at attack-cycle end, before
    // Anchor_Enhance_EnDekubaba_OnAttackComplete. Rolls detach chance
    // if Link was OUT of lunge range at attack time. Returns true iff
    // host committed to detach — caller invokes SetupDetachedSquirm
    // instead of the vanilla SetupDecideLunge chain.
    // One-shot per actor life (Dekubaba doesn't regrow after detach
    // death).
    bool OnHostMaybeDetach(EnDekubaba* actor, PlayState* play);

    // Peer-side flag — mirrors netAcidActive pattern. Applied every
    // tick from HookHandlers before EnDekubaba_ApplyNetState so peer's
    // local SetupDetachedSquirm sees the right state.
    void OnPeerReceiveDetachActiveFlag(EnDekubaba* actor, bool active);

    // Called per-frame from the new EnDekubaba_DetachedSquirm actionFunc.
    // Drives serpentine motion via sine-wave stem angles, ground-follow
    // Y-snap, and bleedout timer (-1 HP every 5 seconds).
    void OnDetachedSquirmTick(EnDekubaba* actor, PlayState* play);

    // Called per-frame from the new EnDekubaba_DetachedDying actionFunc.
    // Plays vanilla ShrinkDie animation at current squirm position;
    // caller Actor_Kills when timer expires.
    void OnDetachedDyingTick(EnDekubaba* actor, PlayState* play);

    // Query — send-side for ENEMY_STATE payload. Peer receives via
    // ApplyPeerDetachActiveFlag and mirrors visual state.
    bool IsDetached(EnDekubaba* actor);

    // ---- Feature C API (seed spawn, #318) --------------------------

    // Called from EnDekubaba_DecideLunge (after acid roll fails).
    // Rolls seedCharge, checks "own child not active" gate, computes
    // behind-Link landing target + nav validation. On fire, writes
    // landing coord to descriptor state so OnSeedFireTick can pass it
    // to the projectile actor. Returns true iff caller should invoke
    // SetupSeedTelegraph instead of continuing to detach/vanilla.
    bool OnHostMaybeSeedFire(EnDekubaba* actor, PlayState* play);

    // Peer-side flag apply — mirrors netAcidActive shape. Applied
    // every tick from HookHandlers before ApplyNetState.
    void OnPeerReceiveSeedActiveFlag(EnDekubaba* actor, bool active);

    // Peer-side landing pos apply — written per-tick from wire.
    void OnPeerReceiveSeedLandingPos(EnDekubaba* actor,
                                       float x, float y, float z);

    // Called per-frame from EnDekubaba_SeedTelegraph actionFunc.
    // Renders the same head-scale + spit telegraph as acid; timing
    // matches the acid telegraph window.
    void OnSeedTelegraphTick(EnDekubaba* actor, PlayState* play, int frame);

    // Called per-frame from EnDekubaba_SeedFire actionFunc. At fire
    // frame, spawns EN_DEKUBABA_SEED projectile at head aimed at
    // landing target. Handles the "spawn child on land" callback.
    void OnSeedFireTick(EnDekubaba* actor, PlayState* play, int frame);

    // Called from EN_DEKUBABA_SEED actor's on-land Update path (via
    // a dedicated bridge). Spawns a child EN_DEKUBABA at landing
    // position with home.pos = landing pos + marks the new actor's
    // descriptor state as spawnedByEnhancement=true (via the
    // g_isSpawningDekubabaChild thread-local flag pattern).
    void OnSeedLanded(EnDekubaba* parent, PlayState* play,
                      float x, float y, float z);

    // Called at EnDekubaba_Init to set spawnedByEnhancement=true when
    // this actor is being spawned via the seed pipeline. Reads a
    // thread-local flag set by OnSeedLanded's Actor_Spawn bracket.
    void OnActorInit(EnDekubaba* actor);

    // Query — send-side. Included in ENEMY_STATE payload.
    bool IsSeedActive(EnDekubaba* actor);
    bool IsSpawnedByEnhancement(EnDekubaba* actor);

    // Seed landing coord accessors for wire-sync send-side.
    float GetSeedLandingX(EnDekubaba* actor);
    float GetSeedLandingY(EnDekubaba* actor);
    float GetSeedLandingZ(EnDekubaba* actor);
};

}  // namespace AnchorEnemyEnhancement
