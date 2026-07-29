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

    // Nav consumption — reads NavConsume CVar internally via
    // NavConsumer::TickNavMovement, which the descriptor's OnNavTick
    // delegates to. Phase 1 helper returns false so this currently
    // has no runtime effect.
    desc->OnNavTick(&actor->actor, play);

    // Gravity tick — engages when ShouldApplyGravity returns true.
    // Phase 1 helper returns false so gravity currently doesn't apply
    // even if descriptor's ShouldApplyGravity would trigger it.
    if (desc->ShouldApplyGravity(&actor->actor, play)) {
        // TickGravity delegates back to descriptor->OnLandedFromFall
        // when it detects a landing edge. Passed a nullptr state for
        // now; Phase 2 Step 5 threads real GravityAdapterState.
        // (Currently no-op in helper body.)
        // TODO Phase 2 Step 5: thread per-actor GravityAdapterState
        // through here via a helper map (same shape as sNavStates in
        // EnSwDescriptor.cpp).
    }
}

extern "C" int Anchor_Enhance_EnSw_GazeOverride(EnSw* actor, PlayState* play,
                                                 Vec3f* outTargetPos) {
    // Returns 1 (true) if the hook selected a target position and
    // wrote it to *outTargetPos, meaning the caller should replace its
    // random `unk_444` assignment with `func_80B0DE34(this, outTargetPos)`.
    // Returns 0 (false) to let vanilla random gaze run unmodified.
    //
    // Phase 2 partial: canActiveAggro is DEFERRED — real body lands
    // alongside the En_Sw combat primitives adaptation (Reading A).
    // Currently always returns 0 so vanilla random gaze runs unchanged
    // even with CVar on.
    (void)actor;
    (void)play;
    (void)outTargetPos;
    return 0;
}
