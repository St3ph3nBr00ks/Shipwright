/*
 * FollowerNPC.cpp — SoH NPC Follower companion lifecycle (Flotilla).
 *
 * Plan: Plans/npc_follower_plan.md.
 * Branch: feature/npc-follower off development-multiplayer @ af57a20e7.
 *
 * Phase 2 scope (this commit): CVar toggle drives spawn/despawn of the
 * local ACTOR_EN_FOLLOWER instance. No state machine yet (Phase 4), no
 * network sync yet (Phase 3) — purely local-client behaviour. Single
 * NPC per client; per-room caps deferred.
 *
 * Subsequent phases land in this same directory:
 *   Phase 3: Packets/FollowerNPCSpawn.cpp + State.cpp + Despawn.cpp.
 *   Phase 4: state machine handlers (IDLE / FOLLOW with Actor_MoveXZGravity).
 *   Phase 5: STUCK + recovery harness.
 *   Phase 6: CLIMBING scripted-climb driver.
 *   Phase 8: G-guards + debug-draw.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/AIFollowerNPC/FollowerNPC.h"
#include "soh/Network/Anchor/Common/ActorTrail.h"     // Phase 5: substrate path consumption
#include "soh/Network/Anchor/Common/DistanceMath.h"   // AnchorDist::DistXZSq
#include "soh/Enhancements/RoomNavData/RoomNavData.h" // Phase 6: ClimbAnchor lookup
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include "variables.h"        // gPlayState, gEnFollowerId, gSaveContext
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "objects/gameplay_keep/gameplay_keep.h"  // gPlayerAnim_link_normal_*
#include "src/overlays/actors/ovl_En_Follower/z_en_follower.h"  // EnFollower struct + state enum
extern PlayState* gPlayState;
extern s16        gEnFollowerId;
}

// ----------------------------------------------------------------------------
// File-scope Phase 5 nav state. Declared here (above SetFollowerNpcActive)
// so the spawn helper can reset it. Single instance — v1 has one NPC per
// client.
// ----------------------------------------------------------------------------
namespace {
// Animation identity tag — tracked so the per-tick handler only calls
// LinkAnimation_Change on actual transitions (calling it every frame
// restarts the animation and freezes the playhead). Stored on the
// EnFollower as `currentAnim` (s32) so peer replicas track their own
// animation independently.
enum class FollowerNpcAnim {
    kNone        = 0,  // initial state — first EnsureAnimation call always fires
    kWait        = 1,  // wait_free (idle) — actually a waitL/waitR blend each frame
    kWalk        = 2,  // walk_free
    kRun         = 3,  // run_free
    kClimbUp     = 4,  // climb-up (looping)
    kStopL       = 5,  // walk_endL_free — one-shot stop anim, left foot forward
    kStopR       = 6,  // walk_endR_free — one-shot stop anim, right foot forward
    // Fidgets — one-shot variants of the standing idle. Cycled by the
    // dispatcher after kFidgetIntervalTicks in kWait. Source = Player's
    // sFidgetAnimations at z_player.c:1014-1056.
    kFidgetLookA = 7,  // gPlayerAnim_link_normal_wait_typeA_20f (look around)
    kFidgetWarmB = 8,  // gPlayerAnim_link_normal_wait_typeB_20f (warm — wipe brow)
    kFidgetStretchD = 9,  // gPlayerAnim_link_wait_typeD_20f (stretch arms)
    kSwim     = 10,  // gPlayerAnim_link_swimer_swim (moving)
    kSwimWait = 11,  // gPlayerAnim_link_swimer_swim_wait (treading water)
    // Ledge-hoist one-shot anims. AnimForState returns one of these
    // during EN_FOLLOWER_STATE_LEDGE_HOIST based on the
    // EnFollower::hoistContext field. Both are ANIMMODE_ONCE; the
    // existing stopAnimPlaying handshake holds the anim until
    // LinkAnimation_Update reports completion.
    kHoistGround = 12,  // gPlayerAnim_link_normal_climb_up (mantle from floor)
    kHoistSwim   = 13,  // gPlayerAnim_link_swimer_swim_15step_up (climb out of water)
    // Auto-jump-off-ledge anims. Player picks between these at
    // z_player.c:5663 based on speed (run-jump when fast + facing
    // forward, regular jump otherwise). One-shot; falls back to
    // walk/run/wait when complete via the stopAnimPlaying handshake.
    kRunJump  = 14,  // gPlayerAnim_link_normal_run_jump
    kJump     = 15,  // gPlayerAnim_link_normal_jump
};

struct LocalNpcNavState {
    AnchorNav::ActorTrail::NavPath path;
    uint64_t lastPathRefreshFrame = 0;
    Vec3f    lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
    Vec3f    stuckCheckPos        = { 0.0f, 0.0f, 0.0f };
    uint64_t lastStuckCheckFrame  = 0;
    // (currentAnim moved to EnFollower::currentAnim — per-actor tracking
    // so peer replicas don't share state with the local owner.)
    // Phase 6 — anchor cached during a CLIMBING run so handler doesn't
    // re-resolve every frame. Cleared on CLIMBING exit.
    const ::AnchorNavRoom::ClimbAnchor* activeClimbAnchor = nullptr;
    // Phase 8 — G10 leash. Counts consecutive ticks the NPC has been
    // beyond kNpcLeashDistance from leader. Reset when within range.
    uint32_t leashFrames = 0;
    // Phase 8 — G14 close-fail. Counts consecutive ticks the NPC is
    // in the close-fail distance band without progress.
    // closeFailBaseline = distance-to-leader at window entry; reset
    // whenever progress > kNpcCloseFailProgressDelta is observed.
    uint32_t closeFailFrames   = 0;
    float    closeFailBaseline = 0.0f;

    // Auto-jump-off-ledge diagnostics. Captured at jump trigger,
    // emitted as periodic per-frame logs while airborne, summary
    // log at landing. `jumpInProgress` flips false on landing.
    bool     jumpInProgress         = false;
    Vec3f    jumpStartPos           = { 0.0f, 0.0f, 0.0f };
    Vec3f    jumpPeakPos            = { 0.0f, 0.0f, 0.0f };  // highest Y reached
    s16      jumpStartYaw           = 0;
    float    jumpStartSpeedXZ       = 0.0f;
    float    jumpStartVelocityY     = 0.0f;
    uint64_t jumpStartFrame         = 0;
    uint64_t jumpLastDiagFrame      = 0;
    bool     jumpWasOnFloorPrevTick = true;
};
}  // namespace
static LocalNpcNavState sLocalNav;

// ----------------------------------------------------------------------------
// Color-bug fix — draw-context flag. Mirrors the pause-menu's
// Anchor_PauseLinkDrawBegin/End / Anchor_IsDrawingPauseLink pattern
// (see soh/Network/Anchor/Common/PauseLinkBuffer.{h,cpp}).
//
// Why needed: Player_DrawImpl receives the local player's Player* as
// its `data` arg (so equipment-draw resolves correctly). The
// VB_APPLY_TUNIC_COLOR hook receives that same `data` and matches
// against `myPlayer == actor` — which TRUE-matches the local-player
// branch and applies the local color to the NPC. But more
// importantly: when the NPC is drawn AFTER a remote DummyPlayer
// draw, the GPU env color from the previous draw leaks onto the
// NPC unless the hook applies an override.
//
// The flag lets the hook know "this draw is an NPC; resolve color
// from the NPC's owner, not from `actor` matching".
// ----------------------------------------------------------------------------
static Actor* sCurrentlyDrawingNpc = nullptr;

extern "C" void Anchor_FollowerNpcDrawBegin(Actor* npc) {
    sCurrentlyDrawingNpc = npc;
}
extern "C" void Anchor_FollowerNpcDrawEnd(void) {
    sCurrentlyDrawingNpc = nullptr;
}
extern "C" Actor* Anchor_GetCurrentlyDrawingFollowerNpc(void) {
    return sCurrentlyDrawingNpc;
}

// ----------------------------------------------------------------------------
// Owner lookup — given an NPC Actor*, return the ownerClientId.
// Local NPC → ownClientId; peer replica → ownerClientId from
// mPeerFollowerNpcs; unknown → 0.
// ----------------------------------------------------------------------------
uint32_t Anchor::FindFollowerNpcOwner(Actor* npc) const {
    if (npc == nullptr) return 0;
    if (npc == mFollowerNpcLocalActor) {
        return ownClientId;
    }
    for (const auto& [cid, replica] : mPeerFollowerNpcs) {
        if (replica == npc) return cid;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Scene-transition pointer cleanup. OoT clears the actor list on
// scene reload; our cached Actor* pointers become dangling. Reading
// dangling-pointer->update is undefined behaviour (the memory may
// have been freed, reused by another actor with a non-null update,
// or contain stale bytes). This explicit clear — called from the
// OnSceneSpawnActors hook in NpcCompanionInit.cpp — is the safe
// alternative.
//
// After the clear, TickFollowerNpcCVar's auto-respawn branch fires
// on the next tick (CVar is on AND mFollowerNpcLocalActor is null
// → SetFollowerNpcActive(true) re-spawns at the new scene's player
// position). For peer replicas: their owners' next FOLLOWER_NPC_SPAWN
// (auto-broadcast by the owner's auto-respawn) will repopulate
// mPeerFollowerNpcs.
// ----------------------------------------------------------------------------
void Anchor::ClearFollowerNpcSceneCache() {
    mFollowerNpcLocalActor = nullptr;
    mPeerFollowerNpcs.clear();
    // Reset the CVar transition baseline too — without this, the
    // next TickFollowerNpcCVar sees CVar=1, last=1, no edge, no
    // auto-respawn (the auto-respawn branch handles this case via
    // the `cur != 0 && mFollowerNpcLocalActor == nullptr` check
    // BEFORE the edge check, so this reset isn't strictly needed
    // — but it's clearer to start the new scene with a clean slate).
    // Don't touch mFollowerNpcCVarLast here; the auto-respawn branch
    // is the one that handles "CVar on but pointer null" correctly.
}

// ----------------------------------------------------------------------------
// CVar transition polling — called once per OnGameFrameUpdate tick.
// ----------------------------------------------------------------------------
void Anchor::TickFollowerNpcCVar() {
    // Stale-pointer cleanup is now handled by the OnSceneSpawnActors
    // hook in NpcCompanionInit.cpp, which calls
    // ClearFollowerNpcSceneCache(). Reading update on a dangling
    // pointer is UB and was unreliable in field test (the dangling
    // read returned non-null often enough that auto-respawn never
    // fired). The explicit clear is the safe alternative.

    const int cur = CVarGetInteger(CVAR_ENHANCEMENT("AI.FollowerNPC.Enabled"), 0);

    // Auto-respawn after scene transition: if the CVar is on but the
    // pointer is null (cleared by OnSceneSpawnActors), spawn now.
    // This is NOT a CVar edge — handle it before the edge check.
    if (cur != 0 && mFollowerNpcLocalActor == nullptr) {
        SetFollowerNpcActive(true);
        // Fall through to edge-check update; mFollowerNpcCVarLast
        // tracking is independent of the auto-respawn.
    }

    if (cur == mFollowerNpcCVarLast) {
        // No edge — nothing more to do.
        return;
    }
    // Edge detected. The spawn case above already handled it; the
    // despawn case (1→0) still needs to fire here.
    if (cur == 0) {
        SetFollowerNpcActive(false);
    }
    mFollowerNpcCVarLast = cur;
}

// ----------------------------------------------------------------------------
// Spawn / despawn the local NPC. Idempotent.
// ----------------------------------------------------------------------------
void Anchor::SetFollowerNpcActive(bool active) {
    if (active) {
        // Idempotent: if we already have a live NPC, don't double-spawn.
        if (mFollowerNpcLocalActor != nullptr &&
            mFollowerNpcLocalActor->update != nullptr) {
            return;
        }

        // Need a live play state + player to spawn at.
        if (gPlayState == nullptr) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but gPlayState is null");
            return;
        }
        Player* player = GET_PLAYER(gPlayState);
        if (player == nullptr) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but no local Player");
            return;
        }
        // Need the dynamic actor id (assigned by ActorDB::AddBuiltInCustomActors).
        if (gEnFollowerId == 0) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but gEnFollowerId is 0 "
                        "(actor not registered? — check ActorDB)");
            return;
        }

        // Spawn at the local player's current pos + facing. Y exactly
        // matches Link's so the NPC stands on the same floor; rotation
        // matches so the NPC initially faces the same way (the AI in
        // Phase 4 will reorient toward leader/path).
        const Vec3f& p = player->actor.world.pos;
        const s16 yaw  = player->actor.shape.rot.y;

        Actor* spawned = Actor_Spawn(
            &gPlayState->actorCtx, gPlayState,
            gEnFollowerId,
            p.x, p.y, p.z,
            0 /* rotX */, yaw, 0 /* rotZ */,
            0 /* params */);

        if (spawned == nullptr) {
            SPDLOG_ERROR("[FollowerNPC] Actor_Spawn failed for gEnFollowerId={} at "
                         "({:.0f},{:.0f},{:.0f})", (int)gEnFollowerId, p.x, p.y, p.z);
            return;
        }

        mFollowerNpcLocalActor = spawned;
        // Reset Phase 5 nav state so a fresh path computes on the
        // first FOLLOW tick. stuckCheckPos seeded to spawn pos so the
        // first stuck check (3s in) compares to where we started.
        sLocalNav.path.Reset();
        sLocalNav.lastPathRefreshFrame = 0;
        sLocalNav.lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
        sLocalNav.stuckCheckPos        = p;
        sLocalNav.lastStuckCheckFrame  = gameFrameCounter.load(std::memory_order_relaxed);
        sLocalNav.activeClimbAnchor    = nullptr;
        // (currentAnim init lives on EnFollower itself — set in
        // EnFollower_Init. The first EnsureAnimation tick will swap
        // from the kWait set up by LinkAnimation_PlayLoop in init to
        // whatever AnimForState chooses based on first-tick state.)
        SPDLOG_INFO("[FollowerNPC] Spawned ACTOR_EN_FOLLOWER(id={}) at "
                    "({:.0f},{:.0f},{:.0f}) yaw={} (CVar 0→1)",
                    (int)gEnFollowerId, p.x, p.y, p.z, (int)yaw);

        // Phase 3: broadcast SPAWN so peers spawn read-only replicas.
        // netId scheme: ownClientId (one NPC per client in v1, so the
        // client id IS the natural unique key).
        Vec3s rotVec3s{ 0, yaw, 0 };
        SendPacket_FollowerNpcSpawn((uint32_t)ownClientId, p, rotVec3s,
                                    (int16_t)gPlayState->sceneNum,
                                    (int8_t)gPlayState->roomCtx.curRoom.num,
                                    (uint8_t)(gSaveContext.linkAge & 0x1));
    } else {
        // Despawn: Actor_Kill the tracked instance if it's still alive.
        if (mFollowerNpcLocalActor == nullptr) {
            return;  // already despawned / never spawned
        }
        if (mFollowerNpcLocalActor->update != nullptr) {
            SPDLOG_INFO("[FollowerNPC] Despawning ACTOR_EN_FOLLOWER (CVar 1→0)");
            Actor_Kill(mFollowerNpcLocalActor);
        }
        // Phase 3: broadcast DESPAWN so peers tear down replicas.
        // reason=0 → cvar_off (the primary v1 trigger). Future phases
        // may pass 1 (died) / 2 (scene_change) / 3 (owner_disconnect).
        SendPacket_FollowerNpcDespawn((uint32_t)ownClientId, /*reason=*/0);
        // Clear our tracking pointer either way — even if update was
        // already NULL (the actor was destroyed by something else, e.g.
        // a scene transition wiping the actor list).
        mFollowerNpcLocalActor = nullptr;
    }
}

