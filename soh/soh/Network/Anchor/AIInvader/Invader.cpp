/**
 * Invader — implementation (steps 15a/15b/15c/15d).
 *
 * Step 15a — scaffold + draw-context flag for hostile-black tint.
 *
 * Step 15b — Phase B equipment swap. Around the Player_DrawImpl call
 * inside EnInvader_Draw, the Begin/End pair save the local Player's
 * current model state, force PLAYER_MODELGROUP_SWORD_AND_SHIELD via
 * Player_SetModels, then restore at End. This makes the Invader
 * appear permanently armed with sword + shield, independent of what
 * Link is actually holding. (Agent 1)
 *
 * Step 15c — Phase 2 locomotion. Anchor_TickInvaderActor implements
 * IDLE / FOLLOW / STUCK plus G18 (cutscene) + G10 (leash) guards.
 * Direct yaw + speedXZ; no substrate path consumption in v1.
 * Cloned from FollowerNPC's same-named states. (Agent 2 reconstructed)
 *
 * Step 15d — Phase 3 combat. ATTACK / BLOCK / ENGAGE / RANGED_ATTACK /
 * STANDBY cloned from NPC Follower Stage 4
 * (AIFollowerNPC/FollowerNPC.cpp) with these intentional deltas:
 *   - Target is the nearest PLAYER actor (not nearest enemy). Uses
 *     PickHostileTargetForInvader (Agent 4) — multi-player picker
 *     respecting director-side state.
 *   - AT collider TYPE is AT_TYPE_ENEMY (Player AC bumpers are
 *     AC_TYPE_PLAYER, which accept ATs of TYPE_ENEMY). The Follower's
 *     equivalent sword AT uses AT_TYPE_PLAYER because Follower
 *     attacks enemies.
 *   - Equipment-visibility swap is owned by Phase B (Agent 1);
 *     Invader is always visually-armed, so the time-based retention
 *     / sheathe-delay polish from FollowerNPC is omitted.
 *
 * TODO post-#208: revisit state shape against canonical follower
 * design pass (currently a verbatim clone of NPC Follower Stage 4).
 *
 * Canonical patterns referenced from AIFollowerNPC/FollowerNPC.cpp
 * lines 422-526 (equipment swap), 916-1109 (IDLE/FOLLOW), 3359-3386
 * (STUCK), 3263-3286 (G10 leash), 3393-3482 (dispatcher G18).
 */

#include "Invader.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"  // PickHostileTargetForInvader (Agent 4)
#include "soh/Network/Anchor/Common/ActorTrail.h"    // Nav-parity Phase A: substrate path consumption
#include "soh/Network/Anchor/Common/DistanceMath.h"  // AnchorDist::DistXZSq
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // Parity gap 5: CrawlspaceAnchor lookup
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include "variables.h"   // gPlayState, gSaveContext
#include "functions.h"   // Player_SetModels, Actor_Spawn, Math_*
#include "z64.h"         // Player, Gfx, PlayState, Actor, Vec3f
#include "macros.h"      // GET_PLAYER, INV_CONTENT, CS_STATE_IDLE
#include "objects/gameplay_keep/gameplay_keep.h"  // gPlayerAnim_*
#include "src/overlays/actors/ovl_En_Invader/z_en_invader.h"
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"
extern PlayState* gPlayState;
}

namespace {

// Set during EnInvader_Draw's Player_DrawImpl call; cleared after.
// Read by the VB_APPLY_TUNIC_COLOR hook to know which actor's tunic
// is being rendered, so it can apply the black-tint override. File-
// scope static rather than class member because actor draws don't
// nest — the gfx context is single-threaded and each draw completes
// before the next starts.
static Actor* sCurrentlyDrawingInvader = nullptr;

// Phase B equipment swap — file-scope save slot. Single instance is
// safe because actor draws don't nest (same justification as in
// FollowerNPC.cpp's equipment-swap save slot). If two actors ever
// share the same gfx context concurrently, this becomes a stack.
//
// sEquipmentSwapActive controls whether End() should restore — set
// true in Begin() only when a real swap happened. Defensive: lets
// End() be a no-op when Begin() short-circuited (e.g. gPlayState
// null, localPlayer null, or already at the intended group).
static bool  sEquipmentSwapActive       = false;
static s32   sSavedPlayerModelGroup     = 0;
static u8    sSavedPlayerLeftHandType   = 0;
static u8    sSavedPlayerRightHandType  = 0;
static u8    sSavedPlayerSheathType     = 0;
static Gfx** sSavedPlayerLeftHandDLists  = nullptr;
static Gfx** sSavedPlayerRightHandDLists = nullptr;
static Gfx** sSavedPlayerSheathDLists    = nullptr;
static Gfx** sSavedPlayerWaistDLists     = nullptr;

// ---------------------------------------------------------------------
// Combat tuning constants (cloned from FollowerNPC).
// ---------------------------------------------------------------------
constexpr float kAttackEngageDist     = 80.0f;
constexpr float kAttackQuadForward    = 60.0f;
constexpr float kAttackQuadHalfWidth  = 25.0f;
constexpr float kAttackQuadBaseY      = 5.0f;
constexpr float kAttackQuadTopY       = 65.0f;
constexpr float kAttackActiveStartFrame = 4.0f;
constexpr float kAttackActiveEndFrame   = 12.0f;

// Detection range bumped 250→1000 on 2026-05-17 (log 233 testing).
// User observation: "AI Invader only started pathfinding to player
// when the player got close, within ~300 units. AI Invader had line
// of sight on the player from much further away and should have begun
// pathfinding."
//
// Invader's PickHostileTargetForInvader (Common/PlayerLookup.cpp)
// only ever returns player-aligned actors (local Player + DummyPlayers
// + NPC Follower when targetable) — vanilla enemies are NEVER
// candidates here, so widening the range does not cause the Invader
// to attack scene enemies. Defensive retaliation against non-player
// enemies remains a deferred feature (post-#208).
constexpr float kEngageAcquireDist  = 1000.0f;
constexpr float kEngageBreakDist    = 1500.0f;
constexpr float kEngageStrikeDist   = 70.0f;
constexpr float kEngageWalkSpeed    = 6.0f;
constexpr float kEngageRunDistance  = 150.0f;
constexpr float kEngageRunSpeed     = 12.0f;

constexpr int   kBlockDurationMs        = 2000;
constexpr float kBlockHpThresholdRatio  = 0.5f;
constexpr int   kBlockFrontalAngle      = 0x4000;  // ±90° in s16

constexpr float kRangedMinDist     = 90.0f;
constexpr float kRangedAcquireDist = 500.0f;
constexpr float kRangedBreakDist   = 800.0f;
constexpr float kRangedSpawnFrame  = 5.0f;
constexpr float kRangedSpawnHeightY = 50.0f;
constexpr float kRangedYFilter     = 250.0f;
constexpr float kRangedElevatedYDelta = 60.0f;

constexpr float kStandbyDetectDist = 600.0f;
// Idle leader-leash radius — STANDBY drops to FOLLOW outside this.
// Was 80u; widened to 150u 2026-05-17 after log 230 showed an
// ATTACK→STANDBY→FOLLOW→ATTACK cycle every ~1s. With 80u the
// hysteresis between STANDBY-exit (80u) and combat-entry
// (kAttackEngageDist=80u) was zero — any player motion in/out of the
// 80u ring triggered a chase-then-immediate-swing pattern. 150u
// creates a "wait at attention" band of 80-150u where the Invader
// holds in STANDBY while the player drifts away, only chasing when
// the player meaningfully retreats.
constexpr float kStandbyIdleRadius = 150.0f;

// Post-combat re-engagement cooldown. Was 1500ms; bumped to 2500ms
// 2026-05-17 alongside kStandbyIdleRadius widening — together they
// space the Invader's swings out into a more "deliberate hunter"
// rhythm. TryEngageCombat checks `curFrame < sCombatCooldownEndFrame`
// before re-firing combat tiers.
constexpr int   kPostCombatCooldownMs = 2500;
static uint64_t sCombatCooldownEndFrame = 0;
// Last combat weapon. 0 = melee (sword); 1 = ranged. Set at every
// combat entry by TryEngageCombat; consumed by STANDBY anim/facing
// preference (no equipment swap here — Agent 1).
static s32 sLastCombatWeapon = 0;

// Per-swing / per-shot state. File-scope is safe because actor combat
// states are non-reentrant per actor (the dispatcher runs at most one
// state handler per actor per tick) and there is one Invader per
// dispatcher invocation. If multiple Invaders share one dispatcher
// in a future revision, this state moves onto EnInvader.
struct AttackState {
    Actor*   target = nullptr;
    bool     swingFiredAT = false;
    uint64_t entryFrame = 0;       // gameFrameCounter at last ATTACK/RANGED_ATTACK entry
};
static AttackState sAttackState;

// Minimum ticks any swing/shot state must hold before the
// curFrame>=endFrame anim-completion check is allowed to exit. This
// guards against a stale endFrame from the previous state's anim
// being non-zero when ATTACK/RANGED_ATTACK is entered — without the
// guard, the very first tick would early-exit because (curFrame >=
// endFrame) is true from leftover idle anim state. Matches roughly
// the kSwordSwing + kBowShoot anim length in ticks.
constexpr int kMinSwingHoldTicks = 6;

struct BlockState {
    uint64_t entryFrame    = 0;
    uint32_t hitAnimFrames = 0;
};
static BlockState sBlockState;

// Parity gap 3 — DEAD state hold timer. Captures the gameFrameCounter
// at DEAD entry; TickDEAD waits for kInvaderDeathHoldMs to elapse, then
// calls Actor_Kill which fires the OnActorKill broadcast path
// (ENEMY_DEFEATED + Director::OnEnemyRemoved). File-scope is safe
// because actor states are non-reentrant per actor.
constexpr int kInvaderDeathHoldMs = 3000;  // 3s — matches FollowerNPC's kFollowerNpcDeathHoldMs
static uint64_t sDeathEntryInvFrame = 0;

// Parity gap 5 — crawlspace traversal state. Same shape as FollowerNPC's
// sCrawlState (FollowerNPC.cpp:2958-2964). anchor pointer is borrowed
// from the room's RoomNavData::crawlspaceAnchors vector; it's invalidated
// on scene transition, but TickCRAWLING bails to FOLLOW + clears the
// pointer if it ever sees a null mid-crawl (defensive).
constexpr float kInvCrawlSpeed         = 3.5f;
constexpr float kInvCrawlEntryRadius   = 150.0f;
constexpr float kInvCrawlMinCrossDist  = 20.0f;
constexpr float kInvCrawlExitMargin    = 30.0f;
constexpr float kInvCrawlMaxDistance   = 400.0f;
constexpr float kInvCrawlExitYDrop     = 20.0f;  // matches FollowerNPC bddc0b598

struct CrawlInvState {
    const ::AnchorNavRoom::CrawlspaceAnchor* anchor = nullptr;
    Vec3f forwardDir = { 0, 0, 0 };  // -entryNormal direction (into the wall)
    Vec3f entryPos   = { 0, 0, 0 };  // captured at entry for max-distance bail
    bool  exitAnimPlaying = false;
};
static CrawlInvState sCrawlInvState;

// ---------------------------------------------------------------------
// Small math helpers — defined here so Phase 2 Tick handlers below
// can call them without forward declarations (C++ single-pass name
// lookup, CLAUDE.md Pitfall 14). Cloned from FollowerNPC's free
// functions; kept inside this TU rather than reaching into
// FollowerNPC's anonymous namespace.
// ---------------------------------------------------------------------
inline float Dist2DSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

inline s16 YawTowardTarget(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

// Forward declaration — PickHostileTarget is defined further down
// (after Phase 2 handlers) because it's an Agent 3 helper that lives
// near the combat constants. The Phase 2 Tick handlers call it for
// target acquisition. C++ single-pass name lookup needs the decl
// here (CLAUDE.md Pitfall 14).
Actor* PickHostileTarget(Actor* self, PlayState* play, float maxRange,
                         float maxYDelta = 60.0f);

// ---------------------------------------------------------------------
// Phase 2 — locomotion tuning constants. Cloned from FollowerNPC
// with the leader-friendly thresholds inverted into target-hostile
// pursuit thresholds. NOTE post-#208: revisit state shape against
// canonical follower design pass.
// ---------------------------------------------------------------------

// Hysteresis: IDLE→FOLLOW fires when target XZ-distance exceeds this.
// Larger than kInvFollowIdleDist so the NPC doesn't oscillate at the
// boundary. Bumped 250→1000 on 2026-05-17 (log 233) to match the
// kEngageAcquireDist range — outside 1000u the Invader returns to
// IDLE and the target picker may select a different hostile. Inside
// 1000u, IDLE→FOLLOW engages and TryEngageCombat Tier 2 immediately
// promotes to ENGAGE pursuit.
constexpr float kInvFollowEngageDist = 1000.0f;
// FOLLOW→IDLE fires when target XZ-distance falls inside this. Slightly
// smaller than kEngageStrikeDist so FOLLOW hands off to combat
// (TryEngageCombat picks ATTACK / ENGAGE) BEFORE the IDLE re-entry
// snaps the chase. Field test will likely retune; this is a starting
// point that mirrors FollowerNPC's 50u kEnterIdle scaled to pursuit.
constexpr float kInvFollowIdleDist = 60.0f;
// FOLLOW pursuit speeds. Same numerics as FollowerNPC's kRunSpeed /
// kRunDistance.
constexpr float kInvWalkSpeed   = 6.0f;
constexpr float kInvRunSpeed    = 12.0f;
constexpr float kInvRunDistance = 200.0f;
// STUCK detection — no progress over this window triggers a one-tick
// nudge. Same shape as FollowerNPC's kStuckCheckMs / kStuckMinProgress
// but Invader's nudge distance is smaller (chase enemies don't have
// the "find route around stairs" goal that the friendly follower does).
constexpr int   kInvStuckCheckMs    = 3000;
constexpr float kInvStuckMinProgress = 20.0f;
constexpr float kInvStuckNudgeDist   = 30.0f;
// G10 leash — 3D distance threshold + timeout. The Invader can be
// far from the target; we use a longer leash than FollowerNPC's
// (1200u/2000ms) so the Invader doesn't teleport away from a hostile
// it's actively pursuing. Fires only when in IDLE/FOLLOW/STUCK
// (combat / engage states stay put). Snaps to target on fire.
constexpr float kInvLeashDistance  = 2000.0f;
constexpr int   kInvLeashTimeoutMs = 5000;

// ── Nav-parity Phase A — substrate path tuning ────────────────────
// Same numerics as FollowerNPC's kPathRefresh*/kAdvanceSubgoalDist.
// Re-querying every 500ms hits the trail-decay sweet spot — long
// enough to amortise the BFS cost, short enough that a target who
// just stepped around a corner doesn't run dry on the current path.
constexpr int   kInvPathRefreshMs       = 500;
constexpr float kInvPathRetargetDist    = 60.0f;
constexpr float kInvAdvanceSubgoalDist  = 30.0f;
// Proximity inside which TickFOLLOW skips substrate-path computation
// and walks straight at the target. Matches FollowerNPC's
// kFollowProxLimit — avoids spurious re-paths around small obstacles
// when the target is right there.
constexpr float kInvFollowProxLimit     = 30.0f;

// ── Nav-parity Phase B — CLIMBING tuning ───────────────────────────
// Same numerics as FollowerNPC's kClimbSpeedY / kClimbBodyOffset.
// Vanilla Link climbs at ~4-5u/frame; we use 4 to keep the Invader
// from out-pacing the player vertically.
constexpr float kInvClimbSpeedY     = 4.0f;
constexpr float kInvClimbBodyOffset = 12.0f;
// Engagement gate when leader-climb force-engage fires. Same shape as
// FollowerNPC's kClimbForceEngageBaseDistSq (200u). Squared form to
// avoid the sqrt at gate-check time.
constexpr float kInvClimbForceEngageBaseDistSq = 200.0f * 200.0f;

// Local nav baseline. Same shape as FollowerNPC's sLocalNav (subset).
// Nav-parity Phase A added substrate path consumption + the cached
// climb anchor used by Phase B (CLIMBING state).
//
// File-scope is safe because actor states are non-reentrant per actor.
// If a future revision spawns multiple Invaders sharing one dispatcher,
// this state moves onto EnInvader (same evolution path as
// FollowerNPC's sLocalNav).
struct LocalInvNavState {
    Vec3f    stuckCheckPos        = { 0.0f, 0.0f, 0.0f };
    uint64_t lastStuckCheckFrame  = 0;
    uint32_t leashFrames          = 0;
    // Cached target for FOLLOW's anim / speed calc. PickHostileTarget
    // re-queried each tick, but we keep a one-tick cache so STUCK can
    // nudge toward the same target without re-running the picker.
    Actor*   lastTarget           = nullptr;
    // Parity gap 6 — G14 close-fail tracking. closeFailFrames counts
    // consecutive ticks inside the close-fail distance band without
    // progress; closeFailBaseline is the dist3D-to-target captured at
    // window entry. Reset when progress > kInvCloseFailProgressDelta
    // is observed.
    uint32_t closeFailFrames      = 0;
    float    closeFailBaseline    = 0.0f;

    // ── Nav-parity Phase A — substrate path consumption ─────────────
    AnchorNav::ActorTrail::NavPath path;
    uint64_t lastPathRefreshFrame = 0;
    Vec3f    lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };

    // ── Nav-parity Phase B — CLIMBING anchor cache ──────────────────
    // Active anchor during a CLIMBING run so the handler doesn't
    // re-resolve every frame. Cleared on CLIMBING exit. Pointer is
    // borrowed from the room's RoomNavData; invalidated on scene
    // transition, but Phase B exits CLIMBING on scene change via the
    // anchor-null check in the handler.
    const ::AnchorNavRoom::ClimbAnchor* activeClimbAnchor = nullptr;
};
static LocalInvNavState sLocalInvNav;

// ---------------------------------------------------------------------
// Phase 2 + Phase 4 — animation kind enum + header / picker / ensurer.
// Cloned from FollowerNPC's larger enum but with combat-anim direct
// LinkAnimation_Change calls in the combat handlers (matching the
// FollowerNPC pattern). Locomotion + idle + swim + hoist + jump +
// fidget anims live in this enum.
//
// Phase 4 additions: kSwim / kSwimWait (SWIMMING state),
// kHoistGround / kHoistSwim (LEDGE_HOIST state one-shots),
// kJump / kRunJump (airborne FOLLOW), kFidgetLookA / kFidgetWarmB /
// kFidgetStretchD (IDLE rotation).
//
// NOTE: OoT does NOT expose a standalone "draw sword" / "sheathe
// sword" anim. Player_SetModels handles equipment-DList swap
// instantaneously and Player vanilla uses brief transitional anims
// (`waitL2defense`) tied to combat-state transitions. For the
// Invader's draw/sheathe smoothing, the equipment-swap path (Phase B,
// Anchor_InvaderDrawBegin/End forcing PLAYER_MODELGROUP_SWORD_AND_SHIELD)
// is already instant; documenting the gap rather than inventing a fake
// transition. Field-test will tell us whether this needs a visible
// transition pass.
// ---------------------------------------------------------------------
enum class InvaderAnim {
    kNone,            // sentinel (no anim selected yet)
    kWait,            // idle wait (free or fighter depending on modelAnimType)
    kWalk,            // pursuit walk
    kRun,             // pursuit run
    kStopL,           // one-shot stop on left foot (FOLLOW→IDLE)
    kStopR,           // one-shot stop on right foot (FOLLOW→IDLE)

