/**
 * ActorTrail — per-entity ring buffer of recent positions.
 *
 * Foundation for the navigation system's "breadcrumb" pursuit: when a
 * navigator loses line-of-sight to its target, it can fall back to the
 * trail of recent positions the target left behind. Players are the
 * primary trailed entities, but synced enemies and AI Player Followers also
 * leave trails when their NavTraits.leavesTrail is true — see
 * feedback_nav_helpers_entity_agnostic.md.
 *
 * Storage: std::unordered_map<TrailKey, EntityTrail>. TrailKey is a
 * tagged uint32 — high bit = player (low byte is clientId);
 * high bit clear = actor (low 31 bits are netId).
 *
 * Sampling rate: 3 captures/sec/entity wall-clock (kCaptureIntervalMs=333).
 * Buffer size: 30 waypoints per entity (~10 s of history wall-clock
 * before the oldest is overwritten). Frame-rate-independent — SoH's
 * game logic ticks at 20 Hz vanilla (interpolation handles render),
 * so per-frame throttles drift; wall-clock throttle keeps the rate
 * consistent regardless of game-logic FPS or VirtualBox slowdowns.
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
    uint64_t captureMs;   // wall-clock ms since program start at capture time
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

    // Low-level: fetch a specific waypoint by wall-clock age. Used by
    // debug tooling and callers that need explicit lag control. Returns
    // the captured waypoint that's closest to (but no younger than)
    // `msAgo` milliseconds before now.
    bool GetWaypointBefore(TrailKey key, uint32_t msAgo, TrailWaypoint& out) const;

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
        std::vector<Vec3f>    waypoints;       // [0] = next subgoal; [N-1] = path tail
        // Per-waypoint NodeFlags bitmap; same length as `waypoints`.
        // Low 16 bits mirror the source NavNode's natural flags (used
        // for NODE_CLIMB_* type bit checks). Upper 16 bits carry
        // synthetic-only flags set by the path planner — currently
        // NODE_REACHED_VIA_LEDGE_GRAB (0x00010000). Widened from
        // uint16_t to uint32_t 2026-05-12 PM when the ledge synthetic
        // exhausted the lower 16-bit space (NODE_DROP_FROM_ABOVE
        // already occupied 0x8000).
        //
        // Layer 1 (LOS direct-to-target) and Layer 2 (trail
        // breadcrumbs) emit waypoints with `flags = 0`; Layer 3
        // (RoomNavData BFS) copies the source NavNode flags + ORs
        // synthetic flags for drop/ledge edges traversed.
        std::vector<uint32_t> waypointFlags;
        // Climb-surface mask the BFS used when this path was computed
        // (NavTraits.climbSurfaceMask snapshot). Consumer re-AND's at
        // engage time so a mid-session CVar flip can be detected
        // defensively. 0 = no climb capability at compute time.
        uint16_t computedClimbMask = 0;
        size_t   cursorIdx          = 0;       // index of current active waypoint
        int16_t  sceneNum           = -1;      // scene at capture time
        uint64_t msAtCapture        = 0;       // wall-clock ms; for diagnostics + max-age policy
        Vec3f    capturedTargetPos  = { 0, 0, 0 };  // for "target moved" invalidation

        void Reset() {
            waypoints.clear();
            waypointFlags.clear();
            computedClimbMask = 0;
            cursorIdx = 0;
            sceneNum = -1;
        }
        bool Empty() const { return waypoints.empty() || cursorIdx >= waypoints.size(); }
        const Vec3f& CurrentSubgoal() const { return waypoints[cursorIdx]; }
        // Per-waypoint flags lookup. Returns 0 when the parallel array
        // hasn't been populated (older callers / Layer 1+2 paths). Safe
        // to call even when `waypointFlags.size() != waypoints.size()`.
        uint32_t CurrentSubgoalFlags() const {
            return (cursorIdx < waypointFlags.size()) ? waypointFlags[cursorIdx] : (uint32_t)0;
        }
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
    //
    // `skipLayer1LOS` forces Layer 1 (direct LOS reachability) to be
    // skipped even when MovementClear succeeds. Use when the caller knows
    // LOS is unreliable for the target — e.g. door-handoff targets across
    // complex collision where MovementClear's pelvis-line passes over
    // short walls / through narrow gaps that the follower can't actually
    // walk through. Forces fallback to Layer 3 (RoomNavData BFS) which
    // gates edges on MovementClearAtPosition + step-up at scan time.
    // (User 2026-05-10 follow-up to Bug 2 fix 1: door-handoff substrate
    // routing was returning pathLen=1 LOS through walls; follower walked
    // into wall, STUCK accumulated, G12 teleport fired.)
    //
    // `preferLeaderTrail` reorders Layer 2 (trail breadcrumbs) AHEAD of
    // Layer 3 (RoomNavData BFS) for this call. Use when the caller has
    // strong evidence the trail is the right path — specifically the
    // "leader exited our room, follow their breadcrumbs to the
    // boundary" case where the leader's recent waypoints in our room
    // ARE the walkable path to wherever they went next. The Layer 2
    // "MovementClear over short walls" weakness doesn't apply when the
    // breadcrumbs come from a player who actually walked them. Pair
    // with `key = TrailKeyForPlayer(leaderClientId)` and `targetPos =
    // leader's current position` (which may be in a different room —
    // Layer 2 finds the most-progress reachable breadcrumb anyway).
    // (User 2026-05-10: "open" room boundaries with no transition-actor
    // door — leader-trail-as-path bypasses the door-detection logic
    // entirely.)
    bool ComputePathTo(TrailKey key,
                       const Actor* navigator,
                       const Vec3f& targetPos,
                       PlayState* play,
                       NavPath& out,
                       bool skipLayer1LOS = false,
                       bool preferLeaderTrail = false) const;

    // Find any captured waypoint of `key`'s trail that lies within
    // `maxGapDistance` of `referencePos` AND closer to `targetPos` than
    // `referencePos` is. Used by JumpResolver to detect when a target's
    // own breadcrumbs offer evidence the gap was crossed (target was
    // there → there's likely walkable ground there → navigator can
    // jump there too).
    //
    // Walks newest→oldest. The first matching waypoint wins (newest =
    // freshest evidence of where target went). Stale waypoints (>12s
    // old, matching GetBestReachableSubgoal's filter) are excluded.
    // Returns true and populates `outLanding` on hit; false otherwise.
    bool FindTrailWaypointBeyondGap(TrailKey key,
                                     const Vec3f& referencePos,
                                     float maxGapDistance,
                                     const Vec3f& targetPos,
                                     Vec3f& outLanding) const;

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

    // Diagnostic: snapshot every computed path (one per TrailKey) whose
    // sceneNum matches `sceneFilter` into `out`. Populated by
    // ComputePathTo on every successful path compute — the most recent
    // path per key replaces any prior. Used by the RoomNavData
    // DebugDrawPaths overlay so the overlay doesn't need access to
    // per-actor consumer state. Output is cleared before append.
    struct ComputedPathSnapshot {
        TrailKey            key;
        std::vector<Vec3f>  waypoints;   // waypoints[cursorIdx..end] = remaining path
        size_t              cursorIdx;
        int16_t             sceneNum;
    };
    void SnapshotComputedPaths(int16_t sceneFilter,
                                std::vector<ComputedPathSnapshot>& out) const;

    // Lifecycle.
    void ClearForKey(TrailKey key);
    void ClearForScene(int16_t sceneNum);
    void ClearAll();

    // True when the master Nav CVar is on AND Nav.ActorTrail is on.
    static bool IsEnabled();

private:
    ActorTrail() = default;

    static constexpr size_t   kMaxWaypoints      = 30;    // ring buffer size per entity
    static constexpr uint64_t kCaptureIntervalMs = 333;   // 3 Hz wall-clock throttle
    static constexpr uint64_t kStaleAgeMs        = 12000; // 12 s — slight margin > buffer
                                                          // duration (30 × 333ms ≈ 10 s)

    struct EntityTrail {
        std::array<TrailWaypoint, kMaxWaypoints> waypoints;
        size_t head = 0;       // next slot to write
        size_t count = 0;      // number of valid entries
        uint64_t lastCaptureMs = 0;
    };

    std::unordered_map<TrailKey, EntityTrail> mTrails;
    uint64_t mNowMs           = 0;  // wall-clock ms at most-recent Tick
    uint64_t mLastCaptureMs   = 0;  // wall-clock ms at most-recent capture

    void CaptureWaypoint(TrailKey key, const Vec3f& pos, int16_t sceneNum,
                         int8_t roomNum, uint8_t timeline);

    // Returns wall-clock milliseconds since process start (steady_clock).
    static uint64_t NowMs();
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
