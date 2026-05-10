/**
 * RoomNavData — pre-scanned navigation graph per room.
 *
 * Commit 1: skeleton + master CVar wiring. Defines the public API and
 * structures, registers the GameInteractor hook, and gates everything
 * behind gEnhancements.RoomNavData.Enabled. No scan / load / save logic
 * yet; those land in commits 2-8.
 *
 * See:
 *   - Claude/Plans/room_nav_data_plan.md
 *   - GitHub #207 (this tracker)
 */

#include "RoomNavData.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

// frame_interpolation.h declares the FrameInterpolation_Record{Open,Close}Child
// helpers with extern "C" linkage. The OPEN_DISPS / CLOSE_DISPS macros in
// soh/include/macros.h embed an inline forward-declaration WITHOUT the
// extern "C" qualifier, so when the macro expands inside a `namespace
// AnchorNavRoom { ... }` block the block-scope decl introduces the name
// as a C++-mangled symbol — defeating the global extern "C" decl from
// the header and producing an unresolved-external link error against the
// real C-linkage definition. Confining the macro expansion to a
// file-scope helper at global namespace (see RoomNavSpliceXluDispList
// below) sidesteps the issue.
#include "soh/frame_interpolation.h"

// Phase 2 commit 11 — slope-3 stuck-on-slope diagnostic. Iterates the
// syncable-actor categories per frame, reads the EnemyNetId extension,
// and updates the per-navigator stuck-on-slope counter.
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/Common/ActorTrail.h"  // breadcrumb overlay (DebugDraw)
#include "soh/ObjectExtension/ObjectExtension.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h" // declares extern SaveContext gSaveContext (variables.h:189)
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------------
// CVar definitions — see plan §3 for full layout.
// ---------------------------------------------------------------------------

#define CVAR_ROOM_NAV_ENABLED               CVAR_ENHANCEMENT("RoomNavData.Enabled")
#define CVAR_ROOM_NAV_AUTO_SCAN             CVAR_ENHANCEMENT("RoomNavData.AutoScan")
#define CVAR_ROOM_NAV_DEBUG_DRAW            CVAR_ENHANCEMENT("RoomNavData.DebugDraw")
#define CVAR_ROOM_NAV_LOG_STUCK_ON_SLOPE    CVAR_ENHANCEMENT("RoomNavData.LogStuckOnSlope")
#define CVAR_ROOM_NAV_DEBUG_DRAW_COMPONENTS CVAR_ENHANCEMENT("RoomNavData.DebugDrawComponents")
#define CVAR_ROOM_NAV_LOG_REJECTED_FLOORS   CVAR_ENHANCEMENT("RoomNavData.LogRejectedFloors")
#define CVAR_ROOM_NAV_PATH_B_CLIMB          CVAR_ENHANCEMENT("RoomNavData.PathBClimbDetection")
#define CVAR_ROOM_NAV_LEDGE_GRAB            CVAR_ENHANCEMENT("RoomNavData.LedgeGrabDetection")
#define CVAR_ROOM_NAV_LEDGE_MAX_DELTA       CVAR_ENHANCEMENT("RoomNavData.LedgeGrabMaxDeltaY")
#define CVAR_ROOM_NAV_PATH_B_DEBUG          CVAR_ENHANCEMENT("RoomNavData.PathBDebugLogWallTypes")
#define CVAR_ROOM_NAV_AUTO_REFRESH_ANCHORS  CVAR_ENHANCEMENT("RoomNavData.AutoRefreshAnchorsOnSceneFlag")
#define CVAR_ROOM_NAV_AUTO_FULL_RESCAN      CVAR_ENHANCEMENT("RoomNavData.AutoFullRescanOnSceneFlag")
#define CVAR_ROOM_NAV_CRAWLSPACE            CVAR_ENHANCEMENT("RoomNavData.CrawlspaceDetection")
#define CVAR_ROOM_NAV_DROP_ANCHOR           CVAR_ENHANCEMENT("RoomNavData.DropAnchorDetection")
#define CVAR_ROOM_NAV_INITIAL_SCAN_DELAY    CVAR_ENHANCEMENT("RoomNavData.InitialScanDelayFrames")
#define CVAR_ROOM_NAV_AUTO_EXPAND           CVAR_ENHANCEMENT("RoomNavData.AutoExpandOnExploration")

// File-scope helper at global namespace. OPEN_DISPS / CLOSE_DISPS
// expand to a block containing an inline forward-declaration of
// FrameInterpolation_Record{Open,Close}Child without an extern "C"
// qualifier; that block-scope declaration would shadow the C-linkage
// declaration from frame_interpolation.h when the macro expands inside
// a C++ namespace block (yielding LNK2001 against the C++-mangled
// symbol). Keeping the splice at global scope preserves the C-linkage
// resolution. Forward-declared in the namespace via the `::`-qualified
// call site below.
static void RoomNavSpliceXluDispList(PlayState* play, Gfx* dl) {
    OPEN_DISPS(play->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, dl);
    CLOSE_DISPS(play->state.gfxCtx);
}

namespace AnchorNavRoom {

// ---------------------------------------------------------------------------
// Master gate. Every per-feature query short-circuits to vanilla behaviour
// when this returns false. Zero overhead when disabled.
// ---------------------------------------------------------------------------

bool IsEnabled() {
    return CVarGetInteger(CVAR_ROOM_NAV_ENABLED, 0) != 0;
}

static bool IsAutoScanEnabled() {
    // AutoScan defaults ON when master is on — see plan §3.
    return CVarGetInteger(CVAR_ROOM_NAV_AUTO_SCAN, 1) != 0;
}

// ---------------------------------------------------------------------------
// In-memory cache. Survives within a session. Keyed on packed (scene, room)
// uint32 — high 16 bits = sceneNum, low 8 bits = roomNum. Negative values
// are clamped to 0xFFFF / 0xFF (sentinel; matches how OoT signals invalid
// scene/room in transient state).
// ---------------------------------------------------------------------------

static uint32_t MakeCacheKey(int16_t sceneNum, int8_t roomNum) {
    uint32_t scenePart = (uint32_t)((uint16_t)sceneNum) << 16;
    uint32_t roomPart  = (uint32_t)((uint8_t)roomNum) & 0xFF;
    return scenePart | roomPart;
}

static std::unordered_map<uint32_t, RoomNavData> sCache;

// ---------------------------------------------------------------------------
// Connected-component cache (DebugDraw v3d, polish wave commit 5). Computed
// lazily on first DebugDraw render of a given room and cached per
// MakeCacheKey(scene, room). The vector is parallel to RoomNavData::nodes
// — sComponentCache[key][i] = canonical component-root index for node i.
//
// Invalidation:
//   - OnExitGameClear() clears the entire cache (alongside sCache).
//   - ForceRescanCurrentRoom() erases the entry for the current room.
//
// NOT persisted; transient debug-only state. Memory cost is ~2 bytes per
// node, capped by the underlying nav graph's size.
// ---------------------------------------------------------------------------
static std::unordered_map<uint32_t, std::vector<uint16_t>> sComponentCache;

// ---------------------------------------------------------------------------
// Public API. GetForRoom now returns from the in-memory cache. Subsequent
// commits add disk-cache load (commit 7) and scan-and-persist (commits 3-7).
// ---------------------------------------------------------------------------

const RoomNavData* GetForRoom(int16_t sceneNum, int8_t roomNum) {
    auto it = sCache.find(MakeCacheKey(sceneNum, roomNum));
    if (it == sCache.end()) {
        return nullptr;
    }
    return &it->second;
}

int FindNearestNode(const RoomNavData* data, const Vec3f& pos) {
    if (data == nullptr || data->nodes.empty()) {
        return -1;
    }
    // v1: linear search across all nodes. ~1,100 nodes per average room
    // → ~1,100 distance calcs per call. Steady-state cost is bounded;
    // callers (subgoal selection) cache results between target moves so
    // this fires once per ~30 frames per navigator. Spatial-index
    // optimization (cell-bucket lookup) deferred until profiling shows
    // need.
    //
    // Climb-surface nodes (schema v7+) are skipped — every existing
    // caller wants a FLOOR node nearest to a world position (Layer 3
    // BFS entry, climb-base/top tagging, IsReachable endpoints). Climb-
    // surface nodes sit on walls at arbitrary altitudes and would
    // produce wrong-class hits when a navigator stands close to a
    // climbable wall. Stage 6 of climb_surface_nav_grid_plan will add a
    // separate FindNearestClimbSurfaceNode helper for the climb-aware
    // path-engagement check.
    int bestIdx = -1;
    float bestDistSq = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < data->nodes.size(); i++) {
        const NavNode& n = data->nodes[i];
        if (n.flags & NODE_CLIMB_ANY) continue;
        float dx = n.pos.x - pos.x;
        float dy = n.pos.y - pos.y;
        float dz = n.pos.z - pos.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestIdx = (int)i;
        }
    }
    return bestIdx;
}

// Threshold for hazard-traversal rejection — see plan §10 for the
// derivation. A path that crosses 1-2 hazard cells before exiting is
// fine; deeper hazard exposure causes that branch of the search to be
// abandoned. Per-actor override (NavTraits.hazardEscapeHops) deferred
// until evidence of need.
static constexpr uint8_t kHazardEscapeHops = 2;

// Helper. Builds a bidirectional adjacency list from data->edges. Each
// NavEdge stored once but traversable both ways. Used by the hazard-aware
// BFS and FindNearestNonHazardExit.
static std::vector<std::vector<uint16_t>>
BuildAdjacencyList(const RoomNavData* data) {
    std::vector<std::vector<uint16_t>> adjacency(data->nodes.size());
    for (const NavEdge& e : data->edges) {
        if (e.fromIdx >= data->nodes.size() || e.toIdx >= data->nodes.size()) continue;
        adjacency[e.fromIdx].push_back(e.toIdx);
        adjacency[e.toIdx].push_back(e.fromIdx);
    }
    // P3.8 (user 2026-05-09 — "AI Follower is not targeting ladders
    // and climbable surfaces as part of its pathfinding"): inject
    // bidirectional edges between each climb anchor's base and top
    // nodes. The floodfill edge graph only contains step-up-allowed
    // grid neighbours; climb anchors (vine walls, ladders, gameplay-
    // relevant climbable surfaces) require a vertical traversal that
    // floodfill never connected. Adding base↔top edges lets the BFS
    // route through climbable surfaces just like ordinary ground
    // edges.
    //
    // Consumers (AI Follower / AI Invader) must recognise that a
    // path waypoint near a NODE_CLIMB_BASE precedes a vertical
    // traversal and engage their climb pipeline (Shape A for the
    // follower; Shape B for non-Link navigators). Without that,
    // the consumer paths to the base and stalls because stick
    // injection alone can't cross the wall the vines/ladder are
    // mounted on. Follow-up commit wires the consumer side.
    for (const ClimbAnchor& anchor : data->climbAnchors) {
        // Find base / top node indices the same way the scan
        // populates NODE_CLIMB_BASE / NODE_CLIMB_TOP flags. Using
        // FindNearestNode here matches that flagging convention.
        int baseIdx = FindNearestNode(data, anchor.basePos);
        int topIdx  = FindNearestNode(data, anchor.topPos);
        if (baseIdx < 0 || topIdx < 0) continue;
        if (baseIdx == topIdx) continue;
        if ((size_t)baseIdx >= adjacency.size() ||
            (size_t)topIdx  >= adjacency.size()) continue;
        adjacency[(size_t)baseIdx].push_back((uint16_t)topIdx);
        adjacency[(size_t)topIdx].push_back((uint16_t)baseIdx);
    }
    return adjacency;
}

int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int fromIdx,
                                  const Vec3f& targetPos,
                                  bool eligibleForSwimming,
                                  bool avoidHazardNodes) {
    if (data == nullptr || data->nodes.empty() || data->edges.empty()) {
        return -1;
    }
    if (fromIdx < 0 || (size_t)fromIdx >= data->nodes.size()) {
        return -1;
    }

    // Hazard-aware BFS per room_nav_data_plan.md §10. The frontier carries
    // a per-entry "consecutive hazard hops" accumulator alongside the node
    // index; candidates exceeding kHazardEscapeHops are rejected. This is
    // cheaper than running a separate hazard-recovery predicate and
    // produces the same emergent three-way behaviour:
    //
    //   1. Hazard surrounds navigator, target on opposite side: target's
    //      path crosses hazard for many hops → diverted to nearest exit
    //      via FindNearestNonHazardExit fallback.
    //   2. Navigator on hazard edge, target right next to hazard: target's
    //      path exits within 1-2 hops → continues toward target.
    //   3. Navigator on hazard, no non-hazard reachable: best-effort
    //      fallback returns -1; caller falls through to direct yaw.
    //
    // NODE_ORPHANED and NODE_STEEP_SLOPE always rejected (orphan = not
    // graph-connected to seed; steep-slope = transient pass-through, never
    // a valid destination).

    auto distSqToTarget = [&](const Vec3f& p) {
        float dx = p.x - targetPos.x;
        float dy = p.y - targetPos.y;
        float dz = p.z - targetPos.z;
        return dx * dx + dy * dy + dz * dz;
    };

    // Strict-improvement filter: chosen subgoal must improve distance-to-
    // target over the navigator's starting node. Without this, an
    // unreachable target (BFS can't cross to it) would trivially return
    // fromIdx itself and the navigator would freeze in place.
    const float fromDistSq = distSqToTarget(data->nodes[(size_t)fromIdx].pos);

    struct QueueEntry {
        uint16_t idx;
        uint8_t  hopsInHazard;  // running count of consecutive hazard hops
    };

    std::vector<std::vector<uint16_t>> adjacency = BuildAdjacencyList(data);
    std::vector<bool> visited(data->nodes.size(), false);
    std::deque<QueueEntry> frontier;
    visited[(size_t)fromIdx] = true;
    frontier.push_back({(uint16_t)fromIdx, 0});

    int   bestIdx    = -1;
    float bestDistSq = fromDistSq;  // strict improvement required

    while (!frontier.empty()) {
        QueueEntry entry = frontier.front();
        frontier.pop_front();

        const NavNode& node = data->nodes[entry.idx];

        // Walkability + always-reject filters.
        if (!(node.flags & NODE_WALKABLE)) continue;
        if (node.flags & (NODE_ORPHANED | NODE_STEEP_SLOPE)) continue;

        // Underwater filter: navigators not eligible for swimming reject
        // these as both pass-through and destination.
        if ((node.flags & NODE_UNDERWATER) && !eligibleForSwimming) continue;

        // Hazard-hop accumulator: increment on hazard nodes, reset on
        // clear nodes. The hop count for the CURRENT node is one more
        // than the path that brought us here when this node is hazard,
        // or zero when it's clear.
        const bool isHazard = (node.flags & NODE_HAZARD) != 0;
        uint8_t curHazardHops = isHazard ? (uint8_t)(entry.hopsInHazard + 1) : 0;

        // Reject branches that would exceed the hazard-hop budget. Honors
        // avoidHazardNodes — heat-resistant navigators (avoidHazardNodes
        // false) skip the rejection so hazard traversal is unbounded.
        if (avoidHazardNodes && curHazardHops > kHazardEscapeHops) {
            continue;
        }

        // Destination filter: when avoidHazardNodes, hazard nodes are
        // valid pass-through (within budget) but never valid destinations.
        // When not avoiding hazards, any walkable node qualifies.
        const bool destinationOK = !isHazard || !avoidHazardNodes;
        if (destinationOK) {
            float d = distSqToTarget(node.pos);
            if (d < bestDistSq) {
                bestDistSq = d;
                bestIdx    = (int)entry.idx;
            }
        }

        // Expand neighbours.
        for (uint16_t nb : adjacency[entry.idx]) {
            if (nb >= visited.size()) continue;
            if (visited[nb]) continue;
            // Don't traverse THROUGH orphan nodes (defensive belt; the
            // orphan-detection pass should've severed orphan↔seed edges).
            if (data->nodes[nb].flags & NODE_ORPHANED) continue;
            visited[nb] = true;
            frontier.push_back({nb, curHazardHops});
        }
    }

    if (bestIdx >= 0) return bestIdx;

    // Cornered fallback: every reachable destination either failed the
    // strict-improvement filter (target genuinely unreachable through the
    // graph) OR was rejected by the hazard-hop budget. The latter case
    // means the navigator is in/near hazard and target is on the other
    // side. Try FindNearestNonHazardExit which ignores target direction
    // and just gets the navigator off hazard. The former case (target
    // unreachable) also produces a useful exit fallback if the navigator
    // happens to be on hazard, but is otherwise harmless because the
    // exit will likely be fromIdx itself or an immediate non-hazard
    // neighbour — the caller's MovementClear gate filters out trivial
    // self-pointing returns.
    if (avoidHazardNodes) {
        return FindNearestNonHazardExit(data, fromIdx, eligibleForSwimming);
    }
    return -1;
}

int FindNearestNonHazardExit(const RoomNavData* data,
                              int fromIdx,
                              bool eligibleForSwimming) {
    if (data == nullptr || data->nodes.empty() || data->edges.empty()) {
        return -1;
    }
    if (fromIdx < 0 || (size_t)fromIdx >= data->nodes.size()) {
        return -1;
    }

    // Plain unfiltered BFS — first non-hazard walkable node encountered
    // wins. No target-direction bias; exit IS the goal. Honors swimming
    // eligibility so non-swimmers don't get pointed at an underwater
    // exit.
    std::vector<std::vector<uint16_t>> adjacency = BuildAdjacencyList(data);
    std::vector<bool> visited(data->nodes.size(), false);
    std::deque<uint16_t> q;
    visited[(size_t)fromIdx] = true;
    q.push_back((uint16_t)fromIdx);

    while (!q.empty()) {
        uint16_t cur = q.front();
        q.pop_front();

        const NavNode& node = data->nodes[cur];
        if (!(node.flags & NODE_WALKABLE)) continue;
        if (node.flags & (NODE_ORPHANED | NODE_STEEP_SLOPE)) continue;

        const bool isHazard = (node.flags & NODE_HAZARD) != 0;
        if (!isHazard) {
            // Found a non-hazard candidate. Validate eligibility before
            // returning — non-swimmers can't use an underwater exit.
            if ((node.flags & NODE_UNDERWATER) && !eligibleForSwimming) {
                // Underwater non-hazard but navigator can't swim — skip,
                // keep searching.
            } else {
                return (int)cur;
            }
        }

        for (uint16_t nb : adjacency[cur]) {
            if (nb >= visited.size()) continue;
            if (visited[nb]) continue;
            if (data->nodes[nb].flags & NODE_ORPHANED) continue;
            visited[nb] = true;
            q.push_back(nb);
        }
    }

    // Fully surrounded by hazard with no reachable non-hazard exit, or
    // exits all behind the swimming gate.
    return -1;
}

bool FindBestReachableSubgoalPath(const RoomNavData* data,
                                   int fromIdx,
                                   const Vec3f& targetPos,
                                   bool eligibleForSwimming,
                                   bool avoidHazardNodes,
                                   std::vector<Vec3f>& out,
                                   std::vector<uint16_t>* outFlags) {
    out.clear();
    if (outFlags) outFlags->clear();
    if (data == nullptr || data->nodes.empty() || data->edges.empty()) return false;
    if (fromIdx < 0 || (size_t)fromIdx >= data->nodes.size()) return false;

    // Same hazard-aware BFS as FindBestReachableSubgoalNode but with a
    // parents array so we can reconstruct the full path from `bestIdx`
    // back to `fromIdx`. Caller wants the materialised chain so they can
    // copy it into their own state and walk it without re-querying the
    // graph each frame.

    auto distSqToTarget = [&](const Vec3f& p) {
        float dx = p.x - targetPos.x;
        float dy = p.y - targetPos.y;
        float dz = p.z - targetPos.z;
        return dx * dx + dy * dy + dz * dz;
    };

    const float fromDistSq = distSqToTarget(data->nodes[(size_t)fromIdx].pos);

    struct QueueEntry { uint16_t idx; uint8_t hopsInHazard; };
    std::vector<std::vector<uint16_t>> adjacency = BuildAdjacencyList(data);
    std::vector<bool> visited(data->nodes.size(), false);
    std::vector<int>  parents(data->nodes.size(), -1);
    std::deque<QueueEntry> frontier;
    visited[(size_t)fromIdx] = true;
    frontier.push_back({(uint16_t)fromIdx, 0});

    int   bestIdx    = -1;
    float bestDistSq = fromDistSq;

    while (!frontier.empty()) {
        QueueEntry entry = frontier.front();
        frontier.pop_front();

        const NavNode& node = data->nodes[entry.idx];
        if (!(node.flags & NODE_WALKABLE)) continue;
        if (node.flags & (NODE_ORPHANED | NODE_STEEP_SLOPE)) continue;
        if ((node.flags & NODE_UNDERWATER) && !eligibleForSwimming) continue;

        const bool isHazard = (node.flags & NODE_HAZARD) != 0;
        uint8_t curHazardHops = isHazard ? (uint8_t)(entry.hopsInHazard + 1) : 0;
        if (avoidHazardNodes && curHazardHops > kHazardEscapeHops) continue;

        const bool destinationOK = !isHazard || !avoidHazardNodes;
        if (destinationOK) {
            float d = distSqToTarget(node.pos);
            if (d < bestDistSq) {
                bestDistSq = d;
                bestIdx    = (int)entry.idx;
            }
        }

        for (uint16_t nb : adjacency[entry.idx]) {
            if (nb >= visited.size()) continue;
            if (visited[nb]) continue;
            if (data->nodes[nb].flags & NODE_ORPHANED) continue;
            visited[nb] = true;
            parents[nb] = (int)entry.idx;
            frontier.push_back({nb, curHazardHops});
        }
    }

    if (bestIdx < 0) return false;

    // Reconstruct path: walk parents from bestIdx back to fromIdx, then
    // reverse. Skip fromIdx itself — navigator is already there. The
    // resulting path has `bestIdx` at the END (closest to target), with
    // intermediate nodes ordered first-to-last.
    std::vector<int> reversed;
    reversed.reserve(32);  // typical path length cap
    int cur = bestIdx;
    while (cur != -1 && cur != fromIdx) {
        reversed.push_back(cur);
        cur = parents[cur];
    }
    if (reversed.empty()) return false;
    out.reserve(reversed.size());
    if (outFlags) outFlags->reserve(reversed.size());
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
        const NavNode& n = data->nodes[(size_t)*it];
        out.push_back(n.pos);
        if (outFlags) outFlags->push_back(n.flags);
    }
    return true;
}

bool FindClimbAnchorAbove(const RoomNavData* data,
                          const Vec3f& pos,
                          float xzRadius,
                          float minHeight,
                          Vec3f& outBase,
                          Vec3f& outTop) {
    if (data == nullptr) return false;
    const float r2 = xzRadius * xzRadius;
    int   bestIdx   = -1;
    float bestDistSq = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < data->climbAnchors.size(); ++i) {
        const ClimbAnchor& a = data->climbAnchors[i];
        // Top must be at least minHeight above `pos` — caller's
        // signal that they want an UPWARD anchor, not a side or down.
        if (a.topPos.y < pos.y + minHeight) continue;
        float dx = a.basePos.x - pos.x;
        float dz = a.basePos.z - pos.z;
        float d2 = dx * dx + dz * dz;
        if (d2 > r2) continue;
        if (d2 < bestDistSq) {
            bestDistSq = d2;
            bestIdx    = (int)i;
        }
    }
    if (bestIdx < 0) return false;
    outBase = data->climbAnchors[(size_t)bestIdx].basePos;
    outTop  = data->climbAnchors[(size_t)bestIdx].topPos;
    return true;
}

