#pragma once

// Pillar C2 Phase 1 — Lifecycle phase enum + transition validator.
//
// Replaces the per-EnemyNetId boolean state machine
// (`hasLocalDeath`, `defeatPacketSent`, `pendingNaturalDeath`,
// `deferredDeadItemDrop`) with a single explicit phase. The 5 booleans
// encode what is morphologically a single state machine; this header
// makes that state machine the source of truth.
//
// Plan: `Claude/Plans/pillar_c2_enemystatesync.md`. Phase 1 introduces
// the enum + validator and SHADOWS the existing booleans (writes go to
// both, reads still consult the booleans). Phase 1 ends by deleting the
// booleans and reading the phase directly.
//
// Phase model (explicit version of the implicit FSM in HookHandlers.cpp):
//
//     Alive
//       ├── DyingByLocal           (local OnEnemyDefeat)
//       │     ├── Dead             (actor->update == NULL after death anim)
//       │     └── Regrowing        (Karebaba host respawn detector)
//       ├── DyingByNetwork         (ENEMY_DEFEATED received, Karebaba)
//       │     └── Regrowing        (Karebaba non-host respawn detector)
//       ├── AwaitingDeadItemDrop   (OnActorSpawn finds netId in pendingKills, Karebaba)
//       │     └── DyingByNetwork   (OnActorInit fires SetupDeadItemDrop)
//       └── Dead                   (ENEMY_DEFEATED received, non-Karebaba; Actor_Kill)
//
//     Dead → Alive                 (scene re-init → fresh actor → fresh state)
//     Regrowing → Alive            (Karebaba reaches Idle)

#include <cstdint>

struct EnemyNetId;

namespace EnemyStateSync {

enum class LifecyclePhase : uint8_t {
    Alive,                  // default — health > 0, no death hint
    DyingByLocal,           // local kill in progress; ENEMY_DEFEATED sent
    DyingByNetwork,         // remote kill received; running natural cycle
    AwaitingDeadItemDrop,   // Karebaba: SetupDeadItemDrop deferred to OnActorInit
    Dead,                   // health == 0 confirmed, no more updates
    Regrowing,              // Karebaba respawn cycle, position resets
    Removed,                // taken out of scene by holder (carry-exit) —
                            // distinct from Dead in that no drop / break VFX
                            // should fire on receive. Recorded in SceneDeaths
                            // so the static placement is suppressed on re-entry.
                            // Producer: PlayerUpdate scene-exit-with-carry.
};

// Stable string for logging. Stable across ports — do NOT translate.
const char* LifecyclePhaseName(LifecyclePhase phase);

// Returns true if the transition is recognised by the state machine.
// Identity transitions (Alive→Alive, etc.) ARE valid — re-asserting a
// phase is not an error. Returns false on unrecognised transitions and
// emits a WARN with the from/to names so we can audit.
//
// The validator does not block the transition — caller flips the phase
// regardless. Validator only surfaces drift in the log so a future
// regression that skips a phase shows up immediately. This is the
// "transition validator" called out in the C2 plan Phase 1.
bool ValidatePhaseTransition(LifecyclePhase from, LifecyclePhase to);

// Update `state.phase` to `newPhase` and validate the transition.
//
// Phase 1 model: tracks alongside the legacy booleans without writing
// them. Each existing call site keeps its current boolean writes; this
// function adds the phase as a shadow signal that step 3's read-side
// assertions verify agrees with the booleans. Once the booleans are
// deleted at the end of Phase 1 this remains as the canonical phase
// write.
void TransitionTo(EnemyNetId& state, LifecyclePhase newPhase);

// Phase-derived predicates. The lifecycle-derivative ones become the
// canonical reads after Phase 1 deletes the booleans they shadow.
//
// NOTE — `defeatPacketSent` is NOT lifecycle-derivative. It tracks
// "did this client emit/own a defeat broadcast for this netId" and
// has different values for the same phase depending on send vs.
// receive path. Phase 1 keeps it as a real field; the predicate below
// exists only as a conservative fallback for callers that want a
// "non-Alive" answer. Not audited.
bool PhaseImpliesHasLocalDeath(LifecyclePhase phase);
bool PhaseImpliesDefeatPacketSent(LifecyclePhase phase);
bool PhaseImpliesPendingNaturalDeath(LifecyclePhase phase);
bool PhaseImpliesDeferredDeadItemDrop(LifecyclePhase phase);

// Phase 1 step 3 — read-side shadow audit. Compares each legacy boolean
// against the predicate derived from `state.phase` and logs a WARN the
// first time any field disagrees per (netId, fieldName). Rate-limited so
// a persistent drift surfaces once rather than per-frame. Pure
// diagnostic — does not mutate state.
//
// `siteTag` identifies the call site in the warning so we can locate the
// missed migration. Convention: short stable string like
// "OnActorUpdate.host.Karebaba.respawn" or "EnemyDefeated.dedup".
void AuditBooleansVsPhase(const EnemyNetId& state, const char* siteTag);

}  // namespace EnemyStateSync
