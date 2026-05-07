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
#include "soh/ObjectExtension/ObjectExtension.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

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

#define CVAR_ROOM_NAV_ENABLED            CVAR_ENHANCEMENT("RoomNavData.Enabled")
#define CVAR_ROOM_NAV_AUTO_SCAN          CVAR_ENHANCEMENT("RoomNavData.AutoScan")
#define CVAR_ROOM_NAV_DEBUG_DRAW         CVAR_ENHANCEMENT("RoomNavData.DebugDraw")
#define CVAR_ROOM_NAV_LOG_STUCK_ON_SLOPE CVAR_ENHANCEMENT("RoomNavData.LogStuckOnSlope")

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
    int bestIdx = -1;
    float bestDistSq = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < data->nodes.size(); i++) {
        const NavNode& n = data->nodes[i];
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

int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int /*fromIdx*/,
                                  const Vec3f& /*targetPos*/) {
    if (data == nullptr) {
        return -1;
    }
    // Phase 2 (commit 9 — nav system integration) implements the
    // hazard-aware BFS per plan §10 (kHazardEscapeHops = 2 + fallback to
    // FindNearestNonHazardExit). Requires NavTraits flags
    // (eligibleForSwimming / avoidHazardNodes / consumeRoomNavData) which
    // land in Phase 2 commit 10. Until those flags exist, this stub
    // returns -1 unconditionally — consumers (GetBestReachableSubgoal
    // Layer 3) treat that as "no static graph available" and fall
    // through to other layers per nav plan §5.
    return -1;
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

static constexpr uint16_t kCurrentSchemaVersion = 1;
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

    uint32_t nodeCount    = (uint32_t)nav.nodes.size();
    uint32_t edgeCount    = (uint32_t)nav.edges.size();
    uint32_t climbCount   = (uint32_t)nav.climbAnchors.size();
    uint32_t hazardCount  = (uint32_t)nav.hazardCentroids.size();
    WriteValue(f, nodeCount);
    WriteValue(f, edgeCount);
    WriteValue(f, climbCount);
    WriteValue(f, hazardCount);

    WriteVector(f, nav.nodes);
    WriteVector(f, nav.edges);
    WriteVector(f, nav.climbAnchors);
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
    uint32_t nodeCount = 0, edgeCount = 0, climbCount = 0, hazardCount = 0;

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
    if (!ReadValue(f, nodeCount))   return;
    if (!ReadValue(f, edgeCount))   return;
    if (!ReadValue(f, climbCount))  return;
    if (!ReadValue(f, hazardCount)) return;

    if (!ReadVector(f, out->nodes, nodeCount))                   return;
    if (!ReadVector(f, out->edges, edgeCount))                   return;
    if (!ReadVector(f, out->climbAnchors, climbCount))           return;
    if (!ReadVector(f, out->hazardCentroids, hazardCount))       return;

    out->magic           = magic;
    out->version         = version;
    out->sceneNum        = fileScene;
    out->roomNum         = fileRoom;
    out->scanTimestamp   = scanTimestamp;
    out->bboxMin         = bboxMin;
    out->bboxMax         = bboxMax;
    out->gridResolution  = gridRes;
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
// `kEstimatedClimbHeight` units above. Field testing extends this list
// when new climbable actor types surface.
struct ClimbableActorEntry {
    int16_t actorId;
    float   estimatedHeight; // climb-top Y delta from actor.world.pos.y
};
static const ClimbableActorEntry kClimbableActors[] = {
    { ACTOR_BG_SPOT18_OBJ,      150.0f }, // Goron City interior ladder
    { ACTOR_BG_DDAN_KD,         200.0f }, // Dodongo's Cavern ladder
    { ACTOR_BG_SPOT06_OBJECTS,  120.0f }, // Lake Hylia (some climbables)
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

    uint8_t flags = 0;

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
    ACTOR_OBJ_OSHIHIKI, // 0x0140 — push blocks
    ACTOR_EN_BOX,       // 0x000A — chests
};

static bool FloorIsRejectedByAllowlist(s32 floorBgId, PlayState* play) {
    if (floorBgId < 0 || floorBgId >= BG_ACTOR_MAX) return false; // not dynamic
    Actor* owner = play->colCtx.dyna.bgActors[floorBgId].actor;
    if (owner == nullptr) return false;
    for (int16_t rejectId : kFloorActorRejectList) {
        if (owner->id == rejectId) return true;
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
        // it to find the real floor below.
        if (FloorIsRejectedByAllowlist(floorBgId, play)) {
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
                extraSeeds++;
            }
            a = a->next;
        }
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

    // Climb anchor detection (commit 6) — Path A scene-actor allowlist.
    // Iterates ACTORCAT_BG and ACTORCAT_PROP actor lists for known
    // climbable scene actors and records (basePos, topPos) anchors.
    // Path B (surface-flag query for vine walls) deferred — bit position
    // unidentified in vanilla source. v1 covers ladders only.
    DetectClimbAnchors(out, play);

    auto totalMsFinal = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStart).count();
    SPDLOG_INFO("[RoomNav] ScanRoom: scene={} room={} nodes={} edges={} climbs={} cells={} "
                "seeds={} scanMs={} edgeMs={} totalMs={}",
                sceneNum, (int)roomNum, out->nodes.size(), out->edges.size(),
                out->climbAnchors.size(), visited.size(), 1 + extraSeeds,
                scanMs, edgeMs, totalMsFinal);
}

