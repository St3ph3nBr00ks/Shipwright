/**
 * ActorTrail — per-entity ring buffer of recent positions.
 *
 * Foundation for the navigation system's "breadcrumb" pursuit: when a
 * navigator loses line-of-sight to its target, it can fall back to the
 * trail of recent positions the target left behind. Players are the
 * primary trailed entities, but synced enemies and AI Followers also
 * leave trails when their NavTraits.leavesTrail is true — see
 * feedback_nav_helpers_entity_agnostic.md.
 *
 * Storage: std::unordered_map<TrailKey, EntityTrail>. TrailKey is a
 * tagged uint32 — high bit = player (low byte is clientId);
 * high bit clear = actor (low 31 bits are netId).
 *
 * Sampling rate: 5 captures/sec/entity at 60fps (kCaptureRateFrames=12).
 * Buffer size: 50 waypoints per entity (10.0s of history before the
 * oldest is overwritten).
 *
 * The MovementClear and VisualLineOfSight primitives also live here as
 * file-scope helpers (perception vs movement layer split per plan §2).
 *
 * See:
 *   - Plans/nav_system_implementation_plan.md §5
 *   - GitHub #206 Phase 1 commit 2
 */

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Trail data structures.
// ---------------------------------------------------------------------------

struct TrailWaypoint {
    Vec3f pos;
    int16_t sceneNum;
    int8_t  roomNum;
    uint8_t timeline;     // linkAge & 1 for players; 0 for non-player actors
    uint64_t frameIdx;    // global frame counter at capture time
};

// Tagged identifier for a trailed entity.
//   High bit set   → player (low byte is clientId)
//   High bit clear → actor  (low 31 bits are netId)
using TrailKey = uint32_t;
constexpr uint32_t kPlayerTrailMask = 0x80000000u;
inline TrailKey TrailKeyForPlayer(uint8_t clientId) { return kPlayerTrailMask | clientId; }
inline TrailKey TrailKeyForActor(uint32_t netId)    { return netId & 0x7FFFFFFFu; }

inline bool IsPlayerTrail(TrailKey key) { return (key & kPlayerTrailMask) != 0; }
inline uint8_t  TrailKey_GetClientId(TrailKey key) { return (uint8_t)(key & 0xFF); }
inline uint32_t TrailKey_GetNetId(TrailKey key)    { return key & 0x7FFFFFFFu; }

// ---------------------------------------------------------------------------
// ActorTrail singleton.
// ---------------------------------------------------------------------------

class ActorTrail {
public:
    static ActorTrail& GetInstance();

    // Tick once per game frame from OnGameFrameUpdate. Captures positions
    // for the local player, remote DummyPlayers, and every syncable actor
    // whose NavTraits.leavesTrail is true. Skips capture during cutscenes
    // / when actor->update == nullptr / out-of-scene sentinel positions.
    void Tick(PlayState* play);

    // Low-level: fetch a specific waypoint by age. Used by debug tooling
    // and callers that need explicit lag control.
    bool GetWaypointBefore(TrailKey key, uint32_t framesAgo, TrailWaypoint& out) const;

    // Primary navigation API — returns the optimal navigable subgoal for
    // `navigator` pursuing entity `key` (player or actor) toward `targetPos`.
    //
    // Algorithm (plan §5):
    //   1. Test target itself: MovementClear(navigator, targetPos) → return target.
    //   2. Walk trail newest→oldest. For each waypoint:
    //      - skip if dist(waypoint, targetPos) >= dist(navigator, targetPos)
    //        (not progress)
    //      - skip if !MovementClear(navigator, waypoint)
    //      - return first survivor (furthest reachable progress).
    //   3. No reachable subgoal → return targetPos as fallback (false return).
    //
    // Returns true if a subgoal was selected (case 1 or 2); false if
    // step 3 fallback was used. Callers may treat false as "stuck" and
    // route to the stuck-recovery state.
    bool GetBestReachableSubgoal(TrailKey key,
                                  const Actor* navigator,
                                  const Vec3f& targetPos,
                                  PlayState* play,
                                  Vec3f& out) const;

    // Convenience overloads.
    bool GetBestReachableSubgoalForPlayer(uint8_t clientId, const Actor* navigator,
                                           const Vec3f& targetPos, PlayState* play,
                                           Vec3f& out) const;
    bool GetBestReachableSubgoalForActor(uint32_t netId, const Actor* navigator,
                                          const Vec3f& targetPos, PlayState* play,
                                          Vec3f& out) const;

    // ---- Path snapshot API ---------------------------------------------------
    //
    // Multi-waypoint path the navigator commits to following, captured at
    // decision time. Caller owns the buffer; trail clears, graph rescans,
    // and target despawns between captures cannot strand the navigator
    // mid-pursuit because the waypoints live in the caller's own memory,
    // not the underlying ActorTrail / RoomNavData state.
    //
    // Usage:
    //   1. At decision time (target acquired / changed / current path
    //      exhausted / current step blocked), call ComputePathTo.
    //   2. Each frame, steer toward `path.CurrentSubgoal()`.
    //   3. When close enough to the current subgoal, call `path.Advance()`.
    //      When `path.Empty()` returns true, query again (target may have
    //      moved meanwhile).
    //   4. Optionally re-query if `play->sceneNum != path.sceneNum` or
    //      `dist(currentTargetPos, path.capturedTargetPos)` exceeds a
    //      consumer-defined tolerance.
    struct NavPath {
        std::vector<Vec3f> waypoints;          // [0] = next subgoal; [N-1] = path tail
        size_t   cursorIdx          = 0;       // index of current active waypoint
        int16_t  sceneNum           = -1;      // scene at capture time
        uint64_t frameAtCapture     = 0;       // for diagnostics + max-age policy
        Vec3f    capturedTargetPos  = { 0, 0, 0 };  // for "target moved" invalidation