bool IsReachable(const RoomNavData* data, const Vec3f& fromPos, const Vec3f& toPos) {
    if (data == nullptr || data->nodes.empty() || data->edges.empty()) return false;

    int fromIdx = FindNearestNode(data, fromPos);
    int toIdx   = FindNearestNode(data, toPos);
    if (fromIdx < 0 || toIdx < 0) return false;

    // Reject orphan endpoints — by definition they're not reachable
    // from the seed-rooted main graph. If the navigator's spawn or
    // target is on an orphan island, no walkable path exists.
    if (data->nodes[(size_t)fromIdx].flags & NODE_ORPHANED) return false;
    if (data->nodes[(size_t)toIdx].flags   & NODE_ORPHANED) return false;

    // Trivial case: same node.
    if (fromIdx == toIdx) return true;

    // BFS from fromIdx through the existing edges. Each edge is
    // bidirectional in this graph (stored once but traversable both
    // ways). Build a sparse adjacency list on the fly — for a
    // one-shot reachability query the up-front cost is fine.
    std::vector<std::vector<uint16_t>> adjacency(data->nodes.size());
    for (const NavEdge& e : data->edges) {
        if (e.fromIdx >= data->nodes.size() || e.toIdx >= data->nodes.size()) continue;
        adjacency[e.fromIdx].push_back(e.toIdx);
        adjacency[e.toIdx].push_back(e.fromIdx);
    }

    std::vector<bool> visited(data->nodes.size(), false);
    std::deque<uint16_t> q;
    visited[(size_t)fromIdx] = true;
    q.push_back((uint16_t)fromIdx);

    while (!q.empty()) {
        uint16_t cur = q.front();
        q.pop_front();
        if (cur == (uint16_t)toIdx) return true;
        for (uint16_t nb : adjacency[cur]) {
            if (nb >= visited.size())  continue;
            if (visited[nb])           continue;
            // Skip orphan nodes — they shouldn't have outbound edges to
            // non-orphan nodes anyway, but defensive belt: never let
            // BFS traverse through an orphan island.
            if (data->nodes[nb].flags & NODE_ORPHANED) continue;
            visited[nb] = true;
            q.push_back(nb);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Scan dispatch. Commit 2 implements the lookup-then-scan FLOW; the actual
// scan logic lands in commit 3 (multi-cast + node classification). Until
// then, ScanRoom is a stub that creates an empty RoomNavData entry to
// register the room as "attempted" and avoid re-attempting every frame.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Binary file format. Plan §4 — magic + version + per-room layout.
// kCurrentSchemaVersion bumps on any layout change; mismatch = silent
// regenerate (no migration logic). Magic guards against wrong-file-type
// or partial-write corruption.
// ---------------------------------------------------------------------------

// Schema v1 → v2 (workstream A polish): NavNode::flags widened from uint8_t
// to uint16_t to make room for NODE_ORPHANED (0x100). The struct grows by
// 1 byte (the cell indices were already 16-bit aligned, so the layout
// shift is a single-byte addition in the flags field's slot). Existing
// v1 .bin files become unreadable; TryLoadFromDisk's version-mismatch
// branch silently regenerates them on next room entry.
//
// Schema v6 → v7: ClimbAnchor extended with surface-grid params
// (planeOrigin/Normal/AxisU/AxisV, cellsU/cellsV, firstNodeIdx,
// nodeCount, surfaceType) for the climb-surface nav grid. NodeFlags
// gains NODE_CLIMB_LADDER / VINE / DESIGNATED_WALL / GENERIC_WALL +
// NODE_CLIMB_BOUNDARY. Stage 1 lands the data plumbing; v6 files
// silently regenerate as v7 (matches every prior version bump). The
// new ClimbAnchor fields default to zero / zero-vector — Stage 2 wires
// the scan to populate them.
static constexpr uint16_t kCurrentSchemaVersion = 7;
static constexpr uint32_t kMagic                = 0x52564E41; // 'RNAV' little-endian

// Scan / sampling constants — declared early so persistence code can
// validate against them. See plan §11 (resolution) and §5 (caps).
static constexpr float    kGridResolution      = 30.0f;  // §11 lock
static constexpr int      kMaxFloorsPerColumn  = 8;      // defensive cap
static constexpr int      kMaxScanIterations   = 50000;  // floodfill runaway guard
static constexpr int64_t  kMaxScanWallTimeMs   = 1000;   // per-scan budget

// Path resolution. roomnavdata/ lives at the executable cwd, sibling to
// logs/. Per plan §8.
static std::filesystem::path RoomNavFilePath(int16_t sceneNum, int8_t roomNum) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "roomnavdata/roomnavdata_%d_%d.bin",
                  (int)sceneNum, (int)roomNum);
    return std::filesystem::path(buf);
}

template <typename T>
static bool ReadValue(std::ifstream& f, T& out) {
    f.read(reinterpret_cast<char*>(&out), sizeof(T));
    return f.good();
}

template <typename T>
static void WriteValue(std::ofstream& f, const T& in) {
    f.write(reinterpret_cast<const char*>(&in), sizeof(T));
}

template <typename T>
static bool ReadVector(std::ifstream& f, std::vector<T>& out, uint32_t count) {
    if (count > 1000000) return false; // sanity cap; corrupted file
    out.resize(count);
    if (count == 0) return true;
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(count * sizeof(T)));
    return f.good();
}

template <typename T>
static void WriteVector(std::ofstream& f, const std::vector<T>& in) {
    if (in.empty()) return;
    f.write(reinterpret_cast<const char*>(in.data()), (std::streamsize)(in.size() * sizeof(T)));
}

// Save the in-memory RoomNavData to disk. Lazy-creates the roomnavdata/
// directory. Best-effort; logs and continues on I/O failure (the
// in-memory copy is canonical for this session regardless).
static void SaveToDisk(const RoomNavData& nav) {
    std::error_code ec;
    std::filesystem::create_directories("roomnavdata", ec);
    if (ec) {
        SPDLOG_WARN("[RoomNav] SaveToDisk: create_directories failed: {}", ec.message());
        return;
    }

    auto path = RoomNavFilePath(nav.sceneNum, nav.roomNum);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.good()) {
        SPDLOG_WARN("[RoomNav] SaveToDisk: open failed for {}", path.string());
        return;
    }

    WriteValue(f, kMagic);
    WriteValue(f, kCurrentSchemaVersion);
    WriteValue(f, nav.sceneNum);
    WriteValue(f, nav.roomNum);
    uint8_t pad3[3] = { 0, 0, 0 }; // align scanTimestamp to 4-byte boundary
    f.write(reinterpret_cast<const char*>(pad3), sizeof(pad3));
    WriteValue(f, nav.scanTimestamp);
    WriteValue(f, nav.bboxMin);
    WriteValue(f, nav.bboxMax);
    WriteValue(f, nav.gridResolution);
    WriteValue(f, nav.firstClimbSurfaceNodeIdx); // schema v7+

    uint32_t nodeCount       = (uint32_t)nav.nodes.size();
    uint32_t edgeCount       = (uint32_t)nav.edges.size();
    uint32_t climbCount      = (uint32_t)nav.climbAnchors.size();
    uint32_t ledgeCount      = (uint32_t)nav.ledgeAnchors.size();      // schema v3+
    uint32_t crawlspaceCount = (uint32_t)nav.crawlspaceAnchors.size(); // schema v4+
    uint32_t dropCount       = (uint32_t)nav.dropAnchors.size();       // schema v5+
    uint32_t histSeedCount   = (uint32_t)nav.historicalSeeds.size();   // schema v6+
    uint32_t hazardCount     = (uint32_t)nav.hazardCentroids.size();
    WriteValue(f, nodeCount);
    WriteValue(f, edgeCount);
    WriteValue(f, climbCount);
    WriteValue(f, ledgeCount);
    WriteValue(f, crawlspaceCount);
    WriteValue(f, dropCount);
    WriteValue(f, histSeedCount);
    WriteValue(f, hazardCount);

    WriteVector(f, nav.nodes);
    WriteVector(f, nav.edges);
    WriteVector(f, nav.climbAnchors);
    WriteVector(f, nav.ledgeAnchors);
    WriteVector(f, nav.crawlspaceAnchors);
    WriteVector(f, nav.dropAnchors);
    WriteVector(f, nav.historicalSeeds);
    WriteVector(f, nav.hazardCentroids);

    if (!f.good()) {
        SPDLOG_WARN("[RoomNav] SaveToDisk: write error for {}", path.string());
    }
}

static void TryLoadFromDisk(int16_t sceneNum, int8_t roomNum, RoomNavData* out) {
    auto path = RoomNavFilePath(sceneNum, roomNum);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return; // no file; cache miss (caller falls through to scan)
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return;

    uint32_t magic = 0;
    uint16_t version = 0;
    int16_t fileScene = -1;
    int8_t fileRoom = -1;
    uint8_t pad3[3] = { 0, 0, 0 };
    uint32_t scanTimestamp = 0;
    Vec3f bboxMin{}, bboxMax{};
    uint16_t gridRes = 0;
    uint32_t nodeCount = 0, edgeCount = 0, climbCount = 0, ledgeCount = 0,
             crawlspaceCount = 0, dropCount = 0, histSeedCount = 0, hazardCount = 0;

    if (!ReadValue(f, magic) || magic != kMagic) {
        SPDLOG_WARN("[RoomNav] LoadFromDisk: magic mismatch for {} (got 0x{:08x}); regenerating",
                    path.string(), magic);
        return;
    }
    if (!ReadValue(f, version) || version != kCurrentSchemaVersion) {
        SPDLOG_INFO("[RoomNav] LoadFromDisk: schema version {} != {} for {}; regenerating",
                    version, kCurrentSchemaVersion, path.string());
        return;
    }
    if (!ReadValue(f, fileScene) || fileScene != sceneNum) return;
    if (!ReadValue(f, fileRoom)  || fileRoom  != roomNum)  return;
    f.read(reinterpret_cast<char*>(pad3), sizeof(pad3));
    if (!ReadValue(f, scanTimestamp))                       return;
    if (!ReadValue(f, bboxMin))                             return;
    if (!ReadValue(f, bboxMax))                             return;
    if (!ReadValue(f, gridRes) || gridRes != (uint16_t)kGridResolution) {
        SPDLOG_INFO("[RoomNav] LoadFromDisk: grid resolution {} != {} for {}; regenerating",
                    gridRes, (uint16_t)kGridResolution, path.string());
        return;
    }
    uint16_t firstClimbSurfaceNodeIdx = UINT16_MAX;
    if (!ReadValue(f, firstClimbSurfaceNodeIdx)) return; // schema v7+
    if (!ReadValue(f, nodeCount))       return;
    if (!ReadValue(f, edgeCount))       return;
    if (!ReadValue(f, climbCount))      return;
    if (!ReadValue(f, ledgeCount))      return;
    if (!ReadValue(f, crawlspaceCount)) return;
    if (!ReadValue(f, dropCount))       return;
    if (!ReadValue(f, histSeedCount))   return;
    if (!ReadValue(f, hazardCount))     return;

    if (!ReadVector(f, out->nodes,             nodeCount))       return;
    if (!ReadVector(f, out->edges,             edgeCount))       return;
    if (!ReadVector(f, out->climbAnchors,      climbCount))      return;
    if (!ReadVector(f, out->ledgeAnchors,      ledgeCount))      return;
    if (!ReadVector(f, out->crawlspaceAnchors, crawlspaceCount)) return;
    if (!ReadVector(f, out->dropAnchors,       dropCount))       return;
    if (!ReadVector(f, out->historicalSeeds,   histSeedCount))   return;
    if (!ReadVector(f, out->hazardCentroids,   hazardCount))     return;

    out->magic                      = magic;
    out->version                    = version;
    out->sceneNum                   = fileScene;
    out->roomNum                    = fileRoom;
    out->scanTimestamp              = scanTimestamp;
    out->bboxMin                    = bboxMin;
    out->bboxMax                    = bboxMax;
    out->gridResolution             = gridRes;
    out->firstClimbSurfaceNodeIdx   = firstClimbSurfaceNodeIdx;
}

// ---------------------------------------------------------------------------
// Scan implementation. Plan §5 — multi-cast per XZ + floodfill from player
// position, bounded by scene-level colCtx.minBounds/maxBounds.
// kGridResolution and defensive caps are declared earlier in the file
// (with the persistence constants) so binary-format validation can
// reference them without forward-reference issues.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Climb anchor detection. Plan §5 — Path A scene-actor allowlist.
// ---------------------------------------------------------------------------

// Confirmed climbable scene actors. Each entry's actorId triggers a
// ClimbAnchor record at the actor's world.pos with an estimated topPos
// `estimatedHeight` units above. Field testing extends this list when
// new climbable actor types surface.
//
// `requiredParams` is an optional discriminator for actors that share an
// actor-id between climbable and non-climbable variants (e.g.
// Bg_Ydan_Maruta is both the rotating spike-log and the falling ladder
// in Inside Deku Tree, distinguished by params at runtime). -1 means
// "match any params". For positive values, the comparison is against
// `actor->params & 0xFF` to absorb the high-byte switch-flag layout
// some actors use.
struct ClimbableActorEntry {
    int16_t actorId;
    int16_t requiredParams;  // -1 = any
    float   estimatedHeight; // climb-top Y delta from actor.world.pos.y
};
static const ClimbableActorEntry kClimbableActors[] = {
    { ACTOR_BG_SPOT18_OBJ,      -1,  150.0f }, // Goron City interior ladder
    { ACTOR_BG_DDAN_KD,         -1,  200.0f }, // Dodongo's Cavern ladder
    { ACTOR_BG_SPOT06_OBJECTS,  -1,  120.0f }, // Lake Hylia (some climbables)
    // Inside Deku Tree falling ladder. params==0 is the rotating spike
    // log (NOT climbable); params==1 is the ladder. `actor->params` is
    // already shifted to the high byte during BgYdanMaruta_Init
    // (z_bg_ydan_maruta.c:93), so checking `params & 0xFF == 1` works
    // post-init regardless of the original 16-bit scene-data layout.
    // Estimated height 280u: BgYdanMaruta_Init pulls home.pos.y down by
    // 280u for the params==1 variant (z_bg_ydan_maruta.c:103), so the
    // ladder spans that range.
    { ACTOR_BG_YDAN_MARUTA,      1,  280.0f }, // Inside Deku Tree falling ladder
};

static void DetectClimbAnchors(RoomNavData* out, PlayState* play) {
    // Walk both ACTORCAT_BG and ACTORCAT_PROP — climbable scene actors
    // may live in either category depending on the actor's setup.
    constexpr ActorCategory kCategoriesToScan[] = { ACTORCAT_BG, ACTORCAT_PROP };

    for (ActorCategory cat : kCategoriesToScan) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            for (const ClimbableActorEntry& entry : kClimbableActors) {
                if (actor->id != entry.actorId) continue;
                if (entry.requiredParams >= 0 &&
                    (actor->params & 0xFF) != entry.requiredParams) {
                    continue; // wrong variant of this actor type
                }
                ClimbAnchor anchor{};
                anchor.basePos = actor->world.pos;
                anchor.topPos  = { actor->world.pos.x,
                                   actor->world.pos.y + entry.estimatedHeight,
                                   actor->world.pos.z };
                anchor.actorId = actor->id;
                out->climbAnchors.push_back(anchor);
                break;
            }
            actor = actor->next;
        }
    }
}

// ---------------------------------------------------------------------------
// Hazard + water classification predicates. Plan §5 — bit positions
// verified against z_player.c usage patterns.
// ---------------------------------------------------------------------------

// v1 hazard predicate. Returns true for surfaces that should bias the
// navigator AWAY (consumer-side preference, not absolute exclusion).
//
// Confirmed hazards (verified from z_player.c references):
//   Floor Property 5  = pit/void (Play_TriggerRespawn on landing)
//   Floor Property 12 = deep-fall (void-out trigger via fall-distance)
//   Floor Type 5      = slippery/ice (slip-recovery interaction)
//   Floor Type 7      = ice/no-friction (defeats iron boots)
//
// NOT marked hazardous here:
//   Floor Type 6 = shallow water — gets UNDERWATER flag instead
//   Floor Type 9 = deep water    — gets UNDERWATER flag instead
//   Floor Type 8 = scene-exit    — navigation-relevant but not a hazard
//
// Lava / hot-floor / quicksand are NOT identifiable from these bit fields
// alone in vanilla OoT. Likely encoded in roomCtx.curRoom.behaviorType1/2
// or detected by separate code paths in Player_UpdateCommon. Future work
// per plan §5: add room-behavior-based hazard tagging during scan.
static bool IsHazardousSurface(CollisionPoly* poly, s32 bgId, CollisionContext* colCtx) {
    if (poly == nullptr) return false;

    u32 floorProperty = func_80041EA4(colCtx, poly, bgId); // data[0] >> 26 & 0xF
    u32 floorType     = func_80041D4C(colCtx, poly, bgId); // data[0] >> 13 & 0x1F

    if (floorProperty == 5)  return true;  // pit/void
    if (floorProperty == 12) return true;  // deep-fall
    if (floorType == 5)      return true;  // slippery/ice
    if (floorType == 7)      return true;  // ice/no-friction
    return false;
}

// Water-volume detection. Returns true if the (x, floorY, z) sample lies
// below a water surface — i.e., the floor at that XZ is submerged.
// Swim-capable navigators (Link-rigged: AI Follower, NPC Invader) traverse
// underwater nodes; non-swimming navigators skip them via NavTraits filter.
static bool IsUnderwater(f32 x, f32 floorY, f32 z, PlayState* play) {
    f32 waterSurfaceY = 0.0f;
    WaterBox* wb = nullptr;
    s32 hit = WaterBox_GetSurface1(play, &play->colCtx, x, z, &waterSurfaceY, &wb);
    if (!hit) return false;
    return floorY < waterSurfaceY;
}

// Pelvis-height movement-clearance line test. Used by BuildEdges to
// determine whether two nodes can be traversed between without hitting
// a wall. Per plan §2 — body offset 20u places the ray above ground but
// below most chest-high obstacles.
//
// Returns true when the segment from `from` to `to` is CLEAR (navigator
// can walk between), false if any wall poly blocks the segment.
//
// In nav system Phase 1 this helper will live in Common/ActorTrail.cpp;
// for Phase 1 of RoomNavData (which lands first) it lives here as a
// file-scope helper. Once both modules co-exist, extract to a shared
// Common/LineOfSight.{h,cpp} module per nav plan §5.
static constexpr float kBodyOffset = 20.0f;

static bool MovementClear(const Vec3f& from, const Vec3f& to, PlayState* play) {
    Vec3f a = { from.x, from.y + kBodyOffset, from.z };
    Vec3f b = { to.x,   to.y   + kBodyOffset, to.z   };
    Vec3f hitPos;
    CollisionPoly* hitPoly = nullptr;
    // Returns nonzero on hit. We want clear (no hit), so invert.
    s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &a, &b, &hitPos, &hitPoly, 0);
    return !hit;
}

// Encodes an integer cell coordinate for the visited-cells set. Cell
// coordinates are XZ grid indices (signed; can go negative if room
// extends below world origin).
struct CellKey {
    int32_t x;
    int32_t z;
    bool operator==(const CellKey& other) const { return x == other.x && z == other.z; }
};
struct CellKeyHash {
    size_t operator()(const CellKey& k) const noexcept {
        return ((size_t)(uint32_t)k.x * 0x9E3779B1u) ^ (size_t)(uint32_t)k.z;
    }
};

// Compute the integer cell index for a world XZ position relative to the
// scan's bounding-box origin and grid resolution.
static CellKey CellKeyForXZ(float x, float z, const Vec3f& bboxMin) {
    return CellKey{
        (int32_t)std::floor((x - bboxMin.x) / kGridResolution),
        (int32_t)std::floor((z - bboxMin.z) / kGridResolution),
    };
}

static Vec3f CellCenterWorld(const CellKey& cell, const Vec3f& bboxMin) {
    return Vec3f{
        bboxMin.x + (cell.x + 0.5f) * kGridResolution,
        0.0f, // caller fills Y from raycast
        bboxMin.z + (cell.z + 0.5f) * kGridResolution,
    };
}

// ---------------------------------------------------------------------------
// Path B — surface-flag-based climb detection. Catches vine walls and any
// other static-scene geometry whose collision polys carry the "ladder"
// wall-flag bit. Path A (above) handles discrete climbable scene actors;
// Path B handles climb surfaces baked into static scene collision.
// ---------------------------------------------------------------------------

// Wall-flag bit semantics (verified against z_bgcheck.c:4022-4028, the
// `CVAR_CHEAT("ClimbEverything")` cheat, and field-test diagnostic data
// captured for Inside Deku Tree main entrance ladder log 9):
//
//   func_80041D94(...)   → 5-bit wall-property INDEX (bits 21-25 of data[0])
//   D_80119D90[index]    → wall-flag BITMASK (lookup table at z_bgcheck.c:32)
//   func_80041DB8(...)   → returns the bitmask, with `(1 << 3)` OR'd in when
//                          ClimbEverything is on.
//
// Lookup table values for indices 0-7 (rest are 0):
//   idx 0 → 0   (solid wall)
//   idx 1 → 1   (bit 0 — no-clip / curtain pass-through)
//   idx 2 → 3   (bits 0+1 — climbable; observed on Inside Deku Tree
//                main entrance ladder polys)
//   idx 3 → 5   (bits 0+2 — climbable; also observed on the same ladder)
//   idx 4 → 8   (bit 3 — climbable; used by vine walls and the
//                ClimbEverything cheat OR's this in to make any wall
//                climbable)
//   idx 5 → 16  (bit 4 — unknown; may catch additional climbables)
//   idx 6 → 32  (bit 5 — unknown)
//   idx 7 → 64  (bit 6 — unknown)
//
// The climbable mask catches indices 2, 3, AND 4 by matching ANY of
// bits 1, 2, OR 3. We deliberately skip bit 0 alone (idx 1) because
// that's no-clip-only without an additional climb bit. If a future
// field test surfaces a climbable surface using bits 4+, we extend
// the mask further.
static constexpr s32 kWallFlagClimbableMask = (1 << 1) | (1 << 2) | (1 << 3); // 0x0E

// Cluster radius for merging adjacent climbable polys into one anchor.
// Vine walls are typically multi-poly meshes; a radius of ~30u (one grid
// cell) collapses adjacent polys into a single anchor whose Y range
// spans the full mesh height.
static constexpr float kClimbClusterRadiusXZ = 30.0f;

