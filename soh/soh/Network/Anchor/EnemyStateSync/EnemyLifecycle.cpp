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
        case LifecyclePhase::Removed:               return "Removed";
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
                // Carry-exit: holder leaves scene while carrying. No drop,
                // no break VFX — actor silently vanishes from the world.
                case LifecyclePhase::Removed:              return true;
                default: return false;
            }
        case LifecyclePhase::Removed:
            // Terminal for the scene visit. Scene reset returns to Alive
            // mirror of the Dead → Alive edge for fresh spawns.
            return to == LifecyclePhase::Alive;
        case LifecyclePhase::AwaitingDeadItemDrop:
            // OnActorInit fires SetupDeadItemDrop which advances the actor
            // into the natural-death cycle.
            return to == LifecyclePhase::DyingByNetwork;
        case LifecyclePhase::DyingByLocal:
            // Death anim completes (actor->update == NULL) → Dead.
            // Karebaba respawn detector on host → Alive (the existing code
            // skips a separate Regrowing intermediate; the actor reaches
            // Idle and the flags clear in one step).
            //
            // DyingByLocal → DyingByNetwork: peer's local OnEnemyDefeat
            // fired because state-machine sync drove peer's actor health
            // to 0 (vanilla EnDekubaba_Hit transitions to ShrinkDie when
            // health == 0). When the authoritative ENEMY_DEFEATED packet
            // then arrives, the receive handler's dedup branch upgrades
            // the phase so Anchor_ShouldSuppress*Drop predicates (which
            // read PhaseImpliesPendingNaturalDeath) catch subsequent
            // dying-state actionFunc ticks and suppress the local
            // Item_DropCollectible call. Without this upgrade peer
            // double-drops: one local EN_ITEM00 + one host-broadcast
            // EN_ITEM00 for the same kill (field log 2026-05-06,
            // Inside Deku Tree).
            return to == LifecyclePhase::Dead ||
                   to == LifecyclePhase::Regrowing ||
                   to == LifecyclePhase::Alive ||
                   to == LifecyclePhase::DyingByNetwork;
        case LifecyclePhase::DyingByNetwork:
            // Karebaba respawn detector on non-host → Alive (same direct
            // transition as DyingByLocal — Regrowing is in the formal model
            // but the code consolidates it into the Alive transition).
            //
            // DyingByNetwork → Dead (2026-05-15 log 118 fix). The death
            // animation eventually completes and OnActorKill fires
            // (HookHandlers.cpp:2866/2875), which unconditionally calls
            // TransitionTo(Dead). Pre-fix this surfaced as the warning
            // "Unrecognised lifecycle transition DyingByNetwork -> Dead"
            // on every network-killed enemy whose anim finished locally.
            // Mirrors DyingByLocal → Dead (line 58) which has been
            // allowed since the table was first written.
            return to == LifecyclePhase::Regrowing ||
                   to == LifecyclePhase::Alive ||
                   to == LifecyclePhase::Dead;
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

    // Reset the peer-side network-drive-dying flag whenever the actor
    // returns to a live state. Karebaba respawn detector, scene re-init
    // replay, and any future regrow-class enemy all funnel through
    // TransitionTo(Alive) / TransitionTo(Regrowing); centralising the
    // reset here means per-actor respawn paths don't need to remember
    // to clear it. Without this, a respawned Karebaba whose previous
    // cycle's ENEMY_STATE carried health=0 would carry the suppression
    // flag forward and silently drop nothing on its next death.
    if (newPhase == LifecyclePhase::Alive ||
        newPhase == LifecyclePhase::Regrowing) {
        state.networkDriveDying = false;
    }

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
           phase == LifecyclePhase::Dead ||
           phase == LifecyclePhase::Removed;
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
