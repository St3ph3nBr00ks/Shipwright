#pragma once

// NPC Follower — balance tuning constants.
//
// Extracted from FollowerNPC.cpp per refactor B.2 (audit Tier B,
// 2026-06-01). Goal: give one place to discover the gameplay knobs
// without having to hunt through a 6,000-LoC TU. Implementation-detail
// timing (animation phase tables, fidget cadence, internal handshakes)
// stays inline with its consumer; only constants that someone tuning
// NPC behaviour would adjust live here.
//
// Names preserve their pre-refactor identifiers verbatim so consumer
// code in FollowerNPC.cpp keeps using bare names via the in-TU
// `using namespace FollowerNpcTunables;` directive.
//
// All ms values are converted to game ticks at call sites via
// `Anchor::Instance->MsToGameTicks(...)`, so the values stay correct at
// any framerate (20fps default vs unlocked).

#include "soh/Network/Anchor/Common/AILocomotion/NavStateTransitions.h"

extern "C" {
#include "z64.h"  // s8 type alias
}

namespace FollowerNpcTunables {

// --- Health / respawn ------------------------------------------------------
// NPC HP mirrors Link's heart capacity. kFollowerNpcMinHealth ensures
// the NPC has at least 3 hearts even before Link picks up his starting
// heart container. kFollowerNpcMaxHealth caps at OoT's 20 hearts (s8
// EnFollower::health field).
inline constexpr s8 kFollowerNpcMinHealth = 3;
inline constexpr s8 kFollowerNpcMaxHealth = 20;

// User-spec: 10s total from death to respawn. DeathHold (anim hold +
// ground lay) + RespawnCooldown ≈ 10s.
//
// 2026-06-05 — DeathHold bumped 3000→3500ms after field-test 406
// confirmed the death animation (gPlayerAnim_link_normal_back_downA,
// ~70 frames @ 20 ticks/s ≈ 3.5s including end-of-motion stillness)
// was being cut off ~10 frames early by the prior 3000ms hold. The
// RespawnCooldown is dropped 7000→6500ms in lockstep to keep the
// total death-to-respawn at the user-spec 10s.
inline constexpr int kFollowerNpcDeathHoldMs       = 3500;
inline constexpr int kFollowerNpcRespawnCooldownMs = 6500;
inline constexpr int kFollowerNpcDrowningMs        = 30000;  // Player default
inline constexpr float kFollowerNpcVoidThresholdY  = -3000.0f;

// --- IDLE / FOLLOW transition hysteresis -----------------------------------
// XZ distance pair with Y hysteresis (P0 audit / log 263 fix). Without
// Y gates, a leader directly above on a ledge made the actor falsely
// declare "arrived" and stop climbing.
//   - dist >= kEnterFollow → IDLE → FOLLOW
//   - dist <= kEnterIdle   → FOLLOW → IDLE
inline constexpr float kEnterFollow  = 80.0f;
inline constexpr float kEnterIdle    = 50.0f;
inline constexpr float kEnterIdleY   = 40.0f;
inline constexpr float kEnterFollowY = 60.0f;
inline constexpr AnchorAI::ThresholdPair kEnterIdleBand   = { kEnterIdle,   kEnterIdleY };
inline constexpr AnchorAI::ThresholdPair kEnterFollowBand = { kEnterFollow, kEnterFollowY };

// --- Locomotion ------------------------------------------------------------
// Walks when close to leader; runs when far. Speeds in OoT u/frame.
// Speed history (see FollowerNPC.cpp comment block near
// pre-refactor line 869): v1=12.0 → v2=8.0 → v3=6.4 → v4=5.12 →
// v5=5.376 (current — slightly slower than AI Player Follower per
// user 2026-05-20).
inline constexpr float kRunDistance = 250.0f;
inline constexpr float kWalkSpeed   = 5.04f;
inline constexpr float kRunSpeed    = 5.376f;

// --- Substrate path consumption + STUCK escalation ------------------------
// Path replan: re-query ComputePathTo every kPathRefreshMs OR when
// captured target moves > kPathRetargetDist (leader walked far enough
// that the path is stale). 500ms = 2Hz refresh cap.
inline constexpr int   kPathRefreshMs       = 500;
inline constexpr float kPathRetargetDist    = 60.0f;
inline constexpr float kAdvanceSubgoalDist  = 30.0f;
inline constexpr int   kStuckCheckMs        = 3000;   // matches player-Follower's tuned 3s
inline constexpr int   kFollowProgressLogMs = 5000;
inline constexpr float kStuckMinProgress    = 20.0f;
inline constexpr float kStuckNudgeDist      = 30.0f;
// STUCK escalation (ported from AI Player Follower's G12):
//   Cycle 1: legacy nudge toward leader
//   Cycle 2: edge-triggered navPath cursor advance
//   Cycle 3+: teleport to next subgoal OR leader
// Cycle counter decays after kStuckCycleWindowMs of no new STUCK.
inline constexpr int kStuckCycleWindowMs  = 3000;
inline constexpr int kStuckCycleEscalation = 3;

// --- Climb tunables -------------------------------------------------------
// Tuned to match Link's vanilla climb feel.
inline constexpr float kClimbSpeedY         = 2.0f;   // u/frame upward (halved 2026-05-19 from 4.0)
inline constexpr float kClimbSubgoalReach3D = 24.0f;  // advance cursor when within 3D
inline constexpr float kClimbXzSnap         = 1.0f;   // smooth XZ snap rate (per-frame fraction)
// Body offset from wall surface. Climb cells sit ON the climbable
// polygon; the NPC's world.pos is at body center, so snapping directly
// embeds the body half-into the wall. Offset along anchor.planeNormal
// (points OUT from wall) so the body sits in front of the wall.
inline constexpr float kClimbBodyOffset     = 12.0f;

// --- G-guard recovery teleports -------------------------------------------
// G10 leash: NPC distance > kNpcLeashDistance for > kNpcLeashTimeoutMs
// → teleport NPC to leader. Catches "stuck behind closed door / left
// in another scene / fell into untracked geometry."
inline constexpr float kNpcLeashDistance      = 1200.0f;
inline constexpr int   kNpcLeashTimeoutMs     = 2000;
// G14 close-fail: NPC in 200-1200u band (close enough G10 won't fire)
// but making < kNpcCloseFailProgressDelta progress across
// kNpcCloseFailTimeoutMs → teleport to current substrate subgoal.
inline constexpr float kNpcCloseFailMinDistance   = 200.0f;
inline constexpr float kNpcCloseFailMaxDistance   = 1200.0f;
inline constexpr int   kNpcCloseFailTimeoutMs     = 10000;
inline constexpr float kNpcCloseFailProgressDelta = 30.0f;

// --- Combat tuning --------------------------------------------------------
// Sheathe-delay: post-combat window where weapon stays visible (mirror
// of Player's vanilla "Link stays armed several seconds after combat").
// Without it, NPC visibly cycled weapon → empty hands → weapon between
// RANGED_ATTACK and STANDBY state transitions.
inline constexpr int kSheatheDelayMs = 4000;

}  // namespace FollowerNpcTunables