// Surface-flag-based climb detection. Iterates static scene polys (NOT
// dynamic Bg-actor polys; those are out of scope for v1 because the raw
// vtxList stores actor-local space and applying the per-actor transform
// per poly is complex). For each wall poly with the climbable flag set,
// computes centroid + Y range and merges into the anchor list with
// proximity clustering.
//
// Room scoping is enforced via the `visited` cell set the floodfill
// already produced — only polys whose centroid cell was floodfill-reached
// contribute. This keeps shared static-scene walls from leaking
// cross-room anchors.
static void DetectClimbAnchorsViaSurfaceFlags(
    RoomNavData* out,
    PlayState* play,
    const std::unordered_set<CellKey, CellKeyHash>& visited)
{
    // Run when EITHER the production Path B CVar is on OR the diagnostic
    // wall-types log is on. The diagnostic wants to see walls even if
    // the user hasn't enabled Path B for production climb-anchor
    // generation.
    const bool pathBProduction = CVarGetInteger(CVAR_ROOM_NAV_PATH_B_CLIMB, 0) != 0;
    const bool pathBDiagnostic = CVarGetInteger(CVAR_ROOM_NAV_PATH_B_DEBUG, 0) != 0;
    if (!pathBProduction && !pathBDiagnostic) return;

    CollisionHeader* hdr = play->colCtx.colHeader;
    if (hdr == nullptr || hdr->polyList == nullptr || hdr->vtxList == nullptr) return;

    Vec3s* vtxList = hdr->vtxList;
    u16 numPolys   = hdr->numPolygons;
    u16 numVtx     = hdr->numVertices;

    // Pre-cluster radius squared to avoid per-poly sqrt.
    constexpr float kClusterRadiusSq = kClimbClusterRadiusXZ * kClimbClusterRadiusXZ;

    // Diagnostic mode: when on, log every wall poly with non-zero
    // wall-property index regardless of whether bit 3 (climbable) is
    // set. Lets the user discover wall-flag patterns we DON'T currently
    // match — useful when a known climbable surface (e.g. Inside Deku
    // Tree main entrance ladder) doesn't show up as a Path B anchor.
    const bool diagLogWallTypes = CVarGetInteger(CVAR_ROOM_NAV_PATH_B_DEBUG, 0) != 0;
    size_t diagLoggedWalls = 0;

    size_t pathBAdded   = 0;
    size_t pathBMerged  = 0;

    for (u16 i = 0; i < numPolys; i++) {
        CollisionPoly* poly = &hdr->polyList[i];

        // Wall classification: |normal.y| small ≈ poly is vertical (a wall).
        // Threshold 0.5 catches anything significantly more vertical than
        // horizontal; floors (normal.y near 1.0) and ceilings (near -1.0)
        // are excluded.
        f32 normalY = (f32)poly->normal.y / 32767.0f;
        if (std::fabs(normalY) > 0.5f) continue;

        // Read both the wall-property INDEX (5-bit value 0-31) and the
        // wall-flag BITMASK (lookup-table value, with the cheat's bit-3
        // injection if active). Diagnostic mode logs both for any poly
        // with a non-trivial property; production mode skips polys that
        // don't carry the climbable bit.
        u32 wallIdx   = func_80041D94(&play->colCtx, poly, BGCHECK_SCENE);
        s32 wallFlags = func_80041DB8(&play->colCtx, poly, BGCHECK_SCENE);

        // Vertex indices (low 13 bits of each field).
        u16 viA = poly->flags_vIA & 0x1FFF;
        u16 viB = poly->flags_vIB & 0x1FFF;
        u16 viC = poly->vIC       & 0x1FFF;
        if (viA >= numVtx || viB >= numVtx || viC >= numVtx) continue;

        const Vec3s& vA = vtxList[viA];
        const Vec3s& vB = vtxList[viB];
        const Vec3s& vC = vtxList[viC];

        f32 cx   = ((f32)vA.x + (f32)vB.x + (f32)vC.x) / 3.0f;
        f32 cz   = ((f32)vA.z + (f32)vB.z + (f32)vC.z) / 3.0f;
        f32 minY = (f32)std::min({vA.y, vB.y, vC.y});
        f32 maxY = (f32)std::max({vA.y, vB.y, vC.y});

        // Room scoping: poly must be in a floodfill-visited cell.
        CellKey centroidCell = CellKeyForXZ(cx, cz, out->bboxMin);
        if (visited.count(centroidCell) == 0) continue;

        // Diagnostic logging — runs after wall + room-scope filtering
        // (so we don't drown in irrelevant polys), but BEFORE the
        // climbable-bit filter (so the user sees walls we'd otherwise
        // skip). Only emit for non-trivial wall properties so plain
        // solid walls don't spam.
        if (diagLogWallTypes && wallIdx != 0) {
            // Compact log so a room with many special walls stays
            // readable. Y centroid included so the user can spot
            // walls at known altitudes.
            f32 cy = ((f32)vA.y + (f32)vB.y + (f32)vC.y) / 3.0f;
            SPDLOG_INFO("[RoomNav][PathBDiag] poly@({:.0f},{:.0f},{:.0f}) "
                        "idx={} flags=0x{:02X} normalY={:.2f}",
                        cx, cy, cz, wallIdx, (unsigned)wallFlags, normalY);
            diagLoggedWalls++;
        }

        // Climbable-flag check (production filter). Matches any of
        // wall-property indices 2, 3, or 4 — see kWallFlagClimbableMask
        // declaration above for index→bitmask semantics.
        if ((wallFlags & kWallFlagClimbableMask) == 0) continue;

        // If only the diagnostic CVar is on (production Path B off),
        // we've already logged this wall — skip the anchor-creation
        // path so diagnostic-only runs don't pollute climbAnchors.
        if (!pathBProduction) continue;

        // Proximity-merge with existing anchors. Two anchors within
        // kClimbClusterRadiusXZ collapse into one whose Y range covers both.
        // Only static-geometry anchors (actorId == 0) are merge candidates;
        // Path A actor-based anchors stay distinct.
        bool merged = false;
        for (ClimbAnchor& existing : out->climbAnchors) {
            if (existing.actorId != 0) continue;
            f32 dx = existing.basePos.x - cx;
            f32 dz = existing.basePos.z - cz;
            if (dx * dx + dz * dz < kClusterRadiusSq) {
                if (minY < existing.basePos.y) existing.basePos.y = minY;
                if (maxY > existing.topPos.y)  existing.topPos.y  = maxY;
                merged = true;
                pathBMerged++;
                break;
            }
        }

        if (!merged) {
            ClimbAnchor anchor{};
            anchor.basePos = { cx, minY, cz };
            anchor.topPos  = { cx, maxY, cz };
            anchor.actorId = 0; // 0 = static-geometry per ClimbAnchor struct
            out->climbAnchors.push_back(anchor);
            pathBAdded++;
        }
    }

    if (pathBAdded > 0 || pathBMerged > 0) {
        SPDLOG_INFO("[RoomNav] Path B climb detection: {} anchors added, {} polys merged",
                    pathBAdded, pathBMerged);
    }
    if (diagLogWallTypes) {
        SPDLOG_INFO("[RoomNav][PathBDiag] summary: {} wall polys logged "
                    "(non-zero wall-property index, in-room only)",
                    diagLoggedWalls);
    }
}

// ---------------------------------------------------------------------------
// Climb-surface grid generation (schema v7+). For each ClimbAnchor
// discovered by Path A or Path B, raycast a 2D grid perpendicular to
// the wall and emit a NavNode at every cell the raycast hits. Each
// node is tagged with a NODE_CLIMB_* surface-type bit so the
// permission-mask BFS gating in Stage 5 can decide which consumers
// may traverse it.
//
// See Plans/climb_surface_nav_grid_plan.md for the full design.
// ---------------------------------------------------------------------------

// Surface-type discrimination from a hit poly's wall-flag bitmap.
// Returns a single NODE_CLIMB_* bit, or 0 if not climbable.
//
// Wall-flag bit semantics from kWallFlagClimbableMask comments:
//   bit 3 (mask 0x08) → vine wall (also injected by ClimbEverything cheat)
//   bits 1-2 (mask 0x06) → designed climbable wall
// We don't currently have a bit-level discriminator for "ladder vs
// designed wall" — Path A scene actors are explicitly ladders and get
// the LADDER tag at a different code path (anchor.actorId != 0 sets
// expectedType in the per-anchor loop). Path B static-geometry surfaces
// always classify as VINE or DESIGNATED_WALL based on the bit pattern.
static uint16_t ClassifyClimbWallFlags(s32 wallFlags) {
    if ((wallFlags & kWallFlagClimbableMask) == 0) return 0;
    if (wallFlags & 0x08) return NODE_CLIMB_VINE;
    if (wallFlags & 0x06) return NODE_CLIMB_DESIGNATED_WALL;
    return NODE_CLIMB_DESIGNATED_WALL; // catch-all if mask matched some other bit
}

// Surface-grid scan tunables. Per-cell raycast cost is ~1 BgCheck call;
// per-anchor cost is dominated by cellsU * cellsV. With the caps below
// the worst case is 24 * 32 = 768 raycasts per anchor; typical room
// has 1-3 anchors → ~2k extra raycasts. Floor scan does ~16k, so this
// is +~12% in the worst case (rare; most rooms have 0-1 anchors).
static constexpr float    kClimbGridCellSpacing  = 30.0f;  // matches kGridResolution
static constexpr float    kClimbRayStandoff      = 100.0f; // ray origin distance from wall
static constexpr float    kClimbRayLength        = 200.0f; // total ray length (standoff + penetration)
static constexpr float    kClimbProbeYAtBase     = 30.0f;  // raise above floor for normal probe
static constexpr int      kClimbWidthMaxStepsPerSide = 12; // ±360u max width search
static constexpr float    kClimbMinAnchorHeight  = 30.0f;  // skip degenerate-height anchors
static constexpr uint16_t kClimbMaxCellsU        = 24;     // width safety cap (~720u)
static constexpr uint16_t kClimbMaxCellsV        = 32;     // height safety cap (~960u)
static constexpr float    kClimbPathAActorWidth  = 30.0f;  // single-column ladder default

// Eight horizontal directions for normal probing.
struct ClimbProbeDir { float x; float z; };
static const ClimbProbeDir kClimbProbeDirs[8] = {
    { 1.0f,  0.0f}, { 0.7071f,  0.7071f}, { 0.0f,  1.0f}, {-0.7071f,  0.7071f},
    {-1.0f,  0.0f}, {-0.7071f, -0.7071f}, { 0.0f, -1.0f}, { 0.7071f, -0.7071f},
};

// Vec3f minimal math (the codebase doesn't have a project-wide vec lib).
static inline Vec3f V3Add(const Vec3f& a, const Vec3f& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
static inline Vec3f V3Sub(const Vec3f& a, const Vec3f& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
static inline Vec3f V3Scale(const Vec3f& a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}
static inline float V3Len(const Vec3f& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
static inline Vec3f V3Cross(const Vec3f& a, const Vec3f& b) {
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}
static inline Vec3f V3Normalize(const Vec3f& a) {
    float L = V3Len(a);
    if (L < 1e-3f) return { 0.0f, 0.0f, 0.0f };
    return { a.x / L, a.y / L, a.z / L };
}

// Cast one cell-probe ray. Returns true on hit; writes hit position +
// the wall-flag bitmap. Floor/ceiling hits (|normal.y| > 0.5) are
// rejected (not a wall — would corrupt grid placement).
static bool RaycastClimbCell(PlayState* play,
                              const Vec3f& cellCenter,
                              const Vec3f& outwardNormal,
                              Vec3f& outHitPos,
                              s32& outWallFlags)
{
    Vec3f a = V3Add(cellCenter, V3Scale(outwardNormal,  kClimbRayStandoff));
    Vec3f b = V3Add(cellCenter, V3Scale(outwardNormal, -(kClimbRayLength - kClimbRayStandoff)));
    CollisionPoly* hitPoly = nullptr;
    s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &a, &b, &outHitPos, &hitPoly, 0);
    if (!hit || hitPoly == nullptr) return false;
    f32 normalY = (f32)hitPoly->normal.y / 32767.0f;
    if (std::fabs(normalY) > 0.5f) return false;
    outWallFlags = func_80041DB8(&play->colCtx, hitPoly, BGCHECK_SCENE);
    return true;
}

// Probe the wall around basePos to determine its outward normal. Casts
// horizontal rays in 8 directions; the direction with the shortest hit
// distance is inward, and the OUTWARD normal is read from the hit
// poly's own normal (more accurate than the probe direction).
static bool ComputeClimbSurfaceNormal(const Vec3f& basePos,
                                       PlayState* play,
                                       Vec3f& outNormal)
{
    Vec3f probeOrigin = { basePos.x,
                          basePos.y + kClimbProbeYAtBase,
                          basePos.z };
    float bestDistSq = (kClimbRayLength + 1.0f) * (kClimbRayLength + 1.0f);
    int bestIdx = -1;
    Vec3s bestPolyNormal = { 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        Vec3f rayEnd = { probeOrigin.x + kClimbProbeDirs[i].x * kClimbRayLength,
                         probeOrigin.y,
                         probeOrigin.z + kClimbProbeDirs[i].z * kClimbRayLength };
        Vec3f hitPos;
        CollisionPoly* hitPoly = nullptr;
        s32 hit = BgCheck_AnyLineTest1(&play->colCtx, &probeOrigin, &rayEnd, &hitPos, &hitPoly, 0);
        if (!hit || hitPoly == nullptr) continue;
        float dx = hitPos.x - probeOrigin.x;
        float dz = hitPos.z - probeOrigin.z;
        float dSq = dx * dx + dz * dz;
        if (dSq < bestDistSq) {
            bestDistSq = dSq;
            bestIdx = i;
            bestPolyNormal = hitPoly->normal;
        }
    }
    if (bestIdx < 0) return false;
    Vec3f n = { (float)bestPolyNormal.x / 32767.0f,
                (float)bestPolyNormal.y / 32767.0f,
                (float)bestPolyNormal.z / 32767.0f };
    n.y = 0.0f; // force horizontal — grid is rotated about vertical axis
    float len = std::sqrt(n.x * n.x + n.z * n.z);
    if (len < 1e-3f) return false;
    n.x /= len;
    n.z /= len;
    outNormal = n;
    return true;
}

// Probe outward from basePos in ±axisU at half-spacing increments.
// Returns the total measured surface width (negSide + posSide + center
// column's own kClimbGridCellSpacing). Stops on first non-climbable
// hit OR on surface-type change (so a designed-wall strip adjacent to
// a vine wall doesn't merge into one over-wide grid).
static float DetermineClimbSurfaceWidth(PlayState* play,
                                         const Vec3f& basePos,
                                         const Vec3f& outwardNormal,
                                         const Vec3f& axisU,
                                         uint16_t expectedType)
{
    auto sideExtent = [&](float dir) -> float {
        float lastValid = 0.0f;
        for (int s = 1; s <= kClimbWidthMaxStepsPerSide; s++) {
            float offset = dir * s * kClimbGridCellSpacing;
            Vec3f probeCenter = { basePos.x + axisU.x * offset,
                                   basePos.y + kClimbProbeYAtBase,
                                   basePos.z + axisU.z * offset };
            Vec3f hitPos;
            s32 wallFlags = 0;
            if (!RaycastClimbCell(play, probeCenter, outwardNormal, hitPos, wallFlags)) break;
            uint16_t typeBit = ClassifyClimbWallFlags(wallFlags);
            if (typeBit == 0) break;
            if (expectedType != 0 && typeBit != expectedType) break;
            lastValid = std::fabs(offset);
        }
        return lastValid;
    };
    float negSide = sideExtent(-1.0f);
    float posSide = sideExtent(+1.0f);
    return negSide + posSide + kClimbGridCellSpacing;
}

// Edge cost constants (Stage 3). Initial values; the plan's open
// question Q5 calls for field-tuning once consumers wire up.
//
// kClimbCellEdgeCost: surface-to-surface step within a grid. Matches
//   the floor-edge spacing-cost convention so a 7-cell ladder climb
//   = 7 × 30 = 210u of "distance" (vs ~30u per floor cell).
//
// kFloorToClimbCost: bidirectional edge between the bottom row of the
//   grid and the nearest floor at basePos (and similarly between the
//   top row and the nearest floor at topPos). Set ABOVE the per-cell
//   cost so BFS prefers an ALL-FLOOR route when one of comparable
//   length exists, but doesn't refuse the climb when it's the
//   only/shorter route.
static constexpr float kClimbCellEdgeCost = 30.0f;
static constexpr float kFloorToClimbCost  = 50.0f;
static constexpr float kClimbToFloorCost  = 50.0f;

// Generate edges for a single anchor's surface grid:
//   1. Surface-to-surface 4-connected (cardinal neighbors only — diagonals
//      are ambiguous on ladders/vines for stick-injection consumers).
//   2. Bottom-row (cellIdxZ == 0) ↔ nearest floor near anchor.basePos.
//   3. Top-row (cellIdxZ == cellsV - 1) ↔ nearest floor near anchor.topPos.
//
// Append-only on out->edges. Each undirected edge is emitted ONCE
// (matches BuildEdges convention at line 1920+); BuildAdjacencyList
// (line 207) walks both directions. MUST run AFTER the anchor's nodes
// have been appended to out->nodes.
//
// Edges generated here are inert until Stage 5 modifies the BFS to
// expand through NODE_CLIMB_* nodes (currently gated off by the
// `node.flags & NODE_WALKABLE` check at FindBestReachableSubgoalPath
// line ~470). Stage 3 lays the data; Stage 5 unlocks traversal.
//
// Rescan cleanup: caller drops climb-surface NODES via nodes.resize();
// the matching edge cleanup (DropClimbSurfaceEdges) runs in the same
// rescan path so edges don't dangle into discarded indices.
static void GenerateClimbSurfaceEdges(RoomNavData* out,
                                       const ClimbAnchor& anchor)
{
    if (anchor.nodeCount == 0) return;

    // (u, v) → nodeIdx lookup for THIS anchor's grid only.
    std::unordered_map<uint32_t, uint16_t> uvToIdx;
    uvToIdx.reserve(anchor.nodeCount);
    for (uint16_t i = 0; i < anchor.nodeCount; i++) {
        const NavNode& n = out->nodes[(size_t)anchor.firstNodeIdx + i];
        uint32_t key = ((uint32_t)n.cellIdxZ << 16) | (uint32_t)n.cellIdxX;
        uvToIdx[key] = (uint16_t)(anchor.firstNodeIdx + i);
    }

    // Surface-to-surface edges. Cardinal (4-connected) only. Each
    // undirected pair emitted once (toIdx > fromIdx guard).
    static const int kClimbDeltas[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    size_t surfaceEdgeCount = 0;
    for (uint16_t i = 0; i < anchor.nodeCount; i++) {
        const NavNode& n = out->nodes[(size_t)anchor.firstNodeIdx + i];
        uint16_t fromIdx = (uint16_t)(anchor.firstNodeIdx + i);
        for (int d = 0; d < 4; d++) {
            int nu = (int)n.cellIdxX + kClimbDeltas[d][0];
            int nv = (int)n.cellIdxZ + kClimbDeltas[d][1];
            if (nu < 0 || nu >= (int)anchor.cellsU) continue;
            if (nv < 0 || nv >= (int)anchor.cellsV) continue;
            uint32_t nbKey = ((uint32_t)nv << 16) | (uint32_t)nu;
            auto it = uvToIdx.find(nbKey);
            if (it == uvToIdx.end()) continue;  // grid hole
            uint16_t toIdx = it->second;
            if (toIdx <= fromIdx) continue;  // dedup undirected pair
            NavEdge edge{};
            edge.fromIdx = fromIdx;
            edge.toIdx   = toIdx;
            edge.cost    = kClimbCellEdgeCost;
            out->edges.push_back(edge);
            surfaceEdgeCount++;
        }
    }

    // Floor ↔ climb boundary edges. FindNearestNode skips climb-surface
    // nodes (Stage 2 defensive change) so these return floor indices.
    int floorBaseIdx = FindNearestNode(out, anchor.basePos);
    int floorTopIdx  = FindNearestNode(out, anchor.topPos);
    size_t boundaryEdgeCount = 0;
    for (uint16_t i = 0; i < anchor.nodeCount; i++) {
        const NavNode& n = out->nodes[(size_t)anchor.firstNodeIdx + i];
        uint16_t climbIdx = (uint16_t)(anchor.firstNodeIdx + i);
        if (n.cellIdxZ == 0 && floorBaseIdx >= 0 &&
            (uint16_t)floorBaseIdx != climbIdx) {
            NavEdge edge{};
            edge.fromIdx = (uint16_t)std::min<int>(floorBaseIdx, (int)climbIdx);
            edge.toIdx   = (uint16_t)std::max<int>(floorBaseIdx, (int)climbIdx);
            edge.cost    = kFloorToClimbCost;
            out->edges.push_back(edge);
            boundaryEdgeCount++;
        }
        if (n.cellIdxZ == (uint16_t)(anchor.cellsV - 1) && floorTopIdx >= 0 &&
            (uint16_t)floorTopIdx != climbIdx) {
            NavEdge edge{};
            edge.fromIdx = (uint16_t)std::min<int>(floorTopIdx, (int)climbIdx);
            edge.toIdx   = (uint16_t)std::max<int>(floorTopIdx, (int)climbIdx);
            edge.cost    = kClimbToFloorCost;
            out->edges.push_back(edge);
            boundaryEdgeCount++;
        }
    }

    SPDLOG_DEBUG("[RoomNav] Climb-surface edges (anchor surfaceType=0x{:04x}): "
                 "{} surface, {} boundary",
                 (unsigned)anchor.surfaceType, surfaceEdgeCount, boundaryEdgeCount);
}

// Drop edges that reference any climb-surface node (used by the rescan
// path before nodes.resize() invalidates the indices). Also called when
// no climb anchors exist after rescan (cleans up stale edges if they
// somehow persisted).
//
// O(N) walk over edges. Climb edges sit at the tail of the vector
// (always appended after BuildEdges runs floor-side), so a single
// resize-from-back is correct in the typical case — but a defensive
// erase-remove handles any out-of-order entries safely.
static void DropClimbSurfaceEdges(RoomNavData* out) {
    if (out->edges.empty()) return;
    if (out->firstClimbSurfaceNodeIdx == UINT16_MAX) return;
    uint16_t firstClimbIdx = out->firstClimbSurfaceNodeIdx;
    auto newEnd = std::remove_if(out->edges.begin(), out->edges.end(),
        [firstClimbIdx](const NavEdge& e) {
            return e.fromIdx >= firstClimbIdx || e.toIdx >= firstClimbIdx;
        });
    out->edges.erase(newEnd, out->edges.end());
}

// For each ClimbAnchor, generate the surface grid: compute plane params
// (normal, axes, origin, extents), raycast each cell, append a NavNode
// per hit, and populate the anchor's firstNodeIdx/nodeCount/surfaceType.
//
// Append-only — never re-orders existing nodes. Caller must invoke this
// AFTER BuildEdges and AFTER the floor-side NODE_CLIMB_BASE/TOP tagging
// (which uses FindNearestNode and would otherwise match a climb-surface
// node instead of the intended floor node). Caller MUST also track
// the first appended index in `out->firstClimbSurfaceNodeIdx` so the
// rescan path can drop the previous round before reconstructing
// nodesByCell from cellIdxX/Z (climb-surface nodes use U/V coords
// there, not world cells).
//
// Stage 3: each anchor's edges are appended via GenerateClimbSurfaceEdges
// immediately after its nodes — keeps per-anchor data co-located so a
// debug overlay can iterate one anchor's footprint without scanning the
// whole vector.
static void GenerateClimbSurfaceGrids(RoomNavData* out, PlayState* play) {
    if (out->climbAnchors.empty()) return;

    out->firstClimbSurfaceNodeIdx = (uint16_t)out->nodes.size();
    size_t totalNodesAdded = 0;

    for (ClimbAnchor& anchor : out->climbAnchors) {
        Vec3f rise = V3Sub(anchor.topPos, anchor.basePos);
        float climbHeight = V3Len(rise);
        if (climbHeight < kClimbMinAnchorHeight) continue;

        Vec3f normal;
        if (!ComputeClimbSurfaceNormal(anchor.basePos, play, normal)) continue;

        // axisV = world-up; axisU = lateral (perpendicular to up + normal).
        // Vertical-only climb (we don't model curved/spiral surfaces).
        Vec3f axisV = { 0.0f, 1.0f, 0.0f };
        Vec3f axisU = V3Normalize(V3Cross(axisV, normal));
        if (V3Len(axisU) < 1e-3f) continue;

        // Surface-type pre-classification: Path A → LADDER. Path B → probe
        // a representative cell at basePos to read the wall-flag bits.
        uint16_t expectedType = 0;
        if (anchor.actorId != 0) {
            expectedType = NODE_CLIMB_LADDER;
        } else {
            Vec3f probeCenter = { anchor.basePos.x,
                                  anchor.basePos.y + kClimbProbeYAtBase,
                                  anchor.basePos.z };
            Vec3f hitPos;
            s32 wallFlags = 0;
            if (RaycastClimbCell(play, probeCenter, normal, hitPos, wallFlags)) {
                expectedType = ClassifyClimbWallFlags(wallFlags);
            }
            if (expectedType == 0) {
                expectedType = NODE_CLIMB_DESIGNATED_WALL; // conservative fallback
            }
        }

        // Determine width (Path A actors are single-column; Path B is probed).
        float climbWidth = (anchor.actorId != 0)
            ? kClimbPathAActorWidth
            : DetermineClimbSurfaceWidth(play, anchor.basePos, normal, axisU, expectedType);
        if (climbWidth < kClimbGridCellSpacing) climbWidth = kClimbGridCellSpacing;

        uint16_t cellsU = (uint16_t)std::min<int>(
            (int)kClimbMaxCellsU,
            std::max<int>(1, (int)std::ceil(climbWidth / kClimbGridCellSpacing)));
        uint16_t cellsV = (uint16_t)std::min<int>(
            (int)kClimbMaxCellsV,
            std::max<int>(1, (int)std::ceil(climbHeight / kClimbGridCellSpacing)));

        // Plane origin = basePos shifted half the width along -axisU
        // (so column u==cellsU/2 is centered on basePos).
        Vec3f origin = { anchor.basePos.x - axisU.x * (climbWidth * 0.5f),
                         anchor.basePos.y,
                         anchor.basePos.z - axisU.z * (climbWidth * 0.5f) };

        uint16_t firstNodeIdx = (uint16_t)out->nodes.size();
        uint16_t nodeCount    = 0;
        for (uint16_t v = 0; v < cellsV; v++) {
            for (uint16_t u = 0; u < cellsU; u++) {
                Vec3f cellCenter = {
                    origin.x + axisU.x * (u * kClimbGridCellSpacing),
                    origin.y + axisV.y * (v * kClimbGridCellSpacing),
                    origin.z + axisU.z * (u * kClimbGridCellSpacing),
                };
                Vec3f hitPos;
                s32 wallFlags = 0;
                if (!RaycastClimbCell(play, cellCenter, normal, hitPos, wallFlags)) continue;
                uint16_t typeBit = ClassifyClimbWallFlags(wallFlags);
                if (typeBit == 0) continue;
                // For Path A actors we override with LADDER regardless of
                // wall-flag classification (the actor's geometry may not
                // carry the wall-flag bit; the actor's identity is the
                // ground truth). For Path B we require type-match (so a
                // mixed-type adjacent wall doesn't pollute the grid).
                if (anchor.actorId != 0) {
                    typeBit = NODE_CLIMB_LADDER;
                } else if (typeBit != expectedType) {
                    continue;
                }
                NavNode node;
                node.pos      = hitPos;
                node.flags    = typeBit;
                if (v == 0 || v == (uint16_t)(cellsV - 1)) {
                    node.flags |= NODE_CLIMB_BOUNDARY;
                }
                node.cellIdxX = u;  // U/V grid coord, NOT world cell
                node.cellIdxZ = v;
                out->nodes.push_back(node);
                nodeCount++;
            }
        }

        anchor.planeOrigin    = origin;
        anchor.planeNormal    = normal;
        anchor.planeAxisU     = axisU;
        anchor.planeAxisV     = axisV;
        anchor.cellsU         = cellsU;
        anchor.cellsV         = cellsV;
        anchor.firstNodeIdx   = firstNodeIdx;
        anchor.nodeCount      = nodeCount;
        anchor.surfaceType    = expectedType;
        totalNodesAdded += nodeCount;

        // Stage 3: append this anchor's edges (surface 4-connected +
        // boundary↔floor). Edges are inert at the BFS layer until
        // Stage 5 lifts the NODE_WALKABLE expansion gate; this just
        // lays the data.
        GenerateClimbSurfaceEdges(out, anchor);
    }

    // If no anchor produced any node, restore sentinel so rescan won't
    // try to truncate the already-empty climb section.
    if (totalNodesAdded == 0) {
        out->firstClimbSurfaceNodeIdx = UINT16_MAX;
    } else {
        SPDLOG_INFO("[RoomNav] Climb-surface grids: {} anchors, {} new nodes appended at idx {}",
                    out->climbAnchors.size(), totalNodesAdded,
                    (unsigned)out->firstClimbSurfaceNodeIdx);
    }
}

// ---------------------------------------------------------------------------
// Ledge-grab detection (Phase 1 — detection only). Identifies pairs of
// walkable nodes (approachPos, topPos) where:
//   - topPos.y - approachPos.y is in the ledge-grab range (above step-up
//     height, below Link's max jump-grab reach)
//   - The two are within an XZ tolerance (so the player can plausibly
//     jump from approachPos and reach topPos's edge)
//   - A wall exists between them (the grabbable rim)
//
// The wall-existence check is the key quality filter: stairs and
// elevation steps satisfy the Y-delta + XZ predicate but DON'T have a
// wall between them, so MovementClear succeeds and they're rejected.
// True ledges have a wall blocking the line test.
//
// Phase 1 (this commit) populates `ledgeAnchors` and supports debug-draw
// visualization. Phase 2 (separate commit) wires AI Follower to USE the
// anchors via jump-injection in HookHandlers.cpp's input hook.
// ---------------------------------------------------------------------------

static void DetectLedgeAnchors(
    RoomNavData* out,
    PlayState* play,
    const std::unordered_map<CellKey, std::vector<uint16_t>, CellKeyHash>& nodesByCell)
{
    if (CVarGetInteger(CVAR_ROOM_NAV_LEDGE_GRAB, 0) == 0) return;

    // Step-up edges already cover deltas <= 30; below that range it's a
    // walkable edge, not a ledge. The upper bound is CVar-tunable so the
    // user can extend the range to catch taller climbables that aren't
    // caught by Path A or Path B (e.g. a static-geometry ladder whose
    // collision polys don't carry the climbable wall-flag bit). 150 is
    // the realistic Link jump-grab reach; values up to ~500 catch
    // multi-rung ladders for diagnostic purposes (with the caveat that
    // the navigator can't actually jump-grab those — they're effectively
    // mis-typed climb anchors).
    constexpr float kLedgeMinDeltaY    = 30.0f;
    // Default 70u matches realistic child-Link jump-grab reach. Adult
    // Link can grab slightly higher (~90u). User can tune via the
    // RoomNavData.LedgeGrabMaxDeltaY CVar slider (range 30-500).
    int32_t maxDeltaInt = CVarGetInteger(CVAR_ROOM_NAV_LEDGE_MAX_DELTA, 70);
    if (maxDeltaInt < 30)   maxDeltaInt = 30;
    if (maxDeltaInt > 1000) maxDeltaInt = 1000;
    const float kLedgeMaxDeltaY = (float)maxDeltaInt;
    // Approach and top must be within ~80u XZ — the player jumps roughly
    // forward; targets further than that aren't physically reachable.
    constexpr float kLedgeMaxXZSq      = 80.0f * 80.0f;
    // Cluster radius: anchors with both endpoints within ~40u of an
    // existing anchor are considered duplicates (different node pairs
    // discovering the same physical ledge from slightly different
    // approach positions).
    constexpr float kClusterRadiusSq   = 40.0f * 40.0f;

    size_t added = 0, merged = 0;

    for (uint16_t i = 0; i < out->nodes.size(); i++) {
        const NavNode& a = out->nodes[i];
        if (!(a.flags & NODE_WALKABLE)) continue;

        CellKey aCell{ (int32_t)(int16_t)a.cellIdxX, (int32_t)(int16_t)a.cellIdxZ };

        // 3×3 cell neighborhood including same cell. Same-cell pairs
        // catch verticals where approach is directly under the ledge.
        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                CellKey nCell{ aCell.x + dx, aCell.z + dz };
                auto it = nodesByCell.find(nCell);
                if (it == nodesByCell.end()) continue;
                for (uint16_t j : it->second) {
                    if (j == i) continue;
                    const NavNode& b = out->nodes[j];
                    if (!(b.flags & NODE_WALKABLE)) continue;

                    // b is the top candidate (must be HIGHER than a).
                    f32 dy = b.pos.y - a.pos.y;
                    if (dy < kLedgeMinDeltaY || dy > kLedgeMaxDeltaY) continue;

                    f32 dxf = b.pos.x - a.pos.x;
                    f32 dzf = b.pos.z - a.pos.z;
                    if (dxf*dxf + dzf*dzf > kLedgeMaxXZSq) continue;

                    // Wall-confirmation: MovementClear at body height
                    // returns true when the line is unblocked. We need
                    // a wall to exist (NOT clear) between approach and
                    // top — that's the rim Link grabs. Walls are what
                    // distinguishes a ledge from a step or staircase.
                    if (MovementClear(a.pos, b.pos, play)) continue;

                    // Cluster: skip if both endpoints match an existing
                    // anchor's endpoints within radius. Different (i, j)
                    // pairs that discover the same physical ledge from
                    // slightly different approach positions collapse to
                    // one anchor.
                    bool isDuplicate = false;
                    for (const LedgeAnchor& existing : out->ledgeAnchors) {
                        f32 ax = existing.approachPos.x - a.pos.x;
                        f32 az = existing.approachPos.z - a.pos.z;
                        f32 tx = existing.topPos.x      - b.pos.x;
                        f32 tz = existing.topPos.z      - b.pos.z;
                        if ((ax*ax + az*az < kClusterRadiusSq) &&
                            (tx*tx + tz*tz < kClusterRadiusSq)) {
                            isDuplicate = true;
                            merged++;
                            break;
                        }
                    }
                    if (isDuplicate) continue;

                    LedgeAnchor anchor{};
                    anchor.approachPos = a.pos;
                    anchor.topPos      = b.pos;
                    out->ledgeAnchors.push_back(anchor);
                    added++;
                }
            }
        }
    }

    if (added > 0 || merged > 0) {
        SPDLOG_INFO("[RoomNav] Ledge-grab detection: {} anchors added, {} duplicates merged",
                    added, merged);
    }
}

// ---------------------------------------------------------------------------
// Crawlspace detection. Identifies wall polys with the crawlspace flag
// bits set and registers them as CrawlspaceAnchor entries. Also tags
// nearby walkable nodes with NODE_CRAWLSPACE.
//
// Detection mechanism (verified against z_player.c:7639):
//   if (!LINK_IS_ADULT && (interactWallFlags & 0x30)) { ...crawlspace
//   prompt + entry... }
//
// 0x30 = bits 4 + 5. In the wall-property lookup table D_80119D90:
//   idx 5 → 0x10 (bit 4) — crawlspace flag part A
//   idx 6 → 0x20 (bit 5) — crawlspace flag part B
//
// OoT crawlspaces have walls on BOTH ends (entry-front + entry-back per
// z_player.c:7758-7762). Each side gets its own set of polys with
// these flags, registered as separate anchors. The consumer (Phase 2)
// pairs them at navigation time.
//
// Nodes within kCrawlspaceTagRadius of any anchor get NODE_CRAWLSPACE
// set. Heuristic — the actual crawlspace tunnel volume isn't bounded
// by the wall polys alone; the tunnel extends between entry and exit
// walls. Future refinement could ray-cast between paired anchors and
// tag nodes along the line.
// ---------------------------------------------------------------------------

static constexpr s32   kWallFlagCrawlspaceMask    = (1 << 4) | (1 << 5);   // 0x30
static constexpr float kCrawlspaceClusterRadiusXZ = 30.0f;
static constexpr float kCrawlspaceTagRadius       = 60.0f; // node tagging proximity

static void DetectCrawlspaces(
    RoomNavData* out,
    PlayState* play,
    const std::unordered_set<CellKey, CellKeyHash>& visited)
{
    if (CVarGetInteger(CVAR_ROOM_NAV_CRAWLSPACE, 0) == 0) return;

    CollisionHeader* hdr = play->colCtx.colHeader;
    if (hdr == nullptr || hdr->polyList == nullptr || hdr->vtxList == nullptr) return;

    Vec3s* vtxList = hdr->vtxList;
    u16    numPolys = hdr->numPolygons;
    u16    numVtx   = hdr->numVertices;

    constexpr float kClusterRadiusSq = kCrawlspaceClusterRadiusXZ * kCrawlspaceClusterRadiusXZ;

    size_t added = 0, merged = 0;

    // Pass 1 — discover crawlspace-flagged walls, cluster, register anchors.
    for (u16 i = 0; i < numPolys; i++) {
        CollisionPoly* poly = &hdr->polyList[i];

        // Wall classification (same threshold as Path B).
        f32 normalY = (f32)poly->normal.y / 32767.0f;
        if (std::fabs(normalY) > 0.5f) continue;

        // Crawlspace-flag check.
        s32 wallFlags = func_80041DB8(&play->colCtx, poly, BGCHECK_SCENE);
        if ((wallFlags & kWallFlagCrawlspaceMask) == 0) continue;

        // Vertex indices.
        u16 viA = poly->flags_vIA & 0x1FFF;
        u16 viB = poly->flags_vIB & 0x1FFF;
        u16 viC = poly->vIC       & 0x1FFF;
        if (viA >= numVtx || viB >= numVtx || viC >= numVtx) continue;

        const Vec3s& vA = vtxList[viA];
        const Vec3s& vB = vtxList[viB];
        const Vec3s& vC = vtxList[viC];

        f32 cx = ((f32)vA.x + (f32)vB.x + (f32)vC.x) / 3.0f;
        f32 cy = ((f32)vA.y + (f32)vB.y + (f32)vC.y) / 3.0f;
        f32 cz = ((f32)vA.z + (f32)vB.z + (f32)vC.z) / 3.0f;

        // Wall normal in world units (Vec3s, /32767 ≈ unit length).
        f32 nx = (f32)poly->normal.x / 32767.0f;
        f32 nz = (f32)poly->normal.z / 32767.0f;

        // Room scoping.
        CellKey centroidCell = CellKeyForXZ(cx, cz, out->bboxMin);
        if (visited.count(centroidCell) == 0) continue;

        // Cluster with existing anchors.
        bool clustered = false;
        for (CrawlspaceAnchor& existing : out->crawlspaceAnchors) {
            f32 dx = existing.entryPos.x - cx;
            f32 dz = existing.entryPos.z - cz;
            if (dx*dx + dz*dz < kClusterRadiusSq) {
                // Average the centroid + normal over clustered polys for
                // a more stable representative position. Simple running
                // mean — not weighted by poly area, which would be more
                // accurate but adds complexity.
                existing.entryPos.x    = (existing.entryPos.x    + cx) * 0.5f;
                existing.entryPos.y    = (existing.entryPos.y    + cy) * 0.5f;
                existing.entryPos.z    = (existing.entryPos.z    + cz) * 0.5f;
                existing.entryNormal.x = (existing.entryNormal.x + nx) * 0.5f;
                existing.entryNormal.y = (existing.entryNormal.y +  0.0f) * 0.5f;
                existing.entryNormal.z = (existing.entryNormal.z + nz) * 0.5f;
                clustered = true;
                merged++;
                break;
            }
        }

        if (!clustered) {
            CrawlspaceAnchor anchor{};
            anchor.entryPos    = { cx, cy, cz };
            anchor.entryNormal = { nx, 0.0f, nz };
            out->crawlspaceAnchors.push_back(anchor);
            added++;
        }
    }

    // Pass 2 — tag walkable nodes near any anchor with NODE_CRAWLSPACE.
    if (!out->crawlspaceAnchors.empty()) {
        constexpr float kTagRadiusSq = kCrawlspaceTagRadius * kCrawlspaceTagRadius;
        size_t taggedNodes = 0;
        for (NavNode& node : out->nodes) {
            if (!(node.flags & NODE_WALKABLE)) continue;
            for (const CrawlspaceAnchor& anchor : out->crawlspaceAnchors) {
                f32 dx = node.pos.x - anchor.entryPos.x;
                f32 dy = node.pos.y - anchor.entryPos.y;
                f32 dz = node.pos.z - anchor.entryPos.z;
                if (dx*dx + dy*dy + dz*dz < kTagRadiusSq) {
                    node.flags |= NODE_CRAWLSPACE;
                    taggedNodes++;
                    break;
                }
            }
        }

        SPDLOG_INFO("[RoomNav] Crawlspace detection: {} anchors added, {} polys merged, "
                    "{} nodes tagged NODE_CRAWLSPACE",
                    added, merged, taggedNodes);
    }
}

// ---------------------------------------------------------------------------
// Drop anchor detection. Symmetric to ledge-grab detection but for
// descent — pairs of (highPos, landingPos) where a navigator can step
// off and safely fall to the lower position.
//
// Detection: for each pair of walkable nodes (A, B) where:
//   - A is HIGHER than B by [30, kDropMaxDeltaY] (above step-up;
//     within survivable fall distance)
//   - XZ distance within ~80u (drop is mostly downward; large lateral
//     drops require explicit jump mechanics not modeled here)
//   - MovementClear from A to B SUCCEEDS (no wall in between — opposite
//     of the ledge predicate)
// Register a DropAnchor (highPos = A.pos, landingPos = B.pos).
//
// Cluster duplicates by proximity at both endpoints (same shape as
// ledge clustering).
//
// Phase 1: detection + viz only. Phase 2 wires consumers (autonomous
// nav for AI Invader / similar) to actually USE drop anchors as
// descent edges in pathfinding. NavTraits gates per-navigator opt-in;
// adult Link can survive larger drops than child Link.
// ---------------------------------------------------------------------------

static void DetectDropAnchors(
    RoomNavData* out,
    PlayState* play,
    const std::unordered_map<CellKey, std::vector<uint16_t>, CellKeyHash>& nodesByCell)
{
    if (CVarGetInteger(CVAR_ROOM_NAV_DROP_ANCHOR, 0) == 0) return;

    // Drop range: above step-up height (covered by walkable edges
    // already), below survivable-fall height. 200u is roughly child
    // Link's pain threshold; adult Link can take larger. Could be
    // CVar-tuned later if needed.
    constexpr float kDropMinDeltaY     = 30.0f;
    constexpr float kDropMaxDeltaY     = 200.0f;
    constexpr float kDropMaxXZSq       = 80.0f * 80.0f;
    constexpr float kClusterRadiusSq   = 40.0f * 40.0f;

    size_t added = 0, merged = 0;

    for (uint16_t i = 0; i < out->nodes.size(); i++) {
        const NavNode& a = out->nodes[i];
        if (!(a.flags & NODE_WALKABLE)) continue;

        CellKey aCell{ (int32_t)(int16_t)a.cellIdxX, (int32_t)(int16_t)a.cellIdxZ };

        // 3×3 cell neighborhood. Same-cell pairs catch verticals where
        // landing is directly below the high position.
        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                CellKey nCell{ aCell.x + dx, aCell.z + dz };
                auto it = nodesByCell.find(nCell);
                if (it == nodesByCell.end()) continue;
                for (uint16_t j : it->second) {
                    if (j == i) continue;
                    const NavNode& b = out->nodes[j];
                    if (!(b.flags & NODE_WALKABLE)) continue;

                    // b is the landing candidate (must be LOWER than a).
                    f32 dy = a.pos.y - b.pos.y;
                    if (dy < kDropMinDeltaY || dy > kDropMaxDeltaY) continue;

                    f32 dxf = b.pos.x - a.pos.x;
                    f32 dzf = b.pos.z - a.pos.z;
                    if (dxf*dxf + dzf*dzf > kDropMaxXZSq) continue;

                    // Drop predicate: line from high to low must be
                    // CLEAR (no wall blocking the fall path). Opposite
                    // of the ledge-grab predicate which requires a
                    // wall between approach and top.
                    if (!MovementClear(a.pos, b.pos, play)) continue;

                    // Cluster: skip if both endpoints match an existing
                    // anchor's endpoints within radius.
                    bool isDuplicate = false;
                    for (const DropAnchor& existing : out->dropAnchors) {
                        f32 hx = existing.highPos.x    - a.pos.x;
                        f32 hz = existing.highPos.z    - a.pos.z;
                        f32 lx = existing.landingPos.x - b.pos.x;
                        f32 lz = existing.landingPos.z - b.pos.z;
                        if ((hx*hx + hz*hz < kClusterRadiusSq) &&
                            (lx*lx + lz*lz < kClusterRadiusSq)) {
                            isDuplicate = true;
                            merged++;
                            break;
                        }
                    }
                    if (isDuplicate) continue;

                    DropAnchor anchor{};
                    anchor.highPos    = a.pos;
                    anchor.landingPos = b.pos;
                    out->dropAnchors.push_back(anchor);
                    added++;
                }
            }
        }
    }

    if (added > 0 || merged > 0) {
        SPDLOG_INFO("[RoomNav] Drop-anchor detection: {} anchors added, {} duplicates merged",
                    added, merged);
    }
}

