/**
 * EnSwBridge — extern "C" call sites for the vanilla En_Sw actor.
 *
 * z_en_sw.c calls into three hook points via plain-`extern` forward
 * declarations (Pitfall 7 — C-linkage is implicit in .c files). Each
 * hook looks up EnSwDescriptor in the enhancement registry and
 * dispatches to the appropriate virtual method. All hooks are cheap
 * no-ops when no descriptor is registered or the actor is a gold-token
 * variant (IsInstanceEnhanced returns false).
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.5 + §7 Phase 2 step 5
 * for the hook contract, and §4.11 for the gaze-override behavior.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates land
// in C++ linkage before EnSwDescriptor.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "EnSwDescriptor.h"
#include "EnSwStateMachine.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementRegistry.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/NavConsumer.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GravityAdapter.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt

#include <unordered_map>

namespace {

// Look up the registered EnSwDescriptor. Returns nullptr when no
// descriptor is registered — should be impossible post-ShipInit but
// checked defensively so hook fires during static-init can't crash.
AnchorEnemyEnhancement::EnSwDescriptor* GetEnSwDescriptor() {
    auto& reg = AnchorEnemyEnhancement::EnhancementRegistry::Instance();
    // Registry::Find returns const*; we hold ownership via unique_ptr
    // and mutate through virtual dispatch, so const_cast is safe here.
    // Callers can freely invoke non-const virtuals on the returned ptr.
    return const_cast<AnchorEnemyEnhancement::EnSwDescriptor*>(
        static_cast<const AnchorEnemyEnhancement::EnSwDescriptor*>(
            reg.Find(ACTOR_EN_SW)));
}

// Per-actor NavConsumer + GravityAdapter state, keyed by Actor*.
// Entries persist across ticks; cleared per-actor when Destroy fires
// (Phase 2+ — currently entries accumulate until scene teardown
// clears the whole map indirectly via actor pointer invalidation. A
// scene-transition sweep would be the next hardening step; not blocking
// for v1 since the map size is small — one entry per living Skullwalltula.
std::unordered_map<Actor*, AnchorEnemyEnhancement::NavConsumerState>    sNavStates;
std::unordered_map<Actor*, AnchorEnemyEnhancement::GravityAdapterState> sGravityStates;

}  // namespace

// --- Public extern "C" bridge API called from z_en_sw.c ---------------

extern "C" void Anchor_Enhance_EnSw_OnInit(EnSw* actor, PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return;
    if (!desc->IsInstanceEnhanced(&actor->actor, play)) return;

    // Snapshot vanilla's ambient actionFunc so the state machine's per-
    // tick dispatch can detect when vanilla has advanced to lunge cycle
    // (self->actionFunc != snapshot) and yield the frame. Snapshot is
    // captured regardless of CVar state so it's available if the CVar
    // toggles on mid-session. Idempotent — repeat calls are no-ops.
    AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_SnapshotAmbient(actor);
}

extern "C" void Anchor_Enhance_EnSw_Tick(EnSw* actor, PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return;
    if (!desc->IsInstanceEnhanced(&actor->actor, play)) return;

    // Enhanced state machine — Option B pilot (Plans/en_sw_enhanced_
    // state_machine_pilot.md). Fires when NavConsume CVar is enabled
    // AND the actor was snapshotted at OnInit. Runs AFTER vanilla
    // actionFunc (this is a post-hook); state machine overrides motion
    // in ambient states, yields to vanilla when actionFunc has advanced
    // to lunge cycle. Internal CVar check happens inside the state
    // machine's Tick to keep the gate + snapshot side-effect in one
    // place.
    if (AnchorCVarSync::GetEnforcedInt(
            "gEnhancements.Skullwalltula.NavConsume", 0) != 0) {
        AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_Tick(actor, play);
    }

    // Legacy NavConsumer + GravityAdapter dispatch — bodies stubbed to
    // no-op post M1 (see NavConsumer.cpp / GravityAdapter.cpp header
    // comments). Left in place for future non-En_Sw wall-crawler
    // descriptors that may want simpler pos+yaw-only nav semantics
    // without a state machine.
    desc->OnNavTick(&actor->actor, play);
    if (desc->ShouldApplyGravity(&actor->actor, play)) {
        AnchorEnemyEnhancement::GravityAdapterState& gState =
            sGravityStates[&actor->actor];
        AnchorEnemyEnhancement::TickGravity(*desc, gState,
                                             &actor->actor, play);
    }
}

extern "C" void Anchor_Enhance_EnSw_OverrideLimb(EnSw* actor, PlayState* play,
                                                   int32_t limbIndex, Vec3s* rot) {
    if (actor == nullptr || rot == nullptr) return;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return;
    if (!desc->IsInstanceEnhanced(&actor->actor, play)) return;

    // Descriptor's OverrideLimbBend mutates *rot in place when the actor
    // meets its ground-walking criteria AND limbIndex is a leg. Return
    // value indicates whether rot was modified; caller doesn't need it
    // (mutation is in-place either way).
    (void)desc->OverrideLimbBend(limbIndex, rot, &actor->actor, play);
}

extern "C" int Anchor_Enhance_EnSw_GazeOverride(EnSw* actor, PlayState* play,
                                                 Vec3f* outTargetPos) {
    // Returns 1 (true) if the hook selected a target position and
    // wrote it to *outTargetPos, meaning the caller should replace its
    // random `unk_444` assignment with `func_80B0DE34(this, outTargetPos)`.
    // Returns 0 (false) to let vanilla random gaze run unmodified.
    //
    // Under AggressiveAcquire ON: PickGazeTarget iterates synced players
    // + returns a climbing candidate's world.pos, so the spider aims
    // deterministically at target instead of picking a random angle.
    // Combined with ShouldRelaxLungeGates below, the state-7 wind-up
    // is stable in MP scenarios where peer motion previously
    // flickered the vanilla per-frame gate re-validation.
    if (actor == nullptr || outTargetPos == nullptr) return 0;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return 0;
    if (!desc->IsInstanceEnhanced(&actor->actor, play)) return 0;
    return desc->PickGazeTarget(&actor->actor, play, outTargetPos) ? 1 : 0;
}

extern "C" int Anchor_Enhance_EnSw_IsJumpAttacking(EnSw* actor) {
    // Consulted by vanilla EnSw_Draw to layer the purple attack fog
    // color while our state machine is running one of its custom
    // attack states: JumpLunge (Tektite-style ballistic wind-up +
    // airborne) OR WalkLunge (ground wind-up + straight-line dash,
    // replaces the vanilla func_80B0E728 walk-lunge for enhanced
    // ground spiders). Both should glow purple through the whole
    // attack cycle, matching the vanilla func_80B0E728 treatment.
    // Function name kept for wire-format compat; semantic covers
    // any custom attack state.
    if (actor == nullptr) return 0;
    const auto st =
        AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_QueryState(actor);
    return (st == AnchorEnemyEnhancement::EnSwState::JumpLunge ||
            st == AnchorEnemyEnhancement::EnSwState::WalkLunge) ? 1 : 0;
}

extern "C" int Anchor_Enhance_EnSw_ShouldRelaxLungeGates(EnSw* actor) {
    // Consulted by vanilla EnSw_PickLungeTarget at the top. Returns 1
    // when descriptor says the flicker-prone gates (STATIONARY_LADDER,
    // angular arc) should be skipped this pick. Distance + LoS gates
    // remain — target must actually escape range or lose sight to fail
    // the picker.
    //
    // Under AggressiveAcquire ON, this is the KEY fix for the
    // "purple-flash-then-abort" bug (see Analysis/en_sw_lunge_abort_
    // when_host_far_from_peer_2026-07-29.md). The state-7 20-frame
    // wind-up otherwise re-validates all gates per frame and aborts
    // whenever peer moves rapidly (climbing) enough to slip out of
    // the ~22° arc gate temporarily.
    if (actor == nullptr) return 0;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return 0;
    // Passing nullptr for play — ShouldRelaxLungeGates only reads the
    // CVar + actor's instance status, doesn't touch play. If a future
    // implementation needs play, we'd thread it through.
    return desc->ShouldRelaxLungeGates(&actor->actor, nullptr) ? 1 : 0;
}

// 2026-08-04 (Pillar 5 Phase 3, GH #210) — mark this En_Sw as spawned
// via En_St→En_Sw swap. Called by EnStBridge FireSwap right after
// Actor_Spawn(EN_SW). Sets a state-block flag which the state
// machine's per-tick handler reads to enforce scale + floor-snap.
extern "C" void Anchor_Enhance_EnSw_MarkFromStSwap(EnSw* actor) {
    if (actor == nullptr) return;
    AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_MarkFromStSwap(actor);
}