    // Phase 4 additions.
    kSwim,            // swimming pursuit (gPlayerAnim_link_swimer_swim)
    kSwimWait,        // treading water idle (gPlayerAnim_link_swimer_swim_wait)
    kHoistGround,     // mantle from floor (gPlayerAnim_link_normal_climb_up; one-shot)
    kHoistSwim,       // climb out of water (gPlayerAnim_link_swimer_swim_15step_up; one-shot)
    kJump,            // standing jump (gPlayerAnim_link_normal_jump; one-shot)
    kRunJump,         // running jump (gPlayerAnim_link_normal_run_jump; one-shot)
    kFidgetLookA,     // idle look-around (gPlayerAnim_link_normal_wait_typeA_20f; one-shot)
    kFidgetWarmB,     // idle warm-up (gPlayerAnim_link_normal_wait_typeB_20f; one-shot)
    kFidgetStretchD,  // idle stretch (gPlayerAnim_link_wait_typeD_20f; one-shot)

    // Parity gap 1 — combat anims. Cloned from FollowerNPC's enum
    // values 21-24 (FollowerNPC.cpp:196-206). kSwordSwing / kBlockHit /
    // kBowShoot are one-shots; kBlockWait loops while the shield is up.
    kSwordSwing,      // vertical sword swing (gPlayerAnim_link_fighter_normal_kiru; one-shot)
    kBlockWait,       // shield-up held pose (gPlayerAnim_link_normal_defense_wait; LOOP)
    kBlockHit,        // shield-deflect reaction (gPlayerAnim_link_normal_defense_hit; one-shot)
    kBowShoot,        // bow draw+release (gPlayerAnim_link_bow_bow_shoot; one-shot)

    // Parity gap 4 — death poses. Generic (back-down) for combat/void;
    // drowning-specific (swim KO) when death cause is drowning. Both
    // one-shot; SkelAnime holds at last frame after completion so
    // TickDEAD's 3s hold timer drives the eventual Actor_Kill.
    kDeath,           // generic death (gPlayerAnim_link_normal_back_downA; one-shot)
    kDeathDrown,      // drowning death (gPlayerAnim_link_swimer_swim_dead; one-shot)

    // Parity gap 5 — child-Link crawlspace anims. kCrawlMove plays
    // once on entry (the get-down-and-crouch motion); after
    // completion the SkelAnime holds at the end frame (low crouch
    // pose) while TickCRAWLING translates the body forward. kCrawlExit
    // plays once on the way out. Mirrors FollowerNPC's kCrawlMove /
    // kCrawlExit (FollowerNPC.cpp:220-221).
    kCrawlMove,       // crouch-enter (gPlayerAnim_link_child_tunnel_start; one-shot, holds end frame)
    kCrawlExit,       // crouch-exit (gPlayerAnim_link_child_tunnel_end; one-shot)

