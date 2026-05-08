/**
 * RoomNavData — pre-scanned navigation graph per room.
 *
 * Layer 3 fallback for the multiplayer navigation system. When a navigator
 * has no fresh trail data and no direct line-to-target, this module
 * supplies a static reachable subgoal from the room's pre-scanned graph.
 *
 * Self-contained — registers GameInteractor hooks from a single .cpp,
 * no edits to pre-Flotilla source. Default-off; gated behind
 * gEnhancements.RoomNavData.Enabled (zero overhead when off).
 *
 * See:
 *   - Claude/Plans/room_nav_data_plan.md
 *   - Claude/Plans/nav_system_implementation_plan.md §18 (forward-pointer)
 *   - GitHub #207 (this tracker)
 *   - GitHub #206 (parent nav system tracker)
 */

#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include "z64.h"
}

namespace AnchorNavRoom {

// ---------------------------------------------------------------------------
// Per-node flags. Bitfield in NavNode::flags.
// ---------------------------------------------------------------------------

enum NodeFlags : uint16_t {
    NODE_NONE            = 0,
    NODE_WALKABLE        = 0x01, // floor exists, slope ≤ 2, no hazard, no water
    NODE_EDGE            = 0x02, // walkable AND adjacent to non-walkable cell (ledge)
    NODE_CLIMB_BASE      = 0x04, // co-located with a climbable surface entry point
    NODE_CLIMB_TOP       = 0x08, // top of a climbable surface
    NODE_HAZARD          = 0x10, // damaging/sticking surface (lava, hot floor, ice, deep sand, etc.)
    NODE_UNDERWATER      = 0x20, // submerged below water surface (swimming-capable navigators only)
    NODE_STEEP_SLOPE     = 0x40, // SurfaceType_GetSlope == 3, transient pass-through (slide-down recovery target)
    NODE_HAZARD_ADJACENT = 0x80, // walkable AND adjacent to a hazard cell (used for cautious pathing)
    NODE_ORPHANED        = 0x100, // walkable but no edges connect this node to any seed-rooted component
                                  // (stacked floor on top of wall/fence with no traversable path from seeds)
    NODE_CRAWLSPACE      = 0x200, // walkable but accessible only via crawlspace traversal (low-clearance
                                  // tunnel; navigator must be in PLAYER_STATE2_CRAWLING). Tagged by
                                  // proximity to a CrawlspaceAnchor. Only child-Link-rigged navigators
                                  // (AI Follower, AI Invader child variant) can use these — adult Link
                                  // and most enemies cannot crawl. Consumer-side gating per NavTraits.
};

// ---------------------------------------------------------------------------
// Per-room data structures. See plan §4 for full schema.
// ---------------------------------------------------------------------------

struct NavNode {
    Vec3f pos;
    uint16_t flags;      // bitfield of NodeFlags (widened to uint16 in schema v2 for NODE_ORPHANED)
    uint16_t cellIdxX;   // grid cell X (for spatial lookup)
    uint16_t cellIdxZ;   // grid cell Z
};

struct NavEdge {
    uint16_t fromIdx;    // index into RoomNavData::nodes
    uint16_t toIdx;
    float    cost;       // distance + slope penalty
};

struct ClimbAnchor {
    Vec3f basePos;
    Vec3f topPos;
    int16_t actorId;     // 0 if static-geometry (vine wall); nonzero if scene actor
};

// Ledge-grab anchor — distinct from ClimbAnchor in motion semantics. A
// climb anchor is something the navigator CLIMBS UP (engine attaches to
// ladder/vine, plays climb animation). A ledge anchor is something the
// navigator JUMPS UP onto and grabs at the rim — the engine then pulls
// the navigator up via the climb-up animation.
//
// Geometry: approachPos is a walkable node BELOW the ledge where the
// navigator stands to begin the jump. topPos is a walkable node above
// where the navigator ends up after the climb-up. A wall exists between
// the two (the ledge edge that gets grabbed).
//
// Consumer pattern (Phase 2): only Link-rigged navigators (AI Follower,
// future allied NPCs) can use ledge grab — it's an engine-level Link
// state, not generally available to enemies. NavTraits.eligibleForLedgeGrab
// gates per-navigator opt-in.
struct LedgeAnchor {
    Vec3f approachPos;   // walkable position below the ledge (jump from here)
    Vec3f topPos;        // walkable position on top after climb-up
};

// Crawlspace anchor — marks a crawlspace entrance (a wall poly with
// the crawlspace flag bit set; engine-detected at z_player.c:7639 via
// `interactWallFlags & 0x30`). Each entry is identified by a clustered
// position + the wall normal direction; an entry-exit pair is implicit
// via the fact that crawlspaces have walls on both ends, both
// independently registered as anchors. Consumers (Phase 2) pair them
// at navigation time by looking for matching anchors with opposing
// normals along the path.
//
// NavTraits gate (Phase 2): consumer must have `useCrawlspaces = true`
// to traverse these. Only child-Link-rigged navigators (AI Follower,
// AI Invader child variant) qualify; adult-rigged or non-Link
// navigators (most enemies, bosses) skip them.
struct CrawlspaceAnchor {
    Vec3f entryPos;      // approximate centroid of the crawlspace-flagged wall(s)
    Vec3f entryNormal;   // wall normal (direction OUT OF the wall — face this way to enter)
};

// Drop anchor — symmetric to ClimbAnchor / LedgeAnchor but for descent.
// A drop is a vertical traversal where a navigator standing at highPos
// can step off (or fall) and safely land at landingPos. Unlike a ledge
// anchor (where a wall blocks the line between approach and top), a
// drop anchor's high→low line is CLEAR — the navigator simply walks off
// the edge.
//
// Detection criteria:
//   - highPos.y - landingPos.y in [30, 200] (above step-up height,
//     within survivable fall distance for child Link)
//   - XZ distance within ~80u (drop is mostly downward; large lateral
//     drops are rare and require explicit jump mechanics)
//   - MovementClear from highPos to landingPos SUCCEEDS (no wall in
//     between — opposite of the ledge predicate)
//
// Consumer use: future autonomous navigators (AI Invader) descending
// from a high walkable area to a lower one. Pairs naturally with
// climb anchors for round-trip routes (climb up here → drop down
// there). NavTraits gates per-navigator opt-in.
struct DropAnchor {
    Vec3f highPos;       // walkable position at the top edge
    Vec3f landingPos;    // walkable position where navigator lands
};

struct RoomNavData {
    // Header
    uint32_t magic = 0x52564E41; // 'RNAV' little-endian — file-format identifier
    uint16_t version = 1;        // kCurrentSchemaVersion at scan time
    int16_t  sceneNum = -1;
    int8_t   roomNum = -1;
    uint32_t scanTimestamp = 0;  // wall-clock seconds, for debug

