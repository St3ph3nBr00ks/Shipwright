/**
 * EnKarebabaDescriptor — Pillar 5 pilot for the withered Deku Baba
 * (ACTOR_EN_KAREBABA).
 *
 * Vanilla En_Karebaba is the smaller Skulltula-style variant of
 * Dekubaba — grows from a mound, sways upright, spins in a windmill
 * arc (the "Spin" state at z_en_karebaba.c:446, ~40 frames),
 * retracts, respawns. Combat-relevant states: Idle → Awaken →
 * Upright → Spin → (bite / retract). Fully MP-synced pre-Pillar 5
 * via EnKarebaba_GetStateIndex / _ApplyNetState.
 *
 * Enhancement scope (Pillar 5, GH #310, plan
 * `Claude/Plans/en_karebaba_enhanced_plan.md`):
 *
 *   - **Geyser AoE acid during Spin** — at Spin entry, host rolls a
 *     33% chance. On success, the Spin plays out as usual + head
 *     scales 1.0×→1.5×→1.0× (sinusoid over spin window) + a
 *     vertical acid plume actor spawns from actor.home.pos on frame
 *     1 of Spin. Plume runs ~30 frames, damages any player within
 *     a small cylinder around home.pos. Vanilla head-collider
 *     contact damage is NOT disabled — both hazards active.
 *
 * Per-instance carve-out (D12): none in v1. All En_Karebaba
 * instances are eligible when the CVar is on (no gold-token / no
 * boss discriminator on Karebaba).
 *
 * MP sync: extended `ENEMY_STATE` payload for Karebaba adds one
 * bool `karebabaEnhancedSpin`. Host writes at Spin entry; peer
 * reads it via a new bridge helper called from HookHandlers BEFORE
 * EnKarebaba_ApplyNetState, so peer's local Spin actionFunc sees
 * the same enhancement flag as host from frame 0. Geyser actor
 * spawn is deterministic (fixed home.pos + fixed frame + fixed
 * sceneNum) so both clients spawn locally — no per-spawn broadcast
 * needed. Damage is host-authoritative via Path A.
 *
 * See `Plans/en_karebaba_enhanced_plan.md` for the full design,
 * infinite-whys analysis, 7-principles evaluation, and open
 * questions.
 */

#pragma once

#include "../EnemyEnhancementDescriptor.h"

extern "C" {
#include "z64.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
}

namespace AnchorEnemyEnhancement {

class EnKarebabaDescriptor : public EnemyEnhancementDescriptor {
public:
    EnKarebabaDescriptor() = default;
    ~EnKarebabaDescriptor() override = default;

    int16_t     ActorId()   const override { return ACTOR_EN_KAREBABA; }
    const char* ActorName() const override { return "Karebaba (En_Karebaba)"; }

    // No capabilities inherited from the shared registry (Nav /
    // Gravity / ActiveAggro / ReplaceCombat) — Karebaba is anchored
    // to home.pos and doesn't consume the nav mesh. The geyser
    // enhancement is a per-state overlay driven entirely by bridge
    // hooks in EnKarebabaBridge.cpp. Base class returns all-false
    // capabilities and no descriptor virtuals override anything;
    // the bridge is a thin dispatch that talks to per-actor state
    // held in the descriptor's own file-static map.
    CapabilityFlags Capabilities() const override {
        return {};
    }

    // CVar for the geyser enhancement. Host-authoritative per shared
    // EnforcedCVarRegistry pattern.
    static const char* GeyserSpinCVarName() {
        return "gEnhancements.Karebaba.GeyserSpin";
    }

    // ---- Enhancement API (called from EnKarebabaBridge extern "C"
    // shims which the vanilla z_en_karebaba.c invokes at hook points)

    // Called from EnKarebaba_SetupSpin on the HOST. Rolls a 33%
    // chance if the CVar is ON. On success, marks this actor's
    // current Spin cycle as enhanced (head-scale + geyser will
    // apply). Returns true if the roll succeeded — caller broadcasts
    // the flag over ENEMY_STATE.
    bool OnHostSetupSpin(EnKarebaba* actor, PlayState* play);

    // Called from HookHandlers on the PEER before EnKarebaba_ApplyNetState.
    // Records the network-received karebabaEnhancedSpin flag into the
    // per-actor state map so that peer's local Setup Spin sees it.
    void OnPeerReceiveEnhancedSpinFlag(EnKarebaba* actor, bool enhanced);

    // Called from EnKarebaba_Spin per-frame on BOTH clients. If this
    // spin cycle is enhanced, applies the head-scale sinusoid and
    // spawns the geyser plume actor on frame 1 (once per spin).
    void OnSpinTick(EnKarebaba* actor, PlayState* play);

    // Called from EnKarebaba_SetupUpright (spin exit) to reset per-
    // actor enhancement state so the next Spin re-rolls fresh.
    void OnSpinExit(EnKarebaba* actor);

    // Query helper used by EnemyState.cpp send-side to include the
    // flag in the outgoing ENEMY_STATE payload when this actor's
    // current spin is enhanced. Returns false when actor isn't
    // currently in an enhanced spin cycle.
    bool IsCurrentSpinEnhanced(EnKarebaba* actor);

    // Cleanup — called from EnKarebaba_Destroy to remove per-actor
    // state entry.
    void OnActorDestroy(EnKarebaba* actor);
};

}  // namespace AnchorEnemyEnhancement
