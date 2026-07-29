/**
 * EnSwDescriptor — Phase 2 pilot for the enemy-enhancement registry.
 *
 * ACTOR_EN_SW is the Skullwalltula (wall-crawling spider variant of the
 * Skulltula family). Vanilla actor discriminates variants via
 * `(params & 0xE000) >> 13`:
 *   0 = combat wall-crawler (this descriptor's target)
 *   1..N = gold-skulltula-token variants (this descriptor MUST NOT touch)
 *
 * Phase 2 goal: opt-in enhancement that lets the combat variant leave
 * its wall anchor when the player is in range — walking on floors,
 * climbing arbitrary walls via RoomNavData substrate, falling when
 * knocked off, and actively acquiring nearest climbing target instead
 * of randomising gaze.
 *
 * Capabilities (all default off via CVars; user opts in per capability):
 *   canNavConsume   — floor + climb-any surface pursuit via NavConsumer
 *   canGravityAware — falls when off-wall + not-on-floor
 *   canActiveAggro  — deterministic gaze-target acquisition
 *
 * Per-instance carve-out (D12): `IsInstanceEnhanced` returns false for
 * gold-token variants regardless of CVar state — token skulltulas retain
 * vanilla static behavior.
 *
 * Combat scope decision (D13 — Reading A): Phase 2 extends this
 * descriptor with vanilla En_Sw combat primitives adapted for ground
 * use. `canReplaceCombat` stays off; combat runs through vanilla
 * actionFunc dispatch with the enhancement layer influencing WHERE the
 * actor is (nav) and WHAT it aims at (active aggro), not replacing the
 * lunge / bite state machine itself.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.11 for canActiveAggro
 * details + §7 Phase 2 for the full step list. Registered from
 * Anchor init (Phase 2 Step 3).
 */

#pragma once

#include "../EnemyEnhancementDescriptor.h"

extern "C" {
#include "z64.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
}

namespace AnchorEnemyEnhancement {

class EnSwDescriptor : public EnemyEnhancementDescriptor {
public:
    EnSwDescriptor() = default;
    ~EnSwDescriptor() override = default;

    // Actor identity.
    int16_t     ActorId()   const override { return ACTOR_EN_SW; }
    const char* ActorName() const override { return "Skullwalltula (En_Sw)"; }

    // Capability flags. Combat replacement DEFERRED per D13; nav +
    // gravity + active-aggro comprise the Phase 2 v1 scope.
    CapabilityFlags Capabilities() const override {
        CapabilityFlags caps;
        caps.canNavConsume    = true;
        caps.canGravityAware  = true;
        caps.canActiveAggro   = true;
        caps.canReplaceCombat = false;  // Reading A — vanilla combat retained
        return caps;
    }

    // CVar names — three independent knobs so authors can bisect
    // behavior. All default 0 (see Phase 2 Step 7 widget landing).
    const char* NavConsumeCVar()   const override {
        return "gEnhancements.Skullwalltula.NavConsume";
    }
    const char* GravityAwareCVar() const override {
        return "gEnhancements.Skullwalltula.GravityAware";
    }
    const char* ActiveAggroCVar()  const override {
        return "gEnhancements.Skullwalltula.AggressiveAcquire";
    }

    // Parameter blocks. Values field-tuned during Phase 2 pilot;
    // adjusted here rather than at call sites so the descriptor stays
    // authoritative.
    NavConsumeParams   NavParams()     const override;
    GravityAwareParams GravityParams() const override;

    // Per-instance carve-out (D12). Returns false for gold-token
    // variants — they never leave the wall regardless of CVar state.
    // Combat variants (params&0xE000==0) return true.
    bool IsInstanceEnhanced(Actor* actor, PlayState* play) override;

    // Nav + gravity hooks. Delegated to NavConsumer / GravityAdapter
    // shared helpers — the descriptor supplies params + eligibility;
    // the helper owns the per-actor state and locomotion primitives.
    void OnNavTick(Actor* actor, PlayState* play) override;
    bool ShouldApplyGravity(Actor* actor, PlayState* play) override;
    void OnLandedFromFall(Actor* actor, PlayState* play) override;

    // Draw-time leg-bend override — Phase 2 Step 8. Adds ~15° downward
    // rotation on leg-limb indices when state machine reads ground-
    // walking. Wall / lunge / dying states leave vanilla pose alone.
    bool OverrideLimbBend(int32_t limbIndex, Vec3s* rotInOut,
                          Actor* actor, PlayState* play) override;
};

}  // namespace AnchorEnemyEnhancement
