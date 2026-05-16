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

constexpr float kEngageAcquireDist  = 250.0f;
constexpr float kEngageBreakDist    = 400.0f;
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
// Idle leader-leash radius — STANDBY drops back to IDLE inside this.
constexpr float kStandbyIdleRadius = 80.0f;

constexpr int   kPostCombatCooldownMs = 1500;
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
// Larger than kEnterIdle so the NPC doesn't oscillate at the boundary.
// We pick 250u as the "lose interest" threshold — outside this range
// the Invader returns to IDLE and the target picker may select a
// different hostile.
constexpr float kInvFollowEngageDist = 250.0f;
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

// Local nav baseline. Same shape as FollowerNPC's sLocalNav (subset —
// no jump / climb / hoist machinery for v1 Invader). File-scope is
// safe because actor states are non-reentrant per actor.
struct LocalInvNavState {
    Vec3f    stuckCheckPos        = { 0.0f, 0.0f, 0.0f };
    uint64_t lastStuckCheckFrame  = 0;
    uint32_t leashFrames          = 0;
    // Cached target for FOLLOW's anim / speed calc. PickHostileTarget
    // re-queried each tick, but we keep a one-tick cache so STUCK can
    // nudge toward the same target without re-running the picker.
    Actor*   lastTarget           = nullptr;
};
static LocalInvNavState sLocalInvNav;

// ---------------------------------------------------------------------
// Phase 2 — animation kind enum + header / picker / ensurer.
// Smaller surface than FollowerNPC's (12 kinds → 5 kinds) since v1
// Invader has no swim / climb / hoist / fidgets. Combat states drive
// their anims directly via LinkAnimation_Change inside their Tick
// handlers (matching FollowerNPC's pattern) — this enum covers only
// the locomotion + alert anims.
// ---------------------------------------------------------------------
enum class InvaderAnim {
    kNone,     // sentinel (no anim selected yet)
    kWait,     // idle wait (free or fighter depending on modelAnimType)
    kWalk,     // pursuit walk
    kRun,      // pursuit run
    kStopL,    // one-shot stop on left foot (FOLLOW→IDLE)
    kStopR,    // one-shot stop on right foot (FOLLOW→IDLE)
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
                          want == InvaderAnim::kStopR);
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
        default:
            return InvaderAnim::kWait;
    }
}

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
        return;
    }
    sLocalInvNav.lastTarget = target;

    // Drive yaw + speed toward the target.
    const s16 yaw = YawTowardTarget(a->world.pos, target->world.pos);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;

    const float distSq = Dist2DSq(a->world.pos, target->world.pos);
    const float dist   = std::sqrt(distSq);
    a->speedXZ = (dist > kInvRunDistance) ? kInvRunSpeed : kInvWalkSpeed;

    // Stuck check — STUCK fires when no real progress happens over
    // the check window.
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    const int stuckCheckTicks = Anchor::Instance->MsToGameTicks(kInvStuckCheckMs);
    if (stuckCheckTicks > 0 &&
        curFrame >= sLocalInvNav.lastStuckCheckFrame + (uint64_t)stuckCheckTicks) {
        const float progress = std::sqrt(
            Dist2DSq(a->world.pos, sLocalInvNav.stuckCheckPos));
        if (progress < kInvStuckMinProgress) {
            this_->state = EN_INVADER_STATE_STUCK;
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
    if (distSq <= kInvFollowIdleDist * kInvFollowIdleDist) {
        this_->state = EN_INVADER_STATE_IDLE;
        a->speedXZ   = 0.0f;
    }
}

void TickSTUCK(EnInvader* this_, PlayState* play) {
    Actor* a = &this_->actor;
    Actor* target = sLocalInvNav.lastTarget;
    if (target == nullptr || target->update == nullptr) {
        // No target — drop to IDLE; the dispatcher's next pass picks
        // up FROM IDLE cleanly.
        this_->state = EN_INVADER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        return;
    }
    const s16 yaw = YawTowardTarget(a->world.pos, target->world.pos);
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
    this_->state = EN_INVADER_STATE_FOLLOW;
    SPDLOG_INFO("[Invader] STUCK→FOLLOW (nudged {:.0f}u toward yaw=0x{:X})",
                kInvStuckNudgeDist, (uint16_t)yaw);
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
        case EN_INVADER_STATE_STUCK:         return "STUCK";
        case EN_INVADER_STATE_DEAD:          return "DEAD";
        case EN_INVADER_STATE_ATTACK:        return "ATTACK";
        case EN_INVADER_STATE_ENGAGE:        return "ENGAGE";
        case EN_INVADER_STATE_BLOCK:         return "BLOCK";
        case EN_INVADER_STATE_RANGED_ATTACK: return "RANGED_ATTACK";
        case EN_INVADER_STATE_STANDBY:       return "STANDBY";
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
                         float maxYDelta = 60.0f) {
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
        SPDLOG_INFO("[Invader] BLOCK entry — HP={}", (int)this_->health);
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

}  // namespace

extern "C" void Anchor_TickInvaderActor(Actor* invader, PlayState* play) {
    if (invader == nullptr || play == nullptr) return;
    EnInvader* this_ = (EnInvader*)invader;

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
    const bool combatState =
        (this_->state == EN_INVADER_STATE_ATTACK) ||
        (this_->state == EN_INVADER_STATE_ENGAGE) ||
        (this_->state == EN_INVADER_STATE_BLOCK) ||
        (this_->state == EN_INVADER_STATE_RANGED_ATTACK) ||
        (this_->state == EN_INVADER_STATE_STANDBY);
    if (!combatState) {
        if (TryFireG10Invader(this_, play)) {
            this_->prevState = this_->state;
            return;
        }
    }

    // Hostile-target acquisition + tier-based engagement check. Fires
    // BEFORE state dispatch so the new state's handler gets the
    // first tick (entry-frame capture, initial yaw, etc.). For the
    // intentional combat-state pre-empt, the dispatcher below picks
    // the new state's handler — locomotion is not preempted unless
    // a tier matches.
    TryEngageCombat(this_, play);

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
        case EN_INVADER_STATE_DEAD:
        default:
            // DEAD / unknown: hold pose, no motion.
            invader->speedXZ = 0.0f;
            break;
    }

    // Phase 2 — drive animation for non-combat states. Combat states
    // own their anims via direct LinkAnimation_Change calls inside
    // their Tick handlers (matching FollowerNPC's pattern). We only
    // pick + ensure for IDLE/FOLLOW/STUCK here. Anim type tracks
    // combat: STANDBY-ish states use fighter (1); locomotion uses
    // _free (0).
    const bool isLocomotion =
        (this_->state == EN_INVADER_STATE_IDLE) ||
        (this_->state == EN_INVADER_STATE_FOLLOW) ||
        (this_->state == EN_INVADER_STATE_STUCK);
    if (isLocomotion) {
        // Animation type: fighter (1) right after combat exit (so the
        // Invader holds the sword+shield stance briefly), _free (0)
        // otherwise. Combat-cooldown overlap drives this — same window
        // as the Agent 3 cooldown so transition timing matches.
        const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                      std::memory_order_relaxed);
        const s8 animType = (curFrame < sCombatCooldownEndFrame) ? 1 : 0;
        const InvaderAnim want = InvAnimForState(this_->state, invader->speedXZ,
                                                  this_->prevState);
        InvEnsureAnimation(this_, play, want, animType);
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