    // Geometry
    std::vector<NavNode>          nodes;
    std::vector<NavEdge>          edges;
    std::vector<ClimbAnchor>      climbAnchors;
    std::vector<LedgeAnchor>      ledgeAnchors;       // schema v3+
    std::vector<CrawlspaceAnchor> crawlspaceAnchors;  // schema v4+
    std::vector<DropAnchor>       dropAnchors;        // schema v5+
    // Historical seed positions accumulated across scans of this room
    // (schema v6+). Each entry is a world-space position used as a
    // floodfill seed in some prior scan. On rescan, these positions
    // are re-used as additional seeds — preserves coverage even when
    // the actors that originally seeded an area have died or
    // despawned. Deduplicated by grid cell; capped at ~1000 entries.
    std::vector<Vec3f>            historicalSeeds;
    std::vector<Vec3f>            hazardCentroids;

    // Scan metadata
    Vec3f bboxMin = { 0.0f, 0.0f, 0.0f };
    Vec3f bboxMax = { 0.0f, 0.0f, 0.0f };
    uint16_t gridResolution = 30; // matches kGridResolution at scan time

    // Diagnostic: positions of floor candidates rejected by the per-actor
    // allowlist (FloorIsRejectedByAllowlist). Populated only when
    // gEnhancements.RoomNavData.LogRejectedFloors is on at scan time;
    // empty otherwise. Used by the DebugDraw overlay (magenta '+' crosses)
    // to tune the rejection list as new scene-furniture surfaces.
    //
    // NOT persisted — SaveToDisk and TryLoadFromDisk skip this field. A
    // disk-loaded RoomNavData has an empty vector; the user re-scans
    // (Force Rescan) to populate.
    std::vector<Vec3f> rejectedFloorPositions; // not persisted
};

// ---------------------------------------------------------------------------
// Public API. Stubbed in commit 1; populated incrementally in subsequent
// Phase 1 commits.
// ---------------------------------------------------------------------------

// Returns the in-memory RoomNavData for the given (scene, room), or nullptr
// if not loaded. Callers can treat nullptr as "no static graph available;
// fall through to other nav layers."
const RoomNavData* GetForRoom(int16_t sceneNum, int8_t roomNum);

// Returns the index of the node nearest to `pos` within `data->nodes`.
// Returns -1 if data is null or has no nodes.
int FindNearestNode(const RoomNavData* data, const Vec3f& pos);

// Returns the index of the node closest to `targetPos` that's reachable
// from `fromIdx` via graph edges, with hazard-aware filtering.
//
// Parameters:
//   `eligibleForSwimming`: when false, NODE_UNDERWATER nodes are rejected
//     as both pass-through and destination. When true (Link-rigged
//     navigators: AI Follower, NPC Invader), underwater nodes are valid.
//   `avoidHazardNodes`: when true (default for most navigators), the BFS
//     limits hazard-node traversal to kHazardEscapeHops (= 2) — a path
//     that crosses 1-2 hazard cells before exiting is fine, but deeper
//     hazard exposure causes the path to be rejected. Hazard nodes are
//     also excluded as destinations. When false (heat-resistant
//     navigators), hazard nodes are valid pass-through and destination
//     and the hop-counter rejection short-circuits.
//
// Returns -1 when:
//   - no reachable node beats fromIdx's distance to target (no progress
//     possible — target is unreachable through the graph), or
//   - the navigator is cornered in hazard with no non-hazard exit
//     reachable within the hop budget.
//
// In both cases the caller falls through to direct yaw / the next nav
// layer.
//
// Hazard-aware BFS per Plans/room_nav_data_plan.md §10. NODE_ORPHANED
// and NODE_STEEP_SLOPE always rejected as destinations.
int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int fromIdx,
                                  const Vec3f& targetPos,
                                  bool eligibleForSwimming = false,
                                  bool avoidHazardNodes    = true);

