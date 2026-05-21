/**
 * VerticalTeleport — bring navigators across vertical separations.
 *
 * Plan §9. Two integration shapes share a CVar:
 *
 *   Shape A (player-derived) — AI Player Follower / DummyPlayer / future Link-
 *   rigged ally NPCs ride OoT's climb state machine via injected input.
 *   The existing follower CLIMBING pipeline at HookHandlers.cpp produces
 *   the real climb animation, real physics, vine lateral tracking.
 *   Shape A is a thin coordinator; the actual mechanics stay in
 *   HookHandlers.cpp (commit 6b wraps the existing path; commit 6c adds
 *   hang-state resolution).
 *
 *   Shape B (non-player) — synced enemies and NPCs without a Link rig.
 *   No input pipeline → direct world.pos teleport. Fast path fires
 *   when target is a player with isClimbing=true; slow path requires
 *   |Δy| > threshold for delayFrames AND a climb path exists AND
 *   navigator hasn't teleported in the cooldown window.
 *
 * This commit (6a) lands Shape B. Shape A wiring lands in 6b. Hang-
 * state resolution lands in 6c.
 *
 * Default-off: gated by gEnhancements.Nav.Enabled AND
 * gEnhancements.Nav.VerticalTeleport. NavTraits.eligibleForVerticalTeleport
 * also gates per-actor (animation-anchored enemies opt out by default;
 * see NavTraits's MakeNoVerticalTeleportTraits override).
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §9
 *   - GitHub #206 Phase 1 commits 6a/6b/6c
 */

#pragma once

#include <cstdint>

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

// Shape B entry point — non-player navigator teleport.
//
// Inspects the navigator's vertical relationship to `target`. If
// conditions are met (see plan §9), teleports the navigator and
// returns true. Otherwise no-op and returns false.
//
// Two paths through the algorithm:
//
//   FAST PATH — bypasses delay + cooldown when target is a player
//   with the authoritative isClimbing flag set. Strict generalization
//   of the existing G1/G2 follower-teleport reactivity. Per-frame
//   re-fire while the climbing flag holds.
//
//   SLOW PATH — kicks in when |Δy(target, navigator)| persists above
//   `verticalTeleportYThreshold` for `verticalTeleportDelayFrames`
//   AND `FindNearestClimbable` returns a hit AND no teleport has
//   fired in the last 240 frames. One teleport per cooldown window.
//
// `target` may be nullptr — in that case the helper reads the held
// target via TargetSelection's per-navigator state (FixedPos kind).
// Returns true ONLY if a teleport actually fired this frame.
//
// Bosses categorically opt out: NavTraits.eligibleForVerticalTeleport
// is false in kBossDefaults so the early-return triggers before any
// position write.
bool TryVerticalTeleportEnemy(Actor* navigator, Actor* target, PlayState* play);

// True when the master Nav CVar is on AND Nav.VerticalTeleport is on.
bool IsVerticalTeleportEnabled();

// Predicate matching the existing climb-flag readers. Reads the local
// player's stateFlags1 OR the remote DummyPlayer's `isClimbing`
// AnchorClient field — same signal the follower G1/G2 path uses.
//
// Returns false for non-player actors (enemies have no climbing state
// signal; the slow path is the only avenue for them).
bool IsTargetClimbing(const Actor* target);

// True when `actor` is a Link-rigged navigator (Shape A applies);
// false otherwise (Shape B applies). v1 list: ACTOR_PLAYER and
// ACTOR_EN_OE2 (DummyPlayer / AI Player Follower). Future Link-rigged ally
// NPCs join via additional entries.
bool IsPlayerDerivedNavigator(const Actor* actor);

// ── Shape A — player-derived navigator climb (commit 6b) ───────────
//
// The existing follower CLIMBING pipeline at HookHandlers.cpp:2241-
// 2260 (entry) and :2800-2831 (state body) IS Shape A: XZ-only snap
// on rising edge, raw stick injection while leader.isClimbing,
// dismount forward-hold on falling edge. This commit does NOT
// re-implement that mechanism — the existing logic produces the
// real climb animation, real physics, and vine lateral tracking,
// and any rewrite would risk regressing those behaviours.
//
// Instead, the nav system exposes Shape A as a discoverable concept:
// callers that want to know "should the navigator route through the
// follower-style input-injection climb" check `IsShapeAEligible(actor)`
// before falling through to Shape B's `TryVerticalTeleportEnemy`.
// The two are mutually exclusive: an actor is either Link-rigged
// (Shape A) or it isn't (Shape B). Bosses opt out of both by
// kBossDefaults.
//
// `IsShapeAEligible` returns true when:
//   - master Nav CVar is on (so the system as a whole is active)
//   - Nav.VerticalTeleport per-feature CVar is on
//   - the actor is a player-derived navigator
//   - NavTraits.eligibleForVerticalTeleport is true (default for
//     non-bosses; false for bosses)
//
// When false, callers should bypass Shape A and use the legacy follower
// CVar gates (gRemoteAnchor.FollowerEnabled etc.) for the existing
// pipeline. With master CVar off, the existing pipeline runs untouched
// — the nav system never blocks legacy behaviour.
bool IsShapeAEligible(const Actor* actor);

// ShipInit registration. Currently a no-op (no per-frame hooks at
// this layer); kept for parity with sibling modules.
void RegisterVerticalTeleport();

} // namespace AnchorNav
