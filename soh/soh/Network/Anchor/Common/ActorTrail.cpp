/**
 * ActorTrail — implementation.
 *
 * Per plan §5. The Tick path captures local player + remote DummyPlayers
 * + every syncable actor with NavTraits.leavesTrail=true. The
 * GetBestReachableSubgoal path walks the trail with MovementClear gating.
 *
 * MovementClear and VisualLineOfSight live as file-scope helpers via
 * BgCheck_AnyLineTest1.
 */

#include "ActorTrail.h"
#include "ActorSyncHelpers.h"  // kSyncableActorCategories, IsSyncableActor
#include "NavTraits.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // Layer 3 fallback
#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <limits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_NAV_ENABLED          CVAR_ENHANCEMENT("Nav.Enabled")
#define CVAR_NAV_ACTOR_TRAIL      CVAR_ENHANCEMENT("Nav.ActorTrail")
#define CVAR_NAV_ROOM_NAV_LAYER   CVAR_ENHANCEMENT("Nav.RoomNavConsumer")

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Line-of-sight primitives.
// ---------------------------------------------------------------------------

bool MovementClear(const Actor* navigator, const Vec3f& candidatePos, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return false;
    Vec3f a = { navigator->world.pos.x, navigator->world.pos.y + kBodyOffset, navigator->world.pos.z };
    Vec3f b = { candidatePos.x,         candidatePos.y         + kBodyOffset, candidatePos.z         };
    Vec3f hitPos;
    CollisionPoly* hitPoly = nullptr;
    s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &a, &b, &hitPos, &hitPoly, 0);
    return !hit;
}

bool VisualLineOfSight(const Actor* navigator, const Vec3f& targetPos, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return false;
    Vec3f a = { navigator->world.pos.x, navigator->world.pos.y + kHeadOffset, navigator->world.pos.z };
    Vec3f b = { targetPos.x,            targetPos.y            + kHeadOffset, targetPos.z            };
    Vec3f hitPos;
    CollisionPoly* hitPoly = nullptr;
    s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &a, &b, &hitPos, &hitPoly, 0);
    return !hit;
}

// ---------------------------------------------------------------------------
// ActorTrail singleton + lifecycle.
// ---------------------------------------------------------------------------

ActorTrail& ActorTrail::GetInstance() {
    static ActorTrail instance;
    return instance;
}

bool ActorTrail::IsEnabled() {
    return CVarGetInteger(CVAR_NAV_ENABLED, 0) != 0
        && CVarGetInteger(CVAR_NAV_ACTOR_TRAIL, 0) != 0;
}

void ActorTrail::ClearForKey(TrailKey key) {
    mTrails.erase(key);
}

