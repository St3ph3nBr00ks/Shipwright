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

struct RoomNavData {
    // Header
    uint32_t magic = 0x52564E41; // 'RNAV' little-endian — file-format identifier
    uint16_t version = 1;        // kCurrentSchemaVersion at scan time
    int16_t  sceneNum = -1;
    int8_t   roomNum = -1;
    uint32_t scanTimestamp = 0;  // wall-clock seconds, for debug

    // Geometry
    std::vector<NavNode>     nodes;
    std::vector<NavEdge>     edges;
    std::vector<ClimbAnchor> climbAnchors;
    std::vector<Vec3f>       hazardCentroids;

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
// from `fromIdx` via graph edges. -1 if no reachable progress node exists.
// v1 implementation: closest-reachable lookup, no A* path search.
int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int fromIdx,
                                  const Vec3f& targetPos);

// True when both gEnhancements.RoomNavData.Enabled is on AND the system
// has been initialized. Used as the master gate by all Phase 1+2 features.
bool IsEnabled();

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
