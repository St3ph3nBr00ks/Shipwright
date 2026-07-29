/**
 * NavConsumer — Phase 2 real implementation.
 *
 * Delegates to Common/AILocomotion/{NavOrDirect,ScriptedFollow,
 * NavStateTransitions} — the same substrate NPC Follower / NPC
 * Invader use. Descriptor supplies params (climb mask, speeds,
 * ranges) via NavConsumeParams; consumer owns per-actor NavState
 * bookkeeping (path cache + refresh tracking).
 *
 * The helper is intentionally generic — no En_Sw-specific state
 * machine awareness lives here. Descriptors are responsible for
 * gating (in their OnNavTick override) so this only runs when the
 * vanilla state machine permits nav-driven movement.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.6 + §7 Phase 2 step 1.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before NavConsumer.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "NavConsumer.h"

#include "soh/Network/Anchor/Common/AILocomotion/NavOrDirect.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"

#include <cmath>
#include <libultraship/bridge/consolevariablebridge.h>

namespace AnchorEnemyEnhancement {

namespace {

// Compute yaw (Y-axis) from `from` toward `to`. Matches
// AnchorYaw::YawTowardTarget shape used by NPC Follower / NPC Invader.
s16 YawTowardXZ(const Vec3f& from, const Vec3f& to) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    return (s16)(std::atan2(dx, dz) * (0x8000 / M_PI));
}

}  // namespace

bool TickNavMovement(EnemyEnhancementDescriptor& descriptor,
                     NavConsumerState& state,
                     Actor* actor,
                     PlayState* play) {
    if (actor == nullptr || play == nullptr) return false;

    // CVar gate — descriptor supplies the CVar name; nullptr means
    // "always on when Capabilities().canNavConsume is true" (v1 always
    // supplies a name so this branch is defensive).
    const char* cvarName = descriptor.NavConsumeCVar();
    if (cvarName != nullptr && CVarGetInteger(cvarName, 0) == 0) {
        return false;
    }

    // Find nearest valid player. NULL means all players are in cutscene
    // or dead — vanilla behaviour continues.
    Actor* target = FindNearestPlayerActor(actor, play);
    if (target == nullptr) return false;

    const NavConsumeParams params = descriptor.NavParams();

    // Distance gating.
    const float dx = target->world.pos.x - actor->world.pos.x;
    const float dz = target->world.pos.z - actor->world.pos.z;
    const float distXZSq = dx * dx + dz * dz;
    const float detectSq = params.detectRange * params.detectRange;
    if (distXZSq > detectSq) return false;  // out of detect range

    const float attackSq = params.attackRange * params.attackRange;
    if (distXZSq <= attackSq) {
        // Within attack range — let vanilla lunge / bite run this tick.
        // Zero speed so the actor holds position pending vanilla combat.
        actor->speedXZ = 0.0f;
        return true;  // we consumed the tick even though no movement
    }

    // ---- Call NavOrDirect substrate --------------------------------
    // FallbackPolicy — enhanced enemies are hostile; direct-yaw when
    // path empty (matches NPC Invader shape).
    AnchorAI::FallbackPolicy policy;
    policy.isHostileActor = true;
    policy.isFriendlyActor = false;
    policy.hasRangedReady  = false;

    // Cheap per-actor trail-key seed. Uses the actor pointer's low bits;
    // consistent across ticks for the same actor. NPC Follower / Invader
    // key by their target's clientId, but for shared enemy substrate
    // the actor's own identity is fine — trail memoization is per
    // navigator anyway.
    state.trailKey = (AnchorNav::TrailKey)((uintptr_t)actor & 0xFFFFFFFFu);

    AnchorAI::NavOrDirectResult nav =
        AnchorAI::ChooseSubgoal(actor, target->world.pos, state, policy, play);

    // ---- Apply locomotion -------------------------------------------
    const Vec3f& subgoal = nav.subgoal;
    const s16 yaw = YawTowardXZ(actor->world.pos, subgoal);
    actor->shape.rot.y = yaw;
    actor->world.rot.y = yaw;

    // Speed selection — walk when close, run when far. Simple two-band
    // model; per-actor NavParams already carries the values. No
    // catch-up bonus (unlike NPC Follower which tracks its owner) —
    // enemy pursuit doesn't need to hover at a stable distance.
    const float dist = std::sqrt(distXZSq);
    const float threshold = (params.detectRange + params.attackRange) * 0.5f;
    actor->speedXZ = (dist > threshold) ? params.runSpeed : params.walkSpeed;

    return true;
}

}  // namespace AnchorEnemyEnhancement