// ----------------------------------------------------------------------------
// Phase 4 — state machine + locomotion.
// ----------------------------------------------------------------------------
//
// Distance thresholds for IDLE / FOLLOW transitions. Hysteresis prevents
// flap when the leader stands at the boundary.
//   - dist >= kEnterFollow → IDLE → FOLLOW
//   - dist <= kEnterIdle   → FOLLOW → IDLE
// Same shape as the player-rigged AI Follower's kFollowThreshold pattern
// but with explicit hysteresis since the NPC's locomotion is
// direct-vector, not stick-injection (smoother but no built-in dead-zone).
static constexpr float kEnterFollow = 80.0f;
static constexpr float kEnterIdle   = 50.0f;

// Walk and run speeds in OoT units/frame. Matches Link's vanilla walk
// (~6.0) and run (~12.0). The NPC walks when close to leader and runs
// when leader is far — gives a natural feel without making the NPC
// always sprint.
static constexpr float kRunDistance = 250.0f;  // beyond this, run instead of walk
static constexpr float kWalkSpeed   = 6.0f;
static constexpr float kRunSpeed    = 12.0f;

// Phase 5 — substrate path consumption + STUCK recovery.
//
// Path refresh policy: re-query ComputePathTo every kPathRefreshMs OR
// when the captured target has moved beyond kPathRetargetDist (leader
// walked far enough that the path is stale). 500ms = 2Hz refresh —
// fast enough to track a moving leader without spamming the planner.
static constexpr int   kPathRefreshMs       = 500;
static constexpr float kPathRetargetDist    = 60.0f;
static constexpr float kAdvanceSubgoalDist  = 30.0f;  // advance cursor when within this XZ
static constexpr int   kStuckCheckMs        = 3000;   // matches player-Follower's tuned 3s
static constexpr float kStuckMinProgress    = 20.0f;
static constexpr float kStuckNudgeDist      = 30.0f;  // direct world.pos nudge in STUCK

// Phase 6 — scripted-climb constants. Tuned to match Link's vanilla
// climb feel; field-test in Inside Deku Tree may refine.
static constexpr float kClimbSpeedY         = 4.0f;   // u/frame upward; vanilla Link is ~4-5
static constexpr float kClimbSubgoalReach3D = 24.0f;  // advance cursor when within 3D
static constexpr float kClimbXzSnap         = 1.0f;   // smooth XZ snap rate to subgoal (per frame fraction)
//
// Body offset from wall surface. Climb cells sit ON the climbable
// polygon; the NPC's world.pos is at its body center, so snapping
// directly to a cell embeds the body half-into the wall. Offset
// along anchor.planeNormal (which points OUT from wall) by this
// amount so the body sits in front of the wall, matching how OoT
// renders Link on ladders/vines.
static constexpr float kClimbBodyOffset     = 12.0f;

// Phase 8 — G-guard recovery teleports. Mirror the player-rigged
// Follower's G10 / G14 semantics but adapted for direct world.pos
// writes (no stick injection).
//
// G10 leash: NPC distance to leader > kNpcLeashDistance for >
// kNpcLeashTimeoutMs → teleport NPC to leader's pos. Catches "NPC
// stuck behind a closed door / left in another scene / fell into
// untracked geometry."
static constexpr float kNpcLeashDistance      = 1200.0f;  // 3D units
static constexpr int   kNpcLeashTimeoutMs     = 2000;     // 2s, framerate-aware
//
// G14 close-fail: NPC in the 200-1200u band (close enough that G10
// won't fire) but making < kNpcCloseFailProgressDelta progress
// across kNpcCloseFailTimeoutMs → teleport NPC to current substrate
// subgoal. Catches "NPC stuck in tight geometry between rooms /
// path-around-obstacle outside the substrate's understanding."
static constexpr float kNpcCloseFailMinDistance   = 200.0f;
static constexpr float kNpcCloseFailMaxDistance   = 1200.0f;
static constexpr int   kNpcCloseFailTimeoutMs     = 10000;  // 10s
static constexpr float kNpcCloseFailProgressDelta = 30.0f;