// Classify a single floor candidate as a NavNode and append to nav.
// Returns the index of the appended node, or -1 if the candidate was
// rejected entirely. v1 classifies WALKABLE / STEEP_SLOPE only; HAZARD
// and UNDERWATER flags are added in commit 5.
static int ClassifyAndAddNode(RoomNavData* nav,
                              float x, float floorY, float z,
                              CollisionPoly* floorPoly,
                              s32 floorBgId,
                              PlayState* play,
                              const CellKey& cell) {
    if (floorPoly == nullptr) return -1;

    uint16_t flags = 0;

    // Slope filter — reject 3 (very-steep) for walkable destinations,
    // tag as STEEP_SLOPE for transient pass-through (slide-down recovery).
    s32 slopeType = SurfaceType_GetSlope(&play->colCtx, floorPoly, floorBgId);
    if (slopeType == 3) {
        flags |= NODE_STEEP_SLOPE;
    } else {
        flags |= NODE_WALKABLE;
    }

    // Hazard classification (commit 5) — see IsHazardousSurface above for
    // verified bit positions and rationale. Hazard nodes are still
    // walkable in the navigation sense (a navigator CAN end up on one
    // and walk off it) but consumer-side preference routes around them.
    if (IsHazardousSurface(floorPoly, floorBgId, &play->colCtx)) {
        flags |= NODE_HAZARD;
    }

    // Underwater classification (commit 5) — water-volume detection via
    // WaterBox_GetSurface1. Underwater nodes are walkable for swim-capable
    // navigators (Link-rigged: AI Follower, NPC Invader) and skipped by
    // non-swimming navigators at consumer-side via the NavTraits filter.
    if (IsUnderwater(x, floorY, z, play)) {
        flags |= NODE_UNDERWATER;
    }

    // Commit 5 will add: HAZARD and UNDERWATER flags.

    NavNode node{};
    node.pos.x = x;
    node.pos.y = floorY;
    node.pos.z = z;
    node.flags = flags;
    // Cell indices clamped to uint16 range; rooms exceeding 65535 cells in
    // either axis are pathological. Defensive truncation; not expected in
    // any real OoT scene.
    node.cellIdxX = (uint16_t)(cell.x & 0xFFFF);
    node.cellIdxZ = (uint16_t)(cell.z & 0xFFFF);

    nav->nodes.push_back(node);
    return (int)nav->nodes.size() - 1;
}

// Multi-cast all stacked floors at one XZ cell. Returns the index of the
// FIRST node added (or -1 if none). Subsequent nodes (for stacked floors)
// are appended to nav.nodes and discoverable via index >= firstIdx.
//
// Uses BgCheck_AnyRaycastFloor2 (functions.h:603) which fills a CollisionPoly
// by value AND returns the bgId; the bgId is needed for SurfaceType_*
// accessors when the floor lies on a Bg actor's dynamic collision rather
// than scene static collision.
// Solution B from Plans/room_nav_floodfill_scoping_investigation.md.
// Defensive filter: when the discovered floor lies on a dynamic Bg actor
// (bgId != BGCHECK_SCENE), check whether that actor belongs to a different
// room than the one currently being scanned. Reject if so.
//
// In practice this filter rarely fires — OoT typically despawns a room's
// Bg actors when the player enters a different room, so cross-room dynamic
// floor isn't usually present in colCtx.dyna at scan time. But the check
// is essentially free (1 pointer deref + 1 comparison) and catches the
// edge case where a transition-in-progress race leaves cross-room actors
// momentarily registered. Defensive layer atop Solution A's MovementClear
// floodfill gate.
//
// `actor->room == -1` means "global; doesn't despawn on room change" —
// these are explicitly NOT rejected. Only mismatched non-negative rooms
// are filtered.
static bool FloorBelongsToOtherRoom(s32 floorBgId, s8 currentRoom, PlayState* play) {
    if (floorBgId < 0 || floorBgId >= BG_ACTOR_MAX) return false; // not dynamic
    Actor* owner = play->colCtx.dyna.bgActors[floorBgId].actor;
    if (owner == nullptr) return false;
    if (owner->room < 0) return false;         // global actor
    return owner->room != currentRoom;
}

// Per-actor floor rejection allowlist. When the discovered floor is owned
// by a Bg actor whose actor->id is in this list, treat it as if the floor
// didn't exist for nav purposes. Common case: chest tops (En_Box) and push
// blocks (Obj_Oshihiki) — physically walkable but undesirable as nav
// targets because their position is dynamic (chests open in place but
// might rise; push blocks move) and stepping onto them is rarely useful
// for AI.
//
// Maintenance: extend kFloorActorRejectList as new problematic dynamic
// actors surface during field testing. Keep entries sorted by ascending
// actor ID for diff readability.
static const int16_t kFloorActorRejectList[] = {
    // Push blocks (Obj_Oshihiki, 0x0140) used to be on this list, but
    // their tops ARE legitimately walkable — and their vertical edges
    // are climbable ledges that NPCs should be able to grab. Removed
    // 2026-05-08 per field-test feedback. The block-top floors now
    // become regular nav nodes; their wall faces feed into Path B
    // and ledge-grab detection naturally.
    ACTOR_EN_BOX,       // 0x000A — chests
};

// Returns true if the floor at (x, floorY, z) sitting on Bg actor `floorBgId`
// is in the per-actor rejection allowlist. When `nav` is non-null AND the
// LogRejectedFloors CVar is on AND a rejection fires, the (x, floorY, z)
// position is appended to nav->rejectedFloorPositions for the DebugDraw
// magenta-cross overlay.
//
// `nav` may be null when called from contexts that don't have a target
// RoomNavData (none today, but the contract leaves the door open).
static bool FloorIsRejectedByAllowlist(s32 floorBgId, PlayState* play,
                                       RoomNavData* nav, float x, float floorY, float z) {
    if (floorBgId < 0 || floorBgId >= BG_ACTOR_MAX) return false; // not dynamic
    Actor* owner = play->colCtx.dyna.bgActors[floorBgId].actor;
    if (owner == nullptr) return false;
    for (int16_t rejectId : kFloorActorRejectList) {
        if (owner->id == rejectId) {
            // Diagnostic capture (DebugDraw v3e, polish wave commit 6).
            // Cheap to leave gated here so the predicate stays a single
            // call site for callers; the inner CVar check resolves to a
            // fast path-1 lookup when off.
            if (nav != nullptr && CVarGetInteger(CVAR_ROOM_NAV_LOG_REJECTED_FLOORS, 0) != 0) {
                nav->rejectedFloorPositions.push_back(Vec3f{ x, floorY, z });
            }
            return true;
        }
    }
    return false;
}