    // Nav-parity Phase B — CLIMBING anims. Vertical alternation
    // (kClimbUpL / kClimbUpR) and lateral alternation (kClimbSideL /
    // kClimbSideR) — each one-shot, the dispatcher fires the next
    // step when the previous completes AND vertical/lateral motion
    // is happening. Mirrors Player's actionVar2 toggle at
    // z_player.c:13412. Headers from gPlayerAnim_link_normal_Fclimb_*.
    // Shared across modelAnimType (climb anims are stance-agnostic in
    // Player's table).
    kClimbUpL,        // gPlayerAnim_link_normal_Fclimb_upL (one-shot)
    kClimbUpR,        // gPlayerAnim_link_normal_Fclimb_upR (one-shot)
    kClimbSideL,      // gPlayerAnim_link_normal_Fclimb_sideL (one-shot)
    kClimbSideR,      // gPlayerAnim_link_normal_Fclimb_sideR (one-shot)
};

LinkAnimationHeader* InvAnimHeaderFor(InvaderAnim kind, s8 modelAnimType) {
    const bool isFighter = (modelAnimType == 1 || modelAnimType == 2);
    switch (kind) {
        case InvaderAnim::kWait:
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free;
        case InvaderAnim::kWalk:
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_free;
        case InvaderAnim::kRun:
            if (modelAnimType == 1) return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_run;
            if (modelAnimType == 2) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_free;
        case InvaderAnim::kStopL:
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL_free;
        case InvaderAnim::kStopR:
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR_free;
        // Phase 4 — modelAnimType-independent (swim anims keep sword
        // sheathed regardless; mantle/jump/fidget are agnostic).
        case InvaderAnim::kSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim;
        case InvaderAnim::kSwimWait:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_wait;
        case InvaderAnim::kHoistGround:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_climb_up;
        case InvaderAnim::kHoistSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_15step_up;
        case InvaderAnim::kJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_jump;
        case InvaderAnim::kRunJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_jump;
        case InvaderAnim::kFidgetLookA:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeA_20f;
        case InvaderAnim::kFidgetWarmB:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeB_20f;
        case InvaderAnim::kFidgetStretchD:
            return (LinkAnimationHeader*)&gPlayerAnim_link_wait_typeD_20f;
        // Parity gap 1 — combat anim headers (modelAnimType-independent;
        // these are always armed/fighter regardless of caller's hint).
        case InvaderAnim::kSwordSwing:
            return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_normal_kiru;
        case InvaderAnim::kBlockWait:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_defense_wait;
        case InvaderAnim::kBlockHit:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_defense_hit;
        case InvaderAnim::kBowShoot:
            return (LinkAnimationHeader*)&gPlayerAnim_link_bow_bow_shoot;
        // Parity gap 4 — death anim headers.
        case InvaderAnim::kDeath:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_back_downA;
        case InvaderAnim::kDeathDrown:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_dead;
        // Parity gap 5 — crawlspace anim headers.
        case InvaderAnim::kCrawlMove:
            return (LinkAnimationHeader*)&gPlayerAnim_link_child_tunnel_start;
        case InvaderAnim::kCrawlExit:
            return (LinkAnimationHeader*)&gPlayerAnim_link_child_tunnel_end;
        // Nav-parity Phase B — climbing anim headers. Shared across
        // modelAnimType (climb stance is identity, not weapon-typed).
        case InvaderAnim::kClimbUpL:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_upL;
        case InvaderAnim::kClimbUpR:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_upR;
        case InvaderAnim::kClimbSideL:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_sideL;
        case InvaderAnim::kClimbSideR:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_sideR;
        case InvaderAnim::kNone:
        default:
            return nullptr;
    }
}

void InvEnsureAnimation(EnInvader* this_, PlayState* play, InvaderAnim want,
                        s8 modelAnimType) {
    LinkAnimationHeader* anim = InvAnimHeaderFor(want, modelAnimType);
    if (anim == nullptr) return;
    // No-op only if BOTH the kind and the resolved anim header match.
    // A modelAnimType change must re-fire the anim with the new
    // variant even when kind is unchanged.
    if ((InvaderAnim)this_->currentAnim == want &&
        this_->skelAnime.animation == anim) {
        return;
    }
    const bool oneShot = (want == InvaderAnim::kStopL ||
                          want == InvaderAnim::kStopR ||
                          want == InvaderAnim::kHoistGround ||
                          want == InvaderAnim::kHoistSwim ||
                          want == InvaderAnim::kJump ||
                          want == InvaderAnim::kRunJump ||
                          want == InvaderAnim::kFidgetLookA ||
                          want == InvaderAnim::kFidgetWarmB ||
                          want == InvaderAnim::kFidgetStretchD ||
                          // Parity gap 1 — combat one-shots.
                          // kBlockWait stays LOOP (not in this list).
                          want == InvaderAnim::kSwordSwing ||
                          want == InvaderAnim::kBlockHit ||
                          want == InvaderAnim::kBowShoot ||
                          // Parity gap 4 — death anims (one-shot; hold at last frame).
                          want == InvaderAnim::kDeath ||
                          want == InvaderAnim::kDeathDrown ||
                          // Parity gap 5 — crawlspace anims (one-shot;
                          // kCrawlMove holds end-frame crouch pose).
                          want == InvaderAnim::kCrawlMove ||
                          want == InvaderAnim::kCrawlExit ||
                          // Nav-parity Phase B — climb step anims. Each
                          // climb step is a one-shot of kClimbUp{L,R} or
                          // kClimbSide{L,R}; the TickCLIMBING handler
                          // fires the next step when the previous
                          // completes AND vertical/lateral motion is
                          // progressing. Mirrors Player's L/R alternation
                          // at z_player.c:13412.
                          want == InvaderAnim::kClimbUpL ||
                          want == InvaderAnim::kClimbUpR ||
                          want == InvaderAnim::kClimbSideL ||
                          want == InvaderAnim::kClimbSideR);
    LinkAnimation_Change(play, &this_->skelAnime, anim,
                         1.0f /* playSpeed */,
                         0.0f /* startFrame */,
                         Animation_GetLastFrame((void*)anim),
                         oneShot ? ANIMMODE_ONCE : ANIMMODE_LOOP,
                         -6.0f /* morphFrames */);
    this_->currentAnim     = (s32)want;
    this_->currentAnimType = modelAnimType;
    this_->stopAnimPlaying = oneShot ? 1 : 0;
}

InvaderAnim InvAnimForState(s32 state, float speedXZ, s32 prevState) {
    switch (state) {
        case EN_INVADER_STATE_FOLLOW: {
            if (speedXZ < 0.1f) return InvaderAnim::kWait;
            return (speedXZ > 4.0f) ? InvaderAnim::kRun : InvaderAnim::kWalk;
        }
        case EN_INVADER_STATE_STUCK:
            return InvaderAnim::kWait;
        case EN_INVADER_STATE_IDLE: {
            // Fire stop-anim ONCE on the FOLLOW→IDLE transition. Caller
            // (dispatcher) is responsible for not re-firing every tick;
            // the stopAnimPlaying flag on this_ guards LinkAnimation_Change
            // from restarting. After the one-shot completes, falls back
            // to kWait.
            if (prevState == EN_INVADER_STATE_FOLLOW) {
                // Pick L/R based on step phase. Same split as FollowerNPC
                // — phase < 14 = stop-on-left, phase >= 14 = stop-on-right.
                // Without a real step phase calculation we approximate
                // via prevState's odd/even quirk; this is "good enough"
                // visual variety for v1. Field-test may want the full
                // step-phase machinery.
                return InvaderAnim::kStopL;
            }
            return InvaderAnim::kWait;
        }
        case EN_INVADER_STATE_SWIMMING:
            // Tread water at low speed; full swim anim when moving.
            // Threshold matches FollowerNPC's 0.5f handoff.
            return (speedXZ > 0.5f) ? InvaderAnim::kSwim
                                    : InvaderAnim::kSwimWait;
        case EN_INVADER_STATE_LEDGE_HOIST:
            // Dispatcher overrides this with the correct kHoistGround
            // vs kHoistSwim pick based on EnInvader::hoistContext (which
            // this function doesn't have access to). Return a sensible
            // default so a code path that bypasses the dispatcher
            // override still gets a valid hoist anim.
            return InvaderAnim::kHoistGround;
        // Parity gap 1 — combat state anims. Dispatcher hoists
        // currentAnimType to 1 (fighter) for combat states so kWait /
        // kWalk / kRun would pick fighter variants if used; the
        // combat states pick their dedicated anim values here instead.
        case EN_INVADER_STATE_ATTACK:
            return InvaderAnim::kSwordSwing;
        case EN_INVADER_STATE_BLOCK:
            // Loop kBlockWait by default; the dispatcher overrides to
            // kBlockHit during the brief frame window after a
            // successful frontal deflect (sBlockState.hitAnimFrames > 0).
            return InvaderAnim::kBlockWait;
        case EN_INVADER_STATE_RANGED_ATTACK:
            return InvaderAnim::kBowShoot;
        case EN_INVADER_STATE_ENGAGE:
            // Pursuit locomotion — same anims as FOLLOW. Speed threshold
            // matches FOLLOW so handoff IDLE→FOLLOW ↔ ENGAGE looks
            // consistent. Returns kWait when stationary so a brief
            // tick at speedXZ=0 doesn't show the NPC mid-stride.
            if (speedXZ > 4.0f) return InvaderAnim::kRun;
            if (speedXZ > 0.5f) return InvaderAnim::kWalk;
            return InvaderAnim::kWait;
        case EN_INVADER_STATE_STANDBY:
            // Alert idle between exchanges. Same kWait anim as IDLE;
            // the visual difference comes from the dispatcher setting
            // currentAnimType=1 (fighter) here.
            return InvaderAnim::kWait;
        // Parity gap 4 — DEAD state default. Dispatcher overrides to
        // kDeathDrown when this_->deathCause == 1 (drowning).
        case EN_INVADER_STATE_DEAD:
            return InvaderAnim::kDeath;
        // Parity gap 5 — CRAWLING. Returns kCrawlMove on entry; the
        // dispatcher overrides to kCrawlExit when sCrawlInvState's
        // exitAnimPlaying flag is set.
        case EN_INVADER_STATE_CRAWLING:
            return InvaderAnim::kCrawlMove;
        // Nav-parity Phase B — CLIMBING. Returns kClimbUpL as a sane
        // default; the dispatcher overrides to kClimbUpR / kClimbSideL/R
        // based on this_->climbNextIsRight + the current motion axis.
        case EN_INVADER_STATE_CLIMBING:
            return InvaderAnim::kClimbUpL;
        default:
            return InvaderAnim::kWait;
    }
}

// ---------------------------------------------------------------------
// Phase 4 — swim constants. Match FollowerNPC's swim constants
// (FollowerNPC.cpp:1907-1911) so the Invader's swim behaviour visually
// mirrors the NPC Follower (and Player). Per-age depth picked from
// Player's ageProperties.unk_24 (z_player.c:453,505): adult 36u,
// child 22u.
// ---------------------------------------------------------------------
static constexpr float kInvSwimDepthAdult     = 36.0f;
static constexpr float kInvSwimDepthChild     = 22.0f;
static constexpr float kInvSwimExtraDrop      = 5.0f;
static constexpr float kInvSwimSpeedMax       = 4.0f;
static constexpr float kInvSwimArrive         = 60.0f;
static constexpr float kInvSwimShoreExitDepth = 30.0f;

inline float InvSwimDepthFor(s8 linkAge) {
    return (linkAge == 0) ? kInvSwimDepthAdult : kInvSwimDepthChild;
}

// Phase 4 — hoist constants. Match FollowerNPC's hoist constants
// (FollowerNPC.cpp:1998-2000) so geometry detection matches.
static constexpr float kInvHoistGroundLiftMin  = 20.0f;
static constexpr float kInvHoistGroundLiftMax  = 90.0f;
static constexpr float kInvHoistSwimProbeStartY = 55.0f;
static constexpr float kInvHoistSwimLiftMin    = 20.0f;
static constexpr float kInvHoistSwimLiftMax    = 90.0f;
// Forward-cast for ground-hoist raycast fallback.
static constexpr float kInvHoistForwardCastDist = 80.0f;
static constexpr float kInvHoistChestHeight     = 20.0f;
static constexpr float kInvHoistPastWallNudge   = 8.0f;
// Swim-exit Y raise during the swim-step-up anim — matches FollowerNPC's
// kSwimHoistRaise (FollowerNPC.cpp:3670) so feet sit at ledge-top level
// while the swim_15step_up anim plays.
static constexpr float kInvSwimHoistRaise       = 43.0f;

// Captured at LEDGE_HOIST entry for the per-tick interpolation lerp
// in TickLEDGE_HOIST. File-scope is safe because a per-Invader hoist
// is non-reentrant (only one Invader's hoist runs per dispatch tick).
static Vec3f sLocalInvHoistStartPos = { 0.0f, 0.0f, 0.0f };

// ---------------------------------------------------------------------
// Phase 2 — TickIDLE / TickFOLLOW / TickSTUCK + G10 leash.
// Cloned from FollowerNPC's same-named handlers, with "leader" swapped
// to "hostile target via PickHostileTarget" and no substrate path
// consumption. Direct yaw + speedXZ.
// ---------------------------------------------------------------------

void TickIDLE(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Look for a hostile target in the FOLLOW-engage range. TryEngageCombat
    // has already run by the time this fires; it picks combat states
    // when in melee/pursuit/ranged range. Anything farther falls to
    // IDLE-vs-FOLLOW arbitration here.
    Actor* target = PickHostileTarget(a, play, kInvFollowEngageDist,
                                       /*maxYDelta=*/kRangedYFilter);
    if (target != nullptr) {
        sLocalInvNav.lastTarget = target;
        this_->state = EN_INVADER_STATE_FOLLOW;
        // Reset stuck baseline on transition so the FOLLOW handler
        // measures progress from the engagement point.
        sLocalInvNav.stuckCheckPos = a->world.pos;
        sLocalInvNav.lastStuckCheckFrame =
            Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
        return;
    }
    sLocalInvNav.lastTarget = nullptr;
}

// Nav-parity Phase A — substrate path consumption. TickFOLLOW now
// computes a path from invader→target using
// AnchorNav::ActorTrail::ComputePathTo, advances the cursor on
// 30u XZ proximity, and yaws toward the current subgoal instead of
// directly toward the target. If the path is empty or invalid, falls
// back to direct yaw (the prior v1 behaviour).
//
// Phase B — CLIMBING transition: if the current path's subgoal flag
// bitmap carries NODE_CLIMB_ANY, transition to EN_INVADER_STATE_CLIMBING
// and let the climb handler take over the snap-to-wall + vertical drive.
//
// Reference: FollowerNPC.cpp:TickFOLLOW (FollowerNPC.cpp:935). Key
// divergences from the Follower:
//   - Target is a hostile actor (PickHostileTarget) instead of a
//     friendly leader (GET_PLAYER).
//   - No `effectiveTarget` leader-climb redirect — the player who is
//     climbing IS the Invader's target; the Invader pursues to the
//     climb-anchor base via the substrate path's regular climb-cell
//     emission and then engages CLIMBING via the climb-cell transition.
//   - `preferLeaderTrail` is false (we're not chasing a friendly
//     leader's exact route; we want the BFS to find a fresh path).
//   - Trail key is computed from the target. For player targets, use
//     TrailKeyForPlayer(client.id) so the BFS can use the target's
//     own breadcrumb history.
void TickFOLLOW(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;

    // Re-acquire target each tick. Sticky-target behaviour is the
    // responsibility of Agent 4's picker (which can return a non-closest
    // target if it's tracking session-level aggro state); our wrapper
    // adds the range / Y-delta filter.
    Actor* target = PickHostileTarget(a, play, kInvFollowEngageDist,
                                       /*maxYDelta=*/kRangedYFilter);
    if (target == nullptr) {
        // Lost target. Hand off to IDLE — the dispatcher's next tick
        // re-runs TryEngageCombat / TickIDLE which will re-scan.
        this_->state = EN_INVADER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalInvNav.lastTarget = nullptr;
        sLocalInvNav.path.Reset();
        return;
    }
    sLocalInvNav.lastTarget = target;

    const Vec3f& targetPos = target->world.pos;

    // ── Proximity check: skip pathfinding when target is close ──────
    // Inside kInvFollowProxLimit (30u 3D), walk straight — substrate
    // pathfinding is overhead when the target is in melee distance.
    // Matches FollowerNPC.cpp:968-977.
    const float pdx0 = targetPos.x - a->world.pos.x;
    const float pdy0 = targetPos.y - a->world.pos.y;
    const float pdz0 = targetPos.z - a->world.pos.z;
    const float dist3DSq = pdx0*pdx0 + pdy0*pdy0 + pdz0*pdz0;
    const bool nearTarget = dist3DSq <= (kInvFollowProxLimit * kInvFollowProxLimit);
    if (nearTarget) {
        sLocalInvNav.path.Reset();
    }

    // ── Path refresh ────────────────────────────────────────────────
    const uint64_t curFrame   = Anchor::Instance->gameFrameCounter.load(
                                    std::memory_order_relaxed);
    const int      refreshTicks = Anchor::Instance->MsToGameTicks(kInvPathRefreshMs);
    const bool needRefresh =
        !nearTarget && (
            sLocalInvNav.path.Empty() ||
            (refreshTicks > 0 &&
             curFrame >= sLocalInvNav.lastPathRefreshFrame + (uint64_t)refreshTicks) ||
            AnchorDist::DistXZSq(targetPos, sLocalInvNav.lastPathTargetPos) >
                kInvPathRetargetDist * kInvPathRetargetDist);
    if (needRefresh) {
        // Trail key — pick the right entity to read breadcrumbs from.
        //   ACTOR_PLAYER (local Link)         → local client's TrailKey.
        //   ACTOR_EN_OE2 (remote DummyPlayer) → owning client's TrailKey
        //                                       (Anchor exposes the cid
        //                                       via GetDummyPlayerClientId).
        //   anything else                     → key=0 (no trail). BFS
        //                                       still works without
        //                                       breadcrumbs — Layer 3
        //                                       (RoomNavData) is the
        //                                       primary reliable layer
        //                                       anyway.
        AnchorNav::TrailKey trailKey = 0;
        if (target->id == ACTOR_PLAYER) {
            trailKey = AnchorNav::TrailKeyForPlayer(
                (uint8_t)Anchor::Instance->ownClientId);
        } else if (target->id == ACTOR_EN_OE2) {
            const uint32_t cid = Anchor::Instance->GetDummyPlayerClientId(target);
            if (cid != 0) {
                trailKey = AnchorNav::TrailKeyForPlayer((uint8_t)cid);
            }
        }
        sLocalInvNav.path.Reset();
        AnchorNav::ActorTrail::GetInstance().ComputePathTo(
            trailKey, a, targetPos, play, sLocalInvNav.path,
            /*skipLayer1LOS=*/false,
            /*preferLeaderTrail=*/false);
        sLocalInvNav.lastPathRefreshFrame = curFrame;
        sLocalInvNav.lastPathTargetPos    = targetPos;
    }

    // ── Phase B: climb-cell transition ───────────────────────────────
    // If the current path subgoal is a climb cell, transition to
    // CLIMBING. The CLIMBING handler takes over snapping XZ to the
    // wall + driving Y. Same shape as FollowerNPC.cpp:1007-1020.
    if (!sLocalInvNav.path.Empty()) {
        const uint32_t flags = sLocalInvNav.path.CurrentSubgoalFlags();
        if (flags & ::AnchorNavRoom::NODE_CLIMB_ANY) {
            this_->state = EN_INVADER_STATE_CLIMBING;
            sLocalInvNav.activeClimbAnchor = nullptr;  // resolved fresh on entry
            SPDLOG_INFO("[Invader] FOLLOW→CLIMBING (path entered climb cell at "
                        "({:.0f},{:.0f},{:.0f}); flags=0x{:X})",
                        sLocalInvNav.path.CurrentSubgoal().x,
                        sLocalInvNav.path.CurrentSubgoal().y,
                        sLocalInvNav.path.CurrentSubgoal().z,
                        flags);
            return;
        }
    }

    // ── Pick subgoal ────────────────────────────────────────────────
    // Substrate path's CurrentSubgoal if available; else direct to target.
    Vec3f subgoal = sLocalInvNav.path.Empty() ? targetPos
                                              : sLocalInvNav.path.CurrentSubgoal();

    // Cursor advancement: if we're close to the current subgoal, step.
    if (!sLocalInvNav.path.Empty() &&
        AnchorDist::DistXZSq(a->world.pos, subgoal) <
            kInvAdvanceSubgoalDist * kInvAdvanceSubgoalDist) {
        sLocalInvNav.path.Advance();
        // Refresh subgoal selection for this same tick — avoid wasting
        // a frame standing in place after an advance.
        subgoal = sLocalInvNav.path.Empty() ? targetPos
                                            : sLocalInvNav.path.CurrentSubgoal();
    }

    // Drive yaw + speed toward the current subgoal.
    const s16 yaw = YawTowardTarget(a->world.pos, subgoal);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;

    // Speed selection — distance is measured to the target (not the
    // subgoal) so pursuit pace stays correlated with how far the chase
    // ultimately is. Mirrors the Invader's prior direct-yaw speed
    // band; substrate routing doesn't change the runtime-energy band.
    const float distSq = Dist2DSq(a->world.pos, targetPos);
    const float dist   = std::sqrt(distSq);
    a->speedXZ = (dist > kInvRunDistance) ? kInvRunSpeed : kInvWalkSpeed;

    // Stuck check — STUCK fires when no real progress happens over
    // the check window. This is the time-based fallback; the path
    // refresh above already short-circuits "stuck because the path
    // was wrong" cases at most every kInvPathRefreshMs.
    const int stuckCheckTicks = Anchor::Instance->MsToGameTicks(kInvStuckCheckMs);
    if (stuckCheckTicks > 0 &&
        curFrame >= sLocalInvNav.lastStuckCheckFrame + (uint64_t)stuckCheckTicks) {
        const float progress = std::sqrt(
            Dist2DSq(a->world.pos, sLocalInvNav.stuckCheckPos));
        if (progress < kInvStuckMinProgress) {
            this_->state = EN_INVADER_STATE_STUCK;
            sLocalInvNav.path.Reset();   // discard the broken path
            sLocalInvNav.lastPathRefreshFrame = 0;
            SPDLOG_INFO("[Invader] FOLLOW→STUCK (no progress {:.1f}u in {}ms @ "
                        "({:.0f},{:.0f},{:.0f}))",
                        progress, kInvStuckCheckMs,
                        a->world.pos.x, a->world.pos.y, a->world.pos.z);
        }
        sLocalInvNav.stuckCheckPos       = a->world.pos;
        sLocalInvNav.lastStuckCheckFrame = curFrame;
    }

    // Transition: arrived at target (IDLE re-entry). Combat states
    // pick up before this fires when target is in melee/strike range.
    // Measure against the actual target (not the subgoal) so the
    // re-entry only fires when we're truly close to the hostile.
    if (distSq <= kInvFollowIdleDist * kInvFollowIdleDist) {
        this_->state = EN_INVADER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalInvNav.path.Reset();
    }
}

// Nav-parity Phase C — path-aware STUCK recovery. The nudge direction
// is taken from the substrate path's current subgoal when available
// (so the Invader nudges around an obstacle in the same direction the
// BFS planned the path), and falls back to a direct nudge toward the
// hostile target only when the path is empty. Mirrors FollowerNPC's
// substrate-aware STUCK in spirit — FollowerNPC's TickSTUCK is
// technically a "nudge toward leader" because its FOLLOW caller
// always resets the path before transitioning to STUCK (the broken
// path is the reason it bailed). We do the same path-reset on the
// FOLLOW→STUCK transition (set in TickFOLLOW above), so the path here
// is empty in the common case — but if FOLLOW didn't get a chance to
// reset (e.g. STUCK reached via some future path that DIDN'T trigger
// the FOLLOW path-reset), we'd still want path-aware steering.
//
// The path Empty() check below is therefore a forward-compat hook: in
// v1 (and matching FollowerNPC's current shape) STUCK practically
// always sees an empty path and nudges straight at the target. As
// future state transitions land STUCK with a populated path, the
// path-aware nudge takes over without code change.
void TickSTUCK(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    Actor* target = sLocalInvNav.lastTarget;
    if (target == nullptr || target->update == nullptr) {
        // No target — drop to IDLE; the dispatcher's next pass picks
        // up FROM IDLE cleanly.
        this_->state = EN_INVADER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalInvNav.path.Reset();
        return;
    }

    // Pick the nudge target. Path subgoal when available (path-aware
    // recovery — nudge in the BFS-planned direction, not blindly toward
    // the hostile). Otherwise fall back to the target's pos.
    Vec3f nudgeTarget = target->world.pos;
    bool  pathAware   = false;
    if (!sLocalInvNav.path.Empty()) {
        nudgeTarget = sLocalInvNav.path.CurrentSubgoal();
        pathAware   = true;
    }

    const s16 yaw = YawTowardTarget(a->world.pos, nudgeTarget);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;
    a->speedXZ     = 0.0f;  // nudge IS the motion, not momentum
    // Direct world.pos write — same shape as FollowerNPC's TickSTUCK.
    // Actor_UpdateBgCheckInfo at the end of EnInvader_Update re-clamps Y.
    const float dx = Math_SinS(yaw) * kInvStuckNudgeDist;
    const float dz = Math_CosS(yaw) * kInvStuckNudgeDist;
    a->world.pos.x += dx;
    a->world.pos.z += dz;
    // Reset baseline so we don't immediately re-trigger STUCK after
    // the nudge.
    sLocalInvNav.stuckCheckPos       = a->world.pos;
    sLocalInvNav.lastStuckCheckFrame =
        Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
    // Discard the path — TickFOLLOW will rebuild it next tick with
    // the new position as the starting point. Without this, the next
    // FOLLOW tick would observe distance > kInvFollowProxLimit AND
    // not yet need a refresh (path is still recent) and steer toward
    // the now-stale subgoal that just caused the stuck condition.
    sLocalInvNav.path.Reset();
    sLocalInvNav.lastPathRefreshFrame = 0;
    this_->state = EN_INVADER_STATE_FOLLOW;
    SPDLOG_INFO("[Invader] STUCK→FOLLOW (nudged {:.0f}u toward yaw=0x{:X}, "
                "{} target)",
                kInvStuckNudgeDist, (uint16_t)yaw,
                pathAware ? "path-aware" : "direct-to");
}

// ---------------------------------------------------------------------
// Nav-parity Phase B — CLIMBING state.
//
// Reference: FollowerNPC.cpp:TickCLIMBING (FollowerNPC.cpp:1632) +
// FindClosestClimbAnchor (FollowerNPC.cpp:1488) + PopulateAnchorClimbPath
// (FollowerNPC.cpp:1517). Mechanics simplified for v1:
//   - No fast-path co-climb (FollowerNPC's "leader is also climbing"
//     direct-track behavior). The Invader chases a hostile, not a
//     friendly leader, so it doesn't get the leader's exact pos as
//     "we're on the wall together"; instead it uses the cell-grid
//     subgoals from the path.
//   - No "leader hoisted over rim" LEDGE_HOIST injection — the target
//     is a hostile, not a follower-leader, so the mantle-out semantics
//     differ. v1 exits to FOLLOW when the path advances to a non-climb
//     subgoal OR the path exhausts.
//   - Mantle-out: snap to topPos when path is exhausted AND NPC is
//     near the anchor's top.
// ---------------------------------------------------------------------

// Walk the room's climb anchors and pick the one whose basePos is
// closest to `pos` in XZ. Cloned from FollowerNPC.cpp:1488.
const ::AnchorNavRoom::ClimbAnchor* FindClosestClimbAnchorInv(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& pos)
{
    if (navData == nullptr || navData->climbAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::ClimbAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& anc : navData->climbAnchors) {
        const float dx = pos.x - anc.basePos.x;
        const float dz = pos.z - anc.basePos.z;
        const float dSq = dx*dx + dz*dz;
        if (dSq < bestDistSq) {
            bestDistSq = dSq;
            best = &anc;
        }
    }
    return best;
}

// Manually populate `path` with this anchor's cells, bottom-to-top,
// strictly above `npcPos.y`. Column-selection projects `referencePos`
// onto the anchor's lateral plane axis and picks cells within ±15u
// (≈ half a 30u cell pitch). The reference is the TARGET position
// (where we want to end up). Falls back to ±30u if the strict
// tolerance yields no cells. Cloned + adapted from
// FollowerNPC.cpp:1517 (PopulateAnchorClimbPath).
//
// Difference from FollowerNPC: the Invader chases a hostile, so the
// reference for column selection is `targetPos`, not the
// follower-leader's pos. The "stay below leader" filter from the
// Follower doesn't apply — we want to keep climbing past the
// hostile's altitude if the path goes higher.
bool PopulateAnchorClimbPathInv(const ::AnchorNavRoom::RoomNavData* navData,
                                 const ::AnchorNavRoom::ClimbAnchor& anchor,
                                 const Vec3f& npcPos, const Vec3f& referencePos,
                                 AnchorNav::ActorTrail::NavPath& path)
{
    if (navData == nullptr || anchor.nodeCount == 0) return false;
    path.Reset();

    const float refU =
        (referencePos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
        (referencePos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;

    struct Entry { float y; uint16_t idx; };
    std::vector<Entry> column;
    constexpr float kInvClimbColTolerance     = 15.0f;
    constexpr float kInvClimbColFallbackTol   = 30.0f;
    auto collectColumn = [&](float tolerance) {
        column.clear();
        for (uint16_t i = 0; i < anchor.nodeCount; i++) {
            const uint16_t idx = anchor.firstNodeIdx + i;
            if (idx >= navData->nodes.size()) break;
            const auto& n = navData->nodes[idx];
            const float nodeU =
                (n.pos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
                (n.pos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;
            if (std::fabs(nodeU - refU) > tolerance) continue;
            column.push_back({n.pos.y, idx});
        }
    };
    collectColumn(kInvClimbColTolerance);
    if (column.empty()) {
        collectColumn(kInvClimbColFallbackTol);
    }
    if (column.empty()) return false;

    std::sort(column.begin(), column.end(),
              [](const Entry& a, const Entry& b){ return a.y < b.y; });

    // Strict ABOVE-NPC filter — no downward slack. Prevents re-entry
    // oscillation (NPC climbs up, re-engages CLIMBING with cells
    // starting BELOW current Y, descends, repeats). Same shape as
    // FollowerNPC.cpp:1591-1602.
    for (const auto& e : column) {
        if (e.y < npcPos.y) continue;
        const auto& n = navData->nodes[e.idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    if (path.waypoints.empty()) {
        // NPC is already at or above the entire column — push the top
        // cell so CLIMBING has something to track.
        const auto& n = navData->nodes[column.back().idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    path.sceneNum = gPlayState->sceneNum;
    return true;
}

void TickCLIMBING(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;

    // Re-acquire target so we know whether to keep climbing. If the
    // target is gone, exit to FOLLOW (next tick will re-IDLE).
    Actor* target = sLocalInvNav.lastTarget;
    if (target == nullptr || target->update == nullptr) {
        this_->state = EN_INVADER_STATE_FOLLOW;
        sLocalInvNav.activeClimbAnchor = nullptr;
        sLocalInvNav.path.Reset();
        return;
    }

    // ── Resolve subgoal ────────────────────────────────────────────
    if (sLocalInvNav.path.Empty()) {
        // Path exhausted. If we have an active anchor, try to refresh
        // toward the target's lateral column — keeps the Invader on
        // the wall when target is still above us. Otherwise mantle out.
        if (sLocalInvNav.activeClimbAnchor != nullptr) {
            const ::AnchorNavRoom::RoomNavData* navData =
                ::AnchorNavRoom::GetForRoom(
                    gPlayState->sceneNum,
                    (int8_t)gPlayState->roomCtx.curRoom.num);
            if (navData != nullptr &&
                target->world.pos.y > a->world.pos.y + 30.0f &&
                PopulateAnchorClimbPathInv(navData,
                                            *sLocalInvNav.activeClimbAnchor,
                                            a->world.pos, target->world.pos,
                                            sLocalInvNav.path)) {
                // Path refreshed; fall through to subgoal resolution.
            }
        }
        if (sLocalInvNav.path.Empty()) {
            // Mantle-out: if NPC is near the top of the wall, snap to
            // topPos so we don't drop. Same shape as
            // FollowerNPC.cpp:1772-1794.
            if (sLocalInvNav.activeClimbAnchor != nullptr) {
                const float topY = sLocalInvNav.activeClimbAnchor->topPos.y;
                if (a->world.pos.y >= topY - 60.0f) {
                    a->world.pos  = sLocalInvNav.activeClimbAnchor->topPos;
                    a->velocity.y = 0.0f;
                    SPDLOG_INFO("[Invader] CLIMBING→FOLLOW (mantle-out: "
                                "snapped to anchor.topPos "
                                "({:.0f},{:.0f},{:.0f}))",
                                a->world.pos.x, a->world.pos.y, a->world.pos.z);
                }
            }
            this_->state = EN_INVADER_STATE_FOLLOW;
            sLocalInvNav.activeClimbAnchor = nullptr;
            return;
        }
    }

    const Vec3f& subgoal        = sLocalInvNav.path.CurrentSubgoal();
    const uint32_t subgoalFlags = sLocalInvNav.path.CurrentSubgoalFlags();
    const bool subgoalIsClimb   = (subgoalFlags & ::AnchorNavRoom::NODE_CLIMB_ANY) != 0;

    // Mantle-out: next subgoal is non-climb → snap to subgoal pos
    // (top of the climb), exit to FOLLOW. Same shape as
    // FollowerNPC.cpp:1800-1810.
    if (!subgoalIsClimb) {
        a->world.pos.x = subgoal.x;
        a->world.pos.y = subgoal.y;
        a->world.pos.z = subgoal.z;
        a->speedXZ     = 0.0f;
        sLocalInvNav.activeClimbAnchor = nullptr;
        this_->state = EN_INVADER_STATE_FOLLOW;
        SPDLOG_INFO("[Invader] CLIMBING→FOLLOW (mantle-out to non-climb "
                    "subgoal ({:.0f},{:.0f},{:.0f}))",
                    subgoal.x, subgoal.y, subgoal.z);
        return;
    }

    // ── Resolve / cache the active anchor ──────────────────────────
    if (sLocalInvNav.activeClimbAnchor == nullptr) {
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        sLocalInvNav.activeClimbAnchor = FindClosestClimbAnchorInv(
            navData, a->world.pos);
        if (sLocalInvNav.activeClimbAnchor == nullptr) {
            // No anchor data — fall back to FOLLOW.
            this_->state = EN_INVADER_STATE_FOLLOW;
            return;
        }
    }
    const auto& anc = *sLocalInvNav.activeClimbAnchor;

    // ── Snap XZ + drive Y ──────────────────────────────────────────
    // Snap XZ to subgoal + planeNormal * bodyOffset so the Invader's
    // body sits in front of the wall surface (not buried). Identical
    // to FollowerNPC's non-leader-climbing branch (FollowerNPC.cpp:1841).
    a->world.pos.x = subgoal.x + anc.planeNormal.x * kInvClimbBodyOffset;
    a->world.pos.z = subgoal.z + anc.planeNormal.z * kInvClimbBodyOffset;

    // Drive Y toward subgoal. Clamp on approach.
    const float dy = subgoal.y - a->world.pos.y;
    if (std::fabs(dy) < kInvClimbSpeedY) {
        a->world.pos.y = subgoal.y;
    } else {
        a->world.pos.y += (dy > 0.0f ? kInvClimbSpeedY : -kInvClimbSpeedY);
    }

    // Face into the wall. Yaw = direction OPPOSITE to planeNormal.
    a->shape.rot.y = Math_Atan2S(-anc.planeNormal.z, -anc.planeNormal.x);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ     = 0.0f;

    // Anim selection — alternate kClimbUpL ↔ kClimbUpR as climb steps
    // complete. Mirrors Player's actionVar2 toggle. The InvEnsureAnimation
    // call lives in the dispatcher post-state-handler block; we just
    // toggle `climbNextIsRight` here when a step finishes (curFrame
    // crosses anim endFrame).
    if (this_->skelAnime.curFrame >=
        Animation_GetLastFrame((void*)this_->skelAnime.animation)) {
        // Step completed — flip phase for the next one-shot.
        this_->climbNextIsRight = !this_->climbNextIsRight;
    }

    // ── Advance cursor on Y-proximity ──────────────────────────────
    // Y-axis only (XZ is snap-clamped to subgoal+offset; full 3D
    // distance would chain advances on the first frame). Same logic
    // as FollowerNPC.cpp:1880-1894.
    const float dyAdv = std::fabs(a->world.pos.y - subgoal.y);
    if (dyAdv < 12.0f) {
        sLocalInvNav.path.Advance();
    }
}

// Try to force-engage CLIMBING based on the target being above the
// Invader AND a climb anchor being reachable. This complements the
// natural FOLLOW→CLIMBING transition (via the path's climb-cell flag)
// for cases where the BFS pathfinder fails to route to a target
// who's mid-wall (the pathfinder's FindNearestNode skips climb cells;
// cross-room nav not supported). Cloned + adapted from
// FollowerNPC.cpp:3584-3671 (leader-climbing force-engage). The
// Invader's gate is "target is meaningfully above us" instead of
// "leader is climbing".
//
// Fires only when in non-CLIMBING states (caller-checked). Returns
// true if engagement succeeded (state set to CLIMBING + path populated).
bool TryEngageAutoClimbInv(EnInvader* this_, PlayState* play, Actor* target) {
    if (this_ == nullptr || play == nullptr || target == nullptr) return false;
    if (this_->state == EN_INVADER_STATE_CLIMBING) return false;
    // Only engage when target is meaningfully above us — climbing
    // downward is rare and the substrate path's drop-anchor route is
    // the right answer when target is below.
    if (target->world.pos.y <= this_->actor.world.pos.y + 40.0f) return false;

    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num);
    const ::AnchorNavRoom::ClimbAnchor* anchor =
        FindClosestClimbAnchorInv(navData, target->world.pos);
    if (anchor == nullptr) return false;

    const float distBaseSq = Dist2DSq(this_->actor.world.pos, anchor->basePos);
    if (distBaseSq >= kInvClimbForceEngageBaseDistSq) return false;

    if (!PopulateAnchorClimbPathInv(navData, *anchor,
                                     this_->actor.world.pos,
                                     target->world.pos,
                                     sLocalInvNav.path)) {
        return false;
    }
    sLocalInvNav.activeClimbAnchor = anchor;
    this_->state                    = EN_INVADER_STATE_CLIMBING;
    sLocalInvNav.leashFrames        = 0;
    sLocalInvNav.closeFailFrames    = 0;
    SPDLOG_INFO("[Invader] Auto-climb force-engage — anchor "
                "base=({:.0f},{:.0f},{:.0f}) top=({:.0f},{:.0f},{:.0f}) "
                "Inv at ({:.0f},{:.0f},{:.0f}) distBase={:.0f}u — "
                "populated {} climb waypoints",
                anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                anchor->topPos.x, anchor->topPos.y, anchor->topPos.z,
                this_->actor.world.pos.x, this_->actor.world.pos.y,
                this_->actor.world.pos.z,
                std::sqrt(distBaseSq),
                (int)sLocalInvNav.path.waypoints.size());
    return true;
}

// ---------------------------------------------------------------------
// Phase 4 — TickSWIMMING / TickLEDGE_HOIST.
//
// Cloned from FollowerNPC's same-named handlers (FollowerNPC.cpp:1929 +
// :2089). Key deltas:
//   - SWIMMING swims toward the nearest hostile target (PickHostileTarget),
//     not toward a leader. Falls through to FOLLOW when target lost.
//   - LEDGE_HOIST does the same one-shot mantle pattern but the entry
//     trigger comes from the dispatcher's geometry probe, NOT a synced
//     hoistContext from peers (Invader v1 doesn't sync hoist state —
//     each side runs its own probe). The probe is fired only on the
//     local host's tick so peer replicas don't double-hoist.
//   - No drown timeout. Invader is hostile and can swim indefinitely.
//     FollowerNPC has a 30s drown timer for friendly-NPC death; an
//     Invader drown would just kill it prematurely. Documented in
//     plan §1.2 deferred.
// ---------------------------------------------------------------------

void TickSWIMMING(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;

    // Sample water surface at NPC's XZ. If no waterbox, exit to FOLLOW
    // (we swam off the edge of the box and are now overland).
    f32 surfaceY = a->world.pos.y;
    WaterBox* wb = nullptr;
    if (!WaterBox_GetSurface1(play, &play->colCtx,
                              a->world.pos.x, a->world.pos.z,
                              &surfaceY, &wb)) {
        SPDLOG_INFO("[Invader] SWIMMING→FOLLOW (no water under NPC at "
                    "({:.0f},{:.0f},{:.0f}))",
                    a->world.pos.x, a->world.pos.y, a->world.pos.z);
        this_->state = EN_INVADER_STATE_FOLLOW;
        return;
    }

    // Shore-shallow exit. Probe floor below NPC. If floor is within
    // kInvSwimShoreExitDepth of surface, NPC can stand — exit swim.
    Vec3f rayStart = { a->world.pos.x, surfaceY + 5.0f, a->world.pos.z };
    CollisionPoly* floorPoly = nullptr;
    const f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx,
                                                    &floorPoly, &rayStart);
    if (floorPoly != nullptr &&
        (surfaceY - floorY) < kInvSwimShoreExitDepth) {
        a->world.pos.y = floorY;
        a->velocity.y  = 0.0f;
        this_->state   = EN_INVADER_STATE_FOLLOW;
        SPDLOG_INFO("[Invader] SWIMMING→FOLLOW (shore-shallow exit: "
                    "surface={:.0f} floor={:.0f} depth={:.1f}u < {:.0f}u)",
                    surfaceY, floorY, surfaceY - floorY,
                    kInvSwimShoreExitDepth);
        return;
    }

    // Clamp Y to (surface - depth). Slightly extra drop matches
    // FollowerNPC's tuning (NPC was 5u too high above water).
    const float swimDepth = InvSwimDepthFor(this_->linkAge) + kInvSwimExtraDrop;
    a->world.pos.y = surfaceY - swimDepth;
    a->velocity.y  = 0.0f;

    // Swim toward nearest hostile target.
    Actor* target = PickHostileTarget(a, play, kInvFollowEngageDist,
                                       /*maxYDelta=*/kRangedYFilter);
    if (target == nullptr) {
        sLocalInvNav.lastTarget = nullptr;
        a->speedXZ = 0.0f;
        // No hostile in range — keep treading water; SWIMMING handler
        // will pick up a target on a later tick if one approaches.
        // Falling back to FOLLOW from here would oscillate against the
        // water-entry trigger in the dispatcher.
        return;
    }
    sLocalInvNav.lastTarget = target;

    const float dx = target->world.pos.x - a->world.pos.x;
    const float dz = target->world.pos.z - a->world.pos.z;
    const float distSq = dx * dx + dz * dz;
    if (distSq > kInvSwimArrive * kInvSwimArrive) {
        const s16 yaw = Math_Atan2S(dz, dx);
        a->shape.rot.y = yaw;
        a->world.rot.y = yaw;
        a->speedXZ     = kInvSwimSpeedMax;
        // Manual XZ motion — Actor_MoveXZGravity is skipped for
        // SWIMMING in EnInvader_Update so we drive position directly.
        a->world.pos.x += Math_SinS(yaw) * a->speedXZ;
        a->world.pos.z += Math_CosS(yaw) * a->speedXZ;
    } else {
        a->speedXZ = 0.0f;  // tread water; kSwimWait picked by AnimForState
    }
}

// Helper — probe straight up from NPC's head for an overhead ledge.
// Matches FollowerNPC's autonomous swim-exit probe pattern
// (FollowerNPC.cpp:3708-3723). Returns true + writes outTopPos when a
// hoistable ledge is detected.
bool InvDetectSwimHoist(PlayState* play, const Vec3f& npcPos, Vec3f& outTopPos) {
    Vec3f probeStart = { npcPos.x, npcPos.y + kInvHoistSwimProbeStartY, npcPos.z };
    CollisionPoly* topPoly = nullptr;
    const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx,
                                                  &topPoly, &probeStart);
    if (topPoly == nullptr) return false;
    const float lift = topY - npcPos.y;
    if (lift <= kInvHoistSwimLiftMin || lift >= kInvHoistSwimLiftMax) {
        return false;
    }
    outTopPos.x = probeStart.x;
    outTopPos.y = topY;
    outTopPos.z = probeStart.z;
    return true;
}

// Helper — probe forward then down to detect a hoistable wall ledge
// in the NPC's facing direction. Matches FollowerNPC's
// RaycastDetectLedge (FollowerNPC.cpp:2040). Used for ground-hoist
// (FOLLOW→LEDGE_HOIST) when target is at higher altitude.
bool InvRaycastDetectLedge(PlayState* play, const Vec3f& npcPos,
                            s16 facingYaw, Vec3f& outTopPos) {
    const float dirX = Math_SinS(facingYaw);
    const float dirZ = Math_CosS(facingYaw);

    Vec3f rayA = { npcPos.x, npcPos.y + kInvHoistChestHeight, npcPos.z };
    Vec3f rayB = { npcPos.x + dirX * kInvHoistForwardCastDist, rayA.y,
                   npcPos.z + dirZ * kInvHoistForwardCastDist };
    Vec3f wallHit;
    CollisionPoly* wallPoly = nullptr;
    if (!BgCheck_AnyLineTest1(&play->colCtx, &rayA, &rayB, &wallHit,
                              &wallPoly, 1)) {
        return false;
    }

    Vec3f downStart = {
        wallHit.x + dirX * kInvHoistPastWallNudge,
        wallHit.y + kInvHoistGroundLiftMax,
        wallHit.z + dirZ * kInvHoistPastWallNudge,
    };
    CollisionPoly* topPoly = nullptr;
    const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx, &topPoly,
                                                  &downStart);
    if (topPoly == nullptr) return false;
    const float lift = topY - npcPos.y;
    if (lift < kInvHoistGroundLiftMin || lift > kInvHoistGroundLiftMax) {
        return false;
    }
    outTopPos.x = downStart.x;
    outTopPos.y = topY;
    outTopPos.z = downStart.z;
    return true;
}

// Helper — fire LEDGE_HOIST with a given context + target pos. Cloned
// from FollowerNPC's enterLedgeHoist lambda (FollowerNPC.cpp:3642).
// Captures the start position for the per-tick lerp in TickLEDGE_HOIST.
void InvEnterLedgeHoist(EnInvader* this_, InvHoistContext ctx,
                         const Vec3f& topPos, const char* source) {
    Actor* a = &this_->actor;
    this_->hoistContext   = (s8)ctx;
    this_->hoistTargetPos = topPos;
    this_->hoistEntryYaw  =
        Math_Atan2S(topPos.z - a->world.pos.z, topPos.x - a->world.pos.x);

    // Snap XZ to ledge top XZ at entry so the anim plays in place
    // (only Y will lerp over the anim duration). Without this, an
    // XZ lerp during a 1-second mantle anim looks like "walking
    // sideways while hunched" — the climb_up anim assumes a
    // stationary body pulling up.
    a->world.pos.x = topPos.x;
    a->world.pos.z = topPos.z;
    sLocalInvHoistStartPos = a->world.pos;

    // For swim-exit, raise NPC ~43u so the swim_15step_up anim plays
    // at ledge level rather than underwater. End-of-anim snap moves
    // NPC to the exact ledge top.
    if (ctx == INV_HOIST_CONTEXT_SWIM) {
        a->world.pos.y += kInvSwimHoistRaise;
        a->velocity.y = 0.0f;
        sLocalInvHoistStartPos = a->world.pos;
    }

    this_->state = EN_INVADER_STATE_LEDGE_HOIST;
    // Clear any in-flight stop/fidget hold so the dispatcher's
    // LEDGE_HOIST anim override survives the stopAnimPlaying check.
    this_->stopAnimPlaying = 0;
    SPDLOG_INFO("[Invader] →LEDGE_HOIST({}) top=({:.0f},{:.0f},{:.0f}) "
                "via {}",
                (ctx == INV_HOIST_CONTEXT_SWIM ? "swim_exit" : "ground"),
                topPos.x, topPos.y, topPos.z, source);
}

void TickLEDGE_HOIST(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    a->speedXZ     = 0.0f;
    a->shape.rot.y = this_->hoistEntryYaw;
    a->world.rot.y = this_->hoistEntryYaw;

    // Wait for the hoist anim to be (a) set up by InvEnsureAnimation
    // AND (b) played to completion. Without check (a), the entry tick
    // would exit immediately because the dispatcher runs TickLEDGE_HOIST
    // BEFORE InvEnsureAnimation in the per-tick order — stopAnimPlaying
    // is still 0 (stale from prior state) on entry. See FollowerNPC's
    // analogous fix at FollowerNPC.cpp:2106.
    const bool hoistAnimSetUp =
        (InvaderAnim)this_->currentAnim == InvaderAnim::kHoistGround ||
        (InvaderAnim)this_->currentAnim == InvaderAnim::kHoistSwim;
    if (!hoistAnimSetUp || this_->stopAnimPlaying) {
        if (hoistAnimSetUp && this_->stopAnimPlaying &&
            this_->skelAnime.endFrame > 0.0f) {
            // Lerp pos from start → target over anim progress so body
            // visibly translates up during the mantle motion.
            const float progress =
                std::min(1.0f, this_->skelAnime.curFrame /
                                this_->skelAnime.endFrame);
            const Vec3f& s = sLocalInvHoistStartPos;
            const Vec3f& t = this_->hoistTargetPos;
            a->world.pos.x = s.x + (t.x - s.x) * progress;
            a->world.pos.y = s.y + (t.y - s.y) * progress;
            a->world.pos.z = s.z + (t.z - s.z) * progress;
        }
        return;
    }

    // Anim complete — snap to ledge top and exit to FOLLOW.
    a->world.pos  = this_->hoistTargetPos;
    a->velocity.y = 0.0f;
    sLocalInvNav.stuckCheckPos = a->world.pos;
    sLocalInvNav.lastStuckCheckFrame =
        Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
    sLocalInvNav.leashFrames = 0;
    this_->state = EN_INVADER_STATE_FOLLOW;
    SPDLOG_INFO("[Invader] LEDGE_HOIST→FOLLOW (snapped to "
                "({:.0f},{:.0f},{:.0f}), context={})",
                this_->hoistTargetPos.x, this_->hoistTargetPos.y,
                this_->hoistTargetPos.z, (int)this_->hoistContext);
}

// G10 leash — Invader too far from its target for > kInvLeashTimeoutMs
// of consecutive ticks → teleport to the target. Catches Invader stuck
// behind closed door / left in another room / fell into untracked
// geometry. Returns true if teleport fired (caller skips rest of tick).
//
// CLIMBING / SWIMMING / LEDGE_HOIST don't apply to v1 Invader — only
// active states are IDLE/FOLLOW/STUCK + combat — so this fires
// whenever target distance exceeds the leash, regardless of state
// EXCEPT when the Invader is currently swinging (ATTACK /
// RANGED_ATTACK / BLOCK / ENGAGE state in active combat) — those are
// scripted moves and shouldn't be cut short by a leash teleport.
bool TryFireG10Invader(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    // Find target — for leash purposes we use the picker's full-range
    // version (no Y/range filter) so a target far away or elevated
    // still counts as "should leash to it". If no target at all, no
    // leash applies.
    Actor* target = PickHostileTargetForInvader(a, play);
    if (target == nullptr || target->update == nullptr) {
        sLocalInvNav.leashFrames = 0;
        return false;
    }
    const float dx = a->world.pos.x - target->world.pos.x;
    const float dy = a->world.pos.y - target->world.pos.y;
    const float dz = a->world.pos.z - target->world.pos.z;
    const float dist3D = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist3D <= kInvLeashDistance) {
        sLocalInvNav.leashFrames = 0;
        return false;
    }
    sLocalInvNav.leashFrames++;
    const int timeoutTicks = Anchor::Instance->MsToGameTicks(kInvLeashTimeoutMs);
    if (timeoutTicks <= 0 || (int)sLocalInvNav.leashFrames < timeoutTicks) {
        return false;
    }
    SPDLOG_INFO("[Invader] G10 leash teleport — dist3D={:.0f}u for {} frames "
                "(>{}u for >{}ms) → snap to target",
                dist3D, sLocalInvNav.leashFrames,
                (int)kInvLeashDistance, kInvLeashTimeoutMs);
    a->world.pos = target->world.pos;
    a->speedXZ   = 0.0f;
    sLocalInvNav.leashFrames        = 0;
    sLocalInvNav.stuckCheckPos      = a->world.pos;
    sLocalInvNav.lastStuckCheckFrame =
        Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
    this_->state = EN_INVADER_STATE_FOLLOW;
    // Snap Y to floor at new pos to avoid sink/float on landing.
    Actor_UpdateBgCheckInfo(play, a, 26.0f, 10.0f, 50.0f, 4);
    return true;
}

const char* StateName(s32 s) {
    switch (s) {
        case EN_INVADER_STATE_IDLE:          return "IDLE";
        case EN_INVADER_STATE_FOLLOW:        return "FOLLOW";
        case EN_INVADER_STATE_CLIMBING:      return "CLIMBING";
        case EN_INVADER_STATE_STUCK:         return "STUCK";
        case EN_INVADER_STATE_DEAD:          return "DEAD";
        case EN_INVADER_STATE_SWIMMING:      return "SWIMMING";
        case EN_INVADER_STATE_LEDGE_HOIST:   return "LEDGE_HOIST";
        case EN_INVADER_STATE_ATTACK:        return "ATTACK";
        case EN_INVADER_STATE_ENGAGE:        return "ENGAGE";
        case EN_INVADER_STATE_BLOCK:         return "BLOCK";
        case EN_INVADER_STATE_RANGED_ATTACK: return "RANGED_ATTACK";
        case EN_INVADER_STATE_STANDBY:       return "STANDBY";
        case EN_INVADER_STATE_CRAWLING:      return "CRAWLING";
        default:                             return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------
// Target picking — thin range/Y-delta filter on top of Agent 4's
// PickHostileTargetForInvader. The Agent 4 picker walks the FULL
// session (local Player + every in-timeline DummyPlayer + the NPC
// Follower when targetable), applies session-level validity gates
// (cutscene / save-loaded / scene-blacklist), and returns the closest
// valid candidate. Our wrapper layers the combat-tier range + Y-delta
// filters on top so each tier (melee 80u, pursue 250u, ranged 500u)
// can independently decide whether the picker's result is in scope.
//
// Caller validates non-null + alive before use.
// ---------------------------------------------------------------------
Actor* PickHostileTarget(Actor* self, PlayState* play, float maxRange,
                         float maxYDelta) {  // default in forward-decl at line 212
    Actor* candidate = PickHostileTargetForInvader(self, play);
    if (candidate == nullptr || candidate->update == nullptr) return nullptr;
    const float dx = candidate->world.pos.x - self->world.pos.x;
    const float dz = candidate->world.pos.z - self->world.pos.z;
    const float dy = candidate->world.pos.y - self->world.pos.y;
    if (std::fabs(dy) > maxYDelta) return nullptr;
    const float distSq = dx * dx + dz * dz;
    if (distSq > maxRange * maxRange) return nullptr;
    return candidate;
}

// ---------------------------------------------------------------------
// AT quad positioning — flat plane in front of the Invader at chest
// height. Same vertex order as FollowerNPC's PositionAttackQuad.
// ---------------------------------------------------------------------
void PositionAttackQuad(EnInvader* this_) {
    Actor* a = &this_->actor;
    const float yawRad = (float)a->shape.rot.y * (3.14159265f / 32768.0f);
    const float fx = sinf(yawRad);
    const float fz = cosf(yawRad);
    const float rx = cosf(yawRad);
    const float rz = -sinf(yawRad);
    const Vec3f& p = a->world.pos;
    Vec3f bottomLeft  = { p.x + fx * kAttackQuadForward - rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward - rz * kAttackQuadHalfWidth };
    Vec3f bottomRight = { p.x + fx * kAttackQuadForward + rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward + rz * kAttackQuadHalfWidth };
    Vec3f topRight    = { bottomRight.x, p.y + kAttackQuadTopY, bottomRight.z };
    Vec3f topLeft     = { bottomLeft.x,  p.y + kAttackQuadTopY, bottomLeft.z };
    Collider_SetQuadVertices(&this_->atCollider, &bottomLeft, &bottomRight,
                             &topLeft, &topRight);
}

bool IsFrontalAttacker(EnInvader* this_, Actor* attacker) {
    if (attacker == nullptr) return false;
    Actor* a = &this_->actor;
    const s16 yawToAttacker = YawTowardTarget(a->world.pos, attacker->world.pos);
    const s16 delta = (s16)(yawToAttacker - a->shape.rot.y);
    const int absDelta = (delta < 0) ? -(int)delta : (int)delta;
    return absDelta < kBlockFrontalAngle;
}

bool InvaderHasRangedWeapon() {
    // v1: gate ranged engagement on the local player's inventory so
    // an Invader spawned in early-game (no bow/slingshot owned) does
    // not fire arrows. Agent 4 may revisit this — Invader's gear is
    // not necessarily tied to player inventory in the final design.
    const u8 bowSlot   = INV_CONTENT(ITEM_BOW);
    const u8 slingSlot = INV_CONTENT(ITEM_SLINGSHOT);
    return (bowSlot != ITEM_NONE) || (slingSlot != ITEM_NONE);
}

// Forward decl — ChooseCombatExitState is defined below the combat
// Tick handlers but called from each of them. C++ single-pass lookup
// for free functions requires the declaration to appear first.
s32 ChooseCombatExitState(EnInvader* this_, PlayState* play);

// ---------------------------------------------------------------------
// Tick handlers.
// ---------------------------------------------------------------------

void TickATTACK(EnInvader* this_, PlayState* play, const Vec3f& targetSeedPos) {
    (void)targetSeedPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    if (this_->prevState != EN_INVADER_STATE_ATTACK) {
        sAttackState.entryFrame = Anchor::Instance->gameFrameCounter.load(
                                       std::memory_order_relaxed);
        // Parity gap 1 — fire kSwordSwing on entry. stopAnimPlaying=0
        // ensures EnsureAnimation can pick a new anim even if an older
        // one-shot was in flight. Combat anims are always armed/fighter
        // (modelAnimType=1) regardless of dispatcher hint.
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kSwordSwing, 1);
    }

    if (sAttackState.target != nullptr &&
        (sAttackState.target->update == nullptr)) {
        sAttackState.target = nullptr;
    }

    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    const float curFrame = this_->skelAnime.curFrame;
    const bool inActiveWindow = (curFrame >= kAttackActiveStartFrame &&
                                  curFrame <= kAttackActiveEndFrame);
    if (inActiveWindow && !sAttackState.swingFiredAT) {
        PositionAttackQuad(this_);
        CollisionCheck_SetAT(play, &play->colChkCtx, &this_->atCollider.base);
    } else if (!inActiveWindow && curFrame > kAttackActiveEndFrame) {
        sAttackState.swingFiredAT = true;
    }

    // Exit only when anim has actually started AND finished, AND a
    // minimum hold has elapsed since entry. If Agent 2 hasn't wired
    // anim selection yet, endFrame may stay at the previous state's
    // value (typically non-zero); without these guards the state
    // would early-exit on the first tick because (curFrame >=
    // endFrame) is true from leftover idle-anim state.
    const uint64_t curTick = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    const bool holdElapsed = curTick >= sAttackState.entryFrame + kMinSwingHoldTicks;
    if (holdElapsed && this_->skelAnime.endFrame > 0.0f &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ATTACK→{} (swing complete)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.swingFiredAT = false;
    }
}

void TickENGAGE(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;

    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ENGAGE→{} (target lost)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    const Vec3f& targetPos = sAttackState.target->world.pos;
    const float dx = targetPos.x - a->world.pos.x;
    const float dz = targetPos.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);

    if (distXZ > kEngageBreakDist) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] ENGAGE→{} (target fled {:.0f}u > {:.0f}u)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"),
                    distXZ, kEngageBreakDist);
        this_->state = next;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    if (distXZ <= kEngageStrikeDist) {
        SPDLOG_INFO("[Invader] ENGAGE→ATTACK (strike range, dist={:.0f}u)", distXZ);
        this_->state = EN_INVADER_STATE_ATTACK;
        sAttackState.swingFiredAT = false;
        a->speedXZ = 0.0f;
        return;
    }

    // Pursuit — walk close, run far.
    a->shape.rot.y = YawTowardTarget(a->world.pos, targetPos);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ = (distXZ > kEngageRunDistance) ? kEngageRunSpeed : kEngageWalkSpeed;
}

void TickBLOCK(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    if (this_->prevState != EN_INVADER_STATE_BLOCK) {
        sBlockState.entryFrame    = curFrame;
        sBlockState.hitAnimFrames = 0;
        // Parity gap 1 — fire kBlockWait on entry. Loop anim so the
        // pose holds for the full block duration. Dispatcher overrides
        // to kBlockHit during the hit-reaction window.
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kBlockWait, 1);
        SPDLOG_INFO("[Invader] BLOCK entry — HP={}", (int)this_->health);
    }

    // Parity gap 1 — kBlockHit override while the hit-reaction counter
    // is non-zero. One-shot anim plays out then dispatcher returns to
    // kBlockWait via the standard hold-then-resume pattern.
    if (sBlockState.hitAnimFrames > 0 &&
        (InvaderAnim)this_->currentAnim != InvaderAnim::kBlockHit) {
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kBlockHit, 1);
    } else if (sBlockState.hitAnimFrames == 0 &&
               (InvaderAnim)this_->currentAnim == InvaderAnim::kBlockHit &&
               this_->skelAnime.endFrame > 0.0f &&
               this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        // Hit anim complete — return to held block pose.
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kBlockWait, 1);
    }

    if (sAttackState.target != nullptr &&
        sAttackState.target->update == nullptr) {
        sAttackState.target = nullptr;
    }

    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // Frontal-deflect — TickInvaderActor runs BEFORE the C-code AC
    // drain in EnInvader_Update, so we can consume the AC_HIT flag
    // here and zero the damage before health is decremented. Only
    // deflect hits where the attacker is in the Invader's frontal
    // cone (block target known + frontally placed). Side/back hits
    // pass through to the normal drain.
    if ((this_->collider.base.acFlags & AC_HIT) &&
        sAttackState.target != nullptr &&
        IsFrontalAttacker(this_, sAttackState.target)) {
        this_->collider.base.acFlags  &= ~AC_HIT;
        this_->actor.colChkInfo.damage = 0;
        sBlockState.hitAnimFrames = 12;  // play kBlockHit reaction (Agent 2 anim)
        SPDLOG_INFO("[Invader] BLOCK deflect — frontal hit absorbed");
    }

    if (sBlockState.hitAnimFrames > 0) {
        sBlockState.hitAnimFrames--;
    }

    const uint64_t durationTicks =
        (uint64_t)Anchor::Instance->MsToGameTicks(kBlockDurationMs);
    if (curFrame >= sBlockState.entryFrame + durationTicks) {
        if (sAttackState.target != nullptr) {
            const float dx = sAttackState.target->world.pos.x - a->world.pos.x;
            const float dz = sAttackState.target->world.pos.z - a->world.pos.z;
            const float distXZ = std::sqrt(dx * dx + dz * dz);
            if (distXZ <= kAttackEngageDist) {
                SPDLOG_INFO("[Invader] BLOCK→ATTACK (timer expired, target in range "
                            "dist={:.0f})", distXZ);
                this_->state = EN_INVADER_STATE_ATTACK;
                sAttackState.swingFiredAT = false;
                return;
            }
        }
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] BLOCK→{} (timer expired)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        if (next != EN_INVADER_STATE_STANDBY) {
            sAttackState.target = nullptr;
        }
    }
}