        void Reset() {
            waypoints.clear();
            cursorIdx = 0;
            sceneNum = -1;
        }
        bool Empty() const { return waypoints.empty() || cursorIdx >= waypoints.size(); }
        const Vec3f& CurrentSubgoal() const { return waypoints[cursorIdx]; }
        // Advance cursor by one waypoint. Returns true if more waypoints remain.
        bool Advance() {
            ++cursorIdx;
            return cursorIdx < waypoints.size();
        }
    };

    // Computes a complete path from `navigator` to `targetPos`. Caller-owned
    // result; the AI commits to following these waypoints in order without
    // re-querying ActorTrail / RoomNavData each frame.
    //
    // Same 3-layer algorithm as GetBestReachableSubgoal, but materializes
    // the full chain instead of a single subgoal:
    //   Layer 1: target directly reachable → [target]
    //   Layer 2: trail breadcrumb           → [breadcrumb] or [breadcrumb, target]
    //                                          (target appended when breadcrumb→target
    //                                          is line-clear)
    //   Layer 3: RoomNavData BFS chain      → [node1, ..., nodeN]
    //                                          (+ target if line-clear from nodeN)
    //
    // Returns true on a non-empty path; false if nothing reachable. Caller
    // treats false as "stuck" — route to recovery / direct-yaw.
    bool ComputePathTo(TrailKey key,
                       const Actor* navigator,
                       const Vec3f& targetPos,
                       PlayState* play,
                       NavPath& out) const;

    // Diagnostic: snapshot every captured waypoint whose sceneNum matches
    // `sceneFilter` into `out`. Used by the DebugDraw overlay so the
    // overlay doesn't need access to internal storage. Filter is required
    // because cross-scene waypoints aren't navigable from the current
    // scene anyway and would just clutter the overlay. Output is cleared
    // before append.
    struct WaypointSnapshot {
        TrailKey key;
        Vec3f    pos;
    };
    void SnapshotActiveWaypoints(int16_t sceneFilter,
                                  std::vector<WaypointSnapshot>& out) const;

    // Lifecycle.
    void ClearForKey(TrailKey key);
    void ClearForScene(int16_t sceneNum);
    void ClearAll();

    // True when the master Nav CVar is on AND Nav.ActorTrail is on.
    static bool IsEnabled();

private:
    ActorTrail() = default;

    static constexpr size_t  kMaxWaypoints       = 50; // 50 × 12f = 10.0s history at 60fps
    static constexpr uint8_t kCaptureRateFrames  = 12; // capture every 12 game frames = 5Hz

    struct EntityTrail {
        std::array<TrailWaypoint, kMaxWaypoints> waypoints;
        size_t head = 0;       // next slot to write
        size_t count = 0;      // number of valid entries
        uint64_t lastFrame = 0;
    };

    std::unordered_map<TrailKey, EntityTrail> mTrails;
    uint64_t mFrameCounter = 0;

    void CaptureWaypoint(TrailKey key, const Vec3f& pos, int16_t sceneNum,
                         int8_t roomNum, uint8_t timeline);
};

// ---------------------------------------------------------------------------
// Line-of-sight primitives — perception vs movement layer split (plan §2).
//
// Both wrap BgCheck_AnyLineTest1 (functions.h:638). Body offset constants
// are tuned for Link's silhouette; per-actor overrides are post-v1.
// ---------------------------------------------------------------------------

constexpr float kBodyOffset = 20.0f;  // pelvis-height movement-clearance ray
constexpr float kHeadOffset = 60.0f;  // eye-height visual-LOS ray (Link reference)

// Returns true if the segment from navigator to candidatePos is clear of
// walls (navigator can walk there). Used by GetBestReachableSubgoal and
// future GroundFollowing refinements.
bool MovementClear(const Actor* navigator, const Vec3f& candidatePos, PlayState* play);

// Position-to-position variant of MovementClear. Used by path-snapshot
// algorithms that need to validate a step from a chosen waypoint to
// another (the navigator isn't there yet, but we want to know whether
// the consecutive segment will be walkable). Body offset still applies.
bool MovementClearAtPosition(const Vec3f& fromPos, const Vec3f& toPos, PlayState* play);

// Returns true if the segment from navigator to targetPos is clear of
// walls (navigator can "see" the target). Defined for completeness;
// reserved for future perception layer (target acquisition / loss
// detection). v1 has no consumer.
bool VisualLineOfSight(const Actor* navigator, const Vec3f& targetPos, PlayState* play);

} // namespace AnchorNav
