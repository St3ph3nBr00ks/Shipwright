#pragma once

// NPC Invader — balance tuning constants.
//
// Extracted from Invader.cpp per refactor B.2 (audit Tier B,
// 2026-06-01). Sibling of FollowerNpcTunables.h.
//
// Names preserve their pre-refactor identifiers verbatim so consumer
// code in Invader.cpp keeps using bare names via the in-TU
// `using namespace InvaderTunables;` directive.
//
// All ms values are converted to game ticks at call sites via
// `Anchor::Instance->MsToGameTicks(...)`.

#include "soh/Network/Anchor/Common/AILocomotion/NavStateTransitions.h"

namespace InvaderTunables {

// --- Combat — sword AT collider ---------------------------------------
// Engagement distance (ATTACK strike) + active-frame window of the
// sword swing animation. AT quad geometry constants
// (kAttackQuadForward / HalfWidth / BaseY / TopY) removed 2026-06-02
// (#238) — the quad is now built per-frame from the actual blade
// tip/base positions via Anchor_BuildAtQuadFromBlade rather than
// using fixed forward-distance vertices.
inline constexpr float kAttackEngageDist       = 80.0f;
inline constexpr float kAttackActiveStartFrame = 4.0f;
inline constexpr float kAttackActiveEndFrame   = 12.0f;

// --- Engagement bands -----------------------------------------------------
// Detection / break / strike thresholds. Bumped 250→1000 on 2026-05-17
// (log 233 testing) so the Invader engages at visual-LOS range, not
// only when the target gets close. Y break gate is wider than the
// Follower's so the Invader pursues across larger maps.
inline constexpr float kEngageAcquireDist  = 1000.0f;
inline constexpr float kEngageBreakDist    = 1500.0f;
inline constexpr float kEngageBreakDistY   = 400.0f;
inline constexpr float kEngageStrikeDist   = 70.0f;
inline constexpr float kEngageStrikeY      = 60.0f;
inline constexpr AnchorAI::ThresholdPair kEngageBreakBand        = { kEngageBreakDist,  kEngageBreakDistY };
inline constexpr AnchorAI::ThresholdPair kEngageStrikeBand       = { kEngageStrikeDist, kEngageStrikeY };
inline constexpr AnchorAI::ThresholdPair kAttackEngageStrikeBand = { kAttackEngageDist, kEngageStrikeY };

// --- Engagement speeds ----------------------------------------------------
// Speed history at Invader.cpp:147-157. Current v6 = 5.376 (+5% from v5
// per user 2026-05-20 — pursuit slightly slower than AI Player
// Follower; bumped to match).
inline constexpr float kEngageWalkSpeed    = 5.04f;
inline constexpr float kEngageRunDistance  = 150.0f;
inline constexpr float kEngageRunSpeed     = 5.376f;

// --- BLOCK -----------------------------------------------------------------
inline constexpr int   kBlockDurationMs        = 2000;
inline constexpr float kBlockHpThresholdRatio  = 0.5f;
inline constexpr int   kBlockFrontalAngle      = 0x4000;  // ±90° in s16

// --- RANGED_ATTACK --------------------------------------------------------
inline constexpr float kRangedMinDist        = 90.0f;
inline constexpr float kRangedAcquireDist    = 500.0f;
inline constexpr float kRangedBreakDist      = 800.0f;
inline constexpr float kRangedSpawnFrame     = 5.0f;
inline constexpr float kRangedSpawnHeightY   = 50.0f;
inline constexpr float kRangedYFilter        = 250.0f;
inline constexpr float kRangedElevatedYDelta = 60.0f;

// --- STANDBY --------------------------------------------------------------
// 150u idle radius (widened from 80u on 2026-05-17 / log 230) creates a
// "wait at attention" band 80-150u where the Invader holds in STANDBY
// while the player drifts away.
inline constexpr float kStandbyDetectDist  = 600.0f;
inline constexpr float kStandbyIdleRadius  = 150.0f;
inline constexpr float kStandbyIdleY       = 100.0f;
inline constexpr AnchorAI::ThresholdPair kStandbyIdleBand = { kStandbyIdleRadius, kStandbyIdleY };

// --- Combat cooldown + sheathe-delay --------------------------------------
// 2500ms post-combat cooldown (bumped from 1500ms on 2026-05-17
// alongside kStandbyIdleRadius widening) — spaces swings into a
// deliberate-hunter rhythm.
inline constexpr int   kPostCombatCooldownMs = 2500;
// 4000ms sheathe-delay matches FollowerNPC's kSheatheDelayMs — same
// vanilla "Link keeps weapon drawn for a few seconds after combat" feel.
inline constexpr int   kInvSheatheDelayMs    = 4000;

// --- Swing/shot anim hold -------------------------------------------------
// Min ticks any ATTACK/RANGED_ATTACK state must hold before the
// curFrame>=endFrame anim-completion exit is allowed. Guards against
// a stale endFrame from leftover idle anim. Matches roughly the
// kSwordSwing + kBowShoot anim length in ticks.
inline constexpr int kMinSwingHoldTicks = 6;

// --- DEAD ----------------------------------------------------------------
// 3s death hold — matches FollowerNPC's kFollowerNpcDeathHoldMs.
inline constexpr int kInvaderDeathHoldMs = 3000;

// --- Environmental death --------------------------------------------------
inline constexpr float kInvaderVoidThresholdY = -3000.0f;
inline constexpr int   kInvaderDrowningMs     = 30000;  // Player default

// --- CRAWLING -------------------------------------------------------------
inline constexpr float kInvCrawlSpeed          = 3.5f;
inline constexpr float kInvCrawlEntryRadius    = 150.0f;
inline constexpr float kInvCrawlMinCrossDist   = 20.0f;
inline constexpr float kInvCrawlExitMargin     = 30.0f;
inline constexpr float kInvCrawlMaxDistance    = 400.0f;
inline constexpr float kInvCrawlExitYDrop      = 20.0f;

// --- Phase 2 FOLLOW pursuit ----------------------------------------------
// IDLE↔FOLLOW hysteresis. 1000u acquire matches kEngageAcquireDist —
// outside 1000u the Invader returns to IDLE so target picker may
// select a different hostile. Y gate prevents the "target on ledge
// above" false-arrival oscillation (log 263 fix class).
inline constexpr float kInvFollowEngageDist = 1000.0f;
inline constexpr float kInvFollowIdleDist   = 60.0f;
inline constexpr float kInvFollowIdleY      = 40.0f;
inline constexpr AnchorAI::ThresholdPair kInvFollowIdleBand = { kInvFollowIdleDist, kInvFollowIdleY };
inline constexpr float kInvWalkSpeed        = 5.04f;
inline constexpr float kInvRunSpeed         = 5.376f;
inline constexpr float kInvRunDistance      = 200.0f;

// --- STUCK detection + escalation ----------------------------------------
inline constexpr int   kInvStuckCheckMs        = 3000;
inline constexpr float kInvStuckMinProgress    = 20.0f;
inline constexpr int   kInvStuckCycleWindowMs  = 3000;
inline constexpr int   kInvStuckCycleEscalation = 3;
inline constexpr int   kInvFollowProgressLogMs = 5000;
inline constexpr float kInvStuckNudgeDist      = 30.0f;

// --- G10 leash ------------------------------------------------------------
// Invader's leash is longer than FollowerNPC's (1200u/2000ms) so the
// Invader doesn't teleport away from a hostile it's actively pursuing.
inline constexpr float kInvLeashDistance  = 2000.0f;
inline constexpr int   kInvLeashTimeoutMs = 5000;

// --- G14 close-fail safety net --------------------------------------------
// Invader "close to target but not closing" — distance unchanged for
// > kInvCloseFailTimeoutMs → snap to target. Catches orbits where G10
// can't fire because Invader's inside the leash band but isn't making
// progress (geometry).
inline constexpr float kInvCloseFailMinDistance   = 200.0f;
inline constexpr float kInvCloseFailMaxDistance   = 1200.0f;
inline constexpr int   kInvCloseFailTimeoutMs     = 10000;
inline constexpr float kInvCloseFailProgressDelta = 30.0f;

// --- Phase A substrate path -----------------------------------------------
// Same numerics as FollowerNPC's kPathRefresh*/kAdvanceSubgoalDist.
inline constexpr int   kInvPathRefreshMs       = 500;
inline constexpr float kInvPathRetargetDist    = 60.0f;
inline constexpr float kInvAdvanceSubgoalDist  = 30.0f;
inline constexpr float kInvFollowProxLimit     = 30.0f;

// --- Phase B CLIMBING -----------------------------------------------------
// Halved to 2.0u/frame on 2026-05-19 PM per user report (4.0 was too
// fast on the vine wall). Body offset 12u matches FollowerNPC.
inline constexpr float kInvClimbSpeedY                  = 2.0f;
inline constexpr float kInvClimbBodyOffset              = 12.0f;
inline constexpr float kInvClimbForceEngageBaseDistSq   = 200.0f * 200.0f;

}  // namespace InvaderTunables
