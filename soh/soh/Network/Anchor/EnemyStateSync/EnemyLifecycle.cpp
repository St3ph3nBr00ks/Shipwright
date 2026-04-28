#include "EnemyLifecycle.h"

#include "soh/Network/Anchor/Anchor.h"  // EnemyNetId definition
#include <libultraship/libultraship.h>  // SPDLOG_*

namespace EnemyStateSync {

const char* LifecyclePhaseName(LifecyclePhase phase) {
    switch (phase) {
        case LifecyclePhase::Alive:                 return "Alive";
        case LifecyclePhase::DyingByLocal:          return "DyingByLocal";
        case LifecyclePhase::DyingByNetwork:        return "DyingByNetwork";
        case LifecyclePhase::AwaitingDeadItemDrop:  return "AwaitingDeadItemDrop";
        case LifecyclePhase::Dead:                  return "Dead";
        case LifecyclePhase::Regrowing:             return "Regrowing";
    }
    return "<unknown>";
}

namespace {

// Encodes the transition table from the C2 plan. Returns true when
// (from -> to) is a recognised edge. Identity transitions are always
// allowed (re-asserting the same phase is not an error).
bool IsRecognisedTransition(LifecyclePhase from, LifecyclePhase to) {
    if (from == to) return true;
    switch (from) {
        case LifecyclePhase::Alive:
            switch (to) {
                case LifecyclePhase::DyingByLocal:         return true;
                case LifecyclePhase::DyingByNetwork:       return true;
                case LifecyclePhase::Dead:                 return true;
                case LifecyclePhase::AwaitingDeadItemDrop: return true;
                default: return false;
            }
        case LifecyclePhase::AwaitingDeadItemDrop:
            // OnActorInit fires SetupDeadItemDrop which advances the actor
            // into the natural-death cycle.
            return to == LifecyclePhase::DyingByNetwork;
        case LifecyclePhase::DyingByLocal:
            // Death anim completes (actor->update == NULL) → Dead.
            // Karebaba respawn detector on host → Alive (the existing code
            // skips a separate Regrowing intermediate; the actor reaches
            // Idle and the flags clear in one step).
            return to == LifecyclePhase::Dead ||
                   to == LifecyclePhase::Regrowing ||
                   to == LifecyclePhase::Alive;
        case LifecyclePhase::DyingByNetwork:
            // Karebaba respawn detector on non-host → Alive (same direct
            // transition as DyingByLocal — Regrowing is in the formal model
            // but the code consolidates it into the Alive transition).
            return to == LifecyclePhase::Regrowing ||
                   to == LifecyclePhase::Alive;
        case LifecyclePhase::Dead:
            // Scene re-init produces a fresh actor → reset to Alive.
            return to == LifecyclePhase::Alive;
        case LifecyclePhase::Regrowing:
            // Karebaba reaches Idle → Alive.
            return to == LifecyclePhase::Alive;
    }
    return false;
}

}  // namespace

bool ValidatePhaseTransition(LifecyclePhase from, LifecyclePhase to) {
    if (IsRecognisedTransition(from, to)) {
        return true;
    }
    SPDLOG_WARN("[EnemyStateSync] Unrecognised lifecycle transition {} -> {}",
                LifecyclePhaseName(from), LifecyclePhaseName(to));
    return false;
}

void TransitionTo(EnemyNetId& state, LifecyclePhase newPhase) {
    const LifecyclePhase oldPhase = state.phase;
    ValidatePhaseTransition(oldPhase, newPhase);
    state.phase = newPhase;

    // Phase 1: phase tracks alongside the legacy booleans without writing
    // them. Each existing call site keeps its current boolean writes — this
    // function adds the phase as a shadow signal so step 3's read-side
    // assertions can verify the two representations agree at every site.
    // Once the booleans are deleted at the end of Phase 1, this body
    // remains as the canonical phase write.
}

bool PhaseImpliesHasLocalDeath(LifecyclePhase phase) {
    // True for ANY death state — the field's purpose is to block
    // ENEMY_UPDATE health overrides on a dying/dead actor regardless of
    // whether the kill was local or network. DyingByNetwork (Karebaba
    // natural cycle from received ENEMY_DEFEATED) explicitly sets
    // hasLocalDeath=true at the existing write site, so the audit
    // correctly expects it true.
    return phase == LifecyclePhase::DyingByLocal ||
           phase == LifecyclePhase::DyingByNetwork ||
           phase == LifecyclePhase::AwaitingDeadItemDrop ||
           phase == LifecyclePhase::Dead;
}

bool PhaseImpliesDefeatPacketSent(LifecyclePhase phase) {
    // NOTE — this predicate is not used by AuditBooleansVsPhase. The
    // C2 plan originally proposed `defeatPacketSent ≡ phase != Alive`,
    // but `defeatPacketSent` tracks *broadcast ownership* on the local
    // client (did THIS client send/receive a defeat packet for the
    // netId), not lifecycle state. Two paths yield the same phase with
    // different boolean values:
    //   - DyingByLocal:    defeatPacketSent=true (we sent the broadcast)
    //   - DyingByNetwork (received ENEMY_DEFEATED, no rebroadcast):
    //                       defeatPacketSent=false (we didn't send)
    // After Phase 1 finishes, defeatPacketSent stays as a non-derivative
    // field — likely migrated into Phase 2's HostBookkeeping module
    // alongside sentDefeatThisScene rather than EnemyState. This
    // predicate exists only as a fallback for future code that wants a
    // conservative "is the actor in any non-Alive state" answer.
    return phase != LifecyclePhase::Alive && phase != LifecyclePhase::Regrowing;
}

bool PhaseImpliesPendingNaturalDeath(LifecyclePhase phase) {
    return phase == LifecyclePhase::DyingByNetwork ||
           phase == LifecyclePhase::AwaitingDeadItemDrop;
}

bool PhaseImpliesDeferredDeadItemDrop(LifecyclePhase phase) {
    return phase == LifecyclePhase::AwaitingDeadItemDrop;
}

void AuditBooleansVsPhase(const EnemyNetId& /*state*/, const char* /*siteTag*/) {
    // Phase 1 step 5c — all lifecycle-derivative booleans
    // (hasLocalDeath, pendingNaturalDeath, deferredDeadItemDrop) have
    // been deleted; reads now use PhaseImplies* directly. defeatPacketSent
    // is broadcast-ownership, not lifecycle, and was never audited here.
    // The function is retained as a no-op so the existing call sites
    // (12 of them across HookHandlers / Packets) compile without
    // touching every site again. They serve as breakpoints if a future
    // bug needs site-tagged tracing — a single edit here re-enables
    // diagnostic output. Otherwise the calls inline-optimise to nothing
    // in release builds.
}

}  // namespace EnemyStateSync
