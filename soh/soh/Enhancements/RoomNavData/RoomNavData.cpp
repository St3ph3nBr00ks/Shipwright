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

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------------
// CVar definitions — see plan §3 for full layout.
// ---------------------------------------------------------------------------

#define CVAR_ROOM_NAV_ENABLED       CVAR_ENHANCEMENT("RoomNavData.Enabled")
#define CVAR_ROOM_NAV_AUTO_SCAN     CVAR_ENHANCEMENT("RoomNavData.AutoScan")
#define CVAR_ROOM_NAV_DEBUG_DRAW    CVAR_ENHANCEMENT("RoomNavData.DebugDraw")

namespace Anchor::Nav::Room {

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

int FindNearestNode(const RoomNavData* data, const Vec3f& /*pos*/) {
    if (data == nullptr || data->nodes.empty()) {
        return -1;
    }
    // Commit 1: stub — actual nearest-node lookup lands in commit 8.
    return -1;
}

int FindBestReachableSubgoalNode(const RoomNavData* data,
                                  int /*fromIdx*/,
                                  const Vec3f& /*targetPos*/) {
    if (data == nullptr) {
        return -1;
    }
    // Commit 1: stub — BFS lands in Phase 2 when nav system integration ships.
    return -1;
}

// ---------------------------------------------------------------------------
// Scan dispatch. Commit 2 implements the lookup-then-scan FLOW; the actual
// scan logic lands in commit 3 (multi-cast + node classification). Until
// then, ScanRoom is a stub that creates an empty RoomNavData entry to
// register the room as "attempted" and avoid re-attempting every frame.
// ---------------------------------------------------------------------------

static void TryLoadFromDisk(int16_t /*sceneNum*/, int8_t /*roomNum*/, RoomNavData* /*out*/) {
    // Commit 7 implements binary file I/O. Until then this is a no-op:
    // disk cache always misses, scan path always fires.
}

// ---------------------------------------------------------------------------
// Scan implementation. Plan §5 — multi-cast per XZ + floodfill from player
// position, bounded by scene-level colCtx.minBounds/maxBounds.
// ---------------------------------------------------------------------------

static constexpr float    kGridResolution      = 30.0f;  // §11 lock
static constexpr int      kMaxFloorsPerColumn  = 8;      // defensive cap
static constexpr int      kMaxScanIterations   = 50000;  // floodfill runaway guard
static constexpr int64_t  kMaxScanWallTimeMs   = 1000;   // per-scan budget

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
static int ScanColumnAt(RoomNavData* nav, float x, float z, PlayState* play, const CellKey& cell) {
    int firstNodeIdx = -1;
    float startY = nav->bboxMax.y + 50.0f; // start above scene ceiling

    for (int i = 0; i < kMaxFloorsPerColumn; i++) {
        Vec3f castOrigin = { x, startY, z };
        CollisionPoly floorPoly{}; // BgCheck_AnyRaycastFloor2 copies the poly by value
        s32 floorBgId = BGCHECK_SCENE;
        f32 floorY = BgCheck_AnyRaycastFloor2(&play->colCtx, &floorPoly, &floorBgId, &castOrigin);

        if (floorY <= BGCHECK_Y_MIN) break;       // no floor below
        if (floorY <= nav->bboxMin.y) break;      // below scene
        if (floorY >= startY) break;              // raycast didn't make progress (degenerate)

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
    out->scanTimestamp  = 0; // commit 7 fills this from wall-clock seconds.
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

        // Enqueue 8-neighbors only if we found at least one floor here.
        // Cells with no floor are terminal (open space, walls, off-room).
        if (firstIdx >= 0) {
            for (int dx = -1; dx <= 1; dx++) {
                for (int dz = -1; dz <= 1; dz++) {
                    if (dx == 0 && dz == 0) continue;
                    CellKey neighbor{ cell.x + dx, cell.z + dz };
                    if (visited.count(neighbor)) continue;
                    pending.push_back(neighbor);
                }
            }
        }
    }

    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scanStart).count();
    SPDLOG_INFO("[RoomNav] ScanRoom: scene={} room={} nodes={} cells={} elapsed={}ms",
                sceneNum, (int)roomNum, out->nodes.size(), visited.size(), totalMs);

    // Commit 4 will add: BuildEdges(out)
    // Commit 5 will add: HAZARD / UNDERWATER classification refinement
    // Commit 6 will add: DetectClimbAnchors(out, play)
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
    sCache.emplace(key, std::move(scanned));
    // Commit 7 will add: SaveToDisk(sceneNum, roomNum, sCache.at(key));
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
// Registration. Single ShipInit entry point; called once at startup.
// ---------------------------------------------------------------------------

static void RegisterRoomNavData() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameTick);
}

} // namespace Anchor::Nav::Room

static RegisterShipInitFunc registerRoomNavData(Anchor::Nav::Room::RegisterRoomNavData);