// Cornered-in-hazard fallback. Returns the nearest non-hazard walkable
// node reachable from `fromIdx`, ignoring target direction — exit is the
// only goal. Used by FindBestReachableSubgoalNode when the hazard-hop
// accumulator rejects every otherwise-suitable destination. Honors
// `eligibleForSwimming` (rejects NODE_UNDERWATER when false). Returns -1
// when the navigator is fully surrounded by hazard with no reachable
// non-hazard exit anywhere in the room.
int FindNearestNonHazardExit(const RoomNavData* data,
                              int fromIdx,
                              bool eligibleForSwimming = false);

// Path-returning sibling of FindBestReachableSubgoalNode. Same hazard-aware
// BFS, but records predecessor pointers and reconstructs the full chain of
// node positions from `fromIdx` to the chosen `bestIdx`. The path EXCLUDES
// `fromIdx` itself (navigator is already there) and includes every node up
// to and including `bestIdx`. Output ordered first-to-last along the path,
// so consumers walk waypoint[0] → waypoint[N-1].
//
// Returns true on a non-empty path; false when no reachable destination
// improves on `fromIdx`'s distance to target (consumer falls through to
// direct yaw / next nav layer). `out` is cleared before append.
//
// Cost: same BFS as FindBestReachableSubgoalNode plus a parents array
// (~2 KB for a 1100-node room) and a reverse-walk pass (worst-case ~30
// iterations for typical room paths).
bool FindBestReachableSubgoalPath(const RoomNavData* data,
                                   int fromIdx,
                                   const Vec3f& targetPos,
                                   bool eligibleForSwimming,
                                   bool avoidHazardNodes,
                                   std::vector<Vec3f>& out);

// True when both gEnhancements.RoomNavData.Enabled is on AND the system
// has been initialized. Used as the master gate by all Phase 1+2 features.
bool IsEnabled();

// True if there's a path through the nav graph from `fromPos` to
// `toPos`. BFS over the existing edges from the node nearest to
// `fromPos`; checks whether the node nearest to `toPos` is in the
// reachable set.
//
// Phase 1: simple yes/no reachability (no movement-mode awareness).
// Doesn't currently distinguish climb/ledge/crawlspace anchors —
// reachability via those requires consumer-side traversal logic that
// isn't part of the basic graph. Phase 2 may extend this with edge
// type tagging for state-aware reachability.
//
// Use case: AI directors validating that a proposed spawn position
// can reach the player before committing to spawn. Returns false for
// orphaned regions (NODE_ORPHANED nodes are excluded from BFS).
//
// Cost: O(N + E) per query. Rooms typically have ~1100 nodes and
// ~3500 edges, so a query runs in ~5-15ms. Suitable for one-shot
// spawn validation; cache results if calling repeatedly.
bool IsReachable(const RoomNavData* data, const Vec3f& fromPos, const Vec3f& toPos);

// Drops the cached nav data for the current (scene, room) — both the
// in-memory cache entry and the on-disk .bin if present — and re-triggers
// a scan on the next frame. Useful for testing scan-algorithm changes
// without restarting the session, and for refreshing rooms whose cached
// graph predates a scan-quality fix.
//
// No-op if gPlayState is null, or if RoomNavData.AutoScan is off (the
// disk file is still removed in that case so a future enable+re-entry
// regenerates).
void ForceRescanCurrentRoom();

} // namespace AnchorNavRoom