namespace {

// True iff `npc` is THIS client's local NPC (not a peer replica). Peer
// replicas have their pos driven by FOLLOWER_NPC_STATE packets and
// should not run the AI tick.
bool IsLocalOwnerNPC(Actor* npc) {
    return Anchor::Instance != nullptr &&
           npc == Anchor::Instance->GetFollowerNpcLocalActor();
}

// (LocalNpcNavState + sLocalNav defined at file scope above so the
// spawn helper can reset them.)

// Compute XZ distance squared between two world positions.
inline float Dist2DSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Yaw toward target XZ (s16 binary angle).
inline s16 YawTowardTarget(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

// Forward-decl — defined further down (with TickCLIMBING since it's
// the primary consumer). ComputeEffectiveTarget needs it for the
// leader-climbing redirect. Pitfall 14: single-pass C++ name lookup
// requires the declaration to appear before its first use.
const ::AnchorNavRoom::ClimbAnchor* FindClosestClimbAnchor(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& pos);

// Compute the "effective target" the NPC should pursue. Normally
// this is the leader's world pos, but when the leader is climbing,
// it's the climb anchor's basePos (the floor entry below the climb)
// so the NPC routes toward the wall instead of toward leader's
// unreachable mid-wall pos. Once NPC is near basePos, the leader-
// climbing force-engage check in the dispatcher transitions to
// CLIMBING with a manually-populated path.
//
// Earlier iteration used topPos (assuming pathfinder would route up
// through climb cells), but cross-room nav isn't supported and the
// pathfinder returned empty path → G14 teleport. Targeting basePos
// keeps NPC in the same room as the wall and trips the force-engage.
Vec3f ComputeEffectiveTarget(const Vec3f& leaderPos) {
    if (!Anchor::Instance->IsLocalPlayerClimbing()) return leaderPos;
    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num);
    const ::AnchorNavRoom::ClimbAnchor* leaderAnchor =
        FindClosestClimbAnchor(navData, leaderPos);
    return leaderAnchor ? leaderAnchor->basePos : leaderPos;
}

// IDLE handler — stand still, face leader, transition to FOLLOW on
// distance exceed.
void TickIDLE(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Face leader so the NPC looks like it's tracking us even while idle.
    a->shape.rot.y = YawTowardTarget(a->world.pos, leaderPos);

    // Transition check: hysteresis upper bound. Measure against
    // effectiveTarget so IDLE→FOLLOW fires when leader starts
    // climbing — without this the NPC near a wall base sees small
    // XZ distance to climbing-leader's XZ and stays IDLE forever.
    const Vec3f effectiveTarget = ComputeEffectiveTarget(leaderPos);
    const float distSq = Dist2DSq(a->world.pos, effectiveTarget);
    if (distSq > kEnterFollow * kEnterFollow) {
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
    }
}

// FOLLOW handler — walk/run toward the current substrate subgoal,
// transition to IDLE on proximity to leader. Phase 5: substrate path
// consumption replaces direct-vector pursuit so the NPC routes
// around obstacles instead of pressing into walls.
void TickFOLLOW(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // ---- Airborne momentum lock ------------------------------------
    // While in a jump (sLocalNav.jumpInProgress), preserve speedXZ at
    // the value captured when the jump fired. Player's airborne
    // handler at z_player.c:7165 uses Math_AsymStepToF with very low
    // rates (0.05/0.1) — effectively constant linearVelocity through
    // the air. Without this, our TickFOLLOW recomputes speedXZ each
    // tick from leader-distance, killing horizontal momentum mid-jump
    // (log 147 jump 1 showed speedXZ decay 4.5 → 0 in ~20 frames).
    // Skip the rest of FOLLOW so speedXZ + yaw stay locked.
    if (sLocalNav.jumpInProgress) {
        a->speedXZ = sLocalNav.jumpStartSpeedXZ;
        a->shape.rot.y = sLocalNav.jumpStartYaw;
        a->world.rot.y = sLocalNav.jumpStartYaw;
        return;
    }

    // ---- Effective target -------------------------------------------
    // When leader is climbing, redirect path target to the climb
    // anchor's topPos (a floor node above the climb). Pathfinder
    // routes through climb cells to reach it, and the climb-cell
    // detection below engages FOLLOW→CLIMBING naturally. Without
    // this, the pathfinder's FindNearestNode skips climb nodes and
    // the path targets some random floor node — NPC reaches it and
    // idles instead of climbing up.
    const Vec3f effectiveTarget = ComputeEffectiveTarget(leaderPos);

    // ---- Path refresh ------------------------------------------------
    const uint64_t curFrame   = Anchor::Instance->gameFrameCounter.load(
                                    std::memory_order_relaxed);
    const int      refreshTicks = Anchor::Instance->MsToGameTicks(kPathRefreshMs);
    const bool needRefresh =
        sLocalNav.path.Empty() ||
        (refreshTicks > 0 &&
         curFrame >= sLocalNav.lastPathRefreshFrame + (uint64_t)refreshTicks) ||
        AnchorDist::DistXZSq(effectiveTarget, sLocalNav.lastPathTargetPos) >
            kPathRetargetDist * kPathRetargetDist;
    if (needRefresh) {
        AnchorNav::TrailKey leaderKey =
            AnchorNav::TrailKeyForPlayer((uint8_t)Anchor::Instance->ownClientId);
        sLocalNav.path.Reset();
        AnchorNav::ActorTrail::GetInstance().ComputePathTo(
            leaderKey, a, effectiveTarget, play, sLocalNav.path,
            /*skipLayer1LOS=*/false,
            /*preferLeaderTrail=*/true);  // leader's breadcrumbs are the natural pursuit hint
        sLocalNav.lastPathRefreshFrame = curFrame;
        sLocalNav.lastPathTargetPos    = effectiveTarget;
    }

    // ---- Phase 6: climb-subgoal transition --------------------------
    // If the current path subgoal is a climb cell, transition to
    // CLIMBING. The CLIMBING handler takes over snapping XZ to the
    // wall + driving Y. Same shape as the player-Follower's Stage 6
    // substrate-driven CLIMBING engagement.
    if (!sLocalNav.path.Empty()) {
        const uint32_t flags = sLocalNav.path.CurrentSubgoalFlags();
        if (flags & ::AnchorNavRoom::NODE_CLIMB_ANY) {
            this_->state = EN_FOLLOWER_STATE_CLIMBING;
            sLocalNav.activeClimbAnchor = nullptr;  // resolved fresh on entry
            SPDLOG_INFO("[FollowerNPC] FOLLOW→CLIMBING (path entered climb cell at "
                        "({:.0f},{:.0f},{:.0f}); flags=0x{:X})",
                        sLocalNav.path.CurrentSubgoal().x,
                        sLocalNav.path.CurrentSubgoal().y,
                        sLocalNav.path.CurrentSubgoal().z,
                        flags);
            return;
        }
    }

    // ---- Pick subgoal -----------------------------------------------
    // Substrate path's CurrentSubgoal if available; else direct to leader.
    Vec3f subgoal = sLocalNav.path.Empty() ? leaderPos : sLocalNav.path.CurrentSubgoal();

    // Cursor advancement: if we're close to the current subgoal, step.
    if (!sLocalNav.path.Empty() &&
        AnchorDist::DistXZSq(a->world.pos, subgoal) <
            kAdvanceSubgoalDist * kAdvanceSubgoalDist) {
        sLocalNav.path.Advance();
        // Refresh subgoal selection for this same tick — avoid wasting
        // a frame standing in place after an advance.
        subgoal = sLocalNav.path.Empty() ? leaderPos : sLocalNav.path.CurrentSubgoal();
    }

    // ---- Drive locomotion -------------------------------------------
    const s16 yaw = YawTowardTarget(a->world.pos, subgoal);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;

    // Speed selection — match leader's pace, with a small catch-up
    // bonus that only kicks in when significantly farther than the
    // IDLE→FOLLOW threshold. Prevents the "sprint up → stop → sprint
    // up" oscillation pattern caused by always-sprint catch-up: NPC
    // would close to within kEnterIdle (50u), drop to IDLE, leader
    // walks, NPC re-enters FOLLOW, sprint, repeat.
    //
    // Bands:
    //   - dist > kRunDistance (250u): always sprint at kRunSpeed.
    //     Big gap — catch up fast.
    //   - dist 100-250u: match leader + (dist - 100) * 0.1 catch-up
    //     bonus, capped at kRunSpeed. Slowly closes the gap.
    //   - dist < 100u (down to kEnterIdle): match leader's speed
    //     EXACTLY. Distance is stable; NPC trails at fixed offset.
    //     No oscillation.
    //
    // When leader is stopped (speedXZ=0) and NPC reaches the close
    // band, NPC effectively halts at the current distance. The
    // IDLE re-entry check (distToTargetSq <= kEnterIdle²) handles
    // the IDLE switch when NPC drifts in further.
    const float distToLeaderSq = Dist2DSq(a->world.pos, leaderPos);
    Player*     leader         = GET_PLAYER(play);
    const float leaderSpeed    = (leader != nullptr) ? leader->actor.speedXZ : 0.0f;
    float       speed;
    if (distToLeaderSq > kRunDistance * kRunDistance) {
        speed = kRunSpeed;
    } else {
        const float dist = std::sqrt(distToLeaderSq);
        const float catchupBonus = std::max(0.0f, (dist - 100.0f) * 0.1f);
        speed = std::min(leaderSpeed + catchupBonus, kRunSpeed);
        // No min-speed floor: NPC matches leader's pace exactly when
        // close, including pace-of-zero when leader is stopped. Prior
        // 0.5 floor produced visible sliding (NPC drifting toward
        // leader while idle anim played, since FOLLOW state never
        // transitioned cleanly to IDLE in the hover band).
    }
    a->speedXZ = speed;

    // ---- Stuck check ------------------------------------------------
    const int stuckCheckTicks = Anchor::Instance->MsToGameTicks(kStuckCheckMs);
    if (stuckCheckTicks > 0 &&
        curFrame >= sLocalNav.lastStuckCheckFrame + (uint64_t)stuckCheckTicks) {
        const float progress = std::sqrt(Dist2DSq(a->world.pos, sLocalNav.stuckCheckPos));
        if (progress < kStuckMinProgress) {
            // No real progress in 3s — nudge in STUCK and force path
            // refresh next tick.
            this_->state                   = EN_FOLLOWER_STATE_STUCK;
            sLocalNav.path.Reset();        // discard the broken path
            sLocalNav.lastPathRefreshFrame = 0;
            SPDLOG_INFO("[FollowerNPC] FOLLOW→STUCK (no progress {:.1f}u in 3s @ "
                        "({:.0f},{:.0f},{:.0f}))",
                        progress, a->world.pos.x, a->world.pos.y, a->world.pos.z);
        }
        sLocalNav.stuckCheckPos       = a->world.pos;
        sLocalNav.lastStuckCheckFrame = curFrame;
    }

    // ---- Transition: arrived at leader -----------------------------
    // Measure against effectiveTarget (which redirects to anchor topPos
    // while leader is climbing). Without this, NPC at the wall base
    // sees small XZ distance to climbing-leader's XZ and enters IDLE
    // before ever engaging CLIMBING.
    const float distToTargetSq = Dist2DSq(a->world.pos, effectiveTarget);
    if (distToTargetSq <= kEnterIdle * kEnterIdle) {
        this_->state = EN_FOLLOWER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalNav.path.Reset();  // discard path; IDLE is local-frame
    }
}

// Animation switching helper — only call LinkAnimation_Change on
// real transitions. Calling every frame restarts the playhead.
// `currentAnim` is per-actor (on EnFollower) so peer replicas track
// their own state independently.
//
// Anim variants: use the _free variants (PLAYER_ANIMTYPE_UNARMED
// equivalent — arms down, no shield). The non-_free anims are the
// fighter / sword-and-shield poses, which would look wrong on an
// NPC that has no combat in v1. Player's lookup table at
// z_player.c:580-605 uses _free for unarmed.
//
// playSpeed defaults to 1.0; the dispatcher overrides skelAnime.playSpeed
// per-frame for walk/run anims so motion-cadence stays in sync as
// speedXZ varies within a single anim (z_player.c:8445 pattern).
// Pick anim header for a given anim kind + Player modelAnimType.
// Mirrors Player's D_80853914 2D table (z_player.c:578) but only for
// the kinds our NPC plays. modelAnimType maps:
//   0 = unarmed / free (no shield, no sword in hand)
//   1 = fighter (sword + shield drawn, ready stance)
//   2 = fighter alt — same locomotion anims as 1 in Player's table
//   3 = long sword (two-handed Biggoron Sword)
//   4/5 = "free" variants — same as 0 in Player's table
// CLIMBING anims and fidgets don't have modelAnimType variants in
// Player's table (they're shared across stances).
LinkAnimationHeader* AnimHeaderFor(FollowerNpcAnim kind, s8 modelAnimType) {
    const bool isLong    = (modelAnimType == 3);
    const bool isFighter = (modelAnimType == 1 || modelAnimType == 2);
    switch (kind) {
        case FollowerNpcAnim::kWait:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_wait_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free;
        case FollowerNpcAnim::kWalk:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_free;
        case FollowerNpcAnim::kRun:
            if (isLong)            return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_run_long;
            if (modelAnimType == 1) return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_run;
            if (modelAnimType == 2) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_free;
        case FollowerNpcAnim::kStopL:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_endL_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL_free;
        case FollowerNpcAnim::kStopR:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_endR_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR_free;
        // Shared across stances:
        case FollowerNpcAnim::kClimbUp:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_upL;
        case FollowerNpcAnim::kFidgetLookA:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeA_20f;
        case FollowerNpcAnim::kFidgetWarmB:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeB_20f;
        case FollowerNpcAnim::kFidgetStretchD:
            return (LinkAnimationHeader*)&gPlayerAnim_link_wait_typeD_20f;
        // Swimming — Player uses the same swim anims regardless of
        // modelAnimType (sword stays sheathed in water). Anim cycle is
        // looping for both.
        case FollowerNpcAnim::kSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim;
        case FollowerNpcAnim::kSwimWait:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_wait;
        case FollowerNpcAnim::kHoistGround:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_climb_up;
        case FollowerNpcAnim::kHoistSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_15step_up;
        case FollowerNpcAnim::kRunJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_jump;
        case FollowerNpcAnim::kJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_jump;
        case FollowerNpcAnim::kNone:
        default:
            return nullptr;
    }
}

void EnsureAnimation(EnFollower* this_, PlayState* play, FollowerNpcAnim want) {
    // Pick the right anim header for the (kind, modelAnimType) pair.
    // currentAnimType is updated by the dispatcher BEFORE this call to
    // reflect the local Player's current armed-stance state.
    LinkAnimationHeader* anim = AnimHeaderFor(want, this_->currentAnimType);
    if (anim == nullptr) return;
    // No-op only if BOTH the kind and the resolved anim header match.
    // This way a modelAnimType change (e.g. unarmed → fighter when
    // player draws sword+shield) correctly re-fires the anim with
    // the armed variant, while same-kind+same-header transitions
    // (e.g. type 4 ↔ type 0 — both pick _free) are no-ops.
    if ((FollowerNpcAnim)this_->currentAnim == want &&
        this_->skelAnime.animation == anim) {
        return;
    }
    const bool oneShot =
        want == FollowerNpcAnim::kStopL ||
        want == FollowerNpcAnim::kStopR ||
        want == FollowerNpcAnim::kFidgetLookA ||
        want == FollowerNpcAnim::kFidgetWarmB ||
        want == FollowerNpcAnim::kFidgetStretchD ||
        want == FollowerNpcAnim::kHoistGround ||
        want == FollowerNpcAnim::kHoistSwim ||
        want == FollowerNpcAnim::kRunJump ||
        want == FollowerNpcAnim::kJump;
    LinkAnimation_Change(play, &this_->skelAnime, anim,
                          1.0f /* playSpeed — caller overrides per-frame */,
                          0.0f /* startFrame */,
                          Animation_GetLastFrame((void*)anim),
                          oneShot ? ANIMMODE_ONCE : ANIMMODE_LOOP,
                          -6.0f /* morphFrames */);
    this_->currentAnim     = (s32)want;
    this_->stopAnimPlaying = oneShot ? 1 : 0;
}

// ── Anim polish helpers (audit 2026-05-16, batch 1 / #4 / #5) ────────

// Step phase cycle length — Player uses 29.0f (z_player.c:8109). The
// walk/run anims are tuned to this cycle. Foot-down frames at ~10 and
// ~24 within the 29-unit cycle (Player calls these with
// func_8084021C(stepPhase, advance, 29.0, 10.0/24.0)).
static constexpr float kStepPhaseCycle    = 29.0f;
static constexpr float kStepPhaseFootDownL = 10.0f;
static constexpr float kStepPhaseFootDownR = 24.0f;
// Step phase threshold for stop-anim L vs R selection (Player at
// z_player.c:6397 splits at sp30 < 14). sp30 = unk_868 - 3, so the
// raw threshold is (3 + 14) = 17 in step-phase units; we ignore the
// -3 phase shift and use 14 directly (close enough for visual L/R).
static constexpr float kStopPhaseLRSplit  = 14.0f;

// Detect step-phase crossing for footstep SFX. Returns true if the
// phase advanced PAST `footDown` in this tick (handles the wrap-around
// at kStepPhaseCycle so a tick that crosses the wrap still fires).
bool StepPhaseCrossed(float prevPhase, float curPhase, float footDown) {
    // No advance, no fire.
    if (curPhase == prevPhase) return false;
    // Wrap case: prev was near the end, cur wrapped to near the start.
    if (curPhase < prevPhase) {
        return (prevPhase < footDown && footDown <= kStepPhaseCycle) ||
               (curPhase >= footDown && footDown >= 0.0f);
    }
    // Normal forward case.
    return (prevPhase < footDown && footDown <= curPhase);
}

// Tick step phase, fire footstep SFX on cross. Returns the previous
// phase (for diagnostics if needed).
void TickStepPhaseAndSfx(EnFollower* this_, PlayState* play) {
    const float playSpeed  = this_->skelAnime.playSpeed;
    const float updateRate = R_UPDATE_RATE * 0.5f;
    const float advance    = playSpeed * updateRate;
    const float prevPhase  = this_->stepPhase;
    float       newPhase   = prevPhase + advance;
    while (newPhase >= kStepPhaseCycle) newPhase -= kStepPhaseCycle;
    while (newPhase < 0.0f)             newPhase += kStepPhaseCycle;
    this_->stepPhase = newPhase;

    // Fire footstep SFX on foot-down frame cross. NA_SE_PL_WALK_GROUND
    // is the base walk-on-ground SFX (Player_ApplyFloorAndAgeSfxOffsets
    // selects the floor-variant version; we use the base for v1 — a
    // future polish item is to honor floor type, but base sfx covers
    // all surfaces audibly). Pitch shift scales with speed (Player
    // passes linearVelocity at z_player.c:8099).
    if (StepPhaseCrossed(prevPhase, newPhase, kStepPhaseFootDownL) ||
        StepPhaseCrossed(prevPhase, newPhase, kStepPhaseFootDownR)) {
        func_800F4010(&this_->actor.projectedPos, NA_SE_PL_WALK_GROUND,
                      this_->actor.speedXZ);
    }
}

// Pick the right animation for the current state. Used by both the
// local-owner path (post-dispatch) and the peer-replica path (state
// arrives via FOLLOWER_NPC_STATE).
//
// FOLLOW chooses walk vs run based on speedXZ. For peers, speedXZ
// comes from the synced field (`syncedSpeedXZ`) populated by the
// state-packet handler; for local owners, it's the value the AI
// just wrote.
// ── Head-look-at-leader ──────────────────────────────────────────────
// Step NPC's `headLimbRot` + `upperLimbRot` toward the relative yaw of
// leader. Pattern mirrors Player's z_player.c:3735-3750 in shape but
// uses simpler step-toward semantics: head turns within a comfortable
// yaw range; if leader is behind, upper body twists too.
//
// Constraints:
//   - Head yaw max ±0x4000 (90°). Beyond that, upper-body twist takes over.
//   - Pitch: small angle (head tilts up/down slightly if leader is above/
//     below). Capped at ±0x2000 (45°).
//   - Step rate: ~0x600 per tick (slower than instant — feels alive).
//
// Drives the NPC's pose values written into Player's headLimbRot /
// upperLimbRot via the EnFollower_Draw save/swap/restore.
void TickHeadLookAtLeader(EnFollower* this_, const Vec3f& leaderPos) {
    const Actor* a = &this_->actor;
    const s16 dirYaw = Math_Atan2S(leaderPos.z - a->world.pos.z,
                                   leaderPos.x - a->world.pos.x);
    const s16 yawRel = dirYaw - a->shape.rot.y;  // relative to body facing

    // Pitch via OoT's Math_Vec3f_Pitch formula at z_lib.c:292-294.
    // Math_Atan2S(forward, side) takes forward axis first; passing
    // (dy, distXZ) was treating dy as forward, producing 0x4000 (90°)
    // for dy=0 → clamped to -0x2000 → head locked looking up.
    //
    // Correct: Math_Atan2S(distXZ, npc.y - leader.y). Returns small
    // negative when leader is above (≈ "looking up" in Player's
    // headLimbRot.x convention); positive when leader is below;
    // 0 when at the same height.
    const float dx     = leaderPos.x - a->world.pos.x;
    const float dz     = leaderPos.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx*dx + dz*dz);
    const s16 pitchRel = (distXZ > 1.0f)
        ? Math_Atan2S(distXZ, a->world.pos.y - leaderPos.y)
        : 0;

    // Apportion yaw: head takes up to ±kHeadYawMax (70° in OoT binary
    // angle), upper body twists for the rest. Field test reported
    // visually strange head angles when the limit was ±0x4000 (90°)
    // — heads turn unnaturally far. Tightened to ±70° = 12743 binary
    // (≈ 0x31C7), matching a natural neck rotation range.
    constexpr s16 kHeadYawMax = 12743;
    s16 headYawTarget  = yawRel;
    s16 upperYawTarget = 0;
    if (headYawTarget >  kHeadYawMax) { upperYawTarget = headYawTarget - kHeadYawMax; headYawTarget =  kHeadYawMax; }
    if (headYawTarget < -kHeadYawMax) { upperYawTarget = headYawTarget + kHeadYawMax; headYawTarget = -kHeadYawMax; }
    // If leader is mostly behind, cap upper twist at ±0x4000 (don't snap-spin).
    if (upperYawTarget >  0x4000) upperYawTarget =  0x4000;
    if (upperYawTarget < -0x4000) upperYawTarget = -0x4000;