void TickRANGED_ATTACK(EnInvader* this_, PlayState* play, const Vec3f& leaderHintPos) {
    (void)leaderHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    if (this_->prevState != EN_INVADER_STATE_RANGED_ATTACK) {
        sAttackState.entryFrame = Anchor::Instance->gameFrameCounter.load(
                                       std::memory_order_relaxed);
        // Parity gap 1 — fire kBowShoot on entry. One-shot draw + release.
        // Arrow spawns at curFrame >= kRangedSpawnFrame (~frame 5).
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kBowShoot, 1);
    }

    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (target lost mid-shoot)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    const Vec3f& tp = sAttackState.target->world.pos;
    const float dx = tp.x - a->world.pos.x;
    const float dz = tp.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ > kRangedBreakDist) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (target fled {:.0f}u > {:.0f}u)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"),
                    distXZ, kRangedBreakDist);
        this_->state = next;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    a->shape.rot.y = YawTowardTarget(a->world.pos, tp);
    a->world.rot.y = a->shape.rot.y;

    if (!sAttackState.swingFiredAT &&
        this_->skelAnime.curFrame >= kRangedSpawnFrame) {
        const float dy = tp.y - a->world.pos.y;
        const s16 pitch = (s16)(-Math_Atan2S(distXZ, -dy));

        Vec3f spawnPos = {
            a->world.pos.x,
            a->world.pos.y + kRangedSpawnHeightY,
            a->world.pos.z,
        };
        Actor* arrow = Actor_Spawn(
            &play->actorCtx, play, ACTOR_EN_ARROW,
            spawnPos.x, spawnPos.y, spawnPos.z,
            pitch, a->shape.rot.y, 0,
            ARROW_NORMAL);
        sAttackState.swingFiredAT = true;
        if (arrow != nullptr) {
            SPDLOG_INFO("[Invader] RANGED_ATTACK fire — dist={:.0f} pitch=0x{:X} "
                        "yaw=0x{:X}",
                        distXZ, (uint16_t)pitch, (uint16_t)a->shape.rot.y);
        } else {
            SPDLOG_WARN("[Invader] RANGED_ATTACK Actor_Spawn(EN_ARROW) returned null");
        }
    }

    // Same anim-not-wired + min-hold guards as ATTACK.
    const uint64_t curTick = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    const bool holdElapsed = curTick >= sAttackState.entryFrame + kMinSwingHoldTicks;
    if (holdElapsed && this_->skelAnime.endFrame > 0.0f &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 next = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[Invader] RANGED_ATTACK→{} (shoot complete)",
                    (next == EN_INVADER_STATE_STANDBY ? "STANDBY" : "IDLE/FOLLOW"));
        this_->state = next;
        if (next != EN_INVADER_STATE_STANDBY) {
            sAttackState.target = nullptr;
        }
        sAttackState.swingFiredAT = false;
    }
}

