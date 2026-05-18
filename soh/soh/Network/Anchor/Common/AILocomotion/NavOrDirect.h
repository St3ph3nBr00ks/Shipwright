/**
 * NavOrDirect — shared substrate-path consumption helper for the three
 * AI actors (Player AI Follower, NPC Follower, AI Invader).
 *
 * Replaces the inline substrate-path consumption code that previously
 * lived independently in each actor's TickFOLLOW / TickENGAGE. The
 * three implementations had drifted apart: AI Invader's ENGAGE used
 * direct-yaw (no substrate at all), NPC Follower's ENGAGE same, and
 * Player AI Follower had two parallel implementations across
 * Follower.cpp and HookHandlers.cpp.
 *
 * This module consolidates the navigation policy into one decision:
 * "given my current position, my target, and my nav-state bookkeeping,
 * what subgoal should I move toward this tick, and what fallback if
 * the substrate can't reach the target?"
 *
 * Mechanism-agnostic. Returns a Vec3f subgoal and a fallback enum.
 * Callers translate the subgoal into their own motion mechanism:
 *   - NPC Follower / AI Invader: write speedXZ + world.rot.y.
 *   - Player AI Follower: convert to camera-relative stick input + inject.
 *
 * The 60u universal direct-yaw threshold lives here. The water-gate
 * (skip substrate when in water) lives here. The fallback selection
 * (RangedInPlace / ReturnToLeader / RetreatHostile / HoldDefensive)
 * is policy-driven by the caller via `FallbackPolicy`.
 *
 * Companion docs:
 *   Claude/Plans/ai_actor_parity_plan.md          (§4.2 helper API)
 *   Claude/Plans/ai_actor_parity_matrix.md        (§2 — ENGAGE substrate)
 *   Claude/Plans/ai_actor_gate_registry.md
 *
 * Reference implementations this module consolidates:
 *   FollowerNPC.cpp:980-1034   — NPC Follower TickFOLLOW (most current).
 *   Invader.cpp:838-933        — AI Invader TickFOLLOW.
 *   Follower.cpp:4080-4124     — Player AI Follower pursuit planner.
 */

#pragma once

#include <cstdint>

#include "../ActorTrail.h"  // AnchorNav::TrailKey + ActorTrail::NavPath

extern "C" {
#include "z64.h"
}

namespace AnchorAI {

// Universal direct-yaw threshold. Inside this radius, the caller uses
// direct-yaw locomotion regardless of nav-mesh availability — the
// substrate path planner's fine-grained obstacle avoidance is not
// worth the BFS cost at close range, and most strike-range combat
// (≤70u for NPC Follower / AI Invader, similar for Player Follower)
// already overlaps this band.
//
// Field-tuned at 60u (2026-05-18). Below this, callers should write
// direct-yaw motion targeting the actor's actual target position,
// not a path subgoal.
constexpr float kDirectYawThreshold = 60.0f;
constexpr float kDirectYawThresholdSq = kDirectYawThreshold * kDirectYawThreshold;

// Water-surface threshold for forcing direct-yaw. When an actor is in
// water deep enough that vanilla Link would be swimming, the floor-
// derived nav mesh is invalid (water surface isn't in the BFS graph).
// In that case the actor falls back to direct-yaw regardless of
// distance; callers with a dedicated SWIMMING state should be in that
// state already.
constexpr float kWaterDepthThreshold = 16.0f;

// ---------------------------------------------------------------------
// Fallback selection.
// ---------------------------------------------------------------------

// What the actor should DO when the substrate path is empty AND the
// target is outside kDirectYawThreshold. The caller specifies which
// fallback class is acceptable for its actor type; the helper returns
// the FIRST one that applies given the current actor state.
enum class PathEmptyFallback : uint8_t {
    DirectYaw,         // dist3D ≤ kDirectYawThreshold; just yaw toward target
    RangedInPlace,     // outside threshold, actor has ranged weapon → fire from here
    ReturnToLeader,    // outside threshold, friendly actor → switch to FOLLOW(leader)
    RetreatHostile,    // outside threshold, hostile no-ranged → compute retreat path
    HoldDefensive,     // retreat path also empty → hold ground in STANDBY
};

// Caller-supplied policy. Each actor type configures which fallbacks
// apply (Player Follower / NPC Follower never use RetreatHostile;
// AI Invader never uses ReturnToLeader; etc.).
//
// Multiple fallback classes can be true simultaneously; the helper picks
// the highest-priority applicable one. Priority order: DirectYaw (inside
// threshold) > RangedInPlace (if available) > ReturnToLeader / RetreatHostile
// (actor-class specific) > HoldDefensive.
struct FallbackPolicy {
    bool hasRangedReady    = false;  // actor has bow/slingshot + ammo + LOS to target
    bool isFriendlyActor   = false;  // Player Follower or NPC Follower
    bool isHostileActor    = false;  // AI Invader (mutually exclusive with isFriendlyActor)
    // Optional override: skip nav-mesh entirely (force direct-yaw + fallback selection).
    // Used by callers that have local knowledge of nav-data unavailability
    // (e.g. unscanned scene).
    bool forceDirectYaw    = false;
};

// ---------------------------------------------------------------------
// NavState — caller-owned bookkeeping shared by FOLLOW + ENGAGE.
// ---------------------------------------------------------------------

// Per-actor instance of nav-bookkeeping. Each AI actor instantiates one
// of these as a file-static or member, then passes it to ChooseSubgoal()
// each tick.
//
// The helper mutates `path` (Reset / Advance / ComputePathTo populate)
// and `lastPathRefreshFrame` / `lastPathTargetPos` (rate-limit tracking).
// Caller treats these fields as opaque except for `path.Reset()` on
// state-change boundaries (e.g. FOLLOW → IDLE transition).
struct NavState {
    AnchorNav::ActorTrail::NavPath path;
    uint64_t                       lastPathRefreshFrame = 0;
    Vec3f                          lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
    AnchorNav::TrailKey            trailKey             = 0;
};

// ---------------------------------------------------------------------
// Refresh tuning — single source of truth across all actors.
// ---------------------------------------------------------------------

// Path refresh rate-limit. Substrate `ComputePathTo` is the most expensive
// per-tick operation in AI dispatch (Layer 3 BFS over RoomNavData), so we
// refresh at most every kPathRefreshMs wall-clock ms. Between refreshes,
// the cached path's cursor advances on subgoal-proximity.
constexpr int kPathRefreshMs = 500;

// Target-drift refresh: if the target moves more than this distance from
// where it was at the last refresh, refresh the path early (don't wait
// for the rate-limit). Prevents stale paths when the target zigzags.
constexpr float kTargetDriftRefresh    = 100.0f;
constexpr float kTargetDriftRefreshSq  = kTargetDriftRefresh * kTargetDriftRefresh;

// Cursor advance distance. When the actor is within this XZ distance of
// the current subgoal, advance to the next waypoint. 30u is roughly half
// the nav-grid cell pitch (60u), so advance fires once per cell.
constexpr float kAdvanceSubgoalDist    = 30.0f;
constexpr float kAdvanceSubgoalDistSq  = kAdvanceSubgoalDist * kAdvanceSubgoalDist;

// ---------------------------------------------------------------------
// API.
// ---------------------------------------------------------------------

// Result of one ChooseSubgoal() call. Caller consumes `subgoal` for
// locomotion and `fallbackEngaged` for state-machine transitions.
struct NavOrDirectResult {
    // World-space position the actor should move toward this tick.
    // When usingNavMesh = true: this is the current path subgoal
    // (possibly modified by cursor advance this tick).
    // When usingNavMesh = false: this is the target position itself
    // (direct-yaw fallback).
    Vec3f subgoal = { 0.0f, 0.0f, 0.0f };

