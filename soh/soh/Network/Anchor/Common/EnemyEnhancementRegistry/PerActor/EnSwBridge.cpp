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
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementRegistry.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/NavConsumer.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GravityAdapter.h"

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

    // Phase 2 partial: descriptor's per-actor init hook. Currently a
    // no-op (Phase 2 Step 4 wires NavConsumer state map). Kept as an
    // entry point so future setup logic lands here rather than at
    // every per-tick call.
    (void)desc;
    (void)play;
    // Future: desc->OnActorInit(&actor->actor, play);
}

extern "C" void Anchor_Enhance_EnSw_Tick(EnSw* actor, PlayState* play) {
    if (actor == nullptr) return;
    auto* desc = GetEnSwDescriptor();
    if (desc == nullptr) return;
    if (!desc->IsInstanceEnhanced(&actor->actor, play)) return;

    // Nav consumption — CVar-gated inside NavConsumer::TickNavMovement.
    // Descriptor's OnNavTick delegates to NavConsumer with its own
    // per-actor NavConsumerState (map in EnSwDescriptor.cpp file scope).
    desc->OnNavTick(&actor->actor, play);

    // Gravity — CVar-gated inside TickGravity. ShouldApplyGravity
    // filters out ineligible states (on-wall, on-floor); only fires
    // TickGravity when actor is genuinely airborne.
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