static int ScanColumnAt(RoomNavData* nav, float x, float z, PlayState* play, const CellKey& cell) {
    int firstNodeIdx = -1;
    float startY = nav->bboxMax.y + 50.0f; // start above scene ceiling
    s8 currentRoom = (s8)play->roomCtx.curRoom.num;

    for (int i = 0; i < kMaxFloorsPerColumn; i++) {
        Vec3f castOrigin = { x, startY, z };
        CollisionPoly floorPoly{}; // BgCheck_AnyRaycastFloor2 copies the poly by value
        s32 floorBgId = BGCHECK_SCENE;
        f32 floorY = BgCheck_AnyRaycastFloor2(&play->colCtx, &floorPoly, &floorBgId, &castOrigin);

        if (floorY <= BGCHECK_Y_MIN) break;       // no floor below
        if (floorY <= nav->bboxMin.y) break;      // below scene
        if (floorY >= startY) break;              // raycast didn't make progress (degenerate)

        // Solution B: skip floors whose owning Bg actor is in a different
        // room. Static-collision floors (bgId == BGCHECK_SCENE) and global
        // dynamic actors (room == -1) pass through.
        if (FloorBelongsToOtherRoom(floorBgId, currentRoom, play)) {
            startY = floorY - 1.0f;  // skip past this floor; keep looking below
            continue;
        }

        // Per-actor allowlist: chest tops, push-block tops, etc. are
        // physically walkable but should not be navigable. Treat the
        // floor as if it didn't exist; the column scan continues past
        // it to find the real floor below. When the LogRejectedFloors
        // diagnostic CVar is on, the rejected (x, floorY, z) is captured
        // into nav->rejectedFloorPositions for the DebugDraw overlay.
        if (FloorIsRejectedByAllowlist(floorBgId, play, nav, x, floorY, z)) {
            startY = floorY - 1.0f;
            continue;
        }

        int idx = ClassifyAndAddNode(nav, x, floorY, z, &floorPoly, floorBgId, play, cell);
        if (idx >= 0 && firstNodeIdx < 0) {
            firstNodeIdx = idx;
        }

        startY = floorY - 1.0f; // step below this floor for next iteration
    }

    return firstNodeIdx;
}

static void ScanRoom(int16_t sceneNum, int8_t roomNum, PlayState* play, RoomNavData* out) {
    out->sceneNum       = sceneNum;
    out->roomNum        = roomNum;
    out->scanTimestamp  = (uint32_t)std::time(nullptr); // wall-clock seconds for debug
    out->bboxMin        = play->colCtx.minBounds;
    out->bboxMax        = play->colCtx.maxBounds;
    out->gridResolution = (uint16_t)kGridResolution;

    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        SPDLOG_WARN("[RoomNav] ScanRoom: no player actor; aborting scan");
        return;
    }

    // Seed the floodfill at the player's current cell.
    Vec3f playerPos = player->actor.world.pos;
    CellKey seedCell = CellKeyForXZ(playerPos.x, playerPos.z, out->bboxMin);

    auto scanStart = std::chrono::steady_clock::now();

    std::unordered_set<CellKey, CellKeyHash> visited;
    std::deque<CellKey> pending;
    pending.push_back(seedCell);

    // Stash every seed cell pushed to `pending` so the post-edge orphan
    // pass below can identify which scanned nodes are reachable from a
    // seed (and which are not). We use the stashed list rather than
    // re-walking the actor lists at orphan-pass time because actors may
    // move or despawn between seed-time and orphan-pass-time, producing a
    // different cell set. Per handoff plan §5 commit 1.
    std::vector<CellKey> seedCells;
    seedCells.push_back(seedCell);

    // Multi-seed: every actor whose room field matches the room being
    // scanned (or is global, room == -1) becomes an additional seed.
    // Required because OoT engine rooms can contain multiple sub-chambers
    // connected only by climbs / drops / jumps, none of which the
    // horizontal MovementClear test traverses. A single player-position
    // seed reaches only the chamber the player entered through; sub-
    // chambers (e.g. the slingshot chamber and the hintnut antechamber
    // sharing one engine room in Inside Deku Tree) end up with zero
    // coverage.
    //
    // Seeding from every in-room actor guarantees each connected sub-
    // region gets at least one seed somewhere inside it. Duplicate seeds
    // (multiple actors in the same cell) are absorbed at the
    // `if (visited.count(cell)) continue;` early-out — no extra work.
    //
    // Solution A's room-scoping is preserved because we only seed from
    // actors whose room field matches the current scan target. The
    // floodfill from each seed is still gated by MovementClear and
    // can't bleed into adjacent rooms.
    int extraSeeds = 0;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        Actor* a = play->actorCtx.actorLists[cat].head;
        while (a != nullptr) {
            if (a->room == roomNum || a->room == -1) {
                CellKey actorCell = CellKeyForXZ(a->world.pos.x, a->world.pos.z, out->bboxMin);
                pending.push_back(actorCell);
                seedCells.push_back(actorCell);
                extraSeeds++;
            }
            a = a->next;
        }
    }

    // Historical seeds — positions used as floodfill seeds in prior
    // scans of this room, persisted across sessions. Re-seeding from
    // them preserves coverage even when the actors that originally
    // seeded an area have died or despawned. Cell-deduped at insertion
    // time, so the historical list is naturally bounded to one entry
    // per grid cell.
    int historicalSeedsUsed = 0;
    for (const Vec3f& histPos : out->historicalSeeds) {
        CellKey histCell = CellKeyForXZ(histPos.x, histPos.z, out->bboxMin);
        pending.push_back(histCell);
        seedCells.push_back(histCell);
        historicalSeedsUsed++;
    }

    int iterations = 0;

    while (!pending.empty()) {
        if (++iterations > kMaxScanIterations) {
            SPDLOG_WARN("[RoomNav] ScanRoom: kMaxScanIterations ({}) exceeded; "
                        "accepting partial scan ({} nodes so far)",
                        kMaxScanIterations, out->nodes.size());
            break;
        }
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - scanStart).count();
        if (elapsedMs > kMaxScanWallTimeMs) {
            SPDLOG_WARN("[RoomNav] ScanRoom: kMaxScanWallTimeMs ({}ms) exceeded; "
                        "accepting partial scan ({} nodes so far)",
                        kMaxScanWallTimeMs, out->nodes.size());
            break;
        }

        CellKey cell = pending.front();
        pending.pop_front();
        if (visited.count(cell)) continue;
        visited.insert(cell);

        // Bounds check
        Vec3f cellWorld = CellCenterWorld(cell, out->bboxMin);
        if (cellWorld.x < out->bboxMin.x || cellWorld.x > out->bboxMax.x) continue;
        if (cellWorld.z < out->bboxMin.z || cellWorld.z > out->bboxMax.z) continue;

        int firstIdx = ScanColumnAt(out, cellWorld.x, cellWorld.z, play, cell);

        // Enqueue 8-neighbors only if (a) we found at least one floor here AND
        // (b) we can actually walk from here to the neighbor without hitting a
        // wall (MovementClear). The (b) gate is the room-scoping fix surfaced
        // by the 2026-05-06 field-test review:
        //
        // Without (b), the floodfill propagates wherever floor exists in the
        // scene — including across door thresholds and into adjacent rooms,
        // because OoT's static scene collision is shared across rooms. Result
        // (verified in Release_b0ea6f1 logs): three rooms in scene 0 produced
        // identical 6644-node / 23563-edge / 5435-cell scans, and Kokiri
        // Forest hit kMaxScanIterations every entry.
        //
        // With (b), the line-test stops at walls (inter-room walls AND
        // closed-door collisions), confining the floodfill to the connected
        // walkable region the navigator can actually reach from the seed.
        // Open doors at scan time still bleed into the adjacent room, which
        // matches the static-only-scan policy (plan §7).
        //
        // Cost: ~1 line-test per neighbor enqueue. ~5,435 cells × ~8 neighbors
        // ≈ 43,480 additional line tests per scan, ~40ms wall time. Within
        // the kMaxScanWallTimeMs = 1000ms budget.
        //
        // See Plans/room_nav_floodfill_scoping_investigation.md for full
        // analysis. Implements Solution A from that document.
        if (firstIdx >= 0) {
            // The "from" pos is the cell's world center at the floor height
            // we just discovered. Use the first node's pos for accuracy
            // (multi-cast may produce stacked floors; we use the topmost).
            const Vec3f& fromPos = out->nodes[firstIdx].pos;

            for (int dx = -1; dx <= 1; dx++) {
                for (int dz = -1; dz <= 1; dz++) {
                    if (dx == 0 && dz == 0) continue;
                    CellKey neighbor{ cell.x + dx, cell.z + dz };
                    if (visited.count(neighbor)) continue;

                    // Pelvis-height line test toward the neighbor's center.
                    // We don't yet know the neighbor's floor Y, so use this
                    // cell's Y as the test elevation. If the neighbor's floor
                    // is at a similar Y the test is meaningful; if drastically
                    // different the floodfill stops at the elevation gap
                    // (correctly — that's a cliff or wall).
                    Vec3f neighborProbe = CellCenterWorld(neighbor, out->bboxMin);
                    neighborProbe.y = fromPos.y;

                    if (!MovementClear(fromPos, neighborProbe, play)) continue;

                    pending.push_back(neighbor);
                }
            }
        }
    }

    auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStart).count();

    // Edge generation (commit 4) — connect nodes whose centers are within
    // one grid step in any direction (8-neighbor + same-XZ stacked-floor)
    // AND for which MovementClear succeeds. Cost weighted by distance plus
    // slope/hazard penalty.
    auto edgeStart = std::chrono::steady_clock::now();

    // Spatial index: bucket nodes by (cellX, cellZ) so adjacency lookup
    // is O(1) per node instead of O(N) over all nodes.
    std::unordered_map<CellKey, std::vector<uint16_t>, CellKeyHash> nodesByCell;
    for (uint16_t i = 0; i < out->nodes.size(); i++) {
        const NavNode& n = out->nodes[i];
        CellKey k{ (int32_t)(int16_t)n.cellIdxX, (int32_t)(int16_t)n.cellIdxZ };
        nodesByCell[k].push_back(i);
    }

    constexpr float kSlopePenalty = 2.0f;  // discourage routing through steep
    constexpr float kHazardPenalty = 5.0f; // strongly discourage hazard (commit 5 sets HAZARD flag)

    // Step-height edge gate. Reject candidate edges whose endpoint Y delta
    // exceeds Link's step-up height. Defends against a class of
    // "physically walkable but bad nav target" geometry that the
    // FloorActorRejectList allowlist doesn't cover: small ledges, scene-
    // static furniture lacking a dedicated actor, etc. Nodes still get
    // scanned so the data exists; they simply don't get edges connecting
    // them to the surrounding ground level, leaving them as unreachable
    // graph islands. Consumers (FindBestReachableSubgoalNode) treat
    // unreachable nodes as no-ops, which is the desired behaviour.
    //
    // 30u matches Link's vanilla step-up tolerance — anything taller
    // requires explicit climb input from the player and shouldn't be a
    // ground-walk edge. Tune downward if too permissive in field tests.
    constexpr float kMaxStepUpHeight = 30.0f;

    for (uint16_t i = 0; i < out->nodes.size(); i++) {
        const NavNode& a = out->nodes[i];
        CellKey aCell{ (int32_t)(int16_t)a.cellIdxX, (int32_t)(int16_t)a.cellIdxZ };

        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dz == 0) {
                    // Same-XZ stacked-floor edges (multi-level rooms): connect
                    // nodes at the same cell with different Y if MovementClear
                    // succeeds. Rare (the cells are vertically separated by
                    // floor/ceiling typically) but valid for some geometry.
                } else {
                    // skip will be handled by neighbor lookup below
                }
                CellKey nCell{ aCell.x + dx, aCell.z + dz };
                auto it = nodesByCell.find(nCell);
                if (it == nodesByCell.end()) continue;
                for (uint16_t j : it->second) {
                    if (j <= i) continue; // dedupe (only insert each undirected edge once)
                    const NavNode& b = out->nodes[j];
                    // Step-height gate: reject before MovementClear (the
                    // line test is O(scene polys); the Y comparison is
                    // O(1) — failing fast keeps the inner-loop cost down).
                    if (std::fabs(b.pos.y - a.pos.y) > kMaxStepUpHeight) continue;
                    if (!MovementClear(a.pos, b.pos, play)) continue;

                    float dxf = b.pos.x - a.pos.x;
                    float dyf = b.pos.y - a.pos.y;
                    float dzf = b.pos.z - a.pos.z;
                    float dist = std::sqrt(dxf*dxf + dyf*dyf + dzf*dzf);

                    float cost = dist;
                    if ((a.flags & NODE_STEEP_SLOPE) || (b.flags & NODE_STEEP_SLOPE)) {
                        cost *= kSlopePenalty;
                    }
                    if ((a.flags & NODE_HAZARD) || (b.flags & NODE_HAZARD)) {
                        cost *= kHazardPenalty;
                    }

                    NavEdge edge{};
                    edge.fromIdx = i;
                    edge.toIdx   = j;
                    edge.cost    = cost;
                    out->edges.push_back(edge);
                }
            }
        }
    }

    auto edgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - edgeStart).count();
    auto totalMs = scanMs + edgeMs;

    // ----- Reserved-flag population (workstream B). -------------------------
    //
    // Populate NODE_EDGE and NODE_HAZARD_ADJACENT for walkable nodes by
    // inspecting the 8 XZ neighbor cells in `nodesByCell`. Both flags are
    // set at scan time so consumers (debug draw, future cautious-pathing)
    // can read them without re-walking the spatial index.
    //
    // Predicate per walkable node A:
    //   - If any of the 8 neighbor cells contains zero nodes → A gets
    //     NODE_EDGE (A is on a ledge / room perimeter).
    //   - For each node B at a populated neighbor cell, if B has
    //     NODE_HAZARD → A gets NODE_HAZARD_ADJACENT.
    //
    // Cost is O(N × 8) cell lookups + O(N × 8 × avg-stack-depth) flag
    // checks. Well under 1ms for typical rooms. Reuses the same
    // nodesByCell built for edge generation; no extra spatial indexing.
    //
    // Runs BEFORE the orphan pass so orphan precedence later can mask
    // these flags visually without disturbing the underlying classification.
    for (uint16_t i = 0; i < out->nodes.size(); i++) {
        NavNode& a = out->nodes[i];
        if (!(a.flags & NODE_WALKABLE)) continue;

        CellKey aCell{ (int32_t)(int16_t)a.cellIdxX, (int32_t)(int16_t)a.cellIdxZ };

        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dz == 0) continue;
                CellKey nCell{ aCell.x + dx, aCell.z + dz };
                auto it = nodesByCell.find(nCell);
                if (it == nodesByCell.end()) {
                    // Empty neighbor cell → A is on a ledge / perimeter.
                    a.flags |= NODE_EDGE;
                    continue;
                }
                // Populated neighbor cell — scan its nodes for hazards.
                for (uint16_t j : it->second) {
                    if (out->nodes[j].flags & NODE_HAZARD) {
                        a.flags |= NODE_HAZARD_ADJACENT;
                        break; // one hazard neighbor is enough
                    }
                }
            }
        }
    }

    // ----- Orphan detection pass (workstream A, schema v2). ----------------
    //
    // The multi-cast column scan finds every floor at every visited XZ cell,
    // including stacked floors on top of walls / fences / scenery. The
    // floodfill never traverses to those upper floors (its MovementClear
    // gate stops at the wall), so the resulting nodes have no edges
    // connecting them to the main graph. They would mislead nav consumers
    // (FindBestReachableSubgoalNode) if treated as legitimate targets.
    //
    // Algorithm (handoff plan §5 commit 1, refined for component-size
    // thresholding):
    //   1. Pick the first node at each stashed seed cell as a "seed-rooted"
    //      starting point. Use the nodesByCell spatial index built for edge
    //      generation; it's still in scope here.
    //   2. BFS via the edges vector from every seed-rooted node. Edges are
    //      undirected (each {fromIdx, toIdx} traverses both ways). Mark
    //      visited nodes — these are reachable from a seed.
    //   3. For each unvisited node, BFS its connected component (within the
    //      unvisited set). If the component size exceeds
    //      kMinValidComponentSize, treat it as a legitimate sub-area that
    //      simply had no actor inside it to seed from (e.g. the slingshot
    //      chamber after the chest is taken on the active save). Don't
    //      flag those nodes — consumers can still reach them via vertical
    //      teleport / climb anchors / future cross-component pathing.
    //   4. For each unvisited component below the threshold, flag every
    //      node in the component as NODE_ORPHANED. These are the small
    //      stacked-floor remnants on top of walls / fences / scenery —
    //      typically 1-20 nodes each.
    //
    // Threshold rationale: legitimate sub-rooms in OoT scenes have hundreds
    // of nodes (Inside Deku Tree slingshot chamber ≈ 200-500 nodes typical).
    // Wall-top remnants are <30 nodes. 50 is the conservative midpoint.
    //
    // Cost: O(|nodes| + |edges|). The component-size pass adds another
    // |nodes| + |edges| traversal; total still negligible compared to edge
    // generation's line tests.
    static constexpr size_t kMinValidComponentSize = 50;
    auto orphanStart = std::chrono::steady_clock::now();
    size_t orphanCount = 0;
    size_t recoveredOrphanCount = 0;
    {
        // Build adjacency list from the (undirected) edges vector.
        std::vector<std::vector<uint16_t>> adjacency(out->nodes.size());
        for (const NavEdge& e : out->edges) {
            if (e.fromIdx >= out->nodes.size() || e.toIdx >= out->nodes.size()) continue;
            adjacency[e.fromIdx].push_back(e.toIdx);
            adjacency[e.toIdx].push_back(e.fromIdx);
        }

        std::vector<bool> visitedNode(out->nodes.size(), false);
        std::deque<uint16_t> bfs;

        // Enqueue the first node at each seed cell as a BFS root.
        for (const CellKey& sc : seedCells) {
            auto it = nodesByCell.find(sc);
            if (it == nodesByCell.end()) continue;
            if (it->second.empty()) continue;
            uint16_t rootIdx = it->second.front();
            if (rootIdx >= out->nodes.size()) continue;
            if (visitedNode[rootIdx]) continue;
            visitedNode[rootIdx] = true;
            bfs.push_back(rootIdx);
        }

        // Standard BFS over the bidirectional adjacency list.
        while (!bfs.empty()) {
            uint16_t cur = bfs.front();
            bfs.pop_front();
            for (uint16_t nb : adjacency[cur]) {
                if (nb >= visitedNode.size()) continue;
                if (visitedNode[nb]) continue;
                visitedNode[nb] = true;
                bfs.push_back(nb);
            }
        }

        // Component-size pass over the unvisited set. Each unvisited node
        // belongs to some unvisited connected component; classify by size.
        std::vector<bool> classifiedComponent(out->nodes.size(), false);
        for (size_t i = 0; i < out->nodes.size(); i++) {
            if (visitedNode[i]) continue;          // reachable from a seed
            if (classifiedComponent[i]) continue;  // already handled

            // BFS the unvisited component containing i.
            std::vector<uint16_t> componentNodes;
            std::deque<uint16_t> q;
            q.push_back((uint16_t)i);
            classifiedComponent[i] = true;
            while (!q.empty()) {
                uint16_t cur = q.front();
                q.pop_front();
                componentNodes.push_back(cur);
                for (uint16_t nb : adjacency[cur]) {
                    if (nb >= classifiedComponent.size()) continue;
                    if (visitedNode[nb]) continue;          // shouldn't happen; defensive
                    if (classifiedComponent[nb]) continue;
                    classifiedComponent[nb] = true;
                    q.push_back(nb);
                }
            }

            if (componentNodes.size() > kMinValidComponentSize) {
                // Large unreachable component — legitimate sub-area that
                // happened to have no actor inside it to seed from. Leave
                // the orphan flag CLEAR; treat as walkable.
                recoveredOrphanCount += componentNodes.size();
            } else {
                // Small unreachable component — wall-top / fence-top
                // remnant. Flag every node so consumers and viz can
                // distinguish.
                for (uint16_t idx : componentNodes) {
                    out->nodes[idx].flags |= NODE_ORPHANED;
                    orphanCount++;
                }
            }
        }
    }
    auto orphanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - orphanStart).count();
    (void)totalMs;            // shadowed by totalMsFinal below; keep symbol for callers
    (void)orphanMs;           // exposed via totalMsFinal aggregate; not separately logged

    // Climb anchor detection. Two paths run sequentially when their CVars
    // are on:
    //   - Path A (always-on): scene-actor allowlist iteration. Catches
    //     discrete climbable actors like ladders and Inside Deku Tree's
    //     falling ladder.
    //   - Path B (gated by RoomNavData.PathBClimbDetection): surface-flag
    //     scan over static scene polys. Catches vine walls and any other
    //     climbable surface baked into static collision (wall-flag bit 3
    //     set). Anchors merge by XZ proximity so a multi-poly vine wall
    //     produces one anchor with the full mesh's Y range, not dozens.
    DetectClimbAnchors(out, play);
    DetectClimbAnchorsViaSurfaceFlags(out, play, visited);

    // Ledge-grab anchor detection (Phase 1) — distinct motion semantics
    // from climb anchors (jump+grab+pull-up vs climb-up animation). Uses
    // the same nodesByCell spatial index built for edge generation.
    // Requires the orphan pass to have run first so NODE_ORPHANED is
    // populated, but we don't currently filter by that flag — orphan
    // tops can still be valid ledge-grab destinations (small platforms
    // disconnected from the floodfill but reachable via jump+grab).
    DetectLedgeAnchors(out, play, nodesByCell);

    // Crawlspace detection — narrow-passage tunnels gated by wall-flag
    // bits 4 + 5 (per z_player.c:7639's `interactWallFlags & 0x30`
    // check). Tags nearby walkable nodes with NODE_CRAWLSPACE so
    // consumers (Phase 2) can detect when their navigation path enters
    // a crawlspace volume and inject the crawl input.
    DetectCrawlspaces(out, play, visited);

    // Drop anchor detection — symmetric to ledge-grab but for descent.
    // Pairs walkable nodes where falling from high to low is safe and
    // unobstructed. Future autonomous-nav consumers use these as
    // descent edges in pathfinding.
    DetectDropAnchors(out, play, nodesByCell);

    // Climb-flag population (polish wave commit 4) — for each climb anchor
    // discovered above, find the nearest walkable nav-node to its basePos
    // and topPos and tag them NODE_CLIMB_BASE / NODE_CLIMB_TOP. Lets nav
    // consumers locate climb entry/exit points by graph node lookup
    // instead of re-querying the climbAnchors vector.
    //
    // FindNearestNode is a linear scan over nav.nodes (~1,100 nodes per
    // typical room). Climb anchors are rare per room (1-3 typical), so the
    // total cost is bounded — well under 10ms even on the largest scenes.
    //
    // -1 returns from FindNearestNode (empty graph) are silently skipped.
    // Same NavNode CAN receive both NODE_CLIMB_BASE and NODE_CLIMB_TOP if
    // base and top happen to map to the same node (degenerate climb with
    // estimatedHeight smaller than kGridResolution); harmless — viz will
    // still draw a single yellow ring at that position.
    for (const ClimbAnchor& anchor : out->climbAnchors) {
        int baseIdx = FindNearestNode(out, anchor.basePos);
        if (baseIdx >= 0) {
            out->nodes[(size_t)baseIdx].flags |= NODE_CLIMB_BASE;
        }
        int topIdx = FindNearestNode(out, anchor.topPos);
        if (topIdx >= 0) {
            out->nodes[(size_t)topIdx].flags |= NODE_CLIMB_TOP;
        }
    }

    // Climb-surface grid generation (schema v7+). Runs AFTER the
    // climb-base/top tagging above so FindNearestNode searches over
    // floor-only nodes (climb-surface nodes use U/V cell indices,
    // not world cells, and would corrupt the spatial-distance heuristic
    // if they were already in nav.nodes when FindNearestNode runs).
    // Stage 2 of climb_surface_nav_grid_plan — nodes only; edges land
    // in Stage 3.
    GenerateClimbSurfaceGrids(out, play);

    // Update historicalSeeds with the union of (current player + actor
    // + previously-historical seed positions). Cell-deduped at insertion
    // so the persisted list stays bounded at one entry per grid cell.
    // Capped at kMaxHistoricalSeeds to prevent unbounded growth even
    // for multi-session, heavily-explored rooms.
    static constexpr size_t kMaxHistoricalSeeds = 1000;
    std::unordered_set<CellKey, CellKeyHash> existingHistoricalCells;
    for (const Vec3f& p : out->historicalSeeds) {
        existingHistoricalCells.insert(CellKeyForXZ(p.x, p.z, out->bboxMin));
    }
    // Accumulate this scan's seed positions (player + actors). Each
    // converted to a cell; only cells not already in the historical
    // list are appended. Vec3f stored is the world position; cell key
    // is reconstituted on next scan from the current bbox.
    auto addHistoricalCandidate = [&](const Vec3f& pos) {
        if (out->historicalSeeds.size() >= kMaxHistoricalSeeds) return;
        CellKey ck = CellKeyForXZ(pos.x, pos.z, out->bboxMin);
        if (existingHistoricalCells.count(ck)) return;
        existingHistoricalCells.insert(ck);
        out->historicalSeeds.push_back(pos);
    };
    addHistoricalCandidate(playerPos);
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        Actor* a = play->actorCtx.actorLists[cat].head;
        while (a != nullptr) {
            if (a->room == roomNum || a->room == -1) {
                addHistoricalCandidate(a->world.pos);
            }
            a = a->next;
        }
    }

    auto totalMsFinal = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStart).count();
    SPDLOG_INFO("[RoomNav] ScanRoom: scene={} room={} playerPos=({:.0f},{:.0f},{:.0f}) "
                "nodes={} edges={} climbs={} ledges={} crawls={} drops={} cells={} seeds={} "
                "histSeeds={} orphans={} recovered={} scanMs={} edgeMs={} totalMs={}",
                sceneNum, (int)roomNum,
                playerPos.x, playerPos.y, playerPos.z,
                out->nodes.size(), out->edges.size(),
                out->climbAnchors.size(), out->ledgeAnchors.size(),
                out->crawlspaceAnchors.size(), out->dropAnchors.size(),
                visited.size(), 1 + extraSeeds + historicalSeedsUsed,
                out->historicalSeeds.size(), orphanCount,
                recoveredOrphanCount, scanMs, edgeMs, totalMsFinal);

    // Per-anchor diagnostic log — emit one line per discovered climb
    // anchor with basePos, topPos, and source (Path A actorId or 0 for
    // Path B static-geometry). Lets the user grep for anchors discovered
    // near a known surface position to validate detection coverage.
    for (size_t i = 0; i < out->climbAnchors.size(); i++) {
        const ClimbAnchor& a = out->climbAnchors[i];
        SPDLOG_INFO("[RoomNav]   ClimbAnchor[{}] kind={} actorId=0x{:04X} "
                    "basePos=({:.0f},{:.0f},{:.0f}) topPos=({:.0f},{:.0f},{:.0f})",
                    i, a.actorId == 0 ? "PathB-static" : "PathA-actor",
                    (int)a.actorId,
                    a.basePos.x, a.basePos.y, a.basePos.z,
                    a.topPos.x,  a.topPos.y,  a.topPos.z);
    }
    for (size_t i = 0; i < out->ledgeAnchors.size(); i++) {
        const LedgeAnchor& l = out->ledgeAnchors[i];
        SPDLOG_INFO("[RoomNav]   LedgeAnchor[{}] approachPos=({:.0f},{:.0f},{:.0f}) "
                    "topPos=({:.0f},{:.0f},{:.0f}) deltaY={:.0f}",
                    i,
                    l.approachPos.x, l.approachPos.y, l.approachPos.z,
                    l.topPos.x,      l.topPos.y,      l.topPos.z,
                    l.topPos.y - l.approachPos.y);
    }
    for (size_t i = 0; i < out->crawlspaceAnchors.size(); i++) {
        const CrawlspaceAnchor& c = out->crawlspaceAnchors[i];
        SPDLOG_INFO("[RoomNav]   CrawlspaceAnchor[{}] entryPos=({:.0f},{:.0f},{:.0f}) "
                    "entryNormal=({:.2f},{:.2f},{:.2f})",
                    i,
                    c.entryPos.x,    c.entryPos.y,    c.entryPos.z,
                    c.entryNormal.x, c.entryNormal.y, c.entryNormal.z);
    }
    for (size_t i = 0; i < out->dropAnchors.size(); i++) {
        const DropAnchor& d = out->dropAnchors[i];
        SPDLOG_INFO("[RoomNav]   DropAnchor[{}] highPos=({:.0f},{:.0f},{:.0f}) "
                    "landingPos=({:.0f},{:.0f},{:.0f}) deltaY={:.0f}",
                    i,
                    d.highPos.x,    d.highPos.y,    d.highPos.z,
                    d.landingPos.x, d.landingPos.y, d.landingPos.z,
                    d.highPos.y - d.landingPos.y);
    }
}