    // True if the subgoal came from the substrate path (Layer 1/2/3).
    // False if the substrate had nothing usable and we fell back.
    bool usingNavMesh = false;

    // Which fallback was selected when usingNavMesh = false. Always
    // PathEmptyFallback::DirectYaw when usingNavMesh = true.
    PathEmptyFallback fallbackEngaged = PathEmptyFallback::DirectYaw;

    // Rising-edge flag: this tick is the FIRST tick the fallback engaged
    // (the previous tick either was inside threshold or had a non-empty
    // path). State-machine transitions key off this flag to avoid
    // re-firing every tick.
    bool fallbackJustEngaged = false;

    // Optional: subgoal carries climb-cell or other special flags from
    // the path. Caller may inspect to drive FOLLOW → CLIMBING / etc.
    // transitions. Zero when usingNavMesh = false.
    uint32_t subgoalFlags = 0;
};

// Single entry point. Decision-only — does NOT modify the actor's
// position, rotation, or speed. Caller applies subgoal via its own
// locomotion mechanism.
//
// Behavior:
//   1. If dist3D(navigator, target) ≤ kDirectYawThreshold:
//      Return DirectYaw fallback with subgoal = targetPos. Skip substrate.
//   2. If navigator in water (yDistToWater > kWaterDepthThreshold):
//      Return DirectYaw fallback with subgoal = targetPos. Skip substrate.
//      (Caller should be in SWIMMING state already; this is a defensive
//      check in case they're not.)
//   3. If policy.forceDirectYaw: same as (1) but evaluates fallback policy
//      for the post-threshold case (caller's own gate.).
//   4. Otherwise:
//      a. Refresh path if needed (rate-limit OR target drift).
//      b. If path empty: select fallback based on policy. Return.
//      c. Advance cursor on subgoal proximity.
//      d. Return path's current subgoal with usingNavMesh = true.
//
// The caller's NavState retains the path + bookkeeping across ticks.
NavOrDirectResult ChooseSubgoal(const Actor*         navigator,
                                 const Vec3f&         targetPos,
                                 NavState&            navState,
                                 const FallbackPolicy& policy,
                                 PlayState*           play);

// Diagnostic counter — incremented every time ChooseSubgoal returns
// usingNavMesh=false. Reset to zero at scene transition. Exposed via
// `GetFallbackInvocationCount()` for the ImGui debug overlay.
uint32_t GetFallbackInvocationCount();
void     ResetFallbackInvocationCount();

}  // namespace AnchorAI
