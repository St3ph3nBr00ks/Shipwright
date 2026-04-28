#include "EnemyLifecycle.h"

#include "soh/Network/Anchor/Anchor.h"  // EnemyNetId definition
#include <libultraship/libultraship.h>  // SPDLOG_*
#include <unordered_map>
#include <unordered_set>
#include <string>

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
            // Karebaba respawn detector on host → Regrowing.
            return to == LifecyclePhase::Dead || to == LifecyclePhase::Regrowing;
        case LifecyclePhase::DyingByNetwork:
            // Karebaba respawn detector on non-host → Regrowing.
            return to == LifecyclePhase::Regrowing;
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
    return phase == LifecyclePhase::DyingByLocal ||
           phase == LifecyclePhase::AwaitingDeadItemDrop ||
           phase == LifecyclePhase::Dead;
}

bool PhaseImpliesDefeatPacketSent(LifecyclePhase phase) {
    return phase != LifecyclePhase::Alive && phase != LifecyclePhase::Regrowing;
}

bool PhaseImpliesPendingNaturalDeath(LifecyclePhase phase) {
    return phase == LifecyclePhase::DyingByNetwork ||
           phase == LifecyclePhase::AwaitingDeadItemDrop;
}

bool PhaseImpliesDeferredDeadItemDrop(LifecyclePhase phase) {
    return phase == LifecyclePhase::AwaitingDeadItemDrop;
}

void AuditBooleansVsPhase(const EnemyNetId& state, const char* siteTag) {
    // Rate-limit: first warning per (netId, fieldName, siteTag) only.
    // Map key: netId; value: set of "<field>@<siteTag>" strings already warned.
    static std::unordered_map<uint32_t, std::unordered_set<std::string>> sWarned;

    auto warnOnce = [&](const char* fieldName, bool boolValue, bool impliedValue) {
        if (boolValue == impliedValue) return;
        std::string key = std::string(fieldName) + "@" + (siteTag ? siteTag : "<null>");
        auto& fieldsForId = sWarned[state.netId];
        if (fieldsForId.insert(key).second) {
            SPDLOG_WARN("[EnemyStateSync] phase mismatch at {} netId={} {}: bool={} phase={}({}=>implied={})",
                        siteTag ? siteTag : "<null>", state.netId, fieldName,
                        boolValue, LifecyclePhaseName(state.phase), fieldName, impliedValue);
        }
    };

    warnOnce("hasLocalDeath",
             state.hasLocalDeath,
             PhaseImpliesHasLocalDeath(state.phase));
    warnOnce("defeatPacketSent",
             state.defeatPacketSent,
             PhaseImpliesDefeatPacketSent(state.phase));
    warnOnce("pendingNaturalDeath",
             state.pendingNaturalDeath,
             PhaseImpliesPendingNaturalDeath(state.phase));
    warnOnce("deferredDeadItemDrop",
             state.deferredDeadItemDrop,
             PhaseImpliesDeferredDeadItemDrop(state.phase));
}

}  // namespace EnemyStateSync