void TickSTANDBY(EnInvader* this_, PlayState* play, const Vec3f& targetHintPos) {
    (void)targetHintPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Face nearest hostile target.
    Actor* faceTarget = sAttackState.target;
    if (faceTarget == nullptr || faceTarget->update == nullptr) {
        faceTarget = PickHostileTarget(a, play, kStandbyDetectDist,
                                       /*maxYDelta=*/kRangedYFilter);
        if (faceTarget != nullptr) {
            sAttackState.target = faceTarget;
        }
    }
    if (faceTarget != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, faceTarget->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // No target in detect range → drop to IDLE (Agent 2's locomotion
    // will pick up from there).
    if (faceTarget == nullptr) {
        sAttackState.target = nullptr;
        SPDLOG_INFO("[Invader] STANDBY→IDLE (no targets in detect range)");
        this_->state = EN_INVADER_STATE_IDLE;
        return;
    }

    // Target is far enough that the locomotion layer should resume
    // pursuit toward it. Hand off to FOLLOW (Agent 2 wires the
    // pursuit toward sAttackState.target via the target hint; for
    // now FOLLOW may default to IDLE if Agent 2's locomotion isn't
    // landed yet — that's acceptable, TryEngageCombat will re-fire
    // once a tier matches).
    const float distSq = Dist2DSq(a->world.pos, faceTarget->world.pos);
    if (distSq > kStandbyIdleRadius * kStandbyIdleRadius) {
        SPDLOG_INFO("[Invader] STANDBY→FOLLOW (target {:.0f}u away — handoff to "
                    "locomotion)",
                    std::sqrt(distSq));
        this_->state = EN_INVADER_STATE_FOLLOW;
    }
    // Otherwise stay in STANDBY; TryEngageCombat will re-engage once
    // a tier matches.
}

// Tier-ordered engagement check. Mirrors FollowerNPC's TryEngageCombat
// but targets are Players (hostile) instead of enemies (friendly).
bool TryEngageCombat(EnInvader* this_, PlayState* play) {
    // Eligible from non-combat states only. Combat states stay in
    // their own handlers; STANDBY is eligible (combat-to-combat
    // re-engage) so a STANDBY→ATTACK chain can happen the moment
    // the target is back in melee range.
    if (this_->state != EN_INVADER_STATE_IDLE &&
        this_->state != EN_INVADER_STATE_FOLLOW &&
        this_->state != EN_INVADER_STATE_STANDBY) {
        return false;
    }

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    if (curFrame < sCombatCooldownEndFrame) return false;

    const char* fromName = StateName(this_->state);
    Actor* selfActor = &this_->actor;

    // Tier 1 — melee range. BLOCK if low HP, otherwise ATTACK.
    Actor* meleeTarget = PickHostileTarget(selfActor, play, kAttackEngageDist);
    if (meleeTarget != nullptr) {
        const s8 maxHp = (this_->maxHealth > 0) ? this_->maxHealth : 1;
        const bool lowHp =
            ((float)this_->health / (float)maxHp <= kBlockHpThresholdRatio);
        if (lowHp) {
            SPDLOG_INFO("[Invader] {}→BLOCK (low HP {}/{}, target Player at "
                        "({:.0f},{:.0f},{:.0f}))",
                        fromName, (int)this_->health, (int)maxHp,
                        meleeTarget->world.pos.x, meleeTarget->world.pos.y,
                        meleeTarget->world.pos.z);
            this_->state = EN_INVADER_STATE_BLOCK;
            sAttackState.target = meleeTarget;
            sAttackState.swingFiredAT = false;
            sLastCombatWeapon = 0;
            return true;
        }
        SPDLOG_INFO("[Invader] {}→ATTACK (target Player at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName,
                    meleeTarget->world.pos.x, meleeTarget->world.pos.y,
                    meleeTarget->world.pos.z);
        this_->state = EN_INVADER_STATE_ATTACK;
        sAttackState.target = meleeTarget;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;
        return true;
    }

    // Tier 2 — ENGAGE pursuit (ground-level targets only via tight Y
    // filter so elevated targets fall through to ranged).
    Actor* pursueTarget = PickHostileTarget(selfActor, play, kEngageAcquireDist);
    if (pursueTarget != nullptr) {
        SPDLOG_INFO("[Invader] {}→ENGAGE (target Player at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName,
                    pursueTarget->world.pos.x, pursueTarget->world.pos.y,
                    pursueTarget->world.pos.z);
        this_->state = EN_INVADER_STATE_ENGAGE;
        sAttackState.target = pursueTarget;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;
        return true;
    }

    // Tier 3 — RANGED_ATTACK. Wide Y filter for elevated targets.
    if (InvaderHasRangedWeapon()) {
        Actor* rangedTarget = PickHostileTarget(selfActor, play,
                                                 kRangedAcquireDist,
                                                 kRangedYFilter);
        if (rangedTarget != nullptr) {
            const float dx = rangedTarget->world.pos.x - selfActor->world.pos.x;
            const float dz = rangedTarget->world.pos.z - selfActor->world.pos.z;
            const float dy = rangedTarget->world.pos.y - selfActor->world.pos.y;
            const float distXZ = std::sqrt(dx * dx + dz * dz);
            const bool isElevated = std::fabs(dy) > kRangedElevatedYDelta;
            if (isElevated && distXZ >= kRangedMinDist) {
                SPDLOG_INFO("[Invader] {}→RANGED_ATTACK (elevated target at "
                            "({:.0f},{:.0f},{:.0f}) dist={:.0f} dy={:+.0f})",
                            fromName,
                            rangedTarget->world.pos.x, rangedTarget->world.pos.y,
                            rangedTarget->world.pos.z, distXZ, dy);
                this_->state = EN_INVADER_STATE_RANGED_ATTACK;
                sAttackState.target = rangedTarget;
                sAttackState.swingFiredAT = false;
                sLastCombatWeapon = 1;
                return true;
            }
        }
    }

    return false;
}

// Defined after the Tick handlers per its forward-decl above.
s32 ChooseCombatExitState(EnInvader* this_, PlayState* play) {
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    sCombatCooldownEndFrame = curFrame +
        (uint64_t)Anchor::Instance->MsToGameTicks(kPostCombatCooldownMs);
    Actor* nearby = PickHostileTarget(&this_->actor, play, kStandbyDetectDist,
                                       /*maxYDelta=*/kRangedYFilter);
    return (nearby != nullptr) ? EN_INVADER_STATE_STANDBY
                               : EN_INVADER_STATE_IDLE;
}

// ---------------------------------------------------------------------
// Parity gap 2 — head-look-at-target.
//
// Cloned from FollowerNPC's TickHeadLookAtLeader (FollowerNPC.cpp:1335).
// Computes desired head + upper-body yaw relative to body facing,
// apportions between the two via ±70° head cap, then steps toward via
// Math_ScaledStepToS. EnInvader_Draw swaps these onto the local Player's
// headLimbRot/upperLimbRot so the override callback renders the
// Invader's head turned toward its target without affecting Player's
// actual rotation.
// ---------------------------------------------------------------------
void TickHeadLookAtTarget(EnInvader* this_, const Vec3f& targetPos) {
    const Actor* a = &this_->actor;
    const s16 dirYaw = Math_Atan2S(targetPos.z - a->world.pos.z,
                                    targetPos.x - a->world.pos.x);
    const s16 yawRel = dirYaw - a->shape.rot.y;

    const float dx     = targetPos.x - a->world.pos.x;
    const float dz     = targetPos.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx*dx + dz*dz);
    const s16 pitchRel = (distXZ > 1.0f)
        ? Math_Atan2S(distXZ, a->world.pos.y - targetPos.y)
        : 0;

    constexpr s16 kHeadYawMax = 12743;  // ±70° binary; matches FollowerNPC
    s16 headYawTarget  = yawRel;
    s16 upperYawTarget = 0;
    if (headYawTarget >  kHeadYawMax) { upperYawTarget = headYawTarget - kHeadYawMax; headYawTarget =  kHeadYawMax; }
    if (headYawTarget < -kHeadYawMax) { upperYawTarget = headYawTarget + kHeadYawMax; headYawTarget = -kHeadYawMax; }
    if (upperYawTarget >  0x4000) upperYawTarget =  0x4000;
    if (upperYawTarget < -0x4000) upperYawTarget = -0x4000;

    s16 headPitchTarget = pitchRel;
    if (headPitchTarget >  0x2000) headPitchTarget =  0x2000;
    if (headPitchTarget < -0x2000) headPitchTarget = -0x2000;

    Math_ScaledStepToS(&this_->headLimbRot.y,  headYawTarget,   0x600);
    Math_ScaledStepToS(&this_->upperLimbRot.y, upperYawTarget,  0x600);
    Math_ScaledStepToS(&this_->headLimbRot.x,  headPitchTarget, 0x600);
}