    // Head pitch cap at ±0x2000.
    s16 headPitchTarget = pitchRel;
    if (headPitchTarget >  0x2000) headPitchTarget =  0x2000;
    if (headPitchTarget < -0x2000) headPitchTarget = -0x2000;

    // Step toward targets. ~0x600 per tick = ~5° at 20fps.
    Math_ScaledStepToS(&this_->headLimbRot.y,  headYawTarget,   0x600);
    Math_ScaledStepToS(&this_->upperLimbRot.y, upperYawTarget,  0x600);
    Math_ScaledStepToS(&this_->headLimbRot.x,  headPitchTarget, 0x600);
}

// ── Idle blend (waitL ↔ waitR) ───────────────────────────────────────
// Continuously blend between waitL_free and waitR_free for a subtle
// breathing-with-side-glance idle. Pattern from Player at z_player.c:8062.
// Free-running phase (sine of frame counter) — no external target driver
// needed for our v1; idle isn't long enough for the slow target-driven
// pattern Player uses.
void TickIdleBlend(EnFollower* this_, PlayState* play) {
    // Phase advances ~1/40 cycle per tick → ~2s per full L↔R↔L cycle.
    this_->idleBlendPhase += (1.0f / 40.0f);
    if (this_->idleBlendPhase > 1.0f) this_->idleBlendPhase -= 1.0f;
    // Blend weight: 0.5 + 0.5 * sin(2π * phase) — oscillates 0..1.
    const float weight = 0.5f + 0.5f * Math_SinS((s16)(this_->idleBlendPhase * 0x10000));

    LinkAnimation_BlendToJoint(
        play, &this_->skelAnime,
        (LinkAnimationHeader*)&gPlayerAnim_link_normal_waitR_free,
        this_->skelAnime.curFrame,
        (LinkAnimationHeader*)&gPlayerAnim_link_normal_waitL_free,
        this_->skelAnime.curFrame,
        weight, this_->blendTable);
}

FollowerNpcAnim AnimForState(s32 state, float speedXZ) {
    switch (state) {
        case EN_FOLLOWER_STATE_FOLLOW: {
            // Truly-stopped speed (≈ 0) → kWait. Anything else plays
            // walk or run based on Player's own walk↔run threshold of
            // speedTarget > 4 (z_player.c:8165). NPC pace-matches the
            // leader, so its speedXZ value range mirrors leader's:
            // walk ~3, run ~7-9, sprint with catch-up bonus up to 12.
            if (speedXZ < 0.1f) return FollowerNpcAnim::kWait;
            return (speedXZ > 4.0f) ? FollowerNpcAnim::kRun : FollowerNpcAnim::kWalk;
        }
        case EN_FOLLOWER_STATE_STUCK:
            // STUCK runs for a single tick before transitioning to
            // FOLLOW; play wait so the brief frame doesn't show the
            // NPC mid-stride at zero motion.
            return FollowerNpcAnim::kWait;
        case EN_FOLLOWER_STATE_CLIMBING:
            return FollowerNpcAnim::kClimbUp;
        case EN_FOLLOWER_STATE_SWIMMING:
            // Treading water at low speed; full swim anim when moving.
            // Threshold matches Player's walk↔run handoff at 4.0.
            return (speedXZ > 0.5f) ? FollowerNpcAnim::kSwim
                                    : FollowerNpcAnim::kSwimWait;
        case EN_FOLLOWER_STATE_LEDGE_HOIST:
            // Picked from hoistContext by the dispatcher's anim
            // resolution path (which has access to `this_`). This
            // case shouldn't normally be hit because the dispatcher
            // overrides localAnim during LEDGE_HOIST before calling
            // EnsureAnimation; return a sensible default in case the
            // dispatcher path is bypassed.
            return FollowerNpcAnim::kHoistGround;
        case EN_FOLLOWER_STATE_DEAD:
            // v1 stub — wait pose. v2 combat will switch to a death anim.
            return FollowerNpcAnim::kWait;
        case EN_FOLLOWER_STATE_IDLE:
        default:
            return FollowerNpcAnim::kWait;
    }
}

