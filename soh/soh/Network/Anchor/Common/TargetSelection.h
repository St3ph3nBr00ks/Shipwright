/**
 * TargetSelection — sticky target acquisition with hysteresis.
 *
 * The navigation system's "decide who to chase" layer. Any navigator
 * (synced enemy, AI Player Follower, future ally NPC) calls AcquireOrHoldTarget
 * to pick a target from a candidate pool (players, enemies, custom set);
 * the result is held for `targetStickyFrames` (per-actor NavTraits row,
 * default 120 ≈ 2s @ 60fps) before re-evaluation. Re-evaluation only
 * fires when (a) held target dies / despawns, (b) sticky timer expires,
 * (c) held target is meaningfully further than another candidate
 * (hysteresis-gated, 0.8× factor).
 *
 * Per-navigator state lives in EnemyNetId (Anchor.h) — see
 * navHeldKind / navTargetClientId / navTargetNetId / navHeldTargetPos /
 * navTargetTimerFrames.
 *
 * Default-off: gated by gEnhancements.Nav.Enabled AND
 * gEnhancements.Nav.TargetSelection. With TargetSelection off, callers
 * fall back to `FindNearestPlayerActor`-style nearest-of-set lookups
 * via the helper.
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §6
 *   - GitHub #206 Phase 1 commit 3
 *   - feedback memory: feedback_nav_helpers_entity_agnostic.md
 */

#pragma once

#include <cstdint>
#include <vector>

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

// Candidate pool the navigator is searching against.
enum class TargetSet : uint8_t {
    Players,        // local player + remote DummyPlayers (cross-timeline filtered)
    Enemies,        // syncable enemies across all 8 sync categories
    EnemiesNoBoss,  // Enemies minus IsSyncedBossActor matches
    Custom,         // caller provides candidate vector via SetCustomCandidates
};

struct TargetChoice {
    Actor* actor       = nullptr; // nullptr if no target
    Vec3f  fallbackPos = { 0.0f, 0.0f, 0.0f }; // valid even when actor is nullptr (last-known)
    bool   isStale     = false;   // true if held past sticky timer with no new acquisition
};

// Acquire or hold a target for `navigator` against the given set.
//
// First call (or after ResetTargetLock): evaluates nearest member of `set`
// and binds it to the navigator's EnemyNetId state. Subsequent calls return
// the held target until invalidated. See plan §6 for full algorithm.
//
// `play` may be nullptr; in that case falls through to legacy nearest-of-set.
//
// When the master Nav CVar is off OR Nav.TargetSelection is off OR the
// navigator's NavTraits.useStickyTargeting is false, behaves like a pure
// nearest-of-set lookup with no state mutation.
Actor* AcquireOrHoldTarget(Actor* navigator, TargetSet set, PlayState* play);

// Same as AcquireOrHoldTarget but returns the rich TargetChoice tuple
// (actor, fallbackPos, isStale). Useful for callers that want to know
// whether they're chasing a stale held target.
TargetChoice ChooseTarget(Actor* navigator, TargetSet set, PlayState* play);

// Pin this navigator to a fixed world position instead of an actor target.
// Used for waypoint-driven navigators (Goroiwa-class, scripted NPC walks).
// The held position counts as a "valid target" for downstream consumers.
//
// Pass nullptr to clear the held position.
void HoldPositionTarget(Actor* navigator, const Vec3f* targetPos);

// Returns the clientId of the held target IF the held target is a player.
// Returns 0xFF if the navigator has no target, or the target is not a player.
// Used by ActorTrail consumers that need to look up the right trail.
uint8_t GetHeldTargetClientId(const Actor* navigator);

// Returns the netId of the held target IF the held target is an enemy.
// Returns 0 if the navigator has no target, or the target is not an enemy.
uint32_t GetHeldTargetNetId(const Actor* navigator);

// Force re-evaluation on next call (e.g. on attack-cycle boundary, on
// follower state-machine transition, on target death notified by hook).
void ResetTargetLock(Actor* navigator);

// For TargetSet::Custom — caller supplies the candidate vector before
// calling AcquireOrHoldTarget. Cleared after the call.
void SetCustomCandidates(Actor* navigator, const std::vector<Actor*>& candidates);

// True when the master Nav CVar is on AND Nav.TargetSelection is on.
bool IsTargetSelectionEnabled();

// ShipInit registration. Registers an OnExitGame clear hook so the
// per-navigator custom-candidate store doesn't leak across save loads.
void RegisterTargetSelection();


} // namespace AnchorNav