// ---------------------------------------------------------------------
// Parity gap 3 — TickDEAD.
//
// Cloned from FollowerNPC's TickDEAD (FollowerNPC.cpp:3128). Captures
// the entry frame on the first tick after state transition, plays the
// death anim (kDeath generic or kDeathDrown when prevState was
// SWIMMING), holds for kInvaderDeathHoldMs, then calls Actor_Kill. The
// terminal Actor_Kill fires OnActorKill which broadcasts ENEMY_DEFEATED
// to peers + invokes Director::OnEnemyRemoved on the host.
//
// Skips combat preempt + G10/G14 + state dispatch — see dispatcher
// guard inserted by parity gap 3.
// ---------------------------------------------------------------------
void TickDEAD(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    // Entry tick — pick death cause (drowning if we were swimming),
    // snap shape.rot to last facing, fire death anim. Anim type 0
    // (free) — both death anims are modelAnimType-agnostic.
    if (this_->prevState != EN_INVADER_STATE_DEAD) {
        sDeathEntryInvFrame = curFrame;

        // Bug fix 2026-05-17 (death anim hovering above ground): reset
        // velocity.y and force-snap Y to floor on death entry. Without
        // these, the Invader can die mid-swing while the actor's
        // velocity.y still has the prior-tick's accumulated value (e.g.
        // from a brief attack-anim hop or a swing that left the actor
        // off-floor). The C update's Actor_MoveXZGravity + UpdateBgCheckInfo
        // chain settles Y eventually but takes several frames — long
        // enough for the user to see the body hovering during the 3s
        // death-anim hold. Snap explicitly on entry so the body lies
        // flat from frame 0 of the anim.
        a->velocity.y = 0.0f;
        Actor_UpdateBgCheckInfo(play, a, 26.0f /* wallCheckHeight */,
                                10.0f /* wallCheckRadius */,
                                50.0f /* ceilingCheckHeight */, 4 /* flags */);

        // Parity gap 4 — death-cause selection. If prevState was
        // SWIMMING, classify as drowning. Otherwise generic. Note we
        // do NOT need to broadcast this to peers — Invader peers see
        // the host's joint table directly via ENEMY_UPDATE, so they
        // play whichever anim the host runs.
        this_->deathCause = (this_->prevState == EN_INVADER_STATE_SWIMMING)
                                ? 1 : 0;
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play,
                            (this_->deathCause == 1) ? InvaderAnim::kDeathDrown
                                                      : InvaderAnim::kDeath,
                            0);
        SPDLOG_INFO("[Invader] DEAD entry — anim={} pos=({:.0f},{:.0f},{:.0f}) "
                    "holdMs={}",
                    (this_->deathCause == 1 ? "kDeathDrown" : "kDeath"),
                    a->world.pos.x, a->world.pos.y, a->world.pos.z,
                    kInvaderDeathHoldMs);
    }

    // Hold for the configured duration so the death anim has time to
    // play visibly. After the hold elapses, fire Actor_Kill which
    // triggers OnActorKill → ENEMY_DEFEATED broadcast +
    // Director::OnEnemyRemoved bookkeeping.
    const uint64_t holdTicks = (uint64_t)Anchor::Instance->MsToGameTicks(
                                   kInvaderDeathHoldMs);
    if (curFrame >= sDeathEntryInvFrame + holdTicks) {
        SPDLOG_INFO("[Invader] DEAD hold complete — firing Actor_Kill");
        Actor_Kill(a);
        // Don't touch state past this point — actor is being torn down.
    }
}

// ---------------------------------------------------------------------
// Parity gap 5 — CRAWLING state + helpers.
//
// Cloned from FollowerNPC's FindCrawlspaceForCrossing /
// TryEnterCrawling / TickCRAWLING (FollowerNPC.cpp:2968 / 3060 / 3003).
// Child-Link-only gate: adult Invader can't fit into crawlspaces (same
// as Player's Player_TryEnteringCrawlspace at z_player.c:7639).
//
// Detection: find a CrawlspaceAnchor whose entryNormal separates the
// Invader (entry-facing side) from its target (far side). When found,
// snap to entryPos + face into the wall + transition to CRAWLING.
// TickCRAWLING moves forward at constant speed until the body crosses
// past the wall plane by margin, then plays the kCrawlExit one-shot
// and returns to FOLLOW.
// ---------------------------------------------------------------------
const ::AnchorNavRoom::CrawlspaceAnchor* FindCrawlspaceForCrossingInv(
    const ::AnchorNavRoom::RoomNavData* navData,
    const Vec3f& selfPos,
    const Vec3f& targetPos)
{
    if (navData == nullptr || navData->crawlspaceAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::CrawlspaceAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& a : navData->crawlspaceAnchors) {
        const float selfSide =
            (selfPos.x - a.entryPos.x) * a.entryNormal.x +
            (selfPos.z - a.entryPos.z) * a.entryNormal.z;
        const float targetSide =
            (targetPos.x - a.entryPos.x) * a.entryNormal.x +
            (targetPos.z - a.entryPos.z) * a.entryNormal.z;
        if (selfSide < kInvCrawlMinCrossDist ||
            targetSide > -kInvCrawlMinCrossDist) {
            continue;
        }
        if (std::fabs(a.entryPos.y - selfPos.y) > 60.0f) continue;
        const float dx = selfPos.x - a.entryPos.x;
        const float dz = selfPos.z - a.entryPos.z;
        const float distSq = dx*dx + dz*dz;
        if (distSq > kInvCrawlEntryRadius * kInvCrawlEntryRadius) continue;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = &a;
        }
    }
    return best;
}

