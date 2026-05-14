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
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <cmath>

extern "C" {
#include "variables.h"        // gPlayState, gEnFollowerId, gSaveContext
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "src/overlays/actors/ovl_En_Follower/z_en_follower.h"  // EnFollower struct + state enum
extern PlayState* gPlayState;
extern s16        gEnFollowerId;
}

// ----------------------------------------------------------------------------
// CVar transition polling — called once per OnGameFrameUpdate tick.
// ----------------------------------------------------------------------------
void Anchor::TickFollowerNpcCVar() {
    // Stale-pointer cleanup: scene transitions wipe the actor list
    // (Actor_Kill chain by OoT scene-loader). Our tracking pointer
    // survives but the actor is dead. Detect via update==NULL and
    // clear the pointer so the next CVar poll respawns the NPC.
    // (Or, if the user has the CVar off, no respawn — correct.)
    if (mFollowerNpcLocalActor != nullptr &&
        mFollowerNpcLocalActor->update == nullptr) {
        mFollowerNpcLocalActor = nullptr;
    }

    const int cur = CVarGetInteger(CVAR_ENHANCEMENT("AI.FollowerNPC.Enabled"), 0);

    // Auto-respawn after scene transition: if the CVar is on but the
    // pointer is null (just cleared above), spawn now. This is NOT a
    // CVar edge — handle it before the edge check.
    if (cur != 0 && mFollowerNpcLocalActor == nullptr) {
        SetFollowerNpcActive(true);
        // Fall through to edge check; mFollowerNpcCVarLast may still
        // need updating if this was also the first tick.
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

namespace {

// True iff `npc` is THIS client's local NPC (not a peer replica). Peer
// replicas have their pos driven by FOLLOWER_NPC_STATE packets and
// should not run the AI tick.
bool IsLocalOwnerNPC(Actor* npc) {
    return Anchor::Instance != nullptr &&
           npc == Anchor::Instance->GetFollowerNpcLocalActor();
}

// Per-NPC nav state. v1 = one local NPC per client, so a single static
// instance suffices (no per-actor map). Reset on owner-side spawn.
struct LocalNpcNavState {
    AnchorNav::ActorTrail::NavPath path;
    uint64_t lastPathRefreshFrame = 0;
    Vec3f    lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
    Vec3f    stuckCheckPos        = { 0.0f, 0.0f, 0.0f };
    uint64_t lastStuckCheckFrame  = 0;
};
static LocalNpcNavState sLocalNav;

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

// IDLE handler — stand still, face leader, transition to FOLLOW on
// distance exceed.
void TickIDLE(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Face leader so the NPC looks like it's tracking us even while idle.
    a->shape.rot.y = YawTowardTarget(a->world.pos, leaderPos);

    // Transition check: hysteresis upper bound.
    const float distSq = Dist2DSq(a->world.pos, leaderPos);
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

    // ---- Path refresh ------------------------------------------------
    const uint64_t curFrame   = Anchor::Instance->gameFrameCounter.load(
                                    std::memory_order_relaxed);
    const int      refreshTicks = Anchor::Instance->MsToGameTicks(kPathRefreshMs);
    const bool needRefresh =
        sLocalNav.path.Empty() ||
        (refreshTicks > 0 &&
         curFrame >= sLocalNav.lastPathRefreshFrame + (uint64_t)refreshTicks) ||
        AnchorDist::DistXZSq(leaderPos, sLocalNav.lastPathTargetPos) >
            kPathRetargetDist * kPathRetargetDist;
    if (needRefresh) {
        AnchorNav::TrailKey leaderKey =
            AnchorNav::TrailKeyForPlayer((uint8_t)Anchor::Instance->ownClientId);
        sLocalNav.path.Reset();
        AnchorNav::ActorTrail::GetInstance().ComputePathTo(
            leaderKey, a, leaderPos, play, sLocalNav.path,
            /*skipLayer1LOS=*/false,
            /*preferLeaderTrail=*/true);  // leader's breadcrumbs are the natural pursuit hint
        sLocalNav.lastPathRefreshFrame = curFrame;
        sLocalNav.lastPathTargetPos    = leaderPos;
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

    // Speed selection uses XZ distance to leader (not subgoal) so the
    // NPC sustains run speed across multi-segment paths when leader is
    // far, and slows to walk only when actually close.
    const float distToLeaderSq = Dist2DSq(a->world.pos, leaderPos);
    const float speed = (distToLeaderSq > kRunDistance * kRunDistance)
                            ? kRunSpeed : kWalkSpeed;
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
    if (distToLeaderSq <= kEnterIdle * kEnterIdle) {
        this_->state = EN_FOLLOWER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalNav.path.Reset();  // discard path; IDLE is local-frame
    }
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
    // machine entirely.
    if (!IsLocalOwnerNPC(npc)) {
        // Still advance physics so gravity applies (the actor falls
        // onto floor if STATE packets place it above ground).
        Actor_MoveXZGravity(npc);
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

    // Dispatch.
    switch (this_->state) {
        default:
        case EN_FOLLOWER_STATE_IDLE:
            TickIDLE(this_, play, leaderPos);
            break;
        case EN_FOLLOWER_STATE_FOLLOW:
            TickFOLLOW(this_, play, leaderPos);
            break;
        case EN_FOLLOWER_STATE_STUCK:
            TickSTUCK(this_, play, leaderPos);
            break;
        // Phase 6+ handlers (CLIMBING / DEAD) — fall through to IDLE
        // behaviour as placeholders.
        case EN_FOLLOWER_STATE_CLIMBING:
        case EN_FOLLOWER_STATE_DEAD:
            TickIDLE(this_, play, leaderPos);
            break;
    }

    // Apply locomotion. Standard OoT NPC pattern — speedXZ + world.rot.y
    // give a per-frame velocity vector; gravity pulls Y to floor.
    Actor_MoveXZGravity(npc);

    // Update collision-with-ground / floor altitude. Without this the
    // NPC's Y can drift away from the floor on slopes / steps.
    Actor_UpdateBgCheckInfo(play, npc, 26.0f /* wallCheckHeight */,
                            10.0f /* wallCheckRadius */,
                            50.0f /* ceilingCheckHeight */,
                            UPDBGCHECKINFO_FLAG_0 | UPDBGCHECKINFO_FLAG_2 |
                            UPDBGCHECKINFO_FLAG_3 | UPDBGCHECKINFO_FLAG_4);
}
