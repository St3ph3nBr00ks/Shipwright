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
#include "DistanceMath.h"
#include "NavCVars.h"
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

#include <chrono>
#include <cmath>
#include <limits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

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

bool MovementClearAtPosition(const Vec3f& fromPos, const Vec3f& toPos, PlayState* play) {
    if (play == nullptr) return false;
    Vec3f a = { fromPos.x, fromPos.y + kBodyOffset, fromPos.z };
    Vec3f b = { toPos.x,   toPos.y   + kBodyOffset, toPos.z   };
    Vec3f hitPos;
    CollisionPoly* hitPoly = nullptr;
    s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &a, &b, &hitPos, &hitPoly, 0);
    return !hit;
}

// Returns true when continuous floor exists along the segment from→to at
// approximately the linearly-interpolated Y. Used by Layer 1 LOS to
// reject "clear at pelvis but no floor below" cases — without this gate,
// MovementClear succeeds across air gaps (raycast at pelvis height
// doesn't see the void), Layer 1 returns the leader directly, and the
// follower walks into the chasm. The fix forces Layer 1 to fall through
// to Layer 3 BFS so jump anchors / climb bridges / drop anchors get
// consulted.
//
// Samples down at ~grid-resolution intervals and verifies floor altitude
// matches the segment's expected Y within ±kFloorTolY. Excludes
// endpoints (they're walkable by construction). Cost: ~lenXZ/30 floor
// raycasts per Layer 1 call.
bool FloorPresentAlongPath(const Vec3f& from, const Vec3f& to, PlayState* play) {
    if (play == nullptr) return true;  // can't verify; let LOS pass
    const f32 dxf = to.x - from.x;
    const f32 dzf = to.z - from.z;
    const f32 dyf = to.y - from.y;
    const f32 lenXZ = std::sqrt(dxf*dxf + dzf*dzf);
    constexpr f32 kSampleSpacing = 30.0f;  // matches kGridResolution
    constexpr f32 kFloorTolY     = 25.0f;  // step-up tolerance
    const int samples = std::max(3, (int)(lenXZ / kSampleSpacing));
    for (int s = 1; s < samples; s++) {
        const f32 t = (f32)s / (f32)samples;
        const f32 sx = from.x + dxf * t;
        const f32 sz = from.z + dzf * t;
        const f32 expectedY = from.y + dyf * t;
        Vec3f probe = { sx, expectedY + 100.0f, sz };
        CollisionPoly poly{};
        f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &poly, &probe);
        if (floorY <= BGCHECK_Y_MIN) return false;          // no floor at all
        if (std::fabs(floorY - expectedY) > kFloorTolY) return false;  // floor far off
    }
    return true;
}

// ---------------------------------------------------------------------------
// ActorTrail singleton + lifecycle.
// ---------------------------------------------------------------------------

ActorTrail& ActorTrail::GetInstance() {
    static ActorTrail instance;
    return instance;
}

bool ActorTrail::IsEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kActorTrail);
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
    mNowMs = 0;
    mLastCaptureMs = 0;
}

uint64_t ActorTrail::NowMs() {
    using clock = std::chrono::steady_clock;
    static const auto epoch = clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now() - epoch);
    return (uint64_t)delta.count();
}

void ActorTrail::CaptureWaypoint(TrailKey key, const Vec3f& pos, int16_t sceneNum,
                                  int8_t roomNum, uint8_t timeline) {
    EntityTrail& trail = mTrails[key];
    TrailWaypoint& wp = trail.waypoints[trail.head];
    wp.pos       = pos;
    wp.sceneNum  = sceneNum;
    wp.roomNum   = roomNum;
    wp.timeline  = timeline;
    wp.captureMs = mNowMs;

    trail.head = (trail.head + 1) % kMaxWaypoints;
    if (trail.count < kMaxWaypoints) trail.count++;
    trail.lastCaptureMs = mNowMs;
}

// ---------------------------------------------------------------------------
// Tick — capture positions for trailed entities.
// ---------------------------------------------------------------------------

