/**
 * GravityAdapter — Phase 2 real implementation.
 *
 * Applies simple gravity + terminal-velocity clamp to an enhanced
 * enemy when the descriptor's ShouldApplyGravity returns true. Uses
 * vanilla `bgCheckFlags & 0x1` as the on-ground signal — same
 * predicate vanilla actor code uses for its own airborne detection.
 *
 * Stun-on-land: when the actor transitions airborne → grounded and
 * the descriptor's GravityParams.stunOnLand is true, we write the
 * stun-frame count to state.stunFramesRemaining. Caller (via
 * descriptor's OnLandedFromFall override) reads this and applies its
 * own "hold still" gate.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.7 + §7 Phase 2 step 2.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before GravityAdapter.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "GravityAdapter.h"

#include <libultraship/bridge/consolevariablebridge.h>

namespace AnchorEnemyEnhancement {

bool TickGravity(EnemyEnhancementDescriptor& descriptor,
                 GravityAdapterState& state,
                 Actor* actor,
                 PlayState* play) {
    if (actor == nullptr || play == nullptr) return false;

    // CVar gate — descriptor supplies the CVar name.
    const char* cvarName = descriptor.GravityAwareCVar();
    if (cvarName != nullptr && CVarGetInteger(cvarName, 0) == 0) {
        return false;
    }

    const GravityAwareParams params = descriptor.GravityParams();

    // On-ground check via vanilla bgCheckFlags bit 0x1.
    const bool onGround = (actor->bgCheckFlags & 0x1) != 0;

    // Landing edge detection — airborne last frame, grounded this frame.
    if (onGround && state.wasAirborneLastFrame) {
        // Zero downward velocity so vanilla update doesn't keep pushing
        // the actor into the floor.
        if (actor->velocity.y < 0.0f) {
            actor->velocity.y = 0.0f;
        }
        // Arm stun timer if configured.
        if (params.stunOnLand) {
            state.stunFramesRemaining = (uint16_t)params.stunFrames;
        }
        state.wasAirborneLastFrame = false;
        descriptor.OnLandedFromFall(actor, play);
        return true;
    }

    if (onGround) {
        // Steady state — grounded. Decrement stun timer if active. No
        // velocity write; vanilla actor keeps its own XZ locomotion.
        if (state.stunFramesRemaining > 0) {
            state.stunFramesRemaining--;
            return true;
        }
        state.wasAirborneLastFrame = false;
        return false;
    }

    // Airborne — apply gravity + terminal-velocity clamp.
    actor->velocity.y += params.gravity;
    if (actor->velocity.y < params.maxFallSpeed) {
        actor->velocity.y = params.maxFallSpeed;
    }
    // Integrate world position from velocity. Vanilla Actor_MoveXZGravity
    // would do this but we avoid calling it — many enhanced enemies
    // have their own custom position integration that we don't want to
    // interfere with. Direct Y integration is sufficient for the
    // fall-through-air case.
    actor->world.pos.y += actor->velocity.y;

    state.wasAirborneLastFrame = true;
    return true;
}

}  // namespace AnchorEnemyEnhancement