// Top-level lookup-then-scan dispatch. Called once per (scene, room)
// transition by OnGameFrameTick.
static void OnRoomEntered(int16_t sceneNum, int8_t roomNum, PlayState* play) {
    uint32_t key = MakeCacheKey(sceneNum, roomNum);

    // Step 1: in-memory cache. Already-loaded rooms early-return.
    if (sCache.count(key)) {
        return;
    }

    // Step 2: disk cache (stub until commit 7).
    RoomNavData fresh{};
    TryLoadFromDisk(sceneNum, roomNum, &fresh);
    if (fresh.sceneNum == sceneNum && fresh.roomNum == roomNum && !fresh.nodes.empty()) {
        sCache.emplace(key, std::move(fresh));
        SPDLOG_INFO("[RoomNav] Loaded cached scene={} room={} from disk", sceneNum, (int)roomNum);
        return;
    }

    // Step 3: scan + persist (scan stubbed in commit 2; populated in commits 3-6;
    // disk-write stubbed until commit 7).
    if (!IsAutoScanEnabled()) {
        // AutoScan off: never scan, even if no cached data exists. Used for
        // "play with this exact baked set, don't generate more" mode.
        return;
    }

    RoomNavData scanned{};
    ScanRoom(sceneNum, roomNum, play, &scanned);
    if (!scanned.nodes.empty()) {
        SaveToDisk(scanned);
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

    int16_t currentScene = play->sceneNum;
    int8_t  currentRoom  = (int8_t)play->roomCtx.curRoom.num;

    if (currentScene == sLastScene && currentRoom == sLastRoom) {
        return; // unchanged; nothing to do
    }
    sLastScene = currentScene;
    sLastRoom  = currentRoom;

    OnRoomEntered(currentScene, currentRoom, play);
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

// Build the in-world overlay for one room's nav data. Groups nodes by
// color (PrimColor switches between groups) so the RDP doesn't reload
// state per quad.
static void BuildOverlayDrawData(const RoomNavData* data) {
    if (data == nullptr) return;

    // Walkable nodes — green. Color precedence: HAZARD > UNDERWATER >
    // STEEP_SLOPE > WALKABLE, so each later group's continue clauses skip
    // any node already drawn by an earlier higher-priority group.
    sXluDl.push_back(gsDPSetPrimColor(0, 0, 0x00, 0xC8, 0x00, 0xFF));
    for (const NavNode& node : data->nodes) {
        if (!(node.flags & NODE_WALKABLE)) continue;
        if (node.flags & NODE_HAZARD)      continue;
        if (node.flags & NODE_UNDERWATER)  continue;
        if (node.flags & NODE_STEEP_SLOPE) continue;
        AddGroundQuad(sXluDl, sVtxDl, node.pos);
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
    // climb anchor → 16 verts + 8 Gfx commands.
    sVtxDl.reserve(data->nodes.size() * 4
                   + data->edges.size() * 4
                   + data->climbAnchors.size() * 16
                   + data->hazardCentroids.size() * 4);
    sXluDl.reserve(data->nodes.size() * 2
                   + data->edges.size() * 2
                   + data->climbAnchors.size() * 8
                   + data->hazardCentroids.size() * 2
                   + 64); // setup + per-color-group PrimColor switches

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

void ForceRescanCurrentRoom() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        SPDLOG_WARN("[RoomNav] ForceRescanCurrentRoom: gPlayState is null; ignoring");
        return;
    }
    int16_t scene = play->sceneNum;
    int8_t  room  = (int8_t)play->roomCtx.curRoom.num;
    uint32_t key = MakeCacheKey(scene, room);

    // Drop in-memory cache entry. GetForRoom and the OnDebugDrawRender
    // path will return nullptr until the next OnRoomEntered re-scan
    // populates a fresh entry.
    sCache.erase(key);

    // Best-effort delete on-disk .bin. Failure is silent — the in-memory
    // re-scan will overwrite the file via SaveToDisk anyway.
    auto path = RoomNavFilePath(scene, room);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Reset the polling tracker so OnGameFrameTick's delta-detection
    // re-fires OnRoomEntered next frame.
    sLastScene = -1;
    sLastRoom  = -1;

    // Reset DebugDraw summary tracker so the post-rescan log reports the
    // refreshed counts even if the (scene, room) didn't change.
    sDebugDrawLastSummaryScene = -1;
    sDebugDrawLastSummaryRoom  = -1;

    SPDLOG_INFO("[RoomNav] ForceRescanCurrentRoom: dropped cache + disk for scene={} room={}; "
                "re-scan triggers next frame", scene, (int)room);
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
}

} // namespace AnchorNavRoom

static RegisterShipInitFunc registerRoomNavData(AnchorNavRoom::RegisterRoomNavData);