// Phase 6 — find the climb anchor whose grid is closest to `pos`.
// Used at CLIMBING entry to pick the anchor; cached in
// sLocalNav.activeClimbAnchor for the duration of the climb run.
//
// "Closest" = min XZ distance to anchor.basePos (the anchor's
// floor-level entry point). Suffices because anchors are well-spaced
// in OoT scenes (Inside Deku Tree's 5 spiral-wall anchors are 100u+
// apart in XZ).
const ::AnchorNavRoom::ClimbAnchor* FindClosestClimbAnchor(
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

// Force-engage CLIMBING by manually populating the substrate path
// with this anchor's climb cells, sorted bottom-to-top by Y. Used
// when leader is climbing and substrate pathfinding can't bridge
// (cross-room target, leader mid-wall) — we bypass the pathfinder
// and just walk up the anchor's known cells.
//
// `topYBound`: only include cells at or below leaderY + 50u so NPC
// doesn't climb past where leader is. Lets the NPC track leader's
// vertical progress without overshooting.
//
// Returns true on success (path populated with ≥1 cell).
bool PopulateAnchorClimbPath(const ::AnchorNavRoom::RoomNavData* navData,
                             const ::AnchorNavRoom::ClimbAnchor& anchor,
                             const Vec3f& npcPos, const Vec3f& leaderPos,
                             AnchorNav::ActorTrail::NavPath& path)
{
    if (navData == nullptr || anchor.nodeCount == 0) return false;
    path.Reset();

    // Collect (Y, idx) pairs for nodes in the column closest to NPC's
    // XZ. Multi-column walls (wide vines) have many columns; pick the
    // one nearest NPC so we don't zigzag laterally during the climb.
    //
    // Column selection: project NPC and each node onto anchor.planeAxisU
    // (lateral wall axis). Keep nodes whose U-coordinate is within
    // 40u of NPC's U-coordinate (≈ 1 cell spacing of 30u + slack).
    // Use LEADER's U projection as the column-selection reference, NOT
    // NPC's. Leader is on the wall, so leader's U reliably matches a
    // cell column. NPC's pos in water (or on the floor near the wall)
    // can be offset along the wall's lateral axis so NPC's U doesn't
    // match any cell — log 142 showed "anchor found, distance OK
    // (172u), but path population returned empty" because NPC's U
    // missed all 8 cells of the anchor. Tracking leader's column
    // guarantees the path follows where the leader actually climbed.
    const float leaderU =
        (leaderPos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
        (leaderPos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;

    struct Entry { float y; uint16_t idx; };
    std::vector<Entry> column;
    // Column filter — cells within ±15u of leader's U (≈ half a 30u
    // cell pitch). Y-axis cap at leader.y - 20 so NPC stays slightly
    // BELOW leader during co-climb (was leader.y + 50 which let NPC
    // outpace leader by 50-100u — Player's input-driven climb is
    // slower than NPC's fixed 4 u/frame).
    constexpr float kClimbColumnTolerance     = 15.0f;
    constexpr float kClimbColumnFallbackTol   = 30.0f;
    constexpr float kClimbStayBelowLeader     = 20.0f;
    auto collectColumn = [&](float tolerance) {
        column.clear();
        for (uint16_t i = 0; i < anchor.nodeCount; i++) {
            const uint16_t idx = anchor.firstNodeIdx + i;
            if (idx >= navData->nodes.size()) break;
            const auto& n = navData->nodes[idx];
            const float nodeU =
                (n.pos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
                (n.pos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;
            if (std::fabs(nodeU - leaderU) > tolerance) continue;
            if (n.pos.y > leaderPos.y - kClimbStayBelowLeader) continue;
            column.push_back({n.pos.y, idx});
        }
    };
    // Try strict (±15u) first. If empty (leader's U doesn't match any
    // column — happens on curved walls / off-grid leader pos), fall
    // back to wider tolerance (±30u, full cell pitch). Captures
    // adjacent columns; minor lateral hop acceptable vs no climb.
    collectColumn(kClimbColumnTolerance);
    if (column.empty()) {
        collectColumn(kClimbColumnFallbackTol);
    }
    if (column.empty()) return false;

    // Sort by Y ascending. NPC starts at bottom; CLIMBING handler
    // advances cursor as it climbs.
    std::sort(column.begin(), column.end(),
              [](const Entry& a, const Entry& b){ return a.y < b.y; });

    // Skip cells at or below NPC's current Y — strict filter, no
    // downward slack. Without this, RE-ENTRY into CLIMBING (after
    // the previous segment's path exhausted) would build a new path
    // starting up to 30u BELOW NPC's current Y, forcing NPC to climb
    // DOWN before resuming the climb up. User observed this as
    // "oscillates up and down" during sustained leader-climbing.
    // With strict filter, every re-engagement starts strictly ABOVE
    // NPC's current Y → monotonic ascent.
    for (const auto& e : column) {
        if (e.y < npcPos.y) continue;
        const auto& n = navData->nodes[e.idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    if (path.waypoints.empty()) {
        // NPC already above the entire column (unlikely but defensive).
        // Push at least the top cell so CLIMBING has SOMETHING to chase.
        const auto& n = navData->nodes[column.back().idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    path.sceneNum = gPlayState->sceneNum;
    return true;
}

// Engagement constants for the leader-climbing trigger.
static constexpr float kClimbForceEngageBaseDistSq = 200.0f * 200.0f;

// CLIMBING handler — Phase 6 scripted-climb driver. Runs while the
// substrate path's current subgoal carries a NODE_CLIMB_* flag.
//
// Mechanics (per Plans/npc_follower_plan.md §3):
//   - Snap NPC XZ each frame to the current climb subgoal's XZ.
//   - Drive Y toward the subgoal at kClimbSpeedY.
//   - Face into wall via the active anchor's planeNormal.
//   - Play gPlayerAnim_link_normal_Fclimb_upL (looping).
//   - Advance path cursor on 3D proximity to current subgoal.
//   - Exit CLIMBING when the next subgoal is non-climb (mantle out
//     to FOLLOW) OR when path is exhausted (FOLLOW handles fallback).
//
// v1 simplifications (deferred to later iterations):
//   - No mantle-hold animation (snaps to top on exit).
//   - No lateral-axis splitting (vines only support vertical motion
//     in v1; lateral happens via XZ snap each frame).
//   - No detach-detection (peer-Follower has it; v1 NPC trusts the
//     scripted snap to keep us on the wall).
//   - No climb-down anim (Y always positive in v1; if leader is below,
//     the substrate path probably routes through a drop anchor not
//     a climb-down).
void TickCLIMBING(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // Resolve subgoal.
    if (sLocalNav.path.Empty()) {
        // Path exhausted — if leader is STILL climbing and we have an
        // active anchor, refresh the path with new cells above NPC's
        // current Y and stay in CLIMBING. Without this, NPC exits to
        // FOLLOW between short climb segments, gravity pulls Y down
        // (no floor at the wall side), force-engage refires at a
        // lower Y, NPC climbs again — visible oscillation. Log 143
        // showed NPC bouncing between Y=79 and Y=93 because the
        // anchor had only 2 cells per column above NPC at any time.
        if (Anchor::Instance->IsLocalPlayerClimbing() &&
            sLocalNav.activeClimbAnchor != nullptr) {
            const ::AnchorNavRoom::RoomNavData* navData =
                ::AnchorNavRoom::GetForRoom(
                    gPlayState->sceneNum,
                    (int8_t)gPlayState->roomCtx.curRoom.num);
            if (navData != nullptr &&
                PopulateAnchorClimbPath(navData, *sLocalNav.activeClimbAnchor,
                                        a->world.pos, leaderPos,
                                        sLocalNav.path)) {
                // Path refreshed in place; continue climbing this tick.
                // Fall through to the subgoal-resolution code below.
            }
        }
        // Re-check after refresh attempt.
        if (sLocalNav.path.Empty()) {
            // Mantle-out: if NPC has reached near the top of the wall
            // (within 60u of anchor.topPos.y), snap to topPos and
            // exit. Without this, NPC at the top of climb falls when
            // CLIMBING exits — leader hoisted to ledge, we lost
            // refresh trigger, NPC mid-wall has no floor below →
            // gravity drops NPC. Snap to topPos puts NPC on the
            // ledge floor.
            if (sLocalNav.activeClimbAnchor != nullptr) {
                const float topY = sLocalNav.activeClimbAnchor->topPos.y;
                if (a->world.pos.y >= topY - 60.0f) {
                    a->world.pos  = sLocalNav.activeClimbAnchor->topPos;
                    a->velocity.y = 0.0f;
                    SPDLOG_INFO("[FollowerNPC] CLIMBING→FOLLOW (mantle-out: "
                                "NPC at top, snapped to anchor.topPos "
                                "({:.0f},{:.0f},{:.0f}))",
                                a->world.pos.x, a->world.pos.y, a->world.pos.z);
                }
            }
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
            sLocalNav.activeClimbAnchor = nullptr;
            return;
        }
    }
    const Vec3f& subgoal      = sLocalNav.path.CurrentSubgoal();
    const uint32_t subgoalFlags = sLocalNav.path.CurrentSubgoalFlags();
    const bool subgoalIsClimb = (subgoalFlags & ::AnchorNavRoom::NODE_CLIMB_ANY) != 0;

    // Mantle-out: next subgoal is non-climb → snap to subgoal pos
    // (effectively the top of the climb), exit to FOLLOW.
    if (!subgoalIsClimb) {
        a->world.pos.x = subgoal.x;
        a->world.pos.y = subgoal.y;
        a->world.pos.z = subgoal.z;
        a->speedXZ     = 0.0f;
        sLocalNav.activeClimbAnchor = nullptr;
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        return;
    }

    // Resolve the active anchor (cache on entry; refresh if the
    // navData was rebuilt or cache is stale).
    if (sLocalNav.activeClimbAnchor == nullptr) {
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        sLocalNav.activeClimbAnchor = FindClosestClimbAnchor(navData, a->world.pos);
        if (sLocalNav.activeClimbAnchor == nullptr) {
            // No anchor data — fall back to FOLLOW.
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
            return;
        }
    }
    const auto& anc = *sLocalNav.activeClimbAnchor;

    // XZ position: when leader is also climbing this anchor, MATCH
    // leader's XZ exactly — leader's pos is on the actual climb
    // surface (vine cell or ladder rung) at the right lateral
    // position. Without this, NPC snaps to nearest CELL.xz which is
    // grid-quantized (~30u pitch) and can be 15u off from leader's
    // actual lateral position.
    //
    // When leader is NOT climbing (NPC alone on the wall), use the
    // cell-based snap with body offset along planeNormal so NPC's
    // body sits in front of the wall surface (not buried).
    if (Anchor::Instance->IsLocalPlayerClimbing()) {
        a->world.pos.x = leaderPos.x;
        a->world.pos.z = leaderPos.z;
    } else {
        a->world.pos.x = subgoal.x + anc.planeNormal.x * kClimbBodyOffset;
        a->world.pos.z = subgoal.z + anc.planeNormal.z * kClimbBodyOffset;
    }

    // Drive Y toward subgoal at kClimbSpeedY. Clamp to subgoal Y on
    // approach so we don't overshoot. (planeNormal.y component
    // intentionally ignored — climbable walls are near-vertical, and
    // applying body-offset to Y would lift NPC above ladder rungs.)
    const float dy = subgoal.y - a->world.pos.y;
    if (std::fabs(dy) < kClimbSpeedY) {
        a->world.pos.y = subgoal.y;
    } else {
        a->world.pos.y += (dy > 0.0f ? kClimbSpeedY : -kClimbSpeedY);
    }

    // Face INTO the wall — opposite of planeNormal (which points OUT).
    a->shape.rot.y = Math_Atan2S(-anc.planeNormal.z, -anc.planeNormal.x);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ     = 0.0f;  // we're scripting position, not using physics speed

    // Climb-up animation. Plays looping; persists across frames via
    // EnsureAnimation's transition guard.
    EnsureAnimation(this_, play, FollowerNpcAnim::kClimbUp);

    // Cursor advance — Y-axis only. NPC's XZ is snapped to subgoal.xz +
    // planeNormal * kClimbBodyOffset every frame (so body sits in front
    // of the wall, not buried), which means XZ distance from subgoal is
    // ~12u baseline regardless of climb progress. A 3D distance check
    // (the earlier kClimbSubgoalReach3D=24u test) would fire on the
    // very first frame after XZ snap because 12² < 24² — chaining
    // multiple advances per tick and zipping through the path.
    //
    // Y is the meaningful progress axis for a vertical climb. Advance
    // when within 12u of the subgoal's Y (≈ 3 frames of climb at
    // kClimbSpeedY=4.0). Produces one cell of progress per ~7 frames
    // at the configured climb speed — smooth and visibly paced.
    const float dyAdv = std::fabs(a->world.pos.y - subgoal.y);
    if (dyAdv < 12.0f) {
        sLocalNav.path.Advance();
    }
}

// Swim constants. Match Player's ageProperties.unk_24 (z_player.c:453,505):
// 36u for adult, 22u for child — that's the submersion depth at which
// Player switches to swimming AND the depth Player maintains while
// swimming (so head sits ~24u above surface for adult, ~28u for child).
//
// kSwimSpeedMax = Link's swim cap (~3-4 u/frame).
// kSwimShoreExitDepth = if floor below NPC is within this much of
//   the water surface, NPC can walk onto the floor → exit SWIMMING.
//   Matches Link's step height (most actors can step up ~30u).
static constexpr float kSwimDepthThresholdAdult = 36.0f;
static constexpr float kSwimDepthThresholdChild = 22.0f;
static constexpr float kSwimSpeedMax            = 4.0f;
static constexpr float kSwimEnterArrive         = 60.0f;
static constexpr float kSwimShoreExitDepth      = 30.0f;

// Pick the per-age swim depth (used both for entry threshold and
// surface-clamp drop — Player maintains depth = unk_24 while swimming).
inline float SwimDepthFor(s8 linkAge) {
    // gSaveContext.linkAge: 0 = adult, 1 = child (matches sAgeProperties
    // indexing at z_player.c:442 / 498).
    return (linkAge == 0) ? kSwimDepthThresholdAdult : kSwimDepthThresholdChild;
}

// SWIMMING handler — clamp Y to water surface at Link's swim depth,
// swim XZ toward leader at swim pace. Exits to FOLLOW when:
//   (a) no water at NPC's XZ (swimming straight off the edge of the
//       waterbox extent), OR
//   (b) shore is shallow enough below NPC to walk on (floor is
//       within kSwimShoreExitDepth of the water surface).
// Dispatcher skips Actor_MoveXZGravity for SWIMMING because gravity
// would fight the Y surface clamp.
void TickSWIMMING(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // Clamp Y to (surface - swim depth). For adult that's surface-36,
    // for child surface-22. Maintains Link's swim pose where head
    // sits above water and most of body submerged.
    f32 surfaceY = a->world.pos.y;
    WaterBox* wb = nullptr;
    if (!WaterBox_GetSurface1(play, &play->colCtx,
                              a->world.pos.x, a->world.pos.z,
                              &surfaceY, &wb)) {
        SPDLOG_INFO("[FollowerNPC] SWIMMING→FOLLOW (no water under NPC at "
                    "({:.0f},{:.0f},{:.0f}))",
                    a->world.pos.x, a->world.pos.y, a->world.pos.z);
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        return;
    }

    // Shore-shallow exit. Raycast floor down from a point slightly
    // above NPC. If the floor is within kSwimShoreExitDepth of the
    // surface, NPC can stand on it — exit swim. Without this, NPC
    // never walks out onto sloped terrain (waterboxes extend right
    // to the shore, so WaterBox_GetSurface1 keeps returning true).
    Vec3f rayStart = { a->world.pos.x, surfaceY + 5.0f, a->world.pos.z };
    CollisionPoly* floorPoly = nullptr;
    const f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &floorPoly, &rayStart);
    if (floorPoly != nullptr && (surfaceY - floorY) < kSwimShoreExitDepth) {
        a->world.pos.y = floorY;
        a->velocity.y  = 0.0f;
        this_->state   = EN_FOLLOWER_STATE_FOLLOW;
        SPDLOG_INFO("[FollowerNPC] SWIMMING→FOLLOW (shore-shallow exit: "
                    "surface={:.0f} floor={:.0f} depth={:.1f}u < {:.0f}u)",
                    surfaceY, floorY, surfaceY - floorY, kSwimShoreExitDepth);
        return;
    }

    // Swim Y clamp. Field test reported NPC sitting 5u too high above
    // water; add a small extra drop on top of the per-age threshold
    // so head sits at a more natural level (slightly less of the body
    // above surface, matching Player's actual swim pose).
    constexpr float kSwimExtraDrop = 5.0f;
    const float swimDepth = SwimDepthFor(this_->linkAge) + kSwimExtraDrop;
    a->world.pos.y  = surfaceY - swimDepth;
    a->velocity.y   = 0.0f;  // reset so a future FOLLOW transition starts fresh

    // Yaw toward leader, move at swim pace. Slower than land speeds —
    // Link's swim caps around 3-4 u/frame. No IDLE substate for
    // SWIMMING — just tread water when close.
    const float dx = leaderPos.x - a->world.pos.x;
    const float dz = leaderPos.z - a->world.pos.z;
    const float distSq = dx*dx + dz*dz;
    if (distSq > kSwimEnterArrive * kSwimEnterArrive) {
        const s16 yaw = Math_Atan2S(dz, dx);
        a->shape.rot.y = yaw;
        a->world.rot.y = yaw;
        a->speedXZ     = kSwimSpeedMax;
        // Manual XZ motion. Math_SinS/CosS convert binary angle → unit
        // vector. Direct write to world.pos.xz mirrors how TickCLIMBING
        // handles XZ outside the standard physics pipeline.
        a->world.pos.x += Math_SinS(yaw) * a->speedXZ;
        a->world.pos.z += Math_CosS(yaw) * a->speedXZ;
    } else {
        a->speedXZ = 0.0f;  // tread water; swim_wait anim selected by AnimForState
    }
}

// ── Ledge-hoist (bundled swim-out + ground-mantle) ───────────────────

// Ledge-anchor lookup constants.
static constexpr float kHoistApproachMatchXZSq   = 60.0f * 60.0f;
static constexpr float kHoistSwimTriggerHeight   = 60.0f;   // leader >60u above water → search for swim-exit
static constexpr float kHoistGroundMaxLift       = 90.0f;   // accept hoists up to this Y delta

// Find the closest LedgeAnchor in this room where approachPos is
// near `nearPos` (XZ) AND topPos is meaningfully above `nearPos.y`.
// `wantHighEnoughForGround` filters out anchors where the lift is
// already-walkable (i.e. step heights handled by gravity, not a
// scripted hoist).
const ::AnchorNavRoom::LedgeAnchor* FindClosestLedgeAnchor(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& nearPos)
{
    if (navData == nullptr || navData->ledgeAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::LedgeAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& anc : navData->ledgeAnchors) {
        const float distSq = Dist2DSq(nearPos, anc.approachPos);
        if (distSq > kHoistApproachMatchXZSq) continue;
        const float lift = anc.topPos.y - nearPos.y;
        if (lift < 20.0f) continue;  // too low; standard step / nothing to hoist
        if (lift > kHoistGroundMaxLift) continue;  // too tall; not a v1 hoist
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = &anc;
        }
    }
    return best;
}

// Raycast fallback — detect a hoistable ledge ahead of NPC even when
// the room doesn't have a LedgeAnchor catalogued for this geometry.
// Probe:
//   1. Cast forward from NPC chest height toward leader direction.
//      A wall hit means there's geometry to hoist over.
//   2. From slightly past the wall hit (along leader direction), cast
//      down from kHoistGroundMaxLift above. The floor that intersects
//      is the ledge top.
//   3. Verify lift (top - NPC.y) is in the hoist range [20, 90].
//
// Returns true and writes outTopPos if a valid hoist target is found.
// chkHeight = how high above NPC.y to start the forward cast (catches
// short walls when small, tall walls when large; tunable).
bool RaycastDetectLedge(PlayState* play, const Vec3f& npcPos,
                        const Vec3f& leaderPos, Vec3f& outTopPos)
{
    const float dx = leaderPos.x - npcPos.x;
    const float dz = leaderPos.z - npcPos.z;
    const float distSq = dx*dx + dz*dz;
    if (distSq < 1.0f) return false;
    const float invDist = 1.0f / std::sqrt(distSq);
    const float dirX = dx * invDist;
    const float dirZ = dz * invDist;

    constexpr float kForwardCastDist = 80.0f;
    constexpr float kChestHeight     = 20.0f;
    constexpr float kPastWallNudge   = 8.0f;
    constexpr float kProbeMaxLift    = 90.0f;

    Vec3f rayA = { npcPos.x, npcPos.y + kChestHeight, npcPos.z };
    Vec3f rayB = { npcPos.x + dirX * kForwardCastDist, rayA.y,
                   npcPos.z + dirZ * kForwardCastDist };
    Vec3f wallHit;
    CollisionPoly* wallPoly = nullptr;
    if (!BgCheck_AnyLineTest1(&play->colCtx, &rayA, &rayB, &wallHit,
                              &wallPoly, 1)) {
        return false;
    }

    Vec3f downStart = {
        wallHit.x + dirX * kPastWallNudge,
        wallHit.y + kProbeMaxLift,
        wallHit.z + dirZ * kPastWallNudge
    };
    CollisionPoly* topPoly = nullptr;
    const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx, &topPoly,
                                                  &downStart);
    if (topPoly == nullptr) return false;
    const float lift = topY - npcPos.y;
    if (lift < 20.0f || lift > kProbeMaxLift) return false;

    outTopPos.x = downStart.x;
    outTopPos.y = topY;
    outTopPos.z = downStart.z;
    return true;
}

// LEDGE_HOIST handler — one-shot mantle. The anim runs once
// (ANIMMODE_ONCE via the kHoist* one-shot table in EnsureAnimation);
// stopAnimPlaying clears when LinkAnimation_Update reports the anim
// reached endFrame. At that point we snap to hoistTargetPos and
// return to FOLLOW.
void TickLEDGE_HOIST(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ     = 0.0f;
    a->shape.rot.y = this_->hoistEntryYaw;
    a->world.rot.y = this_->hoistEntryYaw;

    // Wait until the hoist anim has been (a) set up by EnsureAnimation
    // AND (b) played to completion. Without check (a), the entry tick
    // would exit immediately because the dispatcher runs TickLEDGE_HOIST
    // BEFORE EnsureAnimation in the per-tick order — stopAnimPlaying
    // is still 0 (stale from prior state) on entry.
    //
    // Field-test log 144 showed LEDGE_HOIST entering and exiting on
    // the same timestamp (no anim visible). The "teleport back to top
    // when leader runs off ledge" was the same bug: NPC walked off
    // snap target, GROUND-MANTLE trigger refired, the new LEDGE_HOIST
    // exited instantly with a snap-back to topPos.
    const bool hoistAnimSetUp =
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kHoistGround ||
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kHoistSwim;
    if (!hoistAnimSetUp || this_->stopAnimPlaying) {
        // Hold position. Anim isn't set up yet (entry tick) or is
        // currently playing. EnsureAnimation will fire later in the
        // dispatcher and transition the anim; subsequent ticks see
        // hoistAnimSetUp=true.
        return;
    }

    // Anim complete — snap to ledge top and exit.
    a->world.pos  = this_->hoistTargetPos;
    a->velocity.y = 0.0f;   // reset so gravity starts fresh from the snap
    sLocalNav.path.Reset();   // any pre-hoist path is now stale (NPC moved)
    sLocalNav.lastPathRefreshFrame = 0;
    sLocalNav.leashFrames     = 0;
    sLocalNav.closeFailFrames = 0;
    this_->state = EN_FOLLOWER_STATE_FOLLOW;
    (void)leaderPos;
    SPDLOG_INFO("[FollowerNPC] LEDGE_HOIST→FOLLOW (snapped to "
                "({:.0f},{:.0f},{:.0f}), context={})",
                this_->hoistTargetPos.x, this_->hoistTargetPos.y,
                this_->hoistTargetPos.z, (int)this_->hoistContext);
}

// DEAD handler — Phase 7 stub. v1 NPC is invulnerable, so this state
// is reserved-but-unentered; the handler exists to (a) lock the
// declaration in the dispatcher (we'd otherwise rely on the
// `default` branch) and (b) let the v2 combat redesign drop in
// real death-anim playback + post-death-timer logic without
// touching the dispatcher.
//
// v2 contract sketch (when combat lands):
//   - On entry: switch animation to a death anim; arm a death-anim
//     duration timer.
//   - Each frame: hold pos (no Actor_MoveXZGravity), let anim play.
//   - On timer expiry: SetFollowerNpcActive(false) — the
//     SetFollowerNpcActive(false) path will Actor_Kill +
//     broadcast DESPAWN(reason=died).
//
// v1 behaviour (here): just stop all movement and hold pose.
// Functionally equivalent to TickIDLE without the FOLLOW transition
// check (a dead NPC shouldn't aggro back to following on stand-up).
void TickDEAD(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)play;
    (void)leaderPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;
    // No state transitions in v1 — invulnerable NPC never reaches DEAD.
    // v2 will add the death-anim-complete → SetFollowerNpcActive(false)
    // dispatch here.
}

// Phase 8 — G10/G14 recovery teleport. Direct world.pos write to
// `dest`, reset path + stuck baselines + G-guard counters, force
// FOLLOW state, snap floor altitude. Caller logs the trigger.
void TeleportNpcTo(EnFollower* this_, PlayState* play, const Vec3f& dest) {
    Actor* a = &this_->actor;
    a->world.pos = dest;
    a->speedXZ   = 0.0f;
    // Reset all nav-state baselines so we don't immediately re-fire a
    // G-guard or STUCK detection at the new position.
    sLocalNav.path.Reset();
    sLocalNav.lastPathRefreshFrame = 0;
    sLocalNav.lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
    sLocalNav.stuckCheckPos        = dest;
    sLocalNav.lastStuckCheckFrame  = Anchor::Instance->gameFrameCounter.load(
                                          std::memory_order_relaxed);
    sLocalNav.leashFrames          = 0;
    sLocalNav.closeFailFrames      = 0;
    sLocalNav.closeFailBaseline    = 0.0f;
    sLocalNav.activeClimbAnchor    = nullptr;
    this_->state                   = EN_FOLLOWER_STATE_FOLLOW;
    // Snap Y to floor at the new position so we don't sink / float.
    Actor_UpdateBgCheckInfo(play, a, 26.0f, 10.0f, 50.0f, 4);
}

// Phase 8 — G10 leash safety net. NPC > kNpcLeashDistance from leader
// for > kNpcLeashTimeoutMs of consecutive ticks → teleport to leader
// pos. Catches "NPC stuck behind closed door / left in another room /
// fell into untracked geometry where the substrate can't recover."
//
// 3D distance (not XZ) — vertical separation also counts; an NPC
// stuck at the bottom of a pit beneath the leader should leash.
//
// Returns true if a teleport fired (caller should skip the rest of
// the tick — the state machine is in fresh-FOLLOW with no baselines).
bool TryFireG10(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    const float dx = a->world.pos.x - leaderPos.x;
    const float dy = a->world.pos.y - leaderPos.y;
    const float dz = a->world.pos.z - leaderPos.z;
    const float dist3D = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist3D <= kNpcLeashDistance) {
        sLocalNav.leashFrames = 0;
        return false;
    }
    sLocalNav.leashFrames++;
    const int timeoutTicks = Anchor::Instance->MsToGameTicks(kNpcLeashTimeoutMs);
    if (timeoutTicks <= 0 || (int)sLocalNav.leashFrames < timeoutTicks) {
        return false;
    }
    SPDLOG_INFO("[FollowerNPC] G10 leash teleport — dist3D={:.0f}u for {} frames "
                "(>{}u for >{}ms) → snap to leader",
                dist3D, sLocalNav.leashFrames,
                (int)kNpcLeashDistance, kNpcLeashTimeoutMs);
    TeleportNpcTo(this_, play, leaderPos);
    return true;
}

// Phase 8 — G14 close-fail safety net. NPC in the close-fail distance
// band (200-1200u, i.e. close enough that G10 won't fire) but making
// < kNpcCloseFailProgressDelta progress across kNpcCloseFailTimeoutMs
// → teleport to current substrate subgoal (or leader if no path).
// Catches "NPC stuck in tight geometry between rooms / pressed into
// wall the substrate can't see past."
//
// Progress metric: baseline = distance-to-leader at window entry;
// progress = baseline - current_distance. Reset counter whenever
// progress > kNpcCloseFailProgressDelta (NPC made real headway).
bool TryFireG14(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    const float dx = a->world.pos.x - leaderPos.x;
    const float dy = a->world.pos.y - leaderPos.y;
    const float dz = a->world.pos.z - leaderPos.z;
    const float dist3D = std::sqrt(dx*dx + dy*dy + dz*dz);

    // Outside the band — reset counter, defer to G10 (or normal AI).
    if (dist3D < kNpcCloseFailMinDistance || dist3D > kNpcCloseFailMaxDistance) {
        sLocalNav.closeFailFrames   = 0;
        sLocalNav.closeFailBaseline = 0.0f;
        return false;
    }

    // First tick inside the band → seed baseline.
    if (sLocalNav.closeFailFrames == 0) {
        sLocalNav.closeFailBaseline = dist3D;
        sLocalNav.closeFailFrames   = 1;
        return false;
    }

    // Made meaningful headway → reset window (baseline is updated to
    // current dist so we measure progress from here forward).
    const float progress = sLocalNav.closeFailBaseline - dist3D;
    if (progress > kNpcCloseFailProgressDelta) {
        sLocalNav.closeFailBaseline = dist3D;
        sLocalNav.closeFailFrames   = 1;
        return false;
    }

    sLocalNav.closeFailFrames++;
    const int timeoutTicks = Anchor::Instance->MsToGameTicks(kNpcCloseFailTimeoutMs);
    if (timeoutTicks <= 0 || (int)sLocalNav.closeFailFrames < timeoutTicks) {
        return false;
    }

    // Fire — teleport target is current substrate subgoal if we have
    // one (preserves substrate intent), else leader pos (G10-style
    // fallback).
    Vec3f dest = leaderPos;
    if (!sLocalNav.path.Empty()) {
        dest = sLocalNav.path.CurrentSubgoal();
    }
    SPDLOG_INFO("[FollowerNPC] G14 close-fail teleport — dist3D={:.0f}u, "
                "progress={:.1f}u over {} frames (<{}u in {}ms) → snap to "
                "({:.0f},{:.0f},{:.0f}) [{}]",
                dist3D, progress, sLocalNav.closeFailFrames,
                (int)kNpcCloseFailProgressDelta, kNpcCloseFailTimeoutMs,
                dest.x, dest.y, dest.z,
                sLocalNav.path.Empty() ? "leader pos (no path)" : "substrate subgoal");
    TeleportNpcTo(this_, play, dest);
    return true;
}

// STUCK handler — single-tick world.pos nudge toward leader, then
// return to FOLLOW. The substrate path was just reset by the FOLLOW
// caller; the next FOLLOW tick will recompute. Combined effect:
// "stuck → nudge forward 30u → recompute path → continue."
//
// Equivalent in spirit to player-Follower's STUCK-FWD action but
// simpler — no JumpResolver, no nav-snap. Phase 6+ may extend if
// field-test surfaces stuck patterns the simple nudge can't escape.
void TickSTUCK(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    const s16 yaw = YawTowardTarget(a->world.pos, leaderPos);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;
    a->speedXZ     = 0.0f;  // no momentum from STUCK; the nudge IS the motion

    // Direct world.pos write — the AI Follower's STUCK-FWD action is
    // also a direct write (player-rigged version uses
    // player->actor.world.pos = snappedPos). Skipping floor snapping
    // for v1; Actor_UpdateBgCheckInfo at the end of the tick will
    // re-clamp Y.
    const float dx = Math_SinS(yaw) * kStuckNudgeDist;
    const float dz = Math_CosS(yaw) * kStuckNudgeDist;
    a->world.pos.x += dx;
    a->world.pos.z += dz;

    // Reset stuck-check baseline so we don't immediately re-trigger.
    sLocalNav.stuckCheckPos       = a->world.pos;
    sLocalNav.lastStuckCheckFrame = Anchor::Instance->gameFrameCounter.load(
                                        std::memory_order_relaxed);

    // Resume FOLLOW. Path was reset by the FOLLOW caller; next tick
    // will recompute.
    this_->state = EN_FOLLOWER_STATE_FOLLOW;
    SPDLOG_INFO("[FollowerNPC] STUCK→FOLLOW (nudged {:.0f}u toward yaw={})",
                kStuckNudgeDist, (int)yaw);
}

}  // namespace

// ----------------------------------------------------------------------------
// extern "C" tick called from EnFollower_Update (z_en_follower.c).
// ----------------------------------------------------------------------------
extern "C" void Anchor_TickFollowerNpcActor(Actor* npc, PlayState* play) {
    if (npc == nullptr || play == nullptr) return;
    EnFollower* this_ = (EnFollower*)npc;

    // Peer replicas have their pos applied each frame from
    // FOLLOWER_NPC_STATE packets; they don't run AI. Skip the state
    // machine entirely — but DO drive animation from the synced state
    // field so peer NPCs visibly walk/run/climb instead of sliding
    // around in the wait pose.
    if (!IsLocalOwnerNPC(npc)) {
        FollowerNpcAnim peerAnim = AnimForState(this_->state, this_->syncedSpeedXZ);
        // Override for LEDGE_HOIST — mirror the dispatcher's
        // local-owner path so peer picks kHoistSwim/kHoistGround
        // from the synced hoistContext field.
        if (this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
            peerAnim = (this_->hoistContext == HOIST_CONTEXT_SWIM)
                         ? FollowerNpcAnim::kHoistSwim
                         : FollowerNpcAnim::kHoistGround;
        }
        EnsureAnimation(this_, play, peerAnim);
        // Per-frame playSpeed for walk/run (mirrors local-owner path —
        // fixed 1.0 matching Player_AnimChangeLoopMorph). See local
        // path for explanation of why the earlier velocity-scaled
        // formula was wrong.
        if (peerAnim == FollowerNpcAnim::kWalk || peerAnim == FollowerNpcAnim::kRun) {
            this_->skelAnime.playSpeed = 1.0f;
            // Footstep SFX so peer NPC isn't silent. speedXZ is the
            // synced value (broadcast by owner).
            this_->actor.speedXZ = this_->syncedSpeedXZ;  // for SFX pitch
            TickStepPhaseAndSfx(this_, play);
        }
        // Idle blend DISABLED 2026-05-16 (see local-owner path below).
        // constexpr bool kIdleBlendEnabled = false;
        // if (kIdleBlendEnabled && peerAnim == FollowerNpcAnim::kWait) {
        //     TickIdleBlend(this_, play);
        // }
        // Skip physics — STATE packet pos is authoritative and arrives
        // every ~100ms. If we ran Actor_MoveXZGravity here, gravity
        // (-2.0/frame) would accumulate velocity.y between packets,
        // dropping the peer below packet pos before each snap (visible
        // bobbing — worse at higher framerates where more frames pass
        // between packets). Owner clamps Y to floor before broadcasting,
        // so peer's pos is always on a valid floor.
        return;
    }

    // Need a leader. v1: leader is always the local player.
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        npc->speedXZ = 0.0f;
        Actor_MoveXZGravity(npc);
        return;
    }
    const Vec3f& leaderPos = player->actor.world.pos;

    // Persistent room handoff — set NPC's room to leader's room each
    // tick. Without this, after a within-scene room transition the
    // NPC's actor.room field still points to the old room and the
    // engine renders it there (or culls it). Tracking leader's room
    // makes the actor "tag along" — same instance, no despawn/respawn,
    // visible in whichever room the leader currently occupies.
    //
    // Reusable for AI Invader cross-room pursuit: same per-tick room
    // sync. For cross-SCENE pursuit, the scene transition still kills
    // the actor (engine-level), but the system-level state (CVar /
    // Invader-active flag) drives respawn via OnSceneSpawnActors.
    npc->room = player->actor.room;

    // G18 — cutscene suspension. When a cutscene is running, freeze
    // the NPC entirely (no AI tick, no animation update, no
    // locomotion). Same shape as the player-rigged AI Follower's G18
    // gate. Without this, the NPC can wander into cutscene framing
    // or trigger collision with cutscene-locked actors.
    //
    // csCtx.state values: CS_STATE_IDLE = 0; anything non-zero means
    // a cutscene is in some flavour of running / preparing / ending.
    // The "all non-zero = freeze" rule matches G18 in HookHandlers.cpp.
    if (gPlayState->csCtx.state != CS_STATE_IDLE) {
        npc->speedXZ = 0.0f;
        // Don't call Actor_MoveXZGravity either — gravity during a
        // cutscene can drop the NPC into a void if the cutscene
        // teleported the world out from under us.
        return;
    }

    // Leader-climbing force-engage. Fires BEFORE the G-guards and
    // dispatch so a successful engagement skips both. The substrate
    // pathfinder can't reliably route to a leader who is mid-climb
    // (FindNearestNode skips climb cells; cross-room nav not supported),
    // so we directly populate the path with the closest anchor's cells
    // when:
    //   - leader is in PLAYER_STATE1_CLIMBING_LADDER (vine / ladder),
    //   - NPC is not already CLIMBING,
    //   - NPC is within engagement distance of the anchor's basePos.
    //
    // Pattern adapted from AI Follower's autonomous-climb engagement at
    // Follower.cpp:1880-1940 (uses FindClimbAnchorAbove to detect leader's
    // anchor; we use FindClosestClimbAnchor as a fallback since
    // FindClimbAnchorAbove requires the leader's projection to be inside
    // the anchor's [0,cellsU)×[0,cellsV) grid bounds which may fail on
    // partial-grid anchors).
    if (Anchor::Instance->IsLocalPlayerClimbing() &&
        this_->state != EN_FOLLOWER_STATE_CLIMBING) {
        // Diagnostic throttle — log when force-engage WOULD-fire but
        // doesn't, so we can see which gate is preventing the
        // transition (e.g. swim→climb when leader grabs a vine wall
        // reachable from water). One log per ~2s to avoid spam.
        static uint64_t sLastDiagFrame = 0;
        const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                      std::memory_order_relaxed);
        const bool diagOK = (curFrame > sLastDiagFrame + 40);  // ~2s @ 20fps

        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        const ::AnchorNavRoom::ClimbAnchor* anchor =
            FindClosestClimbAnchor(navData, leaderPos);
        if (anchor != nullptr) {
            const float distBaseSq = Dist2DSq(npc->world.pos, anchor->basePos);
            if (distBaseSq < kClimbForceEngageBaseDistSq) {
                if (PopulateAnchorClimbPath(navData, *anchor,
                                            npc->world.pos, leaderPos,
                                            sLocalNav.path)) {
                    sLocalNav.activeClimbAnchor = anchor;
                    this_->state                = EN_FOLLOWER_STATE_CLIMBING;
                    SPDLOG_INFO("[FollowerNPC] Leader-climbing force-engage — anchor "
                                "base=({:.0f},{:.0f},{:.0f}) top=({:.0f},{:.0f},{:.0f}) "
                                "NPC at ({:.0f},{:.0f},{:.0f}) distBase={:.0f}u — "
                                "populated {} climb waypoints (from state={})",
                                anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                                anchor->topPos.x, anchor->topPos.y, anchor->topPos.z,
                                npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                                std::sqrt(distBaseSq),
                                (int)sLocalNav.path.waypoints.size(),
                                (int)this_->prevState);
                    sLocalNav.leashFrames     = 0;
                    sLocalNav.closeFailFrames = 0;
                } else if (diagOK) {
                    SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — anchor "
                                "found, distance OK ({:.0f}u), but path population "
                                "returned empty. NPC at ({:.0f},{:.0f},{:.0f}) state={} "
                                "anchor U-axis=({:.2f},{:.2f},{:.2f}) cells={}",
                                std::sqrt(distBaseSq),
                                npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                                (int)this_->state,
                                anchor->planeAxisU.x, anchor->planeAxisU.y, anchor->planeAxisU.z,
                                (int)anchor->nodeCount);
                    sLastDiagFrame = curFrame;
                }
            } else if (diagOK) {
                SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — anchor "
                            "found but NPC too far from base ({:.0f}u > {:.0f}u "
                            "threshold). NPC at ({:.0f},{:.0f},{:.0f}) "
                            "anchor.base=({:.0f},{:.0f},{:.0f}) state={}",
                            std::sqrt(distBaseSq),
                            std::sqrt(kClimbForceEngageBaseDistSq),
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                            (int)this_->state);
                sLastDiagFrame = curFrame;
            }
        } else if (diagOK) {
            SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — no "
                        "climb anchor in current room (sceneNum={} roomNum={}). "
                        "NPC at ({:.0f},{:.0f},{:.0f}) state={}",
                        gPlayState->sceneNum,
                        (int)gPlayState->roomCtx.curRoom.num,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        (int)this_->state);
            sLastDiagFrame = curFrame;
        }
    }

    // Phase 8 — G-guard safety nets. Run BEFORE state dispatch so a
    // teleport fully resets the state machine for the same tick (the
    // teleport drops us into FOLLOW with cleared path / baselines).
    // CLIMBING, SWIMMING, and LEDGE_HOIST are exempt — each is its
    // own scripted traversal and shouldn't be aborted by a
    // distance-based leash. Free-fall is also exempt — NPC walking
    // off a ledge after leader needs to land before G14 decides "no
    // progress" and teleports it back to the top. Detect free-fall
    // via downward velocity + not-on-floor (bgCheckFlags bit 1).
    const bool isFreeFalling = (npc->velocity.y < -5.0f) &&
                                !(npc->bgCheckFlags & 1);
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST &&
        !isFreeFalling) {
        if (TryFireG10(this_, play, leaderPos)) {
            // Teleport fired — skip rest of tick. NPC is now at leader
            // in fresh FOLLOW; next tick picks up normally.
            return;
        }
        if (TryFireG14(this_, play, leaderPos)) {
            return;
        }
    }

    // Water-entry detection — if NPC's submersion depth exceeds Link's
    // swim threshold (adult ageProperties.unk_24 = 36.0 at z_player.c:453),
    // transition to SWIMMING. CLIMBING / LEDGE_HOIST exempt — those
    // are their own scripted traversals.
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST) {
        const float swimEntryDepth = SwimDepthFor(this_->linkAge);
        if (npc->yDistToWater > swimEntryDepth) {
            this_->state = EN_FOLLOWER_STATE_SWIMMING;
            sLocalNav.path.Reset();  // discard land path; swim handler
                                     // navigates direct-to-leader.
            SPDLOG_INFO("[FollowerNPC] FOLLOW/IDLE→SWIMMING "
                        "(yDistToWater={:.1f}u > {:.0f}u threshold for "
                        "linkAge={})",
                        npc->yDistToWater, swimEntryDepth,
                        (int)this_->linkAge);
        }
    }

    // Ledge-hoist entry triggers. Two contexts:
    //   SWIM_EXIT  — from SWIMMING when leader is meaningfully above
    //                NPC's water surface AND a LedgeAnchor's approachPos
    //                is near NPC's swim pos. NPC plays swim-step-up
    //                anim then snaps to topPos.
    //   GROUND     — from FOLLOW when a LedgeAnchor's approachPos is
    //                near NPC's land pos AND topPos lifts in the
    //                hoist range. NPC plays mantle anim then snaps.
    //
    // Both are autonomous (no need to mirror leader's exact hoist
    // moment) — fire whenever geometry supports the lift and the
    // NPC's current state would otherwise leave it stranded below
    // the leader.
    // Helper to enter LEDGE_HOIST with a given target pos.
    auto enterLedgeHoist = [&](HoistContext ctx, const Vec3f& topPos,
                                const char* source) {
        this_->hoistContext   = (s8)ctx;
        this_->hoistTargetPos = topPos;
        this_->hoistEntryYaw  =
            Math_Atan2S(topPos.z - npc->world.pos.z,
                        topPos.x - npc->world.pos.x);
        // Swim-exit hoist: raise NPC ~60u so the swim-step-up anim
        // plays at the ledge level instead of underwater. User
        // reported NPC sinking below water during the anim. The
        // raise puts NPC's feet near the ledge top, anim visualizes
        // the climb-out motion correctly. End-of-anim snap moves NPC
        // to the exact ledge top.
        if (ctx == HOIST_CONTEXT_SWIM) {
            constexpr float kSwimHoistRaise = 43.0f;  // tuned 60u → 50u → 45u → 43u over field tests
            npc->world.pos.y += kSwimHoistRaise;
            npc->velocity.y = 0.0f;
        }
        this_->state = EN_FOLLOWER_STATE_LEDGE_HOIST;
        SPDLOG_INFO("[FollowerNPC] {}→LEDGE_HOIST({}) top=({:.0f},{:.0f},{:.0f}) "
                    "NPC at ({:.0f},{:.0f},{:.0f}) via {}",
                    (ctx == HOIST_CONTEXT_SWIM ? "SWIMMING" : "FOLLOW"),
                    (ctx == HOIST_CONTEXT_SWIM ? "swim_exit" : "ground"),
                    topPos.x, topPos.y, topPos.z,
                    npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                    source);
    };

    if (this_->state == EN_FOLLOWER_STATE_SWIMMING) {
        // Autonomous swim-out detection — independent of leader's pos
        // (per user feedback: "Do not rely on the position of the
        // leader to determine if the NPC is in water"). Raycast from
        // 5u above NPC's head straight down. If a floor poly is hit
        // ABOVE NPC's current Y within hoist range, NPC is positioned
        // under a walkable ledge / dock / shore — trigger hoist.
        //
        // BgCheck_EntityRaycastFloor1 only returns floor-normal polys
        // (upward-facing), so cave ceilings / bridge undersides don't
        // false-trigger. Low bridges where NPC's head fits underneath
        // could trigger a premature hoist — edge case accepted in v1.
        //
        // NPC head is roughly at world.pos.y + 50 (Link body height).
        // Probe start: head + 5 = pos.y + 55.
        constexpr float kSwimHoistProbeStartY  = 55.0f;
        constexpr float kSwimHoistMinLift      = 20.0f;
        constexpr float kSwimHoistMaxLift      = 90.0f;
        Vec3f probeStart = { npc->world.pos.x,
                             npc->world.pos.y + kSwimHoistProbeStartY,
                             npc->world.pos.z };
        CollisionPoly* topPoly = nullptr;
        const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx,
                                                      &topPoly, &probeStart);
        if (topPoly != nullptr) {
            const float lift = topY - npc->world.pos.y;
            if (lift > kSwimHoistMinLift && lift < kSwimHoistMaxLift) {
                Vec3f topPos = { probeStart.x, topY, probeStart.z };
                enterLedgeHoist(HOIST_CONTEXT_SWIM, topPos, "head-up-probe");
            }
        }
    } else if (this_->state == EN_FOLLOWER_STATE_FOLLOW &&
               leaderPos.y > npc->world.pos.y + 30.0f) {
        // Ground hoist — leader is on a ledge above NPC. LedgeAnchor
        // first, raycast fallback second (covers walls not catalogued).
        // Fires only when state=FOLLOW because IDLE/STUCK shouldn't
        // auto-hoist (NPC isn't actively pursuing).
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        const auto* ledge = FindClosestLedgeAnchor(navData, npc->world.pos);
        Vec3f targetTop;
        if (ledge != nullptr) {
            enterLedgeHoist(HOIST_CONTEXT_GROUND, ledge->topPos, "LedgeAnchor");
        } else if (RaycastDetectLedge(play, npc->world.pos, leaderPos, targetTop)) {
            enterLedgeHoist(HOIST_CONTEXT_GROUND, targetTop, "raycast");
        }
    }

    // Dispatch.
    switch (this_->state) {
        default:
        case EN_FOLLOWER_STATE_IDLE:        TickIDLE(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_FOLLOW:      TickFOLLOW(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_STUCK:       TickSTUCK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_CLIMBING:    TickCLIMBING(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_SWIMMING:    TickSWIMMING(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_LEDGE_HOIST: TickLEDGE_HOIST(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_DEAD:        TickDEAD(this_, play, leaderPos); break;
    }

    // Clear stop-anim latch when the ONCE anim has reached endFrame.
    // LinkAnimation_Once clamps curFrame to endFrame on completion
    // (z_skelanime.c:1237-1238), so once curFrame == endFrame the
    // stop anim is done and we can swap back to wait.
    if (this_->stopAnimPlaying &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        this_->stopAnimPlaying = 0;
    }

    // Head-look-at-leader. Disabled during CLIMBING + LEDGE_HOIST —
    // head rotation looks visually wrong in those poses (climb anim
    // has Link facing the wall; head turning sideways to track leader
    // produces unnatural angles). Also let head settle to neutral
    // during these phases (Math_ScaledStepToS toward 0 instead of the
    // leader-relative target). LEDGE_HOIST is similarly anim-locked.
    if (this_->state == EN_FOLLOWER_STATE_CLIMBING ||
        this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
        Math_ScaledStepToS(&this_->headLimbRot.y,  0, 0x600);
        Math_ScaledStepToS(&this_->headLimbRot.x,  0, 0x600);
        Math_ScaledStepToS(&this_->upperLimbRot.y, 0, 0x600);
    } else {
        TickHeadLookAtLeader(this_, leaderPos);
    }

    // Animation. Run AFTER dispatch so any state transitions made by
    // the handler are reflected this same tick (e.g. FOLLOW → STUCK
    // immediately switches to wait anim, not next-tick).
    //
    // FOLLOW→IDLE transition gets a one-shot stop anim (walk_endL/R)
    // chosen by current step phase — eliminates the "freeze mid-stride"
    // pop when NPC arrives at leader. Mirrors Player's func_8083BF50
    // (z_player.c:6388).
    // Sync modelAnimType from local Player so EnsureAnimation picks
    // armed (fighter) vs unarmed (_free) anim variants correctly.
    // Player updates modelAnimType in Player_SetModelGroup
    // (z_player_lib.c:655) whenever the held item / shield state
    // changes (e.g. sword drawn/sheathed). Without this, the NPC
    // always uses _free anims even while visually wielding sword+
    // shield (because we inherit Player's equipment-draw via the
    // override callback).
    this_->currentAnimType = (s8)player->modelAnimType;

    FollowerNpcAnim localAnim = AnimForState(this_->state, npc->speedXZ);
    // Ledge-hoist anim variant — AnimForState defaults to kHoistGround,
    // override here based on hoistContext (the dispatcher has access
    // to this_, AnimForState doesn't).
    if (this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
        localAnim = (this_->hoistContext == HOIST_CONTEXT_SWIM)
                      ? FollowerNpcAnim::kHoistSwim
                      : FollowerNpcAnim::kHoistGround;
    }

    // Auto-jump-off-ledge — mimic Player's z_player.c:5818-5836.
    // bgCheckFlags & 4 is set by Actor_UpdateBgCheckInfo's
    // func_8002E234 (z_actor.c:1604) when actor was on floor last
    // frame but the floor dropped >11u (walked off a ledge with
    // a meaningful drop). Player triggers an auto-jump when this
    // hits AND linearVelocity > 3 AND facing forward.
    //
    // For our NPC: same gate plus state must be FOLLOW (don't jump
    // during CLIMBING / SWIMMING / LEDGE_HOIST etc.). Pick run_jump
    // anim when fast, regular jump when slow (mirrors func_8083A4A8).
    // Set velocity.y = positive boost so NPC arcs up (gravity then
    // pulls down — natural jump arc).
    {
        const bool isOnFloor      = (npc->bgCheckFlags & 1) != 0;
        const bool walkedOffEdge  = (npc->bgCheckFlags & 4) != 0;
        const uint64_t curFrame   = Anchor::Instance->gameFrameCounter.load(
                                        std::memory_order_relaxed);

        // Diagnostic — log every gate evaluation at low rate so we can
        // see WHY the jump didn't fire. Throttled to once every 30
        // frames (~1.5s at 20fps) to limit spam, only when in FOLLOW
        // and yDistToFloor is meaningful (suggesting the NPC is near
        // an edge or in the air).
        static uint64_t sLastGateDiag = 0;
        if (this_->state == EN_FOLLOWER_STATE_FOLLOW &&
            curFrame > sLastGateDiag + 30 &&
            (walkedOffEdge || npc->yDistToWater > 1.0f ||
             std::fabs(npc->velocity.y) > 1.0f)) {
            SPDLOG_INFO("[FollowerNPC.jump] gate eval: state=FOLLOW "
                        "bgCheckFlags=0x{:X} (onFloor={} walkedOffEdge={}) "
                        "speedXZ={:.2f} velocity.y={:.2f} pos=({:.0f},{:.0f},{:.0f}) "
                        "shape.rot.y=0x{:X} world.rot.y=0x{:X}",
                        npc->bgCheckFlags, isOnFloor, walkedOffEdge,
                        npc->speedXZ, npc->velocity.y,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        (uint16_t)npc->shape.rot.y, (uint16_t)npc->world.rot.y);
            sLastGateDiag = curFrame;
        }

        // Trigger auto-jump.
        if (walkedOffEdge && npc->speedXZ > 3.0f &&
            this_->state == EN_FOLLOWER_STATE_FOLLOW) {
            // Boost matches Player's typical auto-jump value (run_jump
            // peak ≈ 10 from func_8083A4A8 with default IREG(67)).
            // Earlier value of 6.0 produced only +9u peak rise against
            // our -2.0 gravity; bump to 10.0 + lower gravity to match
            // Player's airborne behavior at z_player.c:9670.
            constexpr float kJumpBoostVy = 10.0f;
            constexpr float kJumpGravity = -1.2f;  // Player_Action_8084411C
            npc->velocity.y = kJumpBoostVy;
            npc->gravity    = kJumpGravity;        // restored on landing
            npc->bgCheckFlags &= ~4;
            localAnim = (npc->speedXZ > 4.0f) ? FollowerNpcAnim::kRunJump
                                              : FollowerNpcAnim::kJump;
            // Capture jump diagnostics
            sLocalNav.jumpInProgress     = true;
            sLocalNav.jumpStartPos       = npc->world.pos;
            sLocalNav.jumpPeakPos        = npc->world.pos;
            sLocalNav.jumpStartYaw       = npc->shape.rot.y;
            sLocalNav.jumpStartSpeedXZ   = npc->speedXZ;
            sLocalNav.jumpStartVelocityY = kJumpBoostVy;
            sLocalNav.jumpStartFrame     = curFrame;
            sLocalNav.jumpLastDiagFrame  = curFrame;
            SPDLOG_INFO("[FollowerNPC.jump] FIRE — anim={} speedXZ={:.2f} "
                        "yaw=0x{:X} startPos=({:.0f},{:.0f},{:.0f}) "
                        "boostVel.y={:.2f} gravity={:.2f}",
                        (localAnim == FollowerNpcAnim::kRunJump ? "run_jump" : "jump"),
                        npc->speedXZ, (uint16_t)npc->shape.rot.y,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        npc->velocity.y, npc->gravity);
        }

        // Per-frame airborne tracking — log every ~6 frames (~0.3s)
        // while jumpInProgress so we can see velocity / pos / peak
        // evolving. Track peak Y for the summary.
        if (sLocalNav.jumpInProgress) {
            // Stuck-detection: if airborne >5s with near-zero
            // velocity.y and not on floor, NPC is in geometry-state
            // limbo (e.g. fell into water that doesn't register
            // yDistToWater, or onto a void floor that doesn't set
            // bgCheckFlags & 1). Force-teleport to leader to recover.
            const uint64_t airborneFrames = curFrame - sLocalNav.jumpStartFrame;
            if (airborneFrames > 100 &&  // ~5s @ 20fps
                std::fabs(npc->velocity.y) < 1.0f && !isOnFloor) {
                SPDLOG_WARN("[FollowerNPC.jump] STUCK in air for {} frames "
                            "(velocity.y={:.2f}, pos=({:.0f},{:.0f},{:.0f})) "
                            "— force-teleport to leader",
                            (int)airborneFrames, npc->velocity.y,
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z);
                TeleportNpcTo(this_, play, leaderPos);
                npc->gravity              = -2.0f;  // restore default
                sLocalNav.jumpInProgress  = false;
                return;
            }

            if (npc->world.pos.y > sLocalNav.jumpPeakPos.y) {
                sLocalNav.jumpPeakPos = npc->world.pos;
            }
            if (curFrame > sLocalNav.jumpLastDiagFrame + 6) {
                const float dxFromStart = npc->world.pos.x - sLocalNav.jumpStartPos.x;
                const float dyFromStart = npc->world.pos.y - sLocalNav.jumpStartPos.y;
                const float dzFromStart = npc->world.pos.z - sLocalNav.jumpStartPos.z;
                const float distXZ = std::sqrt(dxFromStart*dxFromStart +
                                                dzFromStart*dzFromStart);
                SPDLOG_INFO("[FollowerNPC.jump] airborne tick {}: "
                            "pos=({:.0f},{:.0f},{:.0f}) velocity.y={:.2f} "
                            "speedXZ={:.2f} dY={:+.1f} distXZ={:.1f} "
                            "peakY={:.0f} (peakΔY={:+.1f}) onFloor={}",
                            (int)(curFrame - sLocalNav.jumpStartFrame),
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            npc->velocity.y, npc->speedXZ,
                            dyFromStart, distXZ,
                            sLocalNav.jumpPeakPos.y,
                            sLocalNav.jumpPeakPos.y - sLocalNav.jumpStartPos.y,
                            isOnFloor);
                sLocalNav.jumpLastDiagFrame = curFrame;
            }
            // Landing detection — bgCheckFlags & 1 set means NPC
            // touched a floor. Emit summary log + close jump tracking.
            if (isOnFloor && !sLocalNav.jumpWasOnFloorPrevTick) {
                const float dxFromStart = npc->world.pos.x - sLocalNav.jumpStartPos.x;
                const float dyFromStart = npc->world.pos.y - sLocalNav.jumpStartPos.y;
                const float dzFromStart = npc->world.pos.z - sLocalNav.jumpStartPos.z;
                const float distXZ = std::sqrt(dxFromStart*dxFromStart +
                                                dzFromStart*dzFromStart);
                const float peakRise = sLocalNav.jumpPeakPos.y -
                                        sLocalNav.jumpStartPos.y;
                SPDLOG_INFO("[FollowerNPC.jump] LAND — totalFrames={} "
                            "startPos=({:.0f},{:.0f},{:.0f}) "
                            "landPos=({:.0f},{:.0f},{:.0f}) "
                            "peakY={:.0f} (rise={:+.1f}) drop={:+.1f}u "
                            "horizontalDist={:.1f}u final velocity.y={:.2f}",
                            (int)(curFrame - sLocalNav.jumpStartFrame),
                            sLocalNav.jumpStartPos.x, sLocalNav.jumpStartPos.y,
                            sLocalNav.jumpStartPos.z,
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            sLocalNav.jumpPeakPos.y, peakRise,
                            dyFromStart, distXZ, npc->velocity.y);
                npc->gravity = -2.0f;  // restore default ground gravity
                sLocalNav.jumpInProgress = false;
            }
        }
        sLocalNav.jumpWasOnFloorPrevTick = isOnFloor;
    }
    const bool justStoppedMoving =
        (this_->prevState == EN_FOLLOWER_STATE_FOLLOW &&
         this_->state    == EN_FOLLOWER_STATE_IDLE);
    if (justStoppedMoving) {
        localAnim = (this_->stepPhase < kStopPhaseLRSplit)
            ? FollowerNpcAnim::kStopL
            : FollowerNpcAnim::kStopR;
    }

    // Idle fidget rotation. After sustained kWait, swap to a fidget
    // anim (look-around / warm / stretch) cycling through three
    // variants. Adds the look-around variety Link has in his idle.
    // Counter only advances while actually playing kWait (not while
    // stop-anim or another fidget is in progress).
    static constexpr u32 kFidgetIntervalTicks = 120;  // ~6s at 20fps
    if (localAnim == FollowerNpcAnim::kWait && !this_->stopAnimPlaying &&
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kWait) {
        this_->idleTicks++;
        if (this_->idleTicks >= kFidgetIntervalTicks) {
            this_->idleTicks = 0;
            switch (this_->nextFidgetIdx % 3) {
                case 0: localAnim = FollowerNpcAnim::kFidgetLookA;    break;
                case 1: localAnim = FollowerNpcAnim::kFidgetWarmB;    break;
                case 2: localAnim = FollowerNpcAnim::kFidgetStretchD; break;
            }
            this_->nextFidgetIdx++;
        }
    } else if (localAnim != FollowerNpcAnim::kWait) {
        // Out of idle — reset counter so next idle session starts fresh.
        this_->idleTicks = 0;
    }

    // Keep playing the stop anim / fidget until it finishes
    // (LinkAnimation_Update returns true). EnsureAnimation guard on
    // `stopAnimPlaying` prevents mid-anim overrides; once stopAnimPlaying
    // clears, fall through to the normal anim choice (which is kWait
    // for fidgets, so AnimForState swaps us back to the breathing
    // idle automatically).
    if (this_->stopAnimPlaying) {
        localAnim = (FollowerNpcAnim)this_->currentAnim;  // hold current
    }

    // Airborne anim hold — while jumpInProgress, keep the jump anim
    // as the active selection regardless of whether the one-shot has
    // ended. Mirrors Player_Action_8084411C (z_player.c:9663) which
    // doesn't transition anim during fall — Link stays in the jump
    // pose until landing. Without this, our NPC's kRunJump one-shot
    // ends mid-fall, AnimForState returns kWalk/kRun/kWait based on
    // speedXZ, and NPC visibly switches to a walking pose while
    // still in the air. Only released when jumpInProgress clears
    // (on landing or stuck-teleport).
    if (sLocalNav.jumpInProgress) {
        localAnim = (FollowerNpcAnim)this_->currentAnim;
    }
    EnsureAnimation(this_, play, localAnim);

    // Per-frame playSpeed for walk/run. Player's run anim is set via
    // Player_AnimChangeLoopMorph at z_player.c:6634 with playSpeed=1.0
    // — fixed cadence regardless of motion speed. Earlier iteration
    // here used `speedXZ * 0.3 + 1.0` which is actually Player's
    // STEP-COUNTER formula (z_player.c:8170, advances unk_868), NOT
    // the anim playSpeed. That produced ~3× too-fast anim cadence
    // (user observed "twice as fast" — close enough).
    //
    // Match Player: playSpeed = 1.0 for both walk and run. Anim
    // cycle frames per game tick = 1.0 * R_UPDATE_RATE * 0.5 = 1.5,
    // a natural cadence that doesn't drift with motion. Step counter
    // still scales with velocity (handled by TickStepPhaseAndSfx
    // using the actual step-counter formula).
    if (localAnim == FollowerNpcAnim::kWalk || localAnim == FollowerNpcAnim::kRun) {
        this_->skelAnime.playSpeed = 1.0f;
        // Tick step phase + emit footstep SFX on foot-down crossings.
        TickStepPhaseAndSfx(this_, play);
    }

    // Idle blend — DISABLED 2026-05-16 (user reported model collapse +
    // distortion + position jumping while in idle). Suspected causes
    // (any combination):
    //   - LinkAnimation_BlendToJoint queues entries that race with
    //     LinkAnimation_Update's queue entries for the same jointTable,
    //     producing torn / partial joint data when the queue processes.
    //   - Blend table alignment overflow: LinkAnimation_BlendToJoint
    //     ALIGN16's the buffer pointer (up to +15 bytes skew), and
    //     PLAYER_LIMB_BUF_COUNT (24 entries = 144 bytes) provides only
    //     12 bytes of headroom over the 132 bytes of data written —
    //     a 2-byte short in worst-case alignment, stomping adjacent
    //     struct members (headLimbRot / upperLimbRot follow blendTable
    //     in EnFollower).
    //   - waitL/waitR anim lengths may differ from wait_free; passing
    //     skelAnime.curFrame (driven by wait_free's animLength) could
    //     read beyond waitL/R bounds.
    //
    // Re-enabling: flip kIdleBlendEnabled to true AND fix the underlying
    // issue. Likely path: skip LinkAnimation_Update for wait state and
    // let BlendToJoint be the only joint-table writer (Player's
    // pattern — Player doesn't call LinkAnimation_Update when using
    // BlendToJoint for idle, see z_player.c:8061-8064). NPC will fall
    // back to wait_free + LinkAnimation_Update (the prior working
    // path) while this is disabled.
    constexpr bool kIdleBlendEnabled = false;
    if (kIdleBlendEnabled &&
        localAnim == FollowerNpcAnim::kWait && !this_->stopAnimPlaying) {
        TickIdleBlend(this_, play);
    }

    this_->prevState = this_->state;

    // Sync speed for peers — they read syncedSpeedXZ to choose walk
    // vs run anim. (For peers, this is overwritten by the STATE
    // packet handler; for local owner, this is the source of truth.)
    this_->syncedSpeedXZ = npc->speedXZ;

    // Apply locomotion. Standard OoT NPC pattern — speedXZ + world.rot.y
    // give a per-frame velocity vector; gravity pulls Y to floor.
    // CLIMBING bypasses this (climb handler writes world.pos directly,
    // gravity would yank NPC off the wall). SWIMMING also bypasses
    // (swim handler clamps Y to water surface; gravity would drag
    // NPC underwater each frame, fighting the surface clamp).
    // LEDGE_HOIST bypasses too — handler locks pos during the anim
    // and snaps to topPos on completion; gravity would drop NPC
    // below the ledge during the lock-pose phase.
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST) {
        Actor_MoveXZGravity(npc);

        // Update collision-with-ground / floor altitude. Without this
        // the NPC's Y can drift away from the floor on slopes / steps.
        // Flags=4 matches the standard NPC pattern (e.g. z_en_md.c:889).
        Actor_UpdateBgCheckInfo(play, npc, 26.0f /* wallCheckHeight */,
                                10.0f /* wallCheckRadius */,
                                50.0f /* ceilingCheckHeight */,
                                4 /* flags */);
    }
}