// Auto-expand-on-exploration state. When the player walks into a cell
// that wasn't visited by the last scan, we trigger a full rescan to
// extend coverage. The seed-history persistence (RoomNavData::
// historicalSeeds) ensures the rescan never LOSES coverage — it only
// adds to it.
//
// Cooldown prevents rapid re-triggering when the player paces back
// and forth across the visited/unvisited boundary. 600 frames = 10s.
static int32_t sExpansionCooldown = 0;
static constexpr int32_t kExpansionCooldownFrames = 600;

// Per-room visited-cell cache. Built post-scan from the floodfill
// `visited` set, or from disk-loaded node cellIdx fields. Used by
// the auto-expand check to determine whether the player's current
// cell has been scanned. Transient — never written to disk.
//
// Defined here (above RebuildVisitedCellsCache) so the helper can
// reference it without forward-decl gymnastics.
static std::unordered_map<uint32_t, std::unordered_set<CellKey, CellKeyHash>>
    sVisitedCellsCache;

// Historical-seed preservation across DoTriggerFullRescan. The full-
// rescan path drops the in-memory cache AND deletes the on-disk .bin
// before letting the polling re-trigger the scan. Without this
// parallel cache, historicalSeeds would be lost across every rescan
// because OnRoomEntered constructs a fresh empty RoomNavData and
// TryLoadFromDisk fails (file just deleted).
//
// On rescan trigger, copy historicalSeeds to this map BEFORE erasing
// the cache. On the subsequent ScanRoom call, restore them onto the
// fresh RoomNavData so seed accumulation continues across the rescan
// boundary.
static std::unordered_map<uint32_t, std::vector<Vec3f>>
    sPreservedHistoricalSeeds;

// Top-level lookup-then-scan dispatch. Called once per (scene, room)
// transition by OnGameFrameTick.
// Build the visitedCells set for a room from its nodes. Each node's
// cellIdxX/Z fields encode the cell it occupies; insert each into the
// per-room set. Used after both fresh scan and disk-load paths.
static void RebuildVisitedCellsCache(uint32_t cacheKey, const RoomNavData& nav) {
    auto& cells = sVisitedCellsCache[cacheKey];
    cells.clear();
    cells.reserve(nav.nodes.size());
    for (const NavNode& node : nav.nodes) {
        cells.insert(CellKey{
            (int32_t)(int16_t)node.cellIdxX,
            (int32_t)(int16_t)node.cellIdxZ,
        });
    }
}

static void OnRoomEntered(int16_t sceneNum, int8_t roomNum, PlayState* play) {
    uint32_t key = MakeCacheKey(sceneNum, roomNum);

    // Step 1: in-memory cache. Already-loaded rooms early-return.
    if (sCache.count(key)) {
        return;
    }

    // Step 2: disk cache.
    RoomNavData fresh{};
    TryLoadFromDisk(sceneNum, roomNum, &fresh);
    if (fresh.sceneNum == sceneNum && fresh.roomNum == roomNum && !fresh.nodes.empty()) {
        // Build visited-cells lookup from disk-loaded nodes for the
        // auto-expand-on-exploration check.
        RebuildVisitedCellsCache(key, fresh);
        sCache.emplace(key, std::move(fresh));
        SPDLOG_INFO("[RoomNav] Loaded cached scene={} room={} from disk", sceneNum, (int)roomNum);
        return;
    }

    // Step 3: scan + persist.
    if (!IsAutoScanEnabled()) {
        // AutoScan off: never scan, even if no cached data exists. Used for
        // "play with this exact baked set, don't generate more" mode.
        return;
    }

    RoomNavData scanned{};
    // Restore historicalSeeds preserved by a prior DoTriggerFullRescan
    // call so seed accumulation continues across the rescan boundary.
    // If no preservation entry exists, scanned starts with an empty
    // historicalSeeds vector (fresh-room baseline).
    auto preservedIt = sPreservedHistoricalSeeds.find(key);
    if (preservedIt != sPreservedHistoricalSeeds.end()) {
        scanned.historicalSeeds = std::move(preservedIt->second);
        sPreservedHistoricalSeeds.erase(preservedIt);
    }
    ScanRoom(sceneNum, roomNum, play, &scanned);
    if (!scanned.nodes.empty()) {
        SaveToDisk(scanned);
        RebuildVisitedCellsCache(key, scanned);
    }
    sCache.emplace(key, std::move(scanned));
}

// ---------------------------------------------------------------------------
// Slope-3 stuck-on-slope diagnostic (Phase 2 commit 11). Detection-only;
// active intervention deferred to v2 (gEnhancements.RoomNavData
// .ActiveSlopeRecovery) pending evidence the predicate fires in real
// gameplay.
//
// Per-frame predicate: actor's floor poly slope is 3 (very-steep) AND the
// actor isn't sliding down (velocity.y not consistently negative) for more
// than kStuckFrameThreshold frames. On rising edge above the threshold,
// increment the per-actor stuckOnSlopeEventCount and emit a rate-limited
// SPDLOG_WARN.
// ---------------------------------------------------------------------------

static constexpr uint16_t kStuckFrameThreshold     = 30;   // ~0.5s at 60fps
static constexpr uint64_t kStuckLogCooldownFrames  = 300;  // ~5s rate-limit per-actor

// Per-actor last-log frame, keyed by actor pointer (cleared on actor
// destruction implicitly — stale pointers in this map just lose their
// cooldown, no functional issue).
static std::unordered_map<Actor*, uint64_t> sLastStuckLogFrame;
static uint64_t sFrameCounter = 0;

static bool IsStuckOnSlope(Actor* actor, EnemyNetId* ext, PlayState* play) {
    if (actor == nullptr || ext == nullptr) return false;
    if (actor->update == nullptr) return false;          // dead actor
    if (actor->floorPoly == nullptr) return false;       // not standing on anything

    s32 slope = SurfaceType_GetSlope(&play->colCtx, actor->floorPoly, actor->floorBgId);
    if (slope != 3) return false;                         // not on a steep slope

    // Vanilla physics slides actors down slope-3 surfaces. If velocity.y
    // is meaningfully negative the actor is sliding correctly — predicate
    // false. Threshold is permissive (>= -0.5) to allow brief settling
    // moments without false-positive log spam.
    if (actor->velocity.y < -0.5f) return false;

    return true;
}

static void TickStuckOnSlopeDetection(PlayState* play) {
    bool diagnosticOn = CVarGetInteger(CVAR_ROOM_NAV_LOG_STUCK_ON_SLOPE, 0) != 0;
    if (!diagnosticOn) return;

    sFrameCounter++;

    for (uint8_t cat : kSyncableActorCategories /* ActorSyncHelpers.h:22 */) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            Actor* next = actor->next; // capture before any mutation

            if (IsSyncableActor(actor)) {
                EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
                if (ext != nullptr) {
                    if (IsStuckOnSlope(actor, ext, play)) {
                        // Rising edge into the stuck condition counts up.
                        if (ext->stuckOnSlopeFrames < UINT16_MAX) {
                            ext->stuckOnSlopeFrames++;
                        }
                        // Threshold cross: log + accumulate event count.
                        if (ext->stuckOnSlopeFrames == kStuckFrameThreshold) {
                            if (ext->stuckOnSlopeEventCount < UINT16_MAX) {
                                ext->stuckOnSlopeEventCount++;
                            }
                            uint64_t lastLogFrame = sLastStuckLogFrame.count(actor)
                                ? sLastStuckLogFrame[actor]
                                : 0;
                            if (sFrameCounter - lastLogFrame >= kStuckLogCooldownFrames) {
                                sLastStuckLogFrame[actor] = sFrameCounter;
                                SPDLOG_WARN("[RoomNav] actor 0x{:04X} stuck on slope-3 at "
                                            "({:.0f},{:.0f},{:.0f}) for {} frames "
                                            "(event #{} this session)",
                                            (int)actor->id,
                                            actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                                            ext->stuckOnSlopeFrames,
                                            ext->stuckOnSlopeEventCount);
                            }
                        }
                    } else {
                        // Predicate false — reset frame counter. Do NOT reset
                        // stuckOnSlopeEventCount; it accumulates across the
                        // session for visibility.
                        ext->stuckOnSlopeFrames = 0;
                    }
                }
            }

            actor = next;
        }
    }
}

// ---------------------------------------------------------------------------
// OnGameFrameUpdate hook — polling trigger. Detects (sceneNum, roomNum)
// delta and dispatches OnRoomEntered. Scan-skip conditions verified
// against soh/include/z64.h and soh/include/z64save.h.
// ---------------------------------------------------------------------------

static int16_t sLastScene = -1;
static int8_t  sLastRoom  = -1;

// Initial-scan delay state. When OnGameFrameTick detects a room change,
// it doesn't trigger ScanRoom immediately — it queues the scan for
// kInitialScanDelayFrames frames later. Gives actors and dynamic
// collision time to fully initialize before we capture the geometry.
//
// Field-test feedback (2026-05-08): scans on first room entry sometimes
// missed significant geometry that a force-rescan a few seconds later
// captured. Root cause: transitionTrigger == TRANS_TRIGGER_OFF only
// indicates the fade animation is done. Bg actors continue registering
// dynamic collision (DynaPoly_SetBgActor) for many frames after that;
// some script-driven scenery doesn't register until later still.
//
// 30 frames (~0.5s) covers most actor-init settling without an obvious
// user-visible delay. Tunable via the
// RoomNavData.InitialScanDelayFrames CVar.
static int32_t sFramesUntilInitialScan = 0;
static int16_t sPendingInitialScene    = -1;
static int8_t  sPendingInitialRoom     = -1;
static constexpr int32_t kInitialScanDelayFramesDefault = 30;

// Tier 1 / Tier 2 dynamic refresh — pending flags. Set from the
// OnSceneFlagSet hook (which can fire multiple times per frame);
// drained once per frame from DispatchPendingDynamicRefresh inside
// OnGameFrameTick. Both stay false when neither tier's CVar is on.
// Defined here (before OnExitGameClear) so the lifecycle-clear paths
// can reset them.
static bool sPendingAnchorRefresh = false;
static bool sPendingFullRescan    = false;

// Position-stability dispatch. When a scene flag fires, we wait until
// all BG + PROP actors in the room have held their positions stable
// for a settle window before dispatching the rescan. This adapts
// naturally to any animation duration:
//   - Falling ladder settles in ~1s → dispatch ~1.5s after flag fire
//   - Push block reaches destination in ~2s → dispatch ~2.5s
//   - Floor-switch raises 3 platforms over 4s → dispatch ~4.5s
//
// Hash all BG + PROP actor positions each frame. When the hash matches
// the previous frame for kStabilityRequiredFrames consecutive frames,
// dispatch. A kMaxWaitFrames safety net dispatches anyway if actors
// never settle (e.g. continuously moving platforms with no flag at
// each cycle endpoint), so we don't hang forever.
//
// Hash uses position quantized to 1u to avoid floating-point jitter
// flapping the stability counter on actors that "hover" with tiny
// per-frame motion.
static uint64_t sLastBgPropPositionHash = 0;
static int32_t  sStabilityCounter       = 0;
static int32_t  sMaxWaitCountdown       = 0;
static constexpr int32_t kStabilityRequiredFrames = 30;  // ~0.5s settle window
static constexpr int32_t kMaxWaitFrames           = 600; // 10s safety net

// Forward declaration — definition lives further down the file with the
// other dynamic-refresh helpers, but this function is called from
// OnGameFrameTick (right below).
static void DispatchPendingDynamicRefresh();

static void OnGameFrameTick() {
    if (!IsEnabled()) {
        return;
    }
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }

    // Scan-skip conditions — see plan §2 trigger semantics.
    if (play->csCtx.state != CS_STATE_IDLE)            return; // cutscene active
    if (play->transitionTrigger != TRANS_TRIGGER_OFF)  return; // mid-scene-transition
    if (gSaveContext.gameMode != GAMEMODE_NORMAL)      return; // file-select / title-screen
    if (gSaveContext.fileNum < 0)                       return; // no save loaded

    // Slope-3 stuck-on-slope diagnostic (commit 11) runs every frame regardless
    // of whether the room has changed. Internally CVar-gated; no-op when off.
    TickStuckOnSlopeDetection(play);

    // Tier 1 / Tier 2 dynamic refresh — drain any pending refresh/rescan
    // requests queued by the OnSceneFlagSet hook. Runs every frame so
    // multi-flag-per-frame events absorb into a single dispatch. Internally
    // CVar-gated; both pending bools stay false when neither tier is on.
    DispatchPendingDynamicRefresh();

    // Auto-expand on exploration. When the player walks into a cell
    // that wasn't visited by the current cached scan, queue a full
    // rescan to extend coverage. Combined with the persisted
    // historicalSeeds vector (which preserves prior seed positions),
    // each rescan strictly expands coverage — never regresses.
    //
    // Cooldown prevents rapid re-triggering when the player paces
    // back and forth across the visited/unvisited boundary. Position-
    // stability dispatch (existing Tier 2 mechanism) absorbs multiple
    // triggers within a settle window.
    if (sExpansionCooldown > 0) {
        sExpansionCooldown--;
    }
    if (sExpansionCooldown == 0 &&
        CVarGetInteger(CVAR_ROOM_NAV_AUTO_EXPAND, 1) != 0) {
        int16_t curScene = play->sceneNum;
        int8_t  curRoom  = (int8_t)play->roomCtx.curRoom.num;
        uint32_t curKey  = MakeCacheKey(curScene, curRoom);
        auto cacheIt    = sCache.find(curKey);
        auto visitedIt  = sVisitedCellsCache.find(curKey);
        if (cacheIt    != sCache.end()           &&
            visitedIt  != sVisitedCellsCache.end() &&
            !cacheIt->second.nodes.empty()) {
            Player* p = GET_PLAYER(play);
            if (p != nullptr) {
                CellKey playerCell = CellKeyForXZ(
                    p->actor.world.pos.x, p->actor.world.pos.z,
                    cacheIt->second.bboxMin);
                if (visitedIt->second.count(playerCell) == 0) {
                    // Player is in an unvisited cell. Queue a full
                    // rescan with the existing position-stability
                    // dispatch path. The historicalSeeds vector
                    // ensures the rescan keeps prior coverage.
                    sPendingFullRescan      = true;
                    sStabilityCounter       = 0;
                    sMaxWaitCountdown       = kMaxWaitFrames;
                    sExpansionCooldown      = kExpansionCooldownFrames;
                    SPDLOG_INFO("[RoomNav] Auto-expand triggered: scene={} room={} "
                                "playerPos=({:.0f},{:.0f},{:.0f}) "
                                "playerCell=({},{}) — rescan queued",
                                curScene, (int)curRoom,
                                p->actor.world.pos.x,
                                p->actor.world.pos.y,
                                p->actor.world.pos.z,
                                playerCell.x, playerCell.z);
                }
            }
        }
    }

    int16_t currentScene = play->sceneNum;
    int8_t  currentRoom  = (int8_t)play->roomCtx.curRoom.num;

    // Drain a queued delayed scan — the actual ScanRoom call happens
    // here, kInitialScanDelayFrames after the room change was first
    // detected. If the scene/room changed AGAIN during the delay
    // (player walked through two rooms quickly), the queue gets
    // re-armed below with the new target — supersedes the prior
    // pending scan.
    if (sFramesUntilInitialScan > 0) {
        sFramesUntilInitialScan--;
        if (sFramesUntilInitialScan == 0 &&
            currentScene == sPendingInitialScene &&
            currentRoom  == sPendingInitialRoom) {
            // Conditions still match the queued target — fire scan.
            OnRoomEntered(currentScene, currentRoom, play);
        }
    }

    if (currentScene == sLastScene && currentRoom == sLastRoom) {
        return; // unchanged; nothing else to do this frame
    }
    sLastScene = currentScene;
    sLastRoom  = currentRoom;

    // Queue a delayed scan for kInitialScanDelayFrames frames out.
    // Don't call OnRoomEntered immediately — actors / dyna collision
    // may not be fully registered yet on the first post-transition
    // frame. The delayed dispatch above runs ScanRoom once enough
    // settling has occurred.
    int32_t delay = CVarGetInteger(CVAR_ROOM_NAV_INITIAL_SCAN_DELAY,
                                    kInitialScanDelayFramesDefault);
    if (delay < 0)   delay = 0;
    if (delay > 600) delay = 600;
    if (delay == 0) {
        // Zero delay restores immediate-scan behavior for diagnostic
        // purposes (compare against delayed behavior side by side).
        OnRoomEntered(currentScene, currentRoom, play);
    } else {
        sFramesUntilInitialScan = delay;
        sPendingInitialScene    = currentScene;
        sPendingInitialRoom     = currentRoom;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle. Plan §11 — cache is never evicted within a session; once a
// room is scanned/loaded it stays resident. Cleared on game exit so the
// next session starts clean. Scene transitions DO NOT clear the cache —
// the polling delta-detection in OnGameFrameTick handles re-entry to
// already-cached rooms by short-circuiting before scan dispatch.
// ---------------------------------------------------------------------------

static void OnExitGameClear(int32_t /*fileNum*/) {
    if (!sCache.empty()) {
        SPDLOG_INFO("[RoomNav] OnExitGame: clearing {} cached rooms", sCache.size());
        sCache.clear();
    }
    // Drop the component cache (DebugDraw v3d, polish wave commit 5).
    // Transient debug data; recomputed on next debug-draw of each room.
    sComponentCache.clear();
    // Drop any pending dynamic-refresh requests — those reference the
    // current room which is about to become invalid.
    sPendingAnchorRefresh   = false;
    sPendingFullRescan      = false;
    sStabilityCounter       = 0;
    sMaxWaitCountdown       = 0;
    sLastBgPropPositionHash = 0;
    sFramesUntilInitialScan = 0;
    sPendingInitialScene    = -1;
    sPendingInitialRoom     = -1;
    sExpansionCooldown      = 0;
    sVisitedCellsCache.clear();
    sPreservedHistoricalSeeds.clear();
    sLastScene = -1;
    sLastRoom  = -1;
    sLastStuckLogFrame.clear();
    sFrameCounter = 0;
}

// ---------------------------------------------------------------------------
// Debug overlay (Phase 2 commit 12 + v2 polish).
//
// v1 (commit 81b28da07): toggle-edge log + per-room summary, no in-world
// rendering.
//
// v2 commit 1: in-world ground-aligned quads for walkable (green) and
// hazard (red) nodes. Renders one ~10u quad per node, slightly lifted
// above the floor (ZMODE_DEC handles z-fight prevention).
//
// v2 commit 2: edges as thin ground-aligned line quads (white) connecting
// node centres. Same kNodeQuadYLift as nodes so the edge sits on the same
// render plane; endpoint indices bounds-checked against node count so a
// stale-schema disk cache can't escape into the gfx pipeline.
//
// v2 commit 3 (this commit): polish — underwater nodes (blue), steep-slope
// nodes (orange), climb anchors (yellow ground quads at base+top with a
// vertical post connecting them), hazard centroids (deep red, larger
// markers; loop currently no-op since v1 scan doesn't populate the
// centroid list yet).
//
// Approach: ground-aligned quads instead of icospheres — 10× cheaper
// geometry (4 verts vs 12 vert × 20 tri), no per-instance billboard math
// required, and reads more naturally as a top-down "walkable area" map
// overlay than floating spheres would.
//
// Reference pattern from colViewer.cpp (sibling debug-draw module):
//   - sXluDl / sVtxDl static buffers, reset each frame
//   - InitDebugGfx → gsDPSetCycleType + gsDPSetRenderMode + gsDPSetCombineMode
//   - gsSPMatrix(&gMtxClear, ...) to render in world space
//   - gsDPSetPrimColor between color-grouped batches
//   - gsSPVertex + gsSP2Triangles per quad
//   - splice into POLY_XLU_DISP at end
// ---------------------------------------------------------------------------

static bool sDebugDrawWasEnabled = false;
static int16_t sDebugDrawLastSummaryScene = -1;
static int8_t  sDebugDrawLastSummaryRoom  = -1;

// Per-frame display-list buffers. Reset every frame; capacity grows as
// needed. References from sXluDl into sVtxDl require sVtxDl to outlive
// the frame — both vectors are static and reused.
static std::vector<Gfx> sXluDl;
static std::vector<Vtx> sVtxDl;

// Quad parameters.
static constexpr float kNodeQuadHalfExtent     = 5.0f;  // 10u square per node
static constexpr float kNodeQuadYLift          = 1.0f;  // small lift above floor
static constexpr float kEdgeLineHalfWidth      = 1.0f;  // 2u thick line on floor
static constexpr float kClimbPostHalfWidth     = 3.0f;  // 6u-wide vertical post at climb anchors
static constexpr float kHazardCentroidHalfExt  = 8.0f;  // 16u square per hazard-centroid marker
static constexpr float kClimbFlagHalfExt       = 7.0f;  // 14u square at NODE_CLIMB_BASE/TOP flagged nodes
static constexpr float kRejectedFloorCrossArmLen    = 8.0f; // half-length of each '+' arm (16u tip-to-tip)
static constexpr float kRejectedFloorCrossHalfWidth = 1.0f; // perpendicular thickness of each '+' arm

// Reset buffer + reserve based on prior frame (matches colViewer pattern).
template <typename T> static size_t ResetGfxBuffer(T& vec) {
    size_t oldSize = vec.size();
    vec.clear();
    vec.reserve((size_t)(oldSize * 1.2));
    return vec.capacity();
}

// Construct a Vtx with the normal-bearing layout (Vtx_tn). colViewer.cpp
// uses libgfxd's gdSPDefVtxN macro for the same purpose, but that macro
// lives in ZAPDTR/lib/libgfxd/gbi.h which isn't on the include path
// reached from this TU. Local helper avoids the dependency.
//
// Args mirror gdSPDefVtxN(x, y, z, /*s,t omitted*/, nx, ny, nz, a) — the
// s/t texture coords are always 0 in our overlay (no textured quads), so
// we drop them from the signature.
static Vtx MakeVtxN(short x, short y, short z,
                    signed char nx, signed char ny, signed char nz,
                    unsigned char a) {
    Vtx v{};
    v.n.ob[0] = x;  v.n.ob[1] = y;  v.n.ob[2] = z;
    v.n.flag  = 0;
    v.n.tc[0] = 0;  v.n.tc[1] = 0;
    v.n.n[0]  = nx; v.n.n[1]  = ny; v.n.n[2]  = nz;
    v.n.a     = a;
    return v;
}

// Mirrors colViewer::InitGfx for transparent overlay rendering. Sets up
// the RDP for primitive-color blending, with ZMODE_DEC so quads layer
// cleanly on top of floor geometry without z-fighting.
static void InitDebugGfx(std::vector<Gfx>& gfx) {
    uint32_t rm   = Z_CMP | IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_DEC;
    uint32_t blc1 = GBL_c1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    uint32_t blc2 = GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    uint8_t  alpha = 0xC0; // fairly visible but not opaque

    gfx.push_back(gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF));
    gfx.push_back(gsDPSetCycleType(G_CYC_1CYCLE));
    gfx.push_back(gsDPSetRenderMode(rm | blc1, rm | blc2));
    gfx.push_back(gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE));
    gfx.push_back(gsSPLoadGeometryMode(G_ZBUFFER));
    gfx.push_back(gsDPSetEnvColor(0xFF, 0xFF, 0xFF, alpha));
    gfx.push_back(gsSPMatrix(&gMtxClear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));
}