void ActorTrail::Tick(PlayState* play) {
    if (play == nullptr) return;

    mNowMs = NowMs();

    // Wall-clock throttle. Frame-based throttle drifts because SoH game
    // logic ticks at variable rates (20 Hz vanilla up to display refresh
    // depending on FPS settings, plus VirtualBox slowdowns). Wall-clock
    // keeps user-facing capture rate consistent regardless.
    if (mLastCaptureMs != 0 && (mNowMs - mLastCaptureMs) < kCaptureIntervalMs) return;
    mLastCaptureMs = mNowMs;

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

    // Remote DummyPlayers — represent OTHER (remote) players' Links on
    // the local client. Spawned as ACTOR_PLAYER, then immediately moved
    // to ACTORCAT_NPC with id=ACTOR_EN_OE2 and update=DummyPlayer_Update
    // (per session_state.md "DummyPlayer Actor Facts"). Each maps to a
    // remote clientId via Anchor::GetDummyPlayerClientId; capture under
    // TrailKeyForPlayer(clientId) so the AI Follower's substrate path
    // consumer (HandleStateFollow / RETURN) finds the leader's
    // breadcrumb trail when it queries TrailKeyForPlayer(leaderClientId).
    //
    // Without this capture, ComputePathTo Layer 2 (trail breadcrumb
    // walk) returns nothing for the leader and the follower has only
    // Layer 1 (direct LOS) and Layer 3 (RoomNavData BFS) — exactly the
    // "follower can't truly follow leader effectively, relying only on
    // room nav data" symptom reported 2026-05-09.
    if (Anchor::Instance != nullptr) {
        Actor* npc = play->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (npc != nullptr) {
            Actor* nextNpc = npc->next;
            if (npc->id == ACTOR_EN_OE2 &&
                npc->update == (ActorFunc)DummyPlayer_Update) {
                uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(npc);
                const Vec3f& pos = npc->world.pos;
                if (pos.x > -9000.0f) {  // not the out-of-scene sentinel
                    // Cross-timeline gating happens at substrate consumer
                    // sites (Pillar B); capture the timeline tag for
                    // future filtering. clientId fits in 8 bits per the
                    // Anchor handshake design (max 4 clients in v1).
                    uint8_t timeline = (uint8_t)(gSaveContext.linkAge & 1);
                    CaptureWaypoint(TrailKeyForPlayer((uint8_t)clientId), pos,
                                    sceneNum, roomNum, timeline);
                }
            }
            npc = nextNpc;
        }
    }

    // Syncable actors with leavesTrail=true (enemies, etc.).
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

bool ActorTrail::FindTrailWaypointBeyondGap(TrailKey key,
                                              const Vec3f& referencePos,
                                              float maxGapDistance,
                                              const Vec3f& targetPos,
                                              Vec3f& outLanding) const {
    auto it = mTrails.find(key);
    if (it == mTrails.end() || it->second.count == 0) return false;

    const EntityTrail& trail = it->second;
    const float maxGapSq      = maxGapDistance * maxGapDistance;
    const float refToTargetSq = AnchorDist::Dist3DSq(referencePos, targetPos);

    // Walk newest→oldest. Newest match wins because it's the freshest
    // evidence of where target actually went.
    for (size_t i = 0; i < trail.count; i++) {
        size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
        const TrailWaypoint& wp = trail.waypoints[idx];

        // Stale filter (matches GetBestReachableSubgoal — 12s).
        if (mNowMs > wp.captureMs && (mNowMs - wp.captureMs) > kStaleAgeMs) continue;
        // Within jump range of reference?
        if (AnchorDist::Dist3DSq(wp.pos, referencePos) > maxGapSq) continue;
        // Strict progress: closer to target than referencePos.
        if (AnchorDist::Dist3DSq(wp.pos, targetPos) >= refToTargetSq) continue;

        outLanding = wp.pos;
        return true;
    }
    return false;
}

void ActorTrail::SnapshotActiveWaypoints(int16_t sceneFilter,
                                          std::vector<WaypointSnapshot>& out) const {
    out.clear();
    for (const auto& [key, trail] : mTrails) {
        for (size_t i = 0; i < trail.count; ++i) {
            // Iterate newest→oldest. Overlay consumers can ignore order if
            // they don't care; the ordering is just a side-effect of the
            // ring-buffer layout.
            size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
            const TrailWaypoint& wp = trail.waypoints[idx];
            if (wp.sceneNum != sceneFilter) continue;
            out.push_back({key, wp.pos});
        }
    }
}

bool ActorTrail::GetWaypointBefore(TrailKey key, uint32_t msAgo, TrailWaypoint& out) const {
    auto it = mTrails.find(key);
    if (it == mTrails.end() || it->second.count == 0) return false;

    const EntityTrail& trail = it->second;
    // Walk newest→oldest; first waypoint at least `msAgo` old wins. With
    // captures at kCaptureIntervalMs intervals, this typically resolves
    // in 1-2 steps. Falls back to oldest entry if nothing matches.
    const uint64_t cutoff = (mNowMs > msAgo) ? (mNowMs - msAgo) : 0;
    for (size_t i = 0; i < trail.count; ++i) {
        size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
        const TrailWaypoint& wp = trail.waypoints[idx];
        if (wp.captureMs <= cutoff) {
            out = wp;
            return true;
        }
    }
    // No waypoint that old — return oldest available.
    size_t oldestIdx = (trail.head + kMaxWaypoints - trail.count) % kMaxWaypoints;
    out = trail.waypoints[oldestIdx];
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

    // Step 1 — direct pursuit. P3.10 part 2: gate on Y-delta so the
    // navigator doesn't short-circuit to "target reachable" when the
    // target is significantly above/below despite a clear horizontal
    // line. MovementClear is a horizontal line test; a target on a
    // platform 200u up with clear airspace below passes the test but
    // the navigator can't walk vertically. Forces Layer 2/3 fall-
    // through so the trail breadcrumb / RoomNavData BFS can locate
    // the actual route up (slope / ladder / climb anchor).
    constexpr float kLayer1YGate = 50.0f;
    float dyToTarget = std::fabs(targetPos.y - navigator->world.pos.y);
    if (dyToTarget < kLayer1YGate &&
        MovementClear(navigator, targetPos, play) &&
        FloorPresentAlongPath(navigator->world.pos, targetPos, play)) {
        out = targetPos;
        return true;
    }

    const Vec3f& navPos = navigator->world.pos;
    float distNavToTargetSq = AnchorDist::Dist3DSq(navPos, targetPos);

    // Step 2 — RoomNavData static graph. Pre-scanned per-room nav graph
    // with edges built via MovementClearAtPosition + step-up gating
    // between adjacent grid cells, so paths are robustly walkable
    // (short walls and voids excluded at scan time). Now runs BEFORE
    // the trail breadcrumb walk (post P3.11 reorder) — Layer 2's
    // pelvis-pelvis MovementClear test was returning "valid"
    // breadcrumbs the follower couldn't actually reach (short walls
    // / voids the line passes over), and the trail walk would
    // short-circuit before this graph layer ran. User 2026-05-09
    // follow-up reported "AI Follower stuck running into a wall
    // trying to reach the leader instead of using nav data to walk
    // around" — exactly that failure mode.
    if (AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kRoomNavConsumer)) {
        const NavTraits& traits = GetTraitsForActor(navigator->id);
        if (traits.consumeRoomNavData) {
            int16_t scene = gPlayState->sceneNum;
            int8_t  room  = (int8_t)gPlayState->roomCtx.curRoom.num;
            const ::AnchorNavRoom::RoomNavData* navData =
                ::AnchorNavRoom::GetForRoom(scene, room);
            if (navData != nullptr) {
                int fromIdx = ::AnchorNavRoom::FindNearestNode(navData, navPos);
                if (fromIdx >= 0) {
                    const ::AnchorNavRoom::NavQueryOptions opts =
                        BuildNavQueryOptions(navigator);
                    int bestIdx = ::AnchorNavRoom::FindBestReachableSubgoalNode(
                        navData, fromIdx, targetPos, opts);
                    if (bestIdx >= 0 && (size_t)bestIdx < navData->nodes.size()) {
                        const Vec3f& nodePos = navData->nodes[(size_t)bestIdx].pos;
                        // MovementClear gate so the chosen node is reachable
                        // from the navigator's current position via a straight
                        // line. The graph BFS proves graph-reachability across
                        // edges; this proves the FIRST step is line-clear,
                        // which is what the steering layer can actually drive
                        // toward this frame. Failure here is unusual but
                        // possible when the navigator is between graph cells
                        // with a wall between fromIdx and bestIdx — fall
                        // through.
                        if (MovementClear(navigator, nodePos, play)) {
                            out = nodePos;
                            return true;
                        }
                    }
                }
            }
        }
    }

    // Step 3 (fallback) — search trail for furthest reachable progress.
    // Falls through here when the graph layer is unavailable
    // (CVar off, navigator opted out, no scan loaded, or BFS failed
    // / chosen node was line-blocked).
    auto it = mTrails.find(key);
    if (it == mTrails.end() || it->second.count == 0) {
        out = targetPos;
        return false;
    }

    const EntityTrail& trail = it->second;
    // Walk newest→oldest.
    for (size_t i = 0; i < trail.count; i++) {
        size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
        const TrailWaypoint& wp = trail.waypoints[idx];

        // Reject cross-scene waypoints.
        if (wp.sceneNum != gPlayState->sceneNum) continue;

        // Reject stale waypoints (> kStaleAgeMs = 12s wall-clock).
        if (mNowMs > wp.captureMs && (mNowMs - wp.captureMs) > kStaleAgeMs) continue;

        // Reject non-progress waypoints (would walk away from target).
        if (AnchorDist::Dist3DSq(wp.pos, targetPos) >= distNavToTargetSq) continue;

        // Reject blocked waypoints.
        if (!MovementClear(navigator, wp.pos, play)) continue;

        // Found furthest reachable progress.
        out = wp.pos;
        return true;
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

bool ActorTrail::ComputePathTo(TrailKey key,
                                const Actor* navigator,
                                const Vec3f& targetPos,
                                PlayState* play,
                                NavPath& out,
                                bool skipLayer1LOS,
                                bool preferLeaderTrail) const {
    out.Reset();
    if (navigator == nullptr || play == nullptr || gPlayState == nullptr) return false;

    // Stamp metadata up front. Even if no layer succeeds, the caller may
    // want to introspect why (e.g. the scene actually changed mid-pursuit).
    out.sceneNum          = gPlayState->sceneNum;
    out.msAtCapture       = mNowMs;
    out.capturedTargetPos = targetPos;

    // Layer 1 — direct reachability. P3.10 part 2: same Y-delta gate
    // as GetBestReachableSubgoal — skip when target is significantly
    // above/below so the path consumer routes through the trail
    // breadcrumbs / RoomNavData edges that actually encode vertical
    // traversal (slope / ladder / climb anchor).
    //
    // skipLayer1LOS (user 2026-05-10): caller can force Layer 1 to be
    // skipped when LOS is known unreliable for this target (e.g. door-
    // handoff targets across complex collision — pelvis-line MovementClear
    // passes over short walls and through narrow gaps that the follower
    // can't actually walk through). Forces fallback to Layer 3 BFS whose
    // graph edges encode actual walkability.
    //
    // Floor-along-path gate (user 2026-05-12 fix): MovementClear's
    // pelvis-height line passes OVER air gaps because raycast at +20u
    // doesn't see the void. Without the floor check, Layer 1 always
    // wins, returns the leader directly, and the follower walks into
    // the chasm. The floor gate forces fall-through to Layer 3 BFS so
    // jump-anchor / climb-bridge / drop-anchor edges actually get
    // consulted on cross-gap pursuits.
    constexpr float kLayer1YGate = 50.0f;
    float dyToTarget = std::fabs(targetPos.y - navigator->world.pos.y);
    if (!skipLayer1LOS &&
        dyToTarget < kLayer1YGate &&
        MovementClear(navigator, targetPos, play) &&
        FloorPresentAlongPath(navigator->world.pos, targetPos, play)) {
        out.waypoints.push_back(targetPos);
        out.waypointFlags.push_back(0); // Layer 1: target itself, no source node
        return true;
    }

    // Layer 2/3 ordering — user 2026-05-09: post-P3.11 default order is
    // Layer 1 → Layer 3 (graph) → Layer 2 (trail). Layer 2's MovementClear
    // is a single-height pelvis-pelvis line that misses short walls /
    // voids; Layer 3's graph edges were built with proper step-up + line-
    // clear checks at scan time so they're more robust for generic
    // pursuit.
    //
    // preferLeaderTrail (user 2026-05-10) reverses to Layer 1 → Layer 2
    // → Layer 3. Caller's contract: the trail under `key` is a player's
    // actual walked path, so Layer 2's "MovementClear over short walls"
    // weakness doesn't apply (the breadcrumbs are positions a player
    // physically walked through). Use for door-handoff / open-room-
    // boundary cases where "follow leader's footsteps to where they
    // exited our room" is the right semantics.

    auto tryLayer3 = [&]() -> bool {
        // Layer 3 — RoomNavData BFS path. Two-stage gating: CVar and
        // per-actor traits.consumeRoomNavData. The BFS materialises the
        // full chain; we append `target` to the end when the final node
        // has line-clear to the target so the path terminates at the
        // desired destination instead of at a graph node near it.
        if (!AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kRoomNavConsumer)) return false;
        const NavTraits& traits = GetTraitsForActor(navigator->id);
        if (!traits.consumeRoomNavData) return false;
        int16_t scene = gPlayState->sceneNum;
        int8_t  room  = (int8_t)gPlayState->roomCtx.curRoom.num;
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(scene, room);
        if (navData == nullptr) return false;
        int fromIdx = ::AnchorNavRoom::FindNearestNode(navData, navigator->world.pos);
        if (fromIdx < 0) return false;
        // Stage 5: build per-call options including the dynamic climb-mask
        // resolution (overlays session-dependent bits like ClimbAnywhere on
        // top of the static base from NavTraits). Stash on NavPath so the
        // consumer can re-AND at engage time without re-reading the CVar.
        const ::AnchorNavRoom::NavQueryOptions opts = BuildNavQueryOptions(navigator);
        out.computedClimbMask = opts.climbSurfaceMask;
        std::vector<Vec3f> graphPath;
        std::vector<uint32_t> graphPathFlags;  // Stage 4: parallel array (widened to uint32 for synthetic flags)
        bool ok = ::AnchorNavRoom::FindBestReachableSubgoalPath(
            navData, fromIdx, targetPos, opts,
            graphPath, &graphPathFlags);
        if (!ok || graphPath.empty()) return false;
        out.waypoints     = std::move(graphPath);
        out.waypointFlags = std::move(graphPathFlags);
        if (MovementClearAtPosition(out.waypoints.back(), targetPos, play)) {
            out.waypoints.push_back(targetPos);
            out.waypointFlags.push_back(0); // target appended; not from any node
        }
        return true;
    };

    auto tryLayer2 = [&]() -> bool {
        // Layer 2 — trail breadcrumbs. Walked newest→oldest; first
        // MovementClear-passing waypoint that improves distance to
        // target wins. Optimistically appends `target` when the
        // breadcrumb→target segment is also line-clear so the AI
        // doesn't need an immediate re-query at the breadcrumb.
        auto it = mTrails.find(key);
        if (it == mTrails.end() || it->second.count == 0) return false;
        const EntityTrail& trail = it->second;
        const Vec3f& navPos = navigator->world.pos;
        float distNavToTargetSq = AnchorDist::Dist3DSq(navPos, targetPos);
        for (size_t i = 0; i < trail.count; i++) {
            size_t idx = (trail.head + kMaxWaypoints - 1 - i) % kMaxWaypoints;
            const TrailWaypoint& wp = trail.waypoints[idx];
            if (wp.sceneNum != gPlayState->sceneNum) continue;
            if (mNowMs > wp.captureMs && (mNowMs - wp.captureMs) > kStaleAgeMs) continue;
            if (AnchorDist::Dist3DSq(wp.pos, targetPos) >= distNavToTargetSq) continue;
            if (!MovementClear(navigator, wp.pos, play)) continue;
            out.waypoints.push_back(wp.pos);
            out.waypointFlags.push_back(0); // Layer 2: trail breadcrumb, no source NavNode
            if (MovementClearAtPosition(wp.pos, targetPos, play)) {
                out.waypoints.push_back(targetPos);
                out.waypointFlags.push_back(0); // target appended
            }
            return true;
        }
        return false;
    };

    if (preferLeaderTrail) {
        if (tryLayer2()) return true;
        if (tryLayer3()) return true;
    } else {
        if (tryLayer3()) return true;
        if (tryLayer2()) return true;
    }

    // Nothing reachable — caller falls through to direct yaw / recovery.
    return false;
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

// Clear an actor's trail when the actor dies. Without this, navigators
// pursuing a dead target follow stale breadcrumbs to a corpse-position
// (or, after Actor_Kill, to wherever the actor's last waypoint was
// captured before the kill). Two hook sites cover the two death paths:
//
//   OnEnemyDefeat — fires from EnemyNetId-tagged enemy deaths via the
//                   standard combat pipeline.
//   OnActorKill   — fires from any Actor_Kill call. Catches enemies
//                   that die outside the OnEnemyDefeat path (Karebaba
//                   stem, En_Skb at dawn) and any other actor whose
//                   trail was being captured (e.g. NPCs with
//                   leavesTrail=true if added later).
//
// Both call paths look up the EnemyNetId extension to find the netId →
// TrailKey. If no extension is present, the actor wasn't being trailed
// anyway, so the clear is a no-op.
//
// Player trails (TrailKeyForPlayer) are not cleared here — players
// don't "die" in the actor-death sense; their trails reset on scene
// change / disconnect via separate paths.
static void ClearTrailForDeadActor(void* refActor) {
    if (refActor == nullptr) return;
    Actor* actor = static_cast<Actor*>(refActor);
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return;
    ActorTrail::GetInstance().ClearForKey(TrailKeyForActor(ext->netId));
}

static void RegisterActorTrail() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameTick);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnExitGame>(
        OnExitGameClear);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnEnemyDefeat>(
        ClearTrailForDeadActor);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorKill>(
        ClearTrailForDeadActor);
}

} // namespace AnchorNav

static RegisterShipInitFunc registerActorTrail(AnchorNav::RegisterActorTrail);