void TickCRAWLING(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;  // direct world.pos drive; Actor_MoveXZGravity skipped

    if (sCrawlInvState.anchor == nullptr) {
        SPDLOG_WARN("[Invader] CRAWLING tick with null anchor — exiting to FOLLOW");
        this_->state = EN_INVADER_STATE_FOLLOW;
        return;
    }

    if (sCrawlInvState.exitAnimPlaying) {
        // Apply Y-drop for crawl-exit anim — closes parity gap 5
        // sub-fix (matches FollowerNPC commit bddc0b598). The
        // child_tunnel_end anim has the body pivot at standing height
        // while rendering crouched; without the drop, the Invader
        // visually floats 20u above the crawlspace floor for the
        // length of the exit anim.
        a->world.pos.y = sCrawlInvState.entryPos.y - kInvCrawlExitYDrop;
        if (this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
            SPDLOG_INFO("[Invader] CRAWLING→FOLLOW (exit anim complete at "
                        "({:.0f},{:.0f},{:.0f}))",
                        a->world.pos.x, a->world.pos.y, a->world.pos.z);
            this_->state = EN_INVADER_STATE_FOLLOW;
            sCrawlInvState.anchor = nullptr;
            sCrawlInvState.exitAnimPlaying = false;
        }
        return;
    }

    a->world.pos.x += sCrawlInvState.forwardDir.x * kInvCrawlSpeed;
    a->world.pos.z += sCrawlInvState.forwardDir.z * kInvCrawlSpeed;
    a->world.pos.y = sCrawlInvState.entryPos.y;

    const float currentSide =
        (a->world.pos.x - sCrawlInvState.anchor->entryPos.x) *
            sCrawlInvState.anchor->entryNormal.x +
        (a->world.pos.z - sCrawlInvState.anchor->entryPos.z) *
            sCrawlInvState.anchor->entryNormal.z;
    const float dx = a->world.pos.x - sCrawlInvState.entryPos.x;
    const float dz = a->world.pos.z - sCrawlInvState.entryPos.z;
    const float traveled = std::sqrt(dx*dx + dz*dz);

    const bool crossedPlane = currentSide < -kInvCrawlExitMargin;
    const bool tooFar       = traveled > kInvCrawlMaxDistance;
    if (crossedPlane || tooFar) {
        SPDLOG_INFO("[Invader] CRAWLING — exit triggered ({}), traveled {:.0f}u "
                    "from entry; switching to kCrawlExit anim",
                    crossedPlane ? "crossed plane" : "max distance", traveled);
        sCrawlInvState.exitAnimPlaying = true;
        this_->stopAnimPlaying = 0;
        InvEnsureAnimation(this_, play, InvaderAnim::kCrawlExit, 0);
    }
}

// Try to enter CRAWLING from IDLE / FOLLOW. Returns true if entered.
bool TryEnterCrawling(EnInvader* this_, PlayState* play, const Vec3f& targetPos) {
    if (this_->state != EN_INVADER_STATE_IDLE &&
        this_->state != EN_INVADER_STATE_FOLLOW) {
        return false;
    }
    // Child-Link only — same gate as Player's
    // Player_TryEnteringCrawlspace at z_player.c:7639.
    if (this_->linkAge != LINK_AGE_CHILD) return false;

    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            play->sceneNum,
            (int8_t)play->roomCtx.curRoom.num);
    const auto* anchor = FindCrawlspaceForCrossingInv(navData,
                                                       this_->actor.world.pos,
                                                       targetPos);
    if (anchor == nullptr) return false;

    Actor* a = &this_->actor;
    a->world.pos.x = anchor->entryPos.x;
    a->world.pos.z = anchor->entryPos.z;
    a->world.pos.y = anchor->entryPos.y;
    a->shape.rot.y = Math_Atan2S(-anchor->entryNormal.z, -anchor->entryNormal.x);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ     = 0.0f;
    a->velocity.y  = 0.0f;

    sCrawlInvState.anchor    = anchor;
    sCrawlInvState.entryPos  = a->world.pos;
    sCrawlInvState.forwardDir = {
        -anchor->entryNormal.x, 0.0f, -anchor->entryNormal.z
    };
    sCrawlInvState.exitAnimPlaying = false;
    this_->state = EN_INVADER_STATE_CRAWLING;
    this_->stopAnimPlaying = 0;
    InvEnsureAnimation(this_, play, InvaderAnim::kCrawlMove, 0);

    SPDLOG_INFO("[Invader] {}→CRAWLING (entry=({:.0f},{:.0f},{:.0f}) "
                "normal=({:.2f},{:.2f}))",
                StateName(this_->prevState),
                anchor->entryPos.x, anchor->entryPos.y, anchor->entryPos.z,
                anchor->entryNormal.x, anchor->entryNormal.z);
    return true;
}

// ---------------------------------------------------------------------
// Parity gap 6 — G14 close-fail safety net.
//
// Cloned from FollowerNPC's TryFireG14 (FollowerNPC.cpp:3297). Invader
// is "close to target but not closing" — distance unchanged for >
// kInvCloseFailTimeoutMs → snap to target's pos. Catches pathological
// orbits where G10 (long leash) doesn't fire because the Invader is
// inside the leash band, but the Invader can't actually make progress
// because of geometry.
//
// Uses sLocalInvNav.closeFailFrames / closeFailBaseline (added below).
// Skips combat states + scripted-traversal states (SWIMMING /
// LEDGE_HOIST / CRAWLING / DEAD) — those are their own scripted
// motion.
// ---------------------------------------------------------------------
constexpr float kInvCloseFailMinDistance   = 200.0f;
constexpr float kInvCloseFailMaxDistance   = 1200.0f;
constexpr int   kInvCloseFailTimeoutMs     = 10000;
constexpr float kInvCloseFailProgressDelta = 30.0f;

bool TryFireG14Invader(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    Actor* target = PickHostileTargetForInvader(a, play);
    if (target == nullptr || target->update == nullptr) {
        sLocalInvNav.closeFailFrames   = 0;
        sLocalInvNav.closeFailBaseline = 0.0f;
        return false;
    }

    const float dx = a->world.pos.x - target->world.pos.x;
    const float dy = a->world.pos.y - target->world.pos.y;
    const float dz = a->world.pos.z - target->world.pos.z;
    const float dist3D = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist3D < kInvCloseFailMinDistance ||
        dist3D > kInvCloseFailMaxDistance) {
        sLocalInvNav.closeFailFrames   = 0;
        sLocalInvNav.closeFailBaseline = 0.0f;
        return false;
    }

    if (sLocalInvNav.closeFailFrames == 0) {
        sLocalInvNav.closeFailBaseline = dist3D;
        sLocalInvNav.closeFailFrames   = 1;
        return false;
    }

    const float progress = sLocalInvNav.closeFailBaseline - dist3D;
    if (progress > kInvCloseFailProgressDelta) {
        sLocalInvNav.closeFailBaseline = dist3D;
        sLocalInvNav.closeFailFrames   = 1;
        return false;
    }

    sLocalInvNav.closeFailFrames++;
    const int timeoutTicks =
        Anchor::Instance->MsToGameTicks(kInvCloseFailTimeoutMs);
    if (timeoutTicks <= 0 ||
        (int)sLocalInvNav.closeFailFrames < timeoutTicks) {
        return false;
    }

    SPDLOG_INFO("[Invader] G14 close-fail teleport — dist3D={:.0f}u, "
                "progress={:.1f}u over {} frames (<{}u in {}ms) → snap to target",
                dist3D, progress, sLocalInvNav.closeFailFrames,
                (int)kInvCloseFailProgressDelta, kInvCloseFailTimeoutMs);
    a->world.pos = target->world.pos;
    a->speedXZ   = 0.0f;
    sLocalInvNav.closeFailFrames   = 0;
    sLocalInvNav.closeFailBaseline = 0.0f;
    sLocalInvNav.leashFrames       = 0;
    sLocalInvNav.stuckCheckPos     = a->world.pos;
    sLocalInvNav.lastStuckCheckFrame =
        Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
    this_->state = EN_INVADER_STATE_FOLLOW;
    Actor_UpdateBgCheckInfo(play, a, 26.0f, 10.0f, 50.0f, 4);
    return true;
}

}  // namespace