// Append a ground-aligned quad centred at `pos` to the buffers. Quad is
// flat in the XZ plane (Y constant), oriented as if "lying on the floor."
static void AddGroundQuad(std::vector<Gfx>& dl, std::vector<Vtx>& vtxDl, const Vec3f& pos) {
    float h = kNodeQuadHalfExtent;
    float y = pos.y + kNodeQuadYLift;

    // Four corners, CCW from top-down (winding for upward-facing normal).
    Vtx v0 = MakeVtxN((short)(pos.x - h), (short)y, (short)(pos.z - h), 0, 127, 0, 0xFF);
    Vtx v1 = MakeVtxN((short)(pos.x + h), (short)y, (short)(pos.z - h), 0, 127, 0, 0xFF);
    Vtx v2 = MakeVtxN((short)(pos.x + h), (short)y, (short)(pos.z + h), 0, 127, 0, 0xFF);
    Vtx v3 = MakeVtxN((short)(pos.x - h), (short)y, (short)(pos.z + h), 0, 127, 0, 0xFF);

    vtxDl.push_back(v0);
    vtxDl.push_back(v1);
    vtxDl.push_back(v2);
    vtxDl.push_back(v3);

    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// Append a thin ground-aligned line quad from posA to posB, both lifted
// slightly above the floor. The "line" is actually a thin rectangle in
// the XZ plane; long axis = A→B, perpendicular extent = kEdgeLineHalfWidth.
// Each end uses its own Y so edges across slight height differences (e.g.
// at room thresholds) render at the correct elevation per end.
static void AddGroundLineQuad(std::vector<Gfx>& dl, std::vector<Vtx>& vtxDl,
                              const Vec3f& posA, const Vec3f& posB) {
    float dx = posB.x - posA.x;
    float dz = posB.z - posA.z;
    float lenSq = dx * dx + dz * dz;
    if (lenSq < 0.01f) return;  // degenerate; skip
    float invLen = 1.0f / std::sqrt(lenSq);

    // Perpendicular vector in XZ plane, scaled to half-width.
    float px = -dz * invLen * kEdgeLineHalfWidth;
    float pz =  dx * invLen * kEdgeLineHalfWidth;

    float yA = posA.y + kNodeQuadYLift;
    float yB = posB.y + kNodeQuadYLift;

    // Corners CCW from top-down for upward-facing normal.
    Vtx v0 = MakeVtxN((short)(posA.x + px), (short)yA, (short)(posA.z + pz), 0, 127, 0, 0xFF);
    Vtx v1 = MakeVtxN((short)(posA.x - px), (short)yA, (short)(posA.z - pz), 0, 127, 0, 0xFF);
    Vtx v2 = MakeVtxN((short)(posB.x - px), (short)yB, (short)(posB.z - pz), 0, 127, 0, 0xFF);
    Vtx v3 = MakeVtxN((short)(posB.x + px), (short)yB, (short)(posB.z + pz), 0, 127, 0, 0xFF);

    vtxDl.push_back(v0);
    vtxDl.push_back(v1);
    vtxDl.push_back(v2);
    vtxDl.push_back(v3);

    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// Append a vertical "post" connecting basePos to topPos at the same XZ.
// Drawn as two perpendicular vertical quads (X-aligned and Z-aligned) so
// the marker is visible from any camera angle without true billboarding.
// Cheaper than billboard math and reads as a clear vertical climb indicator.
static void AddVerticalPost(std::vector<Gfx>& dl, std::vector<Vtx>& vtxDl,
                            const Vec3f& basePos, const Vec3f& topPos) {
    float h = kClimbPostHalfWidth;
    short bx = (short)basePos.x, by = (short)basePos.y, bz = (short)basePos.z;
    short tx = (short)topPos.x,  ty = (short)topPos.y,  tz = (short)topPos.z;

    // Quad 1: extends in ±X direction (visible looking along Z).
    Vtx a0 = MakeVtxN((short)(bx - h), by, bz, 0, 0, 127, 0xFF);
    Vtx a1 = MakeVtxN((short)(bx + h), by, bz, 0, 0, 127, 0xFF);
    Vtx a2 = MakeVtxN((short)(tx + h), ty, tz, 0, 0, 127, 0xFF);
    Vtx a3 = MakeVtxN((short)(tx - h), ty, tz, 0, 0, 127, 0xFF);
    vtxDl.push_back(a0);
    vtxDl.push_back(a1);
    vtxDl.push_back(a2);
    vtxDl.push_back(a3);
    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));

    // Quad 2: extends in ±Z direction (visible looking along X).
    Vtx b0 = MakeVtxN(bx, by, (short)(bz - h), 0, 0, 127, 0xFF);
    Vtx b1 = MakeVtxN(bx, by, (short)(bz + h), 0, 0, 127, 0xFF);
    Vtx b2 = MakeVtxN(tx, ty, (short)(tz + h), 0, 0, 127, 0xFF);
    Vtx b3 = MakeVtxN(tx, ty, (short)(tz - h), 0, 0, 127, 0xFF);
    vtxDl.push_back(b0);
    vtxDl.push_back(b1);
    vtxDl.push_back(b2);
    vtxDl.push_back(b3);
    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// Append a small ground-aligned '+' cross marker at `pos`. Two
// perpendicular thin XZ-plane quads — one extending along ±X, one along
// ±Z — produce a clear cross visible from any top-down camera angle.
// Used for the rejected-floor diagnostic overlay (DebugDraw v3e, polish
// wave commit 6).
static void AddCrossMarker(std::vector<Gfx>& dl, std::vector<Vtx>& vtxDl, const Vec3f& pos) {
    float arm = kRejectedFloorCrossArmLen;
    float w   = kRejectedFloorCrossHalfWidth;
    float y   = pos.y + kNodeQuadYLift;

    // Arm 1: long axis along X. Thin in Z direction.
    Vtx a0 = MakeVtxN((short)(pos.x - arm), (short)y, (short)(pos.z - w), 0, 127, 0, 0xFF);
    Vtx a1 = MakeVtxN((short)(pos.x + arm), (short)y, (short)(pos.z - w), 0, 127, 0, 0xFF);
    Vtx a2 = MakeVtxN((short)(pos.x + arm), (short)y, (short)(pos.z + w), 0, 127, 0, 0xFF);
    Vtx a3 = MakeVtxN((short)(pos.x - arm), (short)y, (short)(pos.z + w), 0, 127, 0, 0xFF);
    vtxDl.push_back(a0);
    vtxDl.push_back(a1);
    vtxDl.push_back(a2);
    vtxDl.push_back(a3);
    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));

    // Arm 2: long axis along Z. Thin in X direction.
    Vtx b0 = MakeVtxN((short)(pos.x - w), (short)y, (short)(pos.z - arm), 0, 127, 0, 0xFF);
    Vtx b1 = MakeVtxN((short)(pos.x + w), (short)y, (short)(pos.z - arm), 0, 127, 0, 0xFF);
    Vtx b2 = MakeVtxN((short)(pos.x + w), (short)y, (short)(pos.z + arm), 0, 127, 0, 0xFF);
    Vtx b3 = MakeVtxN((short)(pos.x - w), (short)y, (short)(pos.z + arm), 0, 127, 0, 0xFF);
    vtxDl.push_back(b0);
    vtxDl.push_back(b1);
    vtxDl.push_back(b2);
    vtxDl.push_back(b3);
    dl.push_back(gsSPVertex((uintptr_t)&vtxDl[vtxDl.size() - 4], 4, 0));
    dl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// ---------------------------------------------------------------------------
// Connected-component computation (DebugDraw v3d, polish wave commit 5).
//
// Union-find over RoomNavData::edges. Result is a parallel vector keyed by
// node index where each element is the canonical component-root index.
// Cached per-room in sComponentCache; invalidated on game-exit and
// per-room on Force Rescan.
// ---------------------------------------------------------------------------

static uint16_t UnionFindFind(std::vector<uint16_t>& parent, uint16_t i) {
    // Iterative path-walk + flatten. Avoids deep recursion on long chains.
    uint16_t root = i;
    while (parent[root] != root) {
        root = parent[root];
    }
    uint16_t cur = i;
    while (parent[cur] != root) {
        uint16_t next = parent[cur];
        parent[cur] = root;
        cur = next;
    }
    return root;
}

static void UnionFindUnion(std::vector<uint16_t>& parent, uint16_t a, uint16_t b) {
    uint16_t ra = UnionFindFind(parent, a);
    uint16_t rb = UnionFindFind(parent, b);
    if (ra == rb) return;
    // Attach larger-index root to smaller-index root for determinism so the
    // golden-angle palette assignment is stable across cache regenerations.
    if (ra < rb) parent[rb] = ra;
    else         parent[ra] = rb;
}

// Compute (or reuse cached) component-root vector for one room. Returns a
// reference into sComponentCache that remains valid until the next cache
// invalidation. Caller must not hold the reference across cache mutations.
static const std::vector<uint16_t>& GetOrComputeComponents(const RoomNavData* data) {
    uint32_t key = MakeCacheKey(data->sceneNum, data->roomNum);
    auto it = sComponentCache.find(key);
    if (it != sComponentCache.end() && it->second.size() == data->nodes.size()) {
        return it->second;
    }

    std::vector<uint16_t> parent(data->nodes.size());
    for (size_t i = 0; i < parent.size(); i++) {
        parent[i] = (uint16_t)i;
    }
    for (const NavEdge& edge : data->edges) {
        if (edge.fromIdx >= parent.size() || edge.toIdx >= parent.size()) continue;
        UnionFindUnion(parent, edge.fromIdx, edge.toIdx);
    }
    // Final pass to flatten every entry to its root so callers can read
    // parent[i] directly without re-walking the chain.
    for (size_t i = 0; i < parent.size(); i++) {
        parent[i] = UnionFindFind(parent, (uint16_t)i);
    }

    auto& slot = sComponentCache[key];
    slot = std::move(parent);
    return slot;
}

// HSV → RGB. h ∈ [0, 360), s/v ∈ [0, 1]. Output channels in [0, 255].
// Used for the golden-angle component palette so neighboring components
// get visually distinct hues.
static void HsvToRgb(float h, float s, float v, uint8_t* outR, uint8_t* outG, uint8_t* outB) {
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if      (hp < 1.0f) { r = c; g = x; b = 0; }
    else if (hp < 2.0f) { r = x; g = c; b = 0; }
    else if (hp < 3.0f) { r = 0; g = c; b = x; }
    else if (hp < 4.0f) { r = 0; g = x; b = c; }
    else if (hp < 5.0f) { r = x; g = 0; b = c; }
    else                { r = c; g = 0; b = x; }
    float m = v - c;
    auto sat = [](float v8) {
        if (v8 < 0.0f)   return (uint8_t)0;
        if (v8 > 255.0f) return (uint8_t)255;
        return (uint8_t)v8;
    };
    *outR = sat((r + m) * 255.0f);
    *outG = sat((g + m) * 255.0f);
    *outB = sat((b + m) * 255.0f);
}

// Emit walkable nodes grouped by connected component, each component
// colored from a golden-angle-distributed HSV palette. Replaces the
// single green walkable group when DebugDrawComponents is on.
//
// Skips nodes drawn by higher-priority later groups (HAZARD / UNDERWATER /
// STEEP_SLOPE) so cells aren't double-drawn.
static void EmitComponentColoredWalkables(const RoomNavData* data) {
    if (data->nodes.empty()) return;
    const std::vector<uint16_t>& components = GetOrComputeComponents(data);
    if (components.size() != data->nodes.size()) return; // defensive

    // Bucket walkable node indices by their component root.
    std::unordered_map<uint16_t, std::vector<uint16_t>> bucket;
    for (size_t i = 0; i < data->nodes.size(); i++) {
        const NavNode& node = data->nodes[i];
        if (!(node.flags & NODE_WALKABLE)) continue;
        if (node.flags & NODE_HAZARD)      continue;
        if (node.flags & NODE_UNDERWATER)  continue;
        if (node.flags & NODE_STEEP_SLOPE) continue;
        bucket[components[i]].push_back((uint16_t)i);
    }

    // Sort by root index so palette assignment is stable across frames
    // (unordered_map iteration order is not portable).
    std::vector<uint16_t> roots;
    roots.reserve(bucket.size());
    for (auto& kv : bucket) roots.push_back(kv.first);
    std::sort(roots.begin(), roots.end());

    // One color group per component. Hue distributed by golden angle (137°)
    // so neighboring components are visually distinct.
    for (size_t cIdx = 0; cIdx < roots.size(); cIdx++) {
        float hue = std::fmod((float)cIdx * 137.0f, 360.0f);
        uint8_t r, g, b;
        HsvToRgb(hue, 0.7f, 0.8f, &r, &g, &b);
        sXluDl.push_back(gsDPSetPrimColor(0, 0, r, g, b, 0xFF));
        for (uint16_t nodeIdx : bucket[roots[cIdx]]) {
            AddGroundQuad(sXluDl, sVtxDl, data->nodes[nodeIdx].pos);
        }
    }
}

// Build the in-world overlay for one room's nav data. Groups nodes by
// color (PrimColor switches between groups) so the RDP doesn't reload
// state per quad.
static void BuildOverlayDrawData(const RoomNavData* data) {
    if (data == nullptr) return;

    // Walkable family — green / darker-green / yellow-green node groups.
    //
    // Color precedence (post-merge of A + B): HAZARD > UNDERWATER >
    // STEEP_SLOPE > ORPHANED > HAZARD_ADJACENT > EDGE > WALKABLE. Each
    // later group's continue clauses skip any node already drawn by an
    // earlier higher-priority group. Orphan wins over HAZARD_ADJACENT/EDGE
    // so an unreachable node is unambiguously gray regardless of its
    // other walkable-flags.
    //
    // When DebugDrawComponents (workstream C) is on, the entire walkable
    // family is replaced by a per-connected-component palette (golden-
    // angle HSV distribution) over every walkable node, so the user can
    // see how the multi-seed floodfill partitioned the graph into
    // sub-chambers. The component view subsumes EDGE/HAZARD_ADJACENT
    // distinctions — they remain in the underlying flag data but are not
    // separately visualized.
    if (CVarGetInteger(CVAR_ROOM_NAV_DEBUG_DRAW_COMPONENTS, 0) != 0) {
        EmitComponentColoredWalkables(data);
    } else {
        // Walkable nodes — green.
        sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x00, 0xC8, 0x00, 0xFF));
        for (const NavNode& node : data->nodes) {
            if (!(node.flags & NODE_WALKABLE))       continue;
            if (node.flags & NODE_HAZARD)             continue;
            if (node.flags & NODE_UNDERWATER)         continue;
            if (node.flags & NODE_STEEP_SLOPE)        continue;
            if (node.flags & NODE_ORPHANED)           continue;
            if (node.flags & NODE_HAZARD_ADJACENT)    continue;
            if (node.flags & NODE_EDGE)               continue;
            AddGroundQuad(sXluDl, sVtxDl, node.pos);
        }

        // Edge nodes — darker green.
        sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x00, 0x80, 0x00, 0xFF));
        for (const NavNode& node : data->nodes) {
            if (!(node.flags & NODE_WALKABLE))       continue;
            if (node.flags & NODE_HAZARD)             continue;
            if (node.flags & NODE_UNDERWATER)         continue;
            if (node.flags & NODE_STEEP_SLOPE)        continue;
            if (node.flags & NODE_ORPHANED)           continue;
            if (node.flags & NODE_HAZARD_ADJACENT)    continue;
            if (!(node.flags & NODE_EDGE))            continue;
            AddGroundQuad(sXluDl, sVtxDl, node.pos);
        }

        // Hazard-adjacent nodes — yellow-green.
        sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xA0, 0xC0, 0x00, 0xFF));
        for (const NavNode& node : data->nodes) {
            if (!(node.flags & NODE_WALKABLE))         continue;
            if (node.flags & NODE_HAZARD)               continue;
            if (node.flags & NODE_UNDERWATER)           continue;
            if (node.flags & NODE_STEEP_SLOPE)          continue;
            if (node.flags & NODE_ORPHANED)             continue;
            if (!(node.flags & NODE_HAZARD_ADJACENT))   continue;
            AddGroundQuad(sXluDl, sVtxDl, node.pos);
        }
    }

    // Hazard nodes — red.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xE0, 0x20, 0x20, 0xFF));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & NODE_HAZARD)) continue;
        AddGroundQuad(sXluDl, sVtxDl, node.pos);
    }

    // Underwater nodes — blue. Skip if also hazard (drawn red above).
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x40, 0x80, 0xFF, 0xFF));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & NODE_UNDERWATER)) continue;
        if (node.flags & NODE_HAZARD)        continue;
        AddGroundQuad(sXluDl, sVtxDl, node.pos);
    }

    // Steep-slope nodes — orange. Skip if also hazard or underwater.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0x90, 0x10, 0xFF));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & NODE_STEEP_SLOPE)) continue;
        if (node.flags & NODE_HAZARD)         continue;
        if (node.flags & NODE_UNDERWATER)     continue;
        AddGroundQuad(sXluDl, sVtxDl, node.pos);
    }

    // Orphaned nodes — dim gray. Walkable in geometry but unreachable
    // from any seed cell via the edge graph (stacked floors on top of
    // walls / fences / scenery; see ScanRoom orphan-pass comment for
    // detection algorithm). Drawn after higher-priority groups so a node
    // that's BOTH orphaned AND hazard/underwater gets the more
    // safety-critical color. Skip if those groups already claimed the
    // node.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x60, 0x60, 0x60, 0xC0));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & NODE_ORPHANED))   continue;
        if (node.flags & NODE_HAZARD)        continue;
        if (node.flags & NODE_UNDERWATER)    continue;
        if (node.flags & NODE_STEEP_SLOPE)   continue;
        AddGroundQuad(sXluDl, sVtxDl, node.pos);
    }

    // Edges — white. Drawn as thin ground-aligned quads from one node to
    // the next so the connectivity graph is visible against the floor.
    // Endpoint indices are bounds-checked because a corrupted disk cache
    // could carry indices that no longer resolve (stale schema, partial
    // write). nodeCount==0 is already handled by the empty-data early-out
    // in the caller.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0xFF, 0xFF, 0xC0));
    const size_t nodeCount = data->nodes.size();
    for (const NavEdge& edge : data->edges) {
        if (edge.fromIdx >= nodeCount || edge.toIdx >= nodeCount) continue;
        const Vec3f& posA = data->nodes[edge.fromIdx].pos;
        const Vec3f& posB = data->nodes[edge.toIdx].pos;
        AddGroundLineQuad(sXluDl, sVtxDl, posA, posB);
    }

    // Climb anchors — yellow. Ground quad at base + ground quad at top +
    // a vertical post (two perpendicular thin quads) connecting them so
    // the marker reads as a clear vertical climb indicator from any angle.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0xE0, 0x10, 0xFF));
    for (const ClimbAnchor& anchor : data->climbAnchors) {
        AddGroundQuad(sXluDl, sVtxDl, anchor.basePos);
        AddGroundQuad(sXluDl, sVtxDl, anchor.topPos);
        AddVerticalPost(sXluDl, sVtxDl, anchor.basePos, anchor.topPos);
    }

    // Ledge anchors — light purple / lavender. Distinct hue from climb
    // anchors (yellow) so the user can visually tell ladder/vine climbs
    // apart from jump-grab ledge points. Same marker shape: ground quad
    // at approach + ground quad at top + thin connecting post.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xC0, 0x80, 0xFF, 0xFF));
    for (const LedgeAnchor& anchor : data->ledgeAnchors) {
        AddGroundQuad(sXluDl, sVtxDl, anchor.approachPos);
        AddGroundQuad(sXluDl, sVtxDl, anchor.topPos);
        AddVerticalPost(sXluDl, sVtxDl, anchor.approachPos, anchor.topPos);
    }

    // Crawlspace anchors — cyan / teal. Distinct from climb-yellow and
    // ledge-purple. Ground quad at the entry position; a short ground
    // line extending in the wall-normal direction shows which way the
    // crawlspace opens (the AI navigator faces this direction to enter).
    constexpr float kCrawlNormalArrowLen = 24.0f;
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x40, 0xE0, 0xE0, 0xFF));
    for (const CrawlspaceAnchor& anchor : data->crawlspaceAnchors) {
        AddGroundQuad(sXluDl, sVtxDl, anchor.entryPos);
        // Direction line: extend from entry centroid along the wall
        // normal so the user can see which way the crawlspace faces.
        Vec3f tip = {
            anchor.entryPos.x + anchor.entryNormal.x * kCrawlNormalArrowLen,
            anchor.entryPos.y,
            anchor.entryPos.z + anchor.entryNormal.z * kCrawlNormalArrowLen,
        };
        AddGroundLineQuad(sXluDl, sVtxDl, anchor.entryPos, tip);
    }

    // Drop anchors — bright green. Opposite side of the colour wheel
    // from ledge-purple so the two are unambiguous at a glance, and
    // also distinct from climb-yellow (climb-up) and crawlspace-cyan
    // (crawl-through). Ground quad at high position + ground quad at
    // landing + thin vertical post connecting them so the descent
    // direction is visible at a glance.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x40, 0xFF, 0x40, 0xFF));
    for (const DropAnchor& anchor : data->dropAnchors) {
        AddGroundQuad(sXluDl, sVtxDl, anchor.highPos);
        AddGroundQuad(sXluDl, sVtxDl, anchor.landingPos);
        AddVerticalPost(sXluDl, sVtxDl, anchor.landingPos, anchor.highPos);
    }

    // Climb-base / climb-top flagged nodes (polish wave commit 4) — bright
    // yellow ring overlay at every node carrying NODE_CLIMB_BASE or
    // NODE_CLIMB_TOP. Slightly larger than ordinary node quads
    // (kClimbFlagHalfExt = 7.0f vs kNodeQuadHalfExtent = 5.0f) so they
    // read as a distinct "this node is a climb endpoint" marker. Distinct
    // from the climb-anchor group above (which renders the raw scene-actor
    // basePos / topPos as smaller solid squares + a vertical post); these
    // markers live on the nav-graph nodes that consumers will actually
    // navigate to. Drawn inline via open-coded quad rather than
    // AddGroundQuad so the larger half-extent doesn't require a helper
    // parameterisation for a single additional caller.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0xFF, 0x40, 0xFF));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & (NODE_CLIMB_BASE | NODE_CLIMB_TOP))) continue;
        float h = kClimbFlagHalfExt;
        float y = node.pos.y + kNodeQuadYLift;
        Vtx v0 = MakeVtxN((short)(node.pos.x - h), (short)y, (short)(node.pos.z - h), 0, 127, 0, 0xFF);
        Vtx v1 = MakeVtxN((short)(node.pos.x + h), (short)y, (short)(node.pos.z - h), 0, 127, 0, 0xFF);
        Vtx v2 = MakeVtxN((short)(node.pos.x + h), (short)y, (short)(node.pos.z + h), 0, 127, 0, 0xFF);
        Vtx v3 = MakeVtxN((short)(node.pos.x - h), (short)y, (short)(node.pos.z + h), 0, 127, 0, 0xFF);
        sVtxDl.push_back(v0);
        sVtxDl.push_back(v1);
        sVtxDl.push_back(v2);
        sVtxDl.push_back(v3);
        sXluDl.push_back(gsSPVertex((uintptr_t)&sVtxDl[sVtxDl.size() - 4], 4, 0));
        sXluDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
    }

    // Hazard centroids — deep red, slightly larger than node quads so they
    // read as "this whole region is bad." Currently unpopulated by the v1
    // scan; loop is a no-op until a future commit clusters HAZARD nodes
    // into centroid markers. Drawn via the per-node helper with a temp
    // pos that adopts the hazard-centroid half-extent (the helper uses
    // kNodeQuadHalfExtent unconditionally, so we open-code the larger
    // marker directly inline rather than parameterise the helper for one
    // additional caller).
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x80, 0x00, 0x00, 0xFF));
    for (const Vec3f& centroid : data->hazardCentroids) {
        float h = kHazardCentroidHalfExt;
        float y = centroid.y + kNodeQuadYLift;
        Vtx v0 = MakeVtxN((short)(centroid.x - h), (short)y, (short)(centroid.z - h), 0, 127, 0, 0xFF);
        Vtx v1 = MakeVtxN((short)(centroid.x + h), (short)y, (short)(centroid.z - h), 0, 127, 0, 0xFF);
        Vtx v2 = MakeVtxN((short)(centroid.x + h), (short)y, (short)(centroid.z + h), 0, 127, 0, 0xFF);
        Vtx v3 = MakeVtxN((short)(centroid.x - h), (short)y, (short)(centroid.z + h), 0, 127, 0, 0xFF);
        sVtxDl.push_back(v0);
        sVtxDl.push_back(v1);
        sVtxDl.push_back(v2);
        sVtxDl.push_back(v3);
        sXluDl.push_back(gsSPVertex((uintptr_t)&sVtxDl[sVtxDl.size() - 4], 4, 0));
        sXluDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
    }

    // Rejected-floor crosses — magenta. DebugDraw v3e, polish wave commit 6.
    // Populated only when LogRejectedFloors is on at scan time; empty
    // otherwise. Magenta-coloured '+' crosses at each rejected floor sample
    // so the user can confirm allowlist hits visually (e.g. crosses on top
    // of every chest in the room).
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0x00, 0xFF, 0xFF));
    for (const Vec3f& pos : data->rejectedFloorPositions) {
        AddCrossMarker(sXluDl, sVtxDl, pos);
    }

    // ActorTrail breadcrumbs — hot pink/magenta vertical posts (kTrailMarkerHeight
    // tall) at every captured waypoint in the current scene. Distinct hue from
    // every nav-graph colour (yellow climb, lavender ledge, cyan crawlspace,
    // green drop, red hazard, gray orphan) so trail markers don't blend with
    // static graph geometry.
    //
    // Snapshot is filtered to the current scene at copy time (cross-scene
    // waypoints are stale and would just clutter the overlay). When the
    // ActorTrail master CVar (Nav.Enabled + Nav.ActorTrail) is off, the trail
    // map is empty and the snapshot returns nothing — no per-frame cost.
    constexpr float kTrailMarkerHeight = 20.0f;
    static thread_local std::vector<AnchorNav::ActorTrail::WaypointSnapshot> sTrailSnapshot;
    AnchorNav::ActorTrail::GetInstance().SnapshotActiveWaypoints(
        gPlayState->sceneNum, sTrailSnapshot);
    if (!sTrailSnapshot.empty()) {
        sXluDl.push_back(gsDPSetPrimColor(0, 0, 0xFF, 0x20, 0xC0, 0xFF));
        for (const auto& wp : sTrailSnapshot) {
            Vec3f topPos = { wp.pos.x, wp.pos.y + kTrailMarkerHeight, wp.pos.z };
            AddVerticalPost(sXluDl, sVtxDl, wp.pos, topPos);
        }
    }
}

