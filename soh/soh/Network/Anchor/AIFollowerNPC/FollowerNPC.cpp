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
        want == FollowerNpcAnim::kFidgetStretchD;
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

    // Apportion yaw: head takes up to ±0x4000, upper twists for the rest.
    s16 headYawTarget  = yawRel;
    s16 upperYawTarget = 0;
    if (headYawTarget >  0x4000) { upperYawTarget = headYawTarget - 0x4000; headYawTarget =  0x4000; }
    if (headYawTarget < -0x4000) { upperYawTarget = headYawTarget + 0x4000; headYawTarget = -0x4000; }
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
    const float npcU = (npcPos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
                       (npcPos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;

    struct Entry { float y; uint16_t idx; };
    std::vector<Entry> column;
    // Tight column filter — only cells within ±15u of NPC's U (≈ half
    // a 30u cell pitch). Earlier ±40u tolerance picked 2-3 adjacent
    // columns; the path's cursor would advance through cells in
    // different columns, snapping NPC's XZ laterally by ~30-100u per
    // tick (visible "rapid position shifts" on curved walls like
    // Inside Deku Tree's spiral vine wall). Single column produces
    // a straight-up climb with no lateral hop.
    for (uint16_t i = 0; i < anchor.nodeCount; i++) {
        const uint16_t idx = anchor.firstNodeIdx + i;
        if (idx >= navData->nodes.size()) break;
        const auto& n = navData->nodes[idx];
        const float nodeU =
            (n.pos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
            (n.pos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;
        if (std::fabs(nodeU - npcU) > 15.0f) continue;     // single column only
        if (n.pos.y > leaderPos.y + 50.0f) continue;       // past leader
        column.push_back({n.pos.y, idx});
    }
    if (column.empty()) return false;

    // Sort by Y ascending. NPC starts at bottom; CLIMBING handler
    // advances cursor as it climbs.
    std::sort(column.begin(), column.end(),
              [](const Entry& a, const Entry& b){ return a.y < b.y; });

    // Skip cells already below NPC's current Y (NPC is partway up
    // the wall — don't backtrack). Includes a small downward slack
    // so the first cell sits ~10u below NPC if needed for the climb
    // anim to look right.
    for (const auto& e : column) {
        if (e.y < npcPos.y - 30.0f) continue;
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
        // Path exhausted mid-climb — fall back to FOLLOW (which
        // refreshes the path).
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        sLocalNav.activeClimbAnchor = nullptr;
        return;
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

    // Snap XZ to subgoal + offset along planeNormal so body sits in
    // FRONT of the wall (not buried). The subgoal IS on the wall
    // surface; adding planeNormal * kClimbBodyOffset moves us out
    // along the wall's outward-facing normal.
    a->world.pos.x = subgoal.x + anc.planeNormal.x * kClimbBodyOffset;
    a->world.pos.z = subgoal.z + anc.planeNormal.z * kClimbBodyOffset;

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
                                "populated {} climb waypoints",
                                anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                                anchor->topPos.x, anchor->topPos.y, anchor->topPos.z,
                                npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                                std::sqrt(distBaseSq),
                                (int)sLocalNav.path.waypoints.size());
                    // Fall through to dispatch — TickCLIMBING handles
                    // the rest. Reset G-guard counters since we're
                    // about to climb (not "stuck").
                    sLocalNav.leashFrames     = 0;
                    sLocalNav.closeFailFrames = 0;
                }
            }
        }
    }

    // Phase 8 — G-guard safety nets. Run BEFORE state dispatch so a
    // teleport fully resets the state machine for the same tick (the
    // teleport drops us into FOLLOW with cleared path / baselines).
    // CLIMBING is exempt — a vertical-traversal-in-progress shouldn't
    // be aborted by a far-from-leader leash; the climb might be the
    // path to the leader (e.g. leader is on a ledge above).
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING) {
        if (TryFireG10(this_, play, leaderPos)) {
            // Teleport fired — skip rest of tick. NPC is now at leader
            // in fresh FOLLOW; next tick picks up normally.
            return;
        }
        if (TryFireG14(this_, play, leaderPos)) {
            return;
        }
    }

    // Dispatch.
    switch (this_->state) {
        default:
        case EN_FOLLOWER_STATE_IDLE:     TickIDLE(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_FOLLOW:   TickFOLLOW(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_STUCK:    TickSTUCK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_CLIMBING: TickCLIMBING(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_DEAD:     TickDEAD(this_, play, leaderPos); break;
    }

    // Clear stop-anim latch when the ONCE anim has reached endFrame.
    // LinkAnimation_Once clamps curFrame to endFrame on completion
    // (z_skelanime.c:1237-1238), so once curFrame == endFrame the
    // stop anim is done and we can swap back to wait.
    if (this_->stopAnimPlaying &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        this_->stopAnimPlaying = 0;
    }

    // Head-look-at-leader. Re-enabled after isolating the idle-animation
    // bug to TickIdleBlend (now disabled separately). Save/swap/restore
    // of localPlayer's headLimbRot/upperLimbRot happens in EnFollower_Draw.
    TickHeadLookAtLeader(this_, leaderPos);

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
    // CLIMBING bypasses this (we directly write world.pos and gravity
    // would yank us off the wall).
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING) {
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