void ActorTrail::ClearForScene(int16_t sceneNum) {
    for (auto it = mTrails.begin(); it != mTrails.end();) {
        if (it->second.count > 0) {
            // Most recent waypoint = (head + count - 1) % kMaxWaypoints if count <= kMaxWaypoints
            size_t newestIdx = (it->second.head + kMaxWaypoints - 1) % kMaxWaypoints;
            if (it->second.waypoints[newestIdx].sceneNum == sceneNum) {
                it = mTrails.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void ActorTrail::ClearAll() {
    mTrails.clear();
    mFrameCounter = 0;
}

void ActorTrail::CaptureWaypoint(TrailKey key, const Vec3f& pos, int16_t sceneNum,
                                  int8_t roomNum, uint8_t timeline) {
    EntityTrail& trail = mTrails[key];
    TrailWaypoint& wp = trail.waypoints[trail.head];
    wp.pos      = pos;
    wp.sceneNum = sceneNum;
    wp.roomNum  = roomNum;
    wp.timeline = timeline;
    wp.frameIdx = mFrameCounter;

    trail.head = (trail.head + 1) % kMaxWaypoints;
    if (trail.count < kMaxWaypoints) trail.count++;
    trail.lastFrame = mFrameCounter;
}

// ---------------------------------------------------------------------------
// Tick — capture positions for trailed entities.
// ---------------------------------------------------------------------------

void ActorTrail::Tick(PlayState* play) {
    if (play == nullptr) return;

    mFrameCounter++;
    if ((mFrameCounter % kCaptureRateFrames) != 0) return;  // 30Hz throttle

    // Skip capture during cutscenes — scripted positions would mislead
    // trail consumers. Per plan §5 capture rules.
    if (play->csCtx.state != CS_STATE_IDLE) return;

    int16_t sceneNum = play->sceneNum;
    int8_t  roomNum  = (int8_t)play->roomCtx.curRoom.num;

    // Local player.
    Player* localPlayer = GET_PLAYER(play);
    if (localPlayer != nullptr && localPlayer->actor.update != nullptr) {
        const Vec3f& pos = localPlayer->actor.world.pos;
        if (pos.x > -9000.0f) {  // not the out-of-scene sentinel
            uint8_t localClientId = 0; // local player always slot 0 in local trails
            uint8_t timeline = (uint8_t)(gSaveContext.linkAge & 1);
            CaptureWaypoint(TrailKeyForPlayer(localClientId), pos,
                            sceneNum, roomNum, timeline);
        }
    }

    // Remote players + AI Followers + syncable actors with leavesTrail=true.
    // The follower (ACTOR_EN_OE2) is in ACTORCAT_NPC; covered by the
    // syncable-actor scan below (NavTraits explicitly opts it in via the
    // override map's leavesTrail=true). Remote DummyPlayers are also in
    // ACTORCAT_NPC and look like ACTOR_EN_OE2 — same path.
    for (uint8_t cat : kSyncableActorCategories) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            Actor* next = actor->next;
            if (IsSyncableActor(actor) && actor->update != nullptr) {
                const NavTraits& traits = GetTraitsForActor(actor->id);
                if (traits.leavesTrail) {
                    EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
                    if (ext != nullptr && ext->netId != 0) {
                        const Vec3f& pos = actor->world.pos;
                        if (pos.x > -9000.0f) {  // out-of-scene sentinel
                            CaptureWaypoint(TrailKeyForActor(ext->netId), pos,
                                            sceneNum, roomNum, /*timeline=*/0);
                        }
                    }
                }
            }
            actor = next;
        }
    }
}

// ---------------------------------------------------------------------------
// Read APIs.
// ---------------------------------------------------------------------------

bool ActorTrail::GetWaypointBefore(TrailKey key, uint32_t framesAgo, TrailWaypoint& out) const {
    auto it = mTrails.find(key);
    if (it == mTrails.end() || it->second.count == 0) return false;

    const EntityTrail& trail = it->second;
    // Each capture is kCaptureRateFrames apart; convert framesAgo → buffer index lag.
    size_t lagIndex = framesAgo / kCaptureRateFrames;
    if (lagIndex >= trail.count) {
        lagIndex = trail.count - 1; // fall back to oldest
    }

    // Newest = (head - 1) mod kMaxWaypoints. Step back lagIndex from newest.
    size_t idx = (trail.head + kMaxWaypoints - 1 - lagIndex) % kMaxWaypoints;
    out = trail.waypoints[idx];
    return true;
}

bool ActorTrail::GetBestReachableSubgoal(TrailKey key,
                                          const Actor* navigator,
                                          const Vec3f& targetPos,
                                          PlayState* play,
                                          Vec3f& out) const {
    if (navigator == nullptr || play == nullptr) {
        out = targetPos;
        return false;
    }

    // Step 1 — direct pursuit.
    if (MovementClear(navigator, targetPos, play)) {
        out = targetPos;
        return true;
    }

    // Step 2 — search trail for furthest reachable progress.
    auto it = mTrails.find(key);
    if (it == mTrails.end() || it->second.count == 0) {
        out = targetPos;
        return false;
    }

    const EntityTrail& trail = it->second;
    const Vec3f& navPos = navigator->world.pos;
    auto distSq = [](const Vec3f& a, const Vec3f& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };
    float distNavToTargetSq = distSq(navPos, targetPos);

    // Walk newest→oldest.
    for (size_t i = 0; i < trail.count; i++) {
        size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
        const TrailWaypoint& wp = trail.waypoints[idx];

        // Reject cross-scene waypoints.
        if (wp.sceneNum != gPlayState->sceneNum) continue;

        // Reject stale waypoints (>6s old).
        if (mFrameCounter > wp.frameIdx && (mFrameCounter - wp.frameIdx) > 360) continue;

        // Reject non-progress waypoints (would walk away from target).
        if (distSq(wp.pos, targetPos) >= distNavToTargetSq) continue;

        // Reject blocked waypoints.
        if (!MovementClear(navigator, wp.pos, play)) continue;

        // Found furthest reachable progress.
        out = wp.pos;
        return true;
    }

    // Layer 3 — RoomNavData static graph (room_nav_data_plan.md §9). Cold
    // navigators (just-spawned, no trail) and idle scenes (target hasn't
    // moved enough to leave a useful breadcrumb trail) fall through Layer 1
    // and 2 to here. The pre-scanned graph picks the closest-to-target
    // reachable node within the room. CVar-gated separately so the prior
    // 2-layer behaviour can be restored without disabling ActorTrail
    // wholesale; also implicitly gated on RoomNavData.Enabled because
    // GetForRoom returns nullptr when the master switch is off.
    if (CVarGetInteger(CVAR_NAV_ROOM_NAV_LAYER, 0) != 0) {
        int16_t scene  = gPlayState->sceneNum;
        int8_t  room   = (int8_t)gPlayState->roomCtx.curRoom.num;
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(scene, room);
        if (navData != nullptr) {
            int fromIdx = ::AnchorNavRoom::FindNearestNode(navData, navPos);
            if (fromIdx >= 0) {
                int bestIdx = ::AnchorNavRoom::FindBestReachableSubgoalNode(
                    navData, fromIdx, targetPos);
                if (bestIdx >= 0 && (size_t)bestIdx < navData->nodes.size()) {
                    const Vec3f& nodePos = navData->nodes[(size_t)bestIdx].pos;
                    // MovementClear gate so the chosen node is reachable
                    // from the navigator's current position via a straight
                    // line. The graph BFS proves graph-reachability across
                    // edges; this proves the FIRST step is line-clear, which
                    // is what the steering layer can actually drive toward
                    // this frame. Failure here is unusual but possible when
                    // the navigator is between graph cells with a wall
                    // between fromIdx and bestIdx — fall through.
                    if (MovementClear(navigator, nodePos, play)) {
                        out = nodePos;
                        return true;
                    }
                }
            }
        }
    }

    // No reachable progress across all three layers — fallback.
    out = targetPos;
    return false;
}

bool ActorTrail::GetBestReachableSubgoalForPlayer(uint8_t clientId, const Actor* navigator,
                                                    const Vec3f& targetPos, PlayState* play,
                                                    Vec3f& out) const {
    return GetBestReachableSubgoal(TrailKeyForPlayer(clientId), navigator, targetPos, play, out);
}

bool ActorTrail::GetBestReachableSubgoalForActor(uint32_t netId, const Actor* navigator,
                                                   const Vec3f& targetPos, PlayState* play,
                                                   Vec3f& out) const {
    return GetBestReachableSubgoal(TrailKeyForActor(netId), navigator, targetPos, play, out);
}

// ---------------------------------------------------------------------------
// Lifecycle hooks. Tick from OnGameFrameUpdate; clear on game exit so the
// next session starts clean.
// ---------------------------------------------------------------------------

static void OnGameFrameTick() {
    if (!ActorTrail::IsEnabled()) return;
    PlayState* play = gPlayState;
    if (play == nullptr) return;
    ActorTrail::GetInstance().Tick(play);
}

static void OnExitGameClear(int32_t /*fileNum*/) {
    ActorTrail::GetInstance().ClearAll();
}

static void RegisterActorTrail() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameTick);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnExitGame>(
        OnExitGameClear);
}

} // namespace AnchorNav

static RegisterShipInitFunc registerActorTrail(AnchorNav::RegisterActorTrail);