// ---------------------------------------------------------------------------
// Debug overlay registration glue.
// ---------------------------------------------------------------------------

static void OnDebugDraw() {
    bool nowEnabled = IsEnabled() && CVarGetInteger(CVAR_ROOM_NAV_DEBUG_DRAW, 0) != 0;

    if (nowEnabled != sDebugDrawWasEnabled) {
        sDebugDrawWasEnabled = nowEnabled;
        SPDLOG_INFO("[RoomNav] DebugDraw {}", nowEnabled ? "enabled" : "disabled");
    }

    if (!nowEnabled) return;

    // Per-room one-shot summary: when the active room changes while
    // DebugDraw is on, emit the loaded nav graph's stats. Useful for
    // "is the data loaded?" verification without requiring the in-world
    // overlay to be implemented yet.
    //
    // Update tracking variables ONLY on successful summary. If data is
    // nullptr (scan hasn't run yet — common on the first frame after a
    // room transition because OnGameFrameTick's scan dispatch is gated
    // on transitionTrigger == TRANS_TRIGGER_OFF, while OnPlayDrawEnd
    // fires regardless), retry silently on subsequent frames until the
    // scan populates the cache.
    PlayState* play = gPlayState;
    if (play == nullptr) return;
    int16_t scene = play->sceneNum;
    int8_t  room  = (int8_t)play->roomCtx.curRoom.num;
    if (scene == sDebugDrawLastSummaryScene && room == sDebugDrawLastSummaryRoom) return;

    const RoomNavData* data = GetForRoom(scene, room);
    if (data == nullptr) {
        // Silent — wait for the scan to populate the cache. Tracking
        // variables NOT updated, so the next frame retries.
        return;
    }

    SPDLOG_INFO("[RoomNav][DebugDraw] scene={} room={} loaded: nodes={} edges={} climbs={} hazards={}",
                scene, (int)room,
                data->nodes.size(), data->edges.size(),
                data->climbAnchors.size(), data->hazardCentroids.size());
    sDebugDrawLastSummaryScene = scene;
    sDebugDrawLastSummaryRoom  = room;
}

// Per-frame in-world overlay rendering. Called from OnPlayDrawEnd hook.
// Builds a display list of ground-aligned quads for the active room's
// nav graph and splices it into POLY_XLU_DISP.
static void OnDebugDrawRender() {
    if (!IsEnabled()) return;
    if (CVarGetInteger(CVAR_ROOM_NAV_DEBUG_DRAW, 0) == 0) return;

    PlayState* play = gPlayState;
    if (play == nullptr) return;

    int16_t scene = play->sceneNum;
    int8_t  room  = (int8_t)play->roomCtx.curRoom.num;
    const RoomNavData* data = GetForRoom(scene, room);
    if (data == nullptr) return;
    if (data->nodes.empty()) return;

    ResetGfxBuffer(sXluDl);
    ResetGfxBuffer(sVtxDl);
    // Reserve up front so vector reallocation doesn't invalidate the
    // gsSPVertex pointers we embed into sXluDl below.
    // Upper bounds: 4 verts + 2 Gfx commands per node-quad / edge-quad /
    // hazard-centroid; 4 quads (2 ground + 2 perpendicular vertical) per
    // climb anchor → 16 verts + 8 Gfx commands. Orphan group adds another
    // (nodes.size() * 4 verts, nodes.size() * 2 Gfx) for worst-case
    // all-orphan rooms. Climb-base/top flag overlay adds at most 2×
    // climbAnchors (one node tagged per basePos + one per topPos via
    // FindNearestNode), 4 verts + 2 Gfx commands per flagged node.
    // Rejected-floor crosses are 2 quads each → 8 verts + 4 Gfx commands.
    // The mutual-exclusion continue chains in BuildOverlayDrawData mean
    // actual usage is bounded tighter; reserves are additive across groups
    // for forward-compat with parallel workstreams that may add their own
    // groups.
    // ActorTrail breadcrumb upper bound — kMaxWaypoints (30) per entity ×
    // peak entity set (~8 = local player + remote DummyPlayers + AI Follower
    // + synced enemies with leavesTrail=true). Each marker is a 2-quad
    // vertical post → 8 verts + 2 Gfx commands. Reserve slop is fine; the
    // capacity-overshoot is a one-time scene-change allocation.
    constexpr size_t kMaxTrailWaypointsForReserve = 30 * 8;
    sVtxDl.reserve(data->nodes.size() * 4
                   + data->nodes.size() * 4 // orphan group
                   + data->edges.size() * 4
                   + data->climbAnchors.size() * 16
                   + data->climbAnchors.size() * 8 // climb-flag overlay (2 nodes × 4 verts)
                   + data->ledgeAnchors.size() * 16  // ledge: 2 ground quads + 2 perp posts
                   + data->crawlspaceAnchors.size() * 8  // entry quad + direction line
                   + data->dropAnchors.size() * 16    // drop: 2 ground quads + 2 perp posts
                   + data->hazardCentroids.size() * 4
                   + data->rejectedFloorPositions.size() * 8
                   + kMaxTrailWaypointsForReserve * 8);
    // Gfx commands: 2 per node-quad / edge-quad / hazard-centroid quad; 8
    // per climb anchor (2 ground quads + 4 vertical-post quad pairs); 4 per
    // rejected-floor cross (2 quads); 64 for setup + fixed per-color-group
    // PrimColor switches. When DebugDrawComponents is on, each connected
    // component adds one extra gsDPSetPrimColor — worst case is one
    // component per walkable node. Adding `nodes.size()` here absorbs that
    // bound.
    sXluDl.reserve(data->nodes.size() * 2
                   + data->nodes.size() * 2 // orphan group
                   + data->nodes.size()     // worst-case per-component PrimColor switches
                   + data->edges.size() * 2
                   + data->climbAnchors.size() * 8
                   + data->climbAnchors.size() * 4 // climb-flag overlay (2 nodes × 2 Gfx)
                   + data->ledgeAnchors.size() * 8  // ledge: same shape as climb anchor
                   + data->crawlspaceAnchors.size() * 4  // entry quad + direction line
                   + kMaxTrailWaypointsForReserve * 2 + 1  // trail posts: 2 Gfx each + 1 PrimColor
                   + data->dropAnchors.size() * 8    // drop: same shape as ledge
                   + data->hazardCentroids.size() * 2
                   + data->rejectedFloorPositions.size() * 4
                   + 64);

    InitDebugGfx(sXluDl);
    BuildOverlayDrawData(data);
    sXluDl.push_back(gsSPEndDisplayList());

    // Splice into the frame's transparent display list. Delegated to a
    // file-scope helper at global namespace so the OPEN_DISPS / CLOSE_DISPS
    // macros expand outside namespace AnchorNavRoom — see comment on the
    // frame_interpolation.h include for the linkage rationale.
    ::RoomNavSpliceXluDispList(play, sXluDl.data());
}

// ---------------------------------------------------------------------------
// Public force-rescan API. See header for semantics.
// ---------------------------------------------------------------------------

// Internal helper. Drops cache + disk + tracker state for the current
// room and lets OnGameFrameTick re-trigger the scan next frame.
// `trigger` is a short tag included in the log line so the user can tell
// user-initiated Force Rescans apart from auto-triggered ones (Tier 2
// rescan-on-scene-flag).
static void DoTriggerFullRescan(const char* trigger) {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        SPDLOG_WARN("[RoomNav] TriggerFullRescan({}): gPlayState is null; ignoring", trigger);
        return;
    }
    int16_t scene = play->sceneNum;
    int8_t  room  = (int8_t)play->roomCtx.curRoom.num;
    uint32_t key = MakeCacheKey(scene, room);

    // Preserve historicalSeeds across the rescan. ScanRoom on the
    // next polling tick gets a fresh empty RoomNavData; without this,
    // accumulated seed positions from prior scans would be lost.
    auto cacheIt = sCache.find(key);
    if (cacheIt != sCache.end() && !cacheIt->second.historicalSeeds.empty()) {
        sPreservedHistoricalSeeds[key] = cacheIt->second.historicalSeeds;
    }

    sCache.erase(key);
    sComponentCache.erase(key);
    sVisitedCellsCache.erase(key);

    auto path = RoomNavFilePath(scene, room);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    sLastScene = -1;
    sLastRoom  = -1;
    sDebugDrawLastSummaryScene = -1;
    sDebugDrawLastSummaryRoom  = -1;

    f32 px = 0.0f, py = 0.0f, pz = 0.0f;
    Player* player = GET_PLAYER(play);
    if (player != nullptr) {
        px = player->actor.world.pos.x;
        py = player->actor.world.pos.y;
        pz = player->actor.world.pos.z;
    }
    SPDLOG_INFO("[RoomNav] FullRescan({}): scene={} room={} playerPos=({:.0f},{:.0f},{:.0f}); "
                "dropped cache + disk; re-scan triggers next frame",
                trigger, scene, (int)room, px, py, pz);
}

void ForceRescanCurrentRoom() {
    DoTriggerFullRescan("user");
}

// Internal helper. Refreshes only the climb + ledge anchors for the
// current room WITHOUT rerunning floodfill or edge generation. Cost is
// ~5-30ms depending on Path B mask and scene size. Used by the Tier 1
// auto-refresh path (OnSceneFlagSet → anchor-only refresh) so cheap
// state changes (e.g. slingshot ladder fall) don't pay the full
// 50-150ms scan cost.
//
// The detection passes need `visited` (cell set, room-scoping) and
// `nodesByCell` (spatial index). Both are scan-time locals in
// ScanRoom. We reconstruct them from the persisted nav data: every
// node's cellIdxX/Z field tells us which cell it belongs to, so the
// `visited` set is "every cell that has at least one node" and
// `nodesByCell` is the obvious inverse mapping.
static void DoRefreshAnchorsCurrentRoom(const char* trigger) {
    PlayState* play = gPlayState;
    if (play == nullptr) return;
    int16_t scene = play->sceneNum;
    int8_t  room  = (int8_t)play->roomCtx.curRoom.num;
    uint32_t key = MakeCacheKey(scene, room);

    auto it = sCache.find(key);
    if (it == sCache.end()) return; // no cache → nothing to refresh
    RoomNavData& nav = it->second;
    if (nav.nodes.empty()) return;

    auto refreshStart = std::chrono::steady_clock::now();

    size_t prevClimbs = nav.climbAnchors.size();
    size_t prevLedges = nav.ledgeAnchors.size();

    // Clear the anchor flags from nodes so re-tagging is correct.
    for (NavNode& node : nav.nodes) {
        node.flags &= ~(NODE_CLIMB_BASE | NODE_CLIMB_TOP);
    }

    // Also clear NODE_CRAWLSPACE flags on nodes (DetectCrawlspaces below
    // re-tags them). Doing this in the same pass as the climb-flag
    // reset would be cleaner but the loop above already ran; one extra
    // pass is cheap (O(N) over typical 1000-node rooms).
    for (NavNode& node : nav.nodes) {
        node.flags &= ~NODE_CRAWLSPACE;
    }

    // Drop the previous round's climb-surface edges BEFORE the matching
    // node resize — the edges reference indices in
    // [firstClimbSurfaceNodeIdx, oldNodeCount) and would dangle once
    // those nodes go away. Stage 3 emits floor↔climb boundary edges
    // whose floor endpoint is < firstClimbSurfaceNodeIdx but whose
    // climb endpoint is >= it; both directions caught by the predicate
    // in DropClimbSurfaceEdges.
    DropClimbSurfaceEdges(&nav);

    // Drop the previous round's climb-surface nodes (schema v7+). They
    // live in `nav.nodes[firstClimbSurfaceNodeIdx ..]` with cellIdxX/Z
    // holding U/V grid coords (NOT world cells); reconstructing
    // `nodesByCell` from them would corrupt the floor-side spatial
    // index. GenerateClimbSurfaceGrids re-creates them after detection
    // re-runs and re-tagging completes.
    if (nav.firstClimbSurfaceNodeIdx != UINT16_MAX &&
        (size_t)nav.firstClimbSurfaceNodeIdx <= nav.nodes.size()) {
        nav.nodes.resize((size_t)nav.firstClimbSurfaceNodeIdx);
    }
    nav.firstClimbSurfaceNodeIdx = UINT16_MAX;

    nav.climbAnchors.clear();
    nav.ledgeAnchors.clear();
    nav.crawlspaceAnchors.clear();
    nav.dropAnchors.clear();

    // Reconstruct visited + nodesByCell from persistent node data.
    // Only floor nodes remain at this point — climb-surface nodes were
    // resized away above.
    std::unordered_set<CellKey, CellKeyHash> visited;
    std::unordered_map<CellKey, std::vector<uint16_t>, CellKeyHash> nodesByCell;
    for (uint16_t i = 0; i < nav.nodes.size(); i++) {
        CellKey k{ (int32_t)(int16_t)nav.nodes[i].cellIdxX,
                   (int32_t)(int16_t)nav.nodes[i].cellIdxZ };
        visited.insert(k);
        nodesByCell[k].push_back(i);
    }

    // Re-run anchor detection using the reconstructed sets. The detection
    // functions read live actor / collision state via gPlayState, so they
    // pick up any actor moves / wall changes that happened since the
    // previous scan.
    DetectClimbAnchors(&nav, play);
    DetectClimbAnchorsViaSurfaceFlags(&nav, play, visited);
    DetectLedgeAnchors(&nav, play, nodesByCell);
    DetectCrawlspaces(&nav, play, visited);
    DetectDropAnchors(&nav, play, nodesByCell);

    // Re-tag climb-base / climb-top flags from the refreshed anchors.
    // Must run BEFORE GenerateClimbSurfaceGrids appends climb-surface
    // nodes, so FindNearestNode searches over floor-only nodes.
    for (const ClimbAnchor& anchor : nav.climbAnchors) {
        int baseIdx = FindNearestNode(&nav, anchor.basePos);
        if (baseIdx >= 0) {
            nav.nodes[(size_t)baseIdx].flags |= NODE_CLIMB_BASE;
        }
        int topIdx = FindNearestNode(&nav, anchor.topPos);
        if (topIdx >= 0) {
            nav.nodes[(size_t)topIdx].flags |= NODE_CLIMB_TOP;
        }
    }

    // Re-generate climb-surface grids (schema v7+) for the refreshed
    // anchors. Same ordering as the main scan path.
    GenerateClimbSurfaceGrids(&nav, play);

    // Reset the debug-draw summary tracker so the next overlay frame
    // re-emits the loaded-summary line with the refreshed counts.
    sDebugDrawLastSummaryScene = -1;
    sDebugDrawLastSummaryRoom  = -1;

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - refreshStart).count();

    SPDLOG_INFO("[RoomNav] RefreshAnchors({}): scene={} room={} climbs {}->{} ledges {}->{} "
                "in {}ms",
                trigger, scene, (int)room,
                prevClimbs, nav.climbAnchors.size(),
                prevLedges, nav.ledgeAnchors.size(),
                ms);
}

// Hash positions of all BG + PROP actors in the current room. Called
// each frame during a pending-dispatch window to detect when actors
// have settled. Quantized to 1u to absorb floating-point jitter.
static uint64_t HashBgPropPositions(PlayState* play) {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
    constexpr ActorCategory kCats[] = { ACTORCAT_BG, ACTORCAT_PROP };
    for (ActorCategory cat : kCats) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            // Quantize to 1u — small per-frame jitter on hovering or
            // physics-settling actors won't reset the stability counter.
            int32_t qx = (int32_t)actor->world.pos.x;
            int32_t qy = (int32_t)actor->world.pos.y;
            int32_t qz = (int32_t)actor->world.pos.z;
            hash ^= (uint64_t)qx;          hash *= 1099511628211ULL;
            hash ^= (uint64_t)qy;          hash *= 1099511628211ULL;
            hash ^= (uint64_t)qz;          hash *= 1099511628211ULL;
            hash ^= (uint64_t)actor->id;   hash *= 1099511628211ULL;
            actor = actor->next;
        }
    }
    return hash;
}

// Hook handler — fires for every Flags_*Set / scene flag write. Sets
// the appropriate pending flag based on which Tier CVars are on, AND
// arms the position-stability tracking so the actual rescan fires
// AFTER the multi-frame animation triggered by the flag set has
// settled.
//
// Filter: only react to FLAG_SCENE_SWITCH (push blocks, falling
// ladders, scripted scenery, switch-driven events). Other flagTypes
// (treasure, clear, collectible, inf-table, etc.) are item-pickup or
// progress-tracking flags that don't change room topology — reacting
// to them produces wasteful rescans.
//
// Tier 2 (full rescan) takes precedence over Tier 1 if both are
// enabled, since a full rescan implicitly refreshes anchors too.
static void OnSceneFlagSetHookHandler(int16_t /*sceneNum*/, int16_t flagType,
                                       int16_t /*flag*/) {
    if (!IsEnabled()) return;
    if (flagType != FLAG_SCENE_SWITCH) return; // skip non-topology flags

    bool fullRescan    = CVarGetInteger(CVAR_ROOM_NAV_AUTO_FULL_RESCAN, 0) != 0;
    bool anchorRefresh = CVarGetInteger(CVAR_ROOM_NAV_AUTO_REFRESH_ANCHORS, 0) != 0;
    if (!fullRescan && !anchorRefresh) return;

    if (fullRescan) {
        sPendingFullRescan = true;
    } else {
        sPendingAnchorRefresh = true;
    }
    // (Re)arm stability tracking — each new flag set restarts the
    // settle window. Hash captured next frame in
    // DispatchPendingDynamicRefresh becomes the new baseline.
    sStabilityCounter = 0;
    sMaxWaitCountdown = kMaxWaitFrames;
}

// Called from OnGameFrameTick (after the per-frame logic) to drain
// pending refresh/rescan requests. Watches BG + PROP actor positions
// for a settle window: dispatches when positions have been stable for
// kStabilityRequiredFrames consecutive frames, OR when kMaxWaitFrames
// safety net elapses (in case actors never settle).
static void DispatchPendingDynamicRefresh() {
    if (!sPendingFullRescan && !sPendingAnchorRefresh) return;

    PlayState* play = gPlayState;
    if (play == nullptr) return;

    uint64_t hash = HashBgPropPositions(play);
    if (hash != sLastBgPropPositionHash) {
        sLastBgPropPositionHash = hash;
        sStabilityCounter       = 0; // motion detected — restart window
    } else {
        sStabilityCounter++;
    }

    if (sMaxWaitCountdown > 0) sMaxWaitCountdown--;
    bool stable          = (sStabilityCounter   >= kStabilityRequiredFrames);
    bool maxWaitExpired  = (sMaxWaitCountdown   <= 0);
    if (!stable && !maxWaitExpired) return; // keep waiting

    const char* trigger = maxWaitExpired ? "scene-flag-timeout" : "scene-flag";
    if (sPendingFullRescan) {
        sPendingFullRescan    = false;
        sPendingAnchorRefresh = false; // full rescan supersedes
        DoTriggerFullRescan(trigger);
    } else if (sPendingAnchorRefresh) {
        sPendingAnchorRefresh = false;
        DoRefreshAnchorsCurrentRoom(trigger);
    }
    sStabilityCounter = 0;
    sMaxWaitCountdown = 0;
}

// ---------------------------------------------------------------------------
// Registration. Single ShipInit entry point; called once at startup.
// ---------------------------------------------------------------------------

static void RegisterRoomNavData() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameTick);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnExitGame>(
        OnExitGameClear);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawEnd>(
        OnDebugDraw);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawEnd>(
        OnDebugDrawRender);
    // Tier 1 + Tier 2 dynamic refresh — fires for every Flags_SetSwitch /
    // scene flag write. Hook handler is internally CVar-gated; no-op
    // when neither AutoRefreshAnchorsOnSceneFlag nor
    // AutoFullRescanOnSceneFlag is on.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneFlagSet>(
        OnSceneFlagSetHookHandler);

    // Backup signal for room-change detection. The polling-based
    // delta detection in OnGameFrameTick can theoretically miss a
    // single-frame state change if csCtx.state or transitionTrigger
    // is in a "skip" state during the room delta. Hooking OnTransitionEnd
    // lets us reset the polling tracker so the next OnGameFrameTick
    // unconditionally re-detects the current room as "new" and queues
    // the delayed initial scan. Belt-and-suspenders for missed
    // transitions.
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnTransitionEnd>(
        [](int16_t /*sceneNum*/) {
            // Force the next OnGameFrameTick to treat the room as
            // changed regardless of the current sLastScene/sLastRoom
            // values. Doesn't fire the scan directly — lets the
            // existing delayed-scan machinery handle it normally.
            sLastScene = -1;
            sLastRoom  = -1;
        });
}

} // namespace AnchorNavRoom

static RegisterShipInitFunc registerRoomNavData(AnchorNavRoom::RegisterRoomNavData);