extern "C" void Anchor_TickInvaderActor(Actor* invader, PlayState* play) {
    if (invader == nullptr || play == nullptr) return;
    EnInvader* this_ = (EnInvader*)invader;

    // Parity gap 3 — DEAD state short-circuit. When dead, skip combat
    // preempt + G10/G14 + state dispatch — only TickDEAD runs (anim
    // hold + terminal Actor_Kill). Hint pos doesn't matter; pass the
    // actor's own pos as a no-op. Update prevState at the end so the
    // entry-edge detect inside TickDEAD fires on the first DEAD tick.
    if (this_->state == EN_INVADER_STATE_DEAD) {
        TickDEAD(this_, play);
        this_->prevState = this_->state;
        return;
    }

    // G18 — freeze during cutscenes (same shape as FollowerNPC's G18
    // gate). Without this, the Invader could swing or shoot during
    // a cutscene; combat AT/AC during cutscenes is a common source
    // of "what just hit me?" bugs. NOTE: this does NOT cancel an
    // active AT collider — see Race-Audit caveat in commit message.
    if (gPlayState->csCtx.state != CS_STATE_IDLE) {
        invader->speedXZ = 0.0f;
        return;
    }

    // Phase 2 G10 leash — Invader too far from any hostile target for
    // > kInvLeashTimeoutMs → snap to target. Fires only when in
    // non-combat states; combat handlers run scripted moves that
    // shouldn't be cut short. Skipping G10 during combat means a
    // pathologically out-of-range combat actor would stay out of
    // range, but the combat handlers themselves return to STANDBY
    // when target is lost.
    //
    // Phase 4 — also exempt SWIMMING / LEDGE_HOIST. Each is its own
    // scripted traversal (one-shot mantle, surface-clamped swim) and
    // shouldn't be aborted by a distance-based leash. Matches
    // FollowerNPC's same exempt list at FollowerNPC.cpp:3584-3586.
    const bool combatState =
        (this_->state == EN_INVADER_STATE_ATTACK) ||
        (this_->state == EN_INVADER_STATE_ENGAGE) ||
        (this_->state == EN_INVADER_STATE_BLOCK) ||
        (this_->state == EN_INVADER_STATE_RANGED_ATTACK) ||
        (this_->state == EN_INVADER_STATE_STANDBY);
    // Parity gap 5 — CRAWLING joins SWIMMING / LEDGE_HOIST in the
    // "scripted traversal" exempt list. CRAWLING moves the actor at
    // its own constant speed; G10/G14 mid-crawl would yank the body
    // out of the tunnel.
    //
    // Nav-parity Phase B — CLIMBING joins the exempt list. The
    // CLIMBING handler scripts position (snap XZ to subgoal+offset,
    // drive Y at fixed rate); a distance-based teleport mid-climb
    // would dislodge the body from the wall.
    const bool scriptedTraversal =
        (this_->state == EN_INVADER_STATE_SWIMMING) ||
        (this_->state == EN_INVADER_STATE_LEDGE_HOIST) ||
        (this_->state == EN_INVADER_STATE_CRAWLING) ||
        (this_->state == EN_INVADER_STATE_CLIMBING);
    // Bug fix 2026-05-17 (Invader teleporting to player): G10 + G14 are
    // direct-to-target teleports. User clarified that the Invader
    // should NOT teleport directly to its target — only stuck-resolution
    // along a calculated pathfinding path should be permitted (like NPC
    // Follower's substrate-path-aware recovery). Invader doesn't yet
    // have substrate pathfinding wired in, so for now we simply DISABLE
    // both teleport calls.
    //
    // Consequence: if the Invader is genuinely stuck (geometry it can't
    // traverse, target far behind a wall, etc.), it will sit there
    // until the target re-enters the engage radius. The cooldown-aware
    // sticky-target re-evaluation in InvaderDescriptor.cpp handles the
    // long-term despawn case.
    //
    // Future re-enablement: once Invader consumes RoomNavData substrate
    // and computes a path to the target, replace these calls with a
    // path-aware "advance one subgoal" teleport (path-based, not
    // direct-to-target). See GH #207 / general nav-system plan.
    //
    // The helper functions are kept (defined further below) for future
    // reference but unused for now.
    (void)combatState;
    (void)scriptedTraversal;

    // Phase 4 — water-entry detection. Mirrors FollowerNPC's
    // water-entry trigger at FollowerNPC.cpp:3598-3625. yDistToWater
    // is computed by Actor_UpdateBgCheckInfo (called from
    // EnInvader_Update with flags=4). When NPC submerges past Link's
    // per-age swim threshold, transition to SWIMMING. SWIMMING /
    // LEDGE_HOIST / combat exempt — combat in-water is a v2 concern.
    if (!combatState && !scriptedTraversal) {
        const float swimEntryDepth = InvSwimDepthFor(this_->linkAge);
        if (invader->yDistToWater > swimEntryDepth) {
            this_->state = EN_INVADER_STATE_SWIMMING;
            // Clear airborne tracking — entering water from a jump
            // should drop the jump anim hold.
            if (this_->jumpInProgress) {
                invader->gravity      = -2.0f;
                this_->jumpInProgress = 0;
            }
            SPDLOG_INFO("[Invader] →SWIMMING (yDistToWater={:.1f}u > "
                        "{:.0f}u threshold for linkAge={})",
                        invader->yDistToWater, swimEntryDepth,
                        (int)this_->linkAge);
        }
    }

    // Phase 4 — autonomous ledge-hoist detection. Matches FollowerNPC's
    // FollowerNPC.cpp:3693-3760 split:
    //   SWIMMING:  head-up probe finds an overhead floor 20-90u above
    //              NPC → swim-step-up hoist.
    //   FOLLOW:    target is meaningfully above NPC → raycast probe for
    //              a wall ledge ahead → ground mantle.
    if (this_->state == EN_INVADER_STATE_SWIMMING) {
        Vec3f topPos;
        if (InvDetectSwimHoist(play, invader->world.pos, topPos)) {
            InvEnterLedgeHoist(this_, INV_HOIST_CONTEXT_SWIM, topPos,
                                "swim head-up-probe");
        }
    } else if (this_->state == EN_INVADER_STATE_FOLLOW) {
        // Ground hoist gates on target above. Probe forward in NPC's
        // current facing direction. Quiet — most FOLLOW frames don't
        // trigger, no logging.
        if (sLocalInvNav.lastTarget != nullptr &&
            sLocalInvNav.lastTarget->update != nullptr &&
            sLocalInvNav.lastTarget->world.pos.y > invader->world.pos.y + 30.0f) {
            Vec3f topPos;
            if (InvRaycastDetectLedge(play, invader->world.pos,
                                       invader->shape.rot.y, topPos)) {
                InvEnterLedgeHoist(this_, INV_HOIST_CONTEXT_GROUND, topPos,
                                    "ground raycast");
            }
        }
    }

    // Hostile-target acquisition + tier-based engagement check. Fires
    // BEFORE state dispatch so the new state's handler gets the
    // first tick (entry-frame capture, initial yaw, etc.). For the
    // intentional combat-state pre-empt, the dispatcher below picks
    // the new state's handler — locomotion is not preempted unless
    // a tier matches.
    //
    // Phase 4 — TryEngageCombat is eligibility-gated to non-combat,
    // non-scripted states. SWIMMING / LEDGE_HOIST / CRAWLING / CLIMBING
    // → no preempt; let the scripted traversal complete first. (Phase B
    // adds CLIMBING to the exempt list — swinging a sword mid-climb
    // would dislodge the body from the wall.)
    if (this_->state != EN_INVADER_STATE_SWIMMING &&
        this_->state != EN_INVADER_STATE_LEDGE_HOIST &&
        this_->state != EN_INVADER_STATE_CRAWLING &&
        this_->state != EN_INVADER_STATE_CLIMBING) {
        TryEngageCombat(this_, play);
    }

    // Parity gap 5 — try-enter CRAWLING. Fires only when in IDLE /
    // FOLLOW (gated inside TryEnterCrawling) AND child Link AND a
    // crawlspace anchor separates Invader from its current target.
    // If we enter CRAWLING, the dispatch below picks TickCRAWLING.
    if (sLocalInvNav.lastTarget != nullptr &&
        sLocalInvNav.lastTarget->update != nullptr) {
        TryEnterCrawling(this_, play, sLocalInvNav.lastTarget->world.pos);
    }

    // Nav-parity Phase B — force-engage CLIMBING when the target is
    // meaningfully above the Invader AND a climb anchor is reachable.
    // Complements the natural FOLLOW→CLIMBING transition (via a path
    // subgoal carrying NODE_CLIMB_ANY) for cases where the BFS
    // pathfinder fails to route to a mid-wall target (FindNearestNode
    // skips climb cells; cross-room nav not supported). Gated to non-
    // combat / non-scripted states to avoid interrupting a swing or
    // mantle. Returns silently when conditions aren't met.
    if (!combatState && !scriptedTraversal &&
        sLocalInvNav.lastTarget != nullptr &&
        sLocalInvNav.lastTarget->update != nullptr) {
        TryEngageAutoClimbInv(this_, play, sLocalInvNav.lastTarget);
    }

    // Hint pos for handlers that want a "where to face when nothing
    // else applies" — use the target's pos when known, else the
    // actor's own pos (no-op fallback). Agent 4 may replace with
    // a multi-target hint.
    const Vec3f hintPos = sAttackState.target != nullptr
                             ? sAttackState.target->world.pos
                             : invader->world.pos;

    switch (this_->state) {
        // Combat states (Agent 3).
        case EN_INVADER_STATE_ATTACK:
            TickATTACK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_ENGAGE:
            TickENGAGE(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_BLOCK:
            TickBLOCK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_RANGED_ATTACK:
            TickRANGED_ATTACK(this_, play, hintPos);
            break;
        case EN_INVADER_STATE_STANDBY:
            TickSTANDBY(this_, play, hintPos);
            break;
        // Locomotion / non-combat (Phase 2 reconstruction).
        case EN_INVADER_STATE_IDLE:
            TickIDLE(this_, play);
            break;
        case EN_INVADER_STATE_FOLLOW:
            TickFOLLOW(this_, play);
            break;
        case EN_INVADER_STATE_STUCK:
            TickSTUCK(this_, play);
            break;
        // Phase 4 — scripted traversal.
        case EN_INVADER_STATE_SWIMMING:
            TickSWIMMING(this_, play);
            break;
        case EN_INVADER_STATE_LEDGE_HOIST:
            TickLEDGE_HOIST(this_, play);
            break;
        // Parity gap 5 — CRAWLING child-only crawlspace traversal.
        case EN_INVADER_STATE_CRAWLING:
            TickCRAWLING(this_, play);
            break;
        // Nav-parity Phase B — full CLIMBING state. Scripts XZ-snap +
        // Y-drive on the active anchor's cell column. Cloned from
        // FollowerNPC's TickCLIMBING (FollowerNPC.cpp:1632) minus the
        // friendly-leader fast-path co-climb branches.
        case EN_INVADER_STATE_CLIMBING:
            TickCLIMBING(this_, play);
            break;
        // Parity gap 3 — DEAD is handled at top of function (early
        // short-circuit); this case is unreachable but kept defensive.
        case EN_INVADER_STATE_DEAD:
        default:
            // DEAD short-circuited above; unknown states hold pose.
            invader->speedXZ = 0.0f;
            break;
    }

    // Phase 4 — airborne auto-jump anim selection for FOLLOW. Tracks
    // walked-off-edge transitions so kJump / kRunJump play as the
    // body falls. Matches FollowerNPC's logic at FollowerNPC.cpp:3909
    // but simpler (no jump-arc boost — Invader uses default gravity
    // and just rides the natural arc; no field-test instrumentation;
    // no STUCK-in-air position-stuck detection — gravity will land it
    // eventually). Triggered only in FOLLOW; CLIMBING/SWIMMING/
    // LEDGE_HOIST handle their own anims.
    InvaderAnim airborneAnimOverride = InvaderAnim::kNone;
    {
        const bool isOnFloor     = (invader->bgCheckFlags & 1) != 0;
        const bool walkedOffEdge = (invader->bgCheckFlags & 4) != 0;
        if (walkedOffEdge && invader->speedXZ > 3.0f &&
            this_->state == EN_INVADER_STATE_FOLLOW) {
            invader->bgCheckFlags &= ~4;
            // Clear any in-flight stop hold so the jump anim override
            // below survives the stopAnimPlaying check.
            this_->stopAnimPlaying = 0;
            this_->jumpInProgress  = 1;
            airborneAnimOverride =
                (invader->speedXZ > 4.0f) ? InvaderAnim::kRunJump
                                          : InvaderAnim::kJump;
            SPDLOG_INFO("[Invader.jump] FIRE anim={} speedXZ={:.2f} "
                        "pos=({:.0f},{:.0f},{:.0f})",
                        (airborneAnimOverride == InvaderAnim::kRunJump
                            ? "run_jump" : "jump"),
                        invader->speedXZ,
                        invader->world.pos.x, invader->world.pos.y,
                        invader->world.pos.z);
        }
        // Landing detection — clears jumpInProgress when bgCheckFlags & 1
        // returns. Matches FollowerNPC's landing branch at
        // FollowerNPC.cpp:4043 (simplified — no fall-damage logic).
        if (this_->jumpInProgress && isOnFloor) {
            this_->jumpInProgress = 0;
        }
    }

    // Phase 2 + 4 — drive animation for non-combat states. Combat
    // states fire their own anim via InvEnsureAnimation calls at state
    // entry inside the Tick handlers; the dispatcher anim block
    // intentionally skips them so a re-fire doesn't truncate the
    // one-shot anim. We pick + ensure for IDLE / FOLLOW / STUCK /
    // SWIMMING / LEDGE_HOIST / CRAWLING here. Anim type tracks
    // combat: STANDBY-ish states use fighter (1); locomotion uses
    // _free (0).
    //
    // Parity gap 5 — CRAWLING joins this list so kCrawlMove / kCrawlExit
    // get resolved via the standard pipeline (TryEnterCrawling fires
    // kCrawlMove on entry, TickCRAWLING fires kCrawlExit when the
    // body crosses the wall plane). The dispatcher's stopAnimPlaying
    // hold keeps the SkelAnime at end-frame (crouch pose) during
    // mid-tunnel translation.
    // Bug fix 2026-05-17 (attack-anim freeze): the previous gate
    // `if (isLocomotion)` skipped the per-tick EnsureAnimation block
    // for combat states. After kSwordSwing ran once, currentAnim
    // stayed at kSwordSwing forever (no state in the gate could reset
    // it), so re-entering ATTACK from STANDBY found
    // currentAnim==kSwordSwing already → EnsureAnimation no-oped and
    // the anim never restarted. Visual: Invader frozen in
    // post-swing pose with sword extended.
    //
    // Drop the gate so the per-tick EnsureAnimation runs for ALL
    // states. Matches NPC Follower's pattern at FollowerNPC.cpp:4354
    // (unconditional). Combat states' Tick handlers still fire their
    // own EnsureAnimation on entry-edge — those become idempotent
    // no-ops once the dispatcher's call also fires the same anim.
    if (true) {
        // Animation type: fighter (1) right after combat exit (so the
        // Invader holds the sword+shield stance briefly), _free (0)
        // otherwise. Combat-cooldown overlap drives this — same window
        // as the Agent 3 cooldown so transition timing matches.
        const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                      std::memory_order_relaxed);
        const s8 animType = (curFrame < sCombatCooldownEndFrame) ? 1 : 0;
        InvaderAnim want = InvAnimForState(this_->state, invader->speedXZ,
                                            this_->prevState);

        // Phase 4 — LEDGE_HOIST anim override. AnimForState returns a
        // default (kHoistGround); resolve the real pick from
        // hoistContext here where we have access to `this_`.
        if (this_->state == EN_INVADER_STATE_LEDGE_HOIST) {
            want = (this_->hoistContext == (s8)INV_HOIST_CONTEXT_SWIM)
                       ? InvaderAnim::kHoistSwim
                       : InvaderAnim::kHoistGround;
        }

        // Parity gap 5 — CRAWLING exit-anim override. AnimForState
        // returns kCrawlMove for CRAWLING; switch to kCrawlExit when
        // TickCRAWLING set the exitAnimPlaying flag.
        if (this_->state == EN_INVADER_STATE_CRAWLING &&
            sCrawlInvState.exitAnimPlaying) {
            want = InvaderAnim::kCrawlExit;
        }

        // Nav-parity Phase B — CLIMBING anim override. AnimForState
        // returns kClimbUpL as a default; pick L/R based on the
        // alternation tracker the TickCLIMBING handler maintains. This
        // mirrors Player's L/R step alternation. Side variants
        // (kClimbSideL/R) are reserved for future lateral-motion
        // detection — v1 climbs purely vertically.
        if (this_->state == EN_INVADER_STATE_CLIMBING) {
            want = this_->climbNextIsRight ? InvaderAnim::kClimbUpR
                                           : InvaderAnim::kClimbUpL;
        }

        // Phase 4 — airborne anim override. If jump fired this tick OR
        // we're still mid-jump and the current anim is a jump anim,
        // hold the jump anim until landing. Mirrors FollowerNPC's
        // airborne anim hold at FollowerNPC.cpp:4249.
        if (airborneAnimOverride != InvaderAnim::kNone) {
            want = airborneAnimOverride;
        } else if (this_->jumpInProgress &&
                   ((InvaderAnim)this_->currentAnim == InvaderAnim::kRunJump ||
                    (InvaderAnim)this_->currentAnim == InvaderAnim::kJump)) {
            want = (InvaderAnim)this_->currentAnim;
        }

        // Phase 4 — idle fidget rotation. After sustained kWait,
        // rotate through {kFidgetLookA, kFidgetWarmB, kFidgetStretchD}.
        // Matches FollowerNPC's pattern at FollowerNPC.cpp:4132-4153.
        static constexpr u32 kInvFidgetIntervalTicks = 120;  // ~6s @ 20fps
        if (want == InvaderAnim::kWait && !this_->stopAnimPlaying &&
            (InvaderAnim)this_->currentAnim == InvaderAnim::kWait) {
            this_->idleTicks++;
            if (this_->idleTicks >= kInvFidgetIntervalTicks) {
                this_->idleTicks = 0;
                switch (this_->nextFidgetIdx % 3) {
                    case 0: want = InvaderAnim::kFidgetLookA;    break;
                    case 1: want = InvaderAnim::kFidgetWarmB;    break;
                    case 2: want = InvaderAnim::kFidgetStretchD; break;
                }
                this_->nextFidgetIdx++;
            }
        } else if (want != InvaderAnim::kWait) {
            this_->idleTicks = 0;
        }

        // If a one-shot is in flight (stop / fidget / hoist / jump) and
        // we haven't otherwise overridden `want`, hold the current
        // anim until LinkAnimation_Update reports completion. Matches
        // FollowerNPC's hold-during-stop pattern at FollowerNPC.cpp:4172.
        //
        // Parity gap 5 — exempt CRAWLING: kCrawlMove is intentionally a
        // one-shot that holds at end-frame (crouch pose); we WANT the
        // dispatcher to keep returning kCrawlMove / kCrawlExit (the
        // computed `want`) instead of re-asserting whatever currentAnim
        // happens to be. Without this exemption, the exit-anim override
        // above would never take effect because stopAnimPlaying is
        // still set from kCrawlMove's entry.
        if (this_->stopAnimPlaying &&
            airborneAnimOverride == InvaderAnim::kNone &&
            this_->state != EN_INVADER_STATE_LEDGE_HOIST &&
            this_->state != EN_INVADER_STATE_CRAWLING &&
            this_->state != EN_INVADER_STATE_CLIMBING) {
            want = (InvaderAnim)this_->currentAnim;
        }

        // Cancel stop-anim hold if NPC has resumed locomotion. Without
        // this, body slides along ground while idle/fidget anim plays
        // out. Matches FollowerNPC's cancel at FollowerNPC.cpp:4160.
        if (this_->stopAnimPlaying &&
            this_->state == EN_INVADER_STATE_FOLLOW &&
            invader->speedXZ > 0.5f) {
            this_->stopAnimPlaying = 0;
        }

        InvEnsureAnimation(this_, play, want, animType);

        // Clear stop-anim latch when the ONCE anim reaches endFrame.
        // LinkAnimation_Once clamps curFrame to endFrame on completion.
        if (this_->stopAnimPlaying &&
            this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
            this_->stopAnimPlaying = 0;
        }
    }

    // Parity gap 2 — head-look-at-target. Compute desired
    // headLimbRot/upperLimbRot toward the current target each tick
    // (or settle to neutral when no target / scripted-anim states).
    // EnInvader_Draw's save/swap/restore makes the local Player's
    // limb rotation reflect THIS computation during the Player_DrawImpl
    // call. Disabled during LEDGE_HOIST / CRAWLING — anim is body-locked
    // (climb up + crouch); head turning sideways looks wrong.
    if (this_->state == EN_INVADER_STATE_LEDGE_HOIST ||
        this_->state == EN_INVADER_STATE_CRAWLING) {
        Math_ScaledStepToS(&this_->headLimbRot.y,  0, 0x600);
        Math_ScaledStepToS(&this_->headLimbRot.x,  0, 0x600);
        Math_ScaledStepToS(&this_->upperLimbRot.y, 0, 0x600);
    } else {
        // Prefer the current combat target; fall back to the cached
        // locomotion target; otherwise settle to neutral. Same as
        // FollowerNPC's TickHeadLookAtLeader call site.
        Actor* lookTarget = sAttackState.target;
        if (lookTarget == nullptr || lookTarget->update == nullptr) {
            lookTarget = sLocalInvNav.lastTarget;
        }
        if (lookTarget != nullptr && lookTarget->update != nullptr) {
            TickHeadLookAtTarget(this_, lookTarget->world.pos);
        } else {
            Math_ScaledStepToS(&this_->headLimbRot.y,  0, 0x600);
            Math_ScaledStepToS(&this_->headLimbRot.x,  0, 0x600);
            Math_ScaledStepToS(&this_->upperLimbRot.y, 0, 0x600);
        }
    }

    // Update prevState tail (after dispatch so combat handlers can
    // edge-detect via prevState != state on the entry tick).
    this_->prevState = this_->state;
}

extern "C" void Anchor_InvaderDrawBegin(Actor* invader) {
    sCurrentlyDrawingInvader = invader;

    // Phase B equipment swap. Save Player's current model state and
    // force the Invader's intended model (SWORD_AND_SHIELD for v1).
    // End() restores.
    sEquipmentSwapActive = false;
    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    // v1: Invader is always armed. Future combat AI may swap based on
    // an Invader state-machine — mirror NpcStateToModelGroup at that
    // point.
    const s32 intendedGroup = PLAYER_MODELGROUP_SWORD_AND_SHIELD;

    // No-op if we're already at the intended group (saves a redundant
    // SetModels call when Player is in fighter stance — common in
    // combat scenarios where the Invader is most likely to appear).
    if (intendedGroup == localPlayer->modelGroup) {
        return;
    }

    // Save. We save BOTH the modelGroup (for the canonical
    // Player_SetModels restore) AND the raw DList/type fields
    // (defensive — in case Player_SetModels's EquipmentAlwaysVisible
    // CVar branches resolve slightly different DLists on restore than
    // the user originally had).
    sSavedPlayerModelGroup       = localPlayer->modelGroup;
    sSavedPlayerLeftHandType     = localPlayer->leftHandType;
    sSavedPlayerRightHandType    = localPlayer->rightHandType;
    sSavedPlayerSheathType       = localPlayer->sheathType;
    sSavedPlayerLeftHandDLists   = localPlayer->leftHandDLists;
    sSavedPlayerRightHandDLists  = localPlayer->rightHandDLists;
    sSavedPlayerSheathDLists     = localPlayer->sheathDLists;
    sSavedPlayerWaistDLists      = localPlayer->waistDLists;

    // Apply Invader's intended model. Player_SetModels writes
    // leftHandType/DLists, rightHandType/DLists, sheathType/DLists,
    // waistDLists from sPlayerDListGroups[type][linkAge]. Does NOT
    // write modelGroup itself — the trailing assignment handles that
    // (matches FollowerNPC.cpp:489-490).
    Player_SetModels(localPlayer, intendedGroup);
    localPlayer->modelGroup = (u8)intendedGroup;
    sEquipmentSwapActive = true;
}

extern "C" void Anchor_InvaderDrawEnd(void) {
    sCurrentlyDrawingInvader = nullptr;

    if (!sEquipmentSwapActive) return;
    sEquipmentSwapActive = false;

    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    // Restore. Both the canonical SetModels call (so any internal
    // bookkeeping inside Player_SetModels remains consistent) AND
    // the raw fields (defensive — the EquipmentAlwaysVisible branches
    // inside Player_SetModels may pick slightly different DLists than
    // the user originally had).
    Player_SetModels(localPlayer, sSavedPlayerModelGroup);
    localPlayer->modelGroup      = (u8)sSavedPlayerModelGroup;
    localPlayer->leftHandType    = sSavedPlayerLeftHandType;
    localPlayer->rightHandType   = sSavedPlayerRightHandType;
    localPlayer->sheathType      = sSavedPlayerSheathType;
    localPlayer->leftHandDLists  = sSavedPlayerLeftHandDLists;
    localPlayer->rightHandDLists = sSavedPlayerRightHandDLists;
    localPlayer->sheathDLists    = sSavedPlayerSheathDLists;
    localPlayer->waistDLists     = sSavedPlayerWaistDLists;
}

extern "C" Actor* Anchor_GetCurrentlyDrawingInvader(void) {
    return sCurrentlyDrawingInvader;
}
