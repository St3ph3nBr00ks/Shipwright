/**
 * SceneLog — per-room troubleshooting / pre-flight diagnostic logging.
 *
 * Goal: emit structured log lines on actor spawn, room transition,
 * scene init events, plus persist a static representation of each
 * room's contents so any future bug investigation knows what should
 * be in scene X room Y without having to rescan the room.
 *
 * Self-contained — registers GameInteractor hooks from a single file,
 * no edits to pre-Flotilla source. Gated behind CVar
 * gDeveloperTools.SceneLog.Level (default 0 = no output, zero overhead).
 *
 * Two output streams:
 *   - Standard log file: human-readable [Tag] scene=N room=M ... lines
 *     via SPDLOG_INFO (alongside existing log content).
 *   - Per-room manifest files (roommanifests/scene_<n>_room_<m>.json):
 *     one JSON file per (scene, room). Latest visit's static-actor list
 *     overwrites; provenance updates; visitCount increments;
 *     cutscenesObserved / otrResourceHashes preserved across visits
 *     (populated by Phase B). Atomic write (temp + rename) avoids
 *     corruption on crash.
 *
 * Used in conjunction with SoH's built-in log: SoH log says
 * "Room Init - curRoom.num: 0xN" → look up
 * roommanifests/scene_<scene>_room_<N>.json for canonical room contents.
 *
 * See:
 *   - Claude/Plans/agent_brief_scenelog_completion.md
 *   - Claude/Plans/implementation_plan_logging_and_scene_data.md
 *   - GitHub #201 (logging plan)
 *   - GitHub #202 (scene_data persistent layer)
 */

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ActorDB.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

// soh/ActorDB.h declares ActorDB_Retrieve only inside its `#else`
// (C-only) branch (ActorDB.h:78-83). When included from a C++ TU the
// C function is not visible, producing C3861. The implementation in
// soh/ActorDB.cpp:619 is already `extern "C"`, so a local extern "C"
// forward-declaration is the minimum-risk fix.
extern "C" ActorDBEntry* ActorDB_Retrieve(const int id);

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_SCENE_LOG_LEVEL CVAR_DEVELOPER_TOOLS("SceneLog.Level")
#define SCENE_LOG_LEVEL_OFF 0
#define SCENE_LOG_LEVEL_PREFLIGHT 1
#define SCENE_LOG_LEVEL_INVENTORY_STATE 2
#define SCENE_LOG_LEVEL_RUNTIME 3
#define SCENE_LOG_LEVEL_VERBOSE 4

#define SCENELOG_SCHEMA_VERSION 1

static int SceneLogLevel() {
    return CVarGetInteger(CVAR_SCENE_LOG_LEVEL, SCENE_LOG_LEVEL_OFF);
}

static const char* GetActorSymbolicName(int16_t actorId) {
    ActorDBEntry* dbEntry = ActorDB_Retrieve(actorId);
    if (dbEntry != nullptr && dbEntry->valid && dbEntry->name != nullptr) {
        return dbEntry->name;
    }
    return "<unnamed>";
}

// Subset of soh/include/tables/scene_table.h scene constant names, keyed
// by scene number. Used only for the sceneName field in the per-room
// manifest — purely informational, falls back to "UNKNOWN_SCENE_<hex>"
// when not in this list. Extend as needed for additional scene coverage.
static const char* GetSceneSymbolicName(int sceneNum) {
    switch (sceneNum) {
        case 0x00: return "INSIDE_GREAT_DEKU_TREE";
        case 0x01: return "DODONGOS_CAVERN";
        case 0x02: return "INSIDE_JABU_JABU";
        case 0x03: return "FOREST_TEMPLE";
        case 0x04: return "FIRE_TEMPLE";
        case 0x05: return "WATER_TEMPLE";
        case 0x06: return "SPIRIT_TEMPLE";
        case 0x07: return "SHADOW_TEMPLE";
        case 0x08: return "BOTTOM_OF_THE_WELL";
        case 0x09: return "ICE_CAVERN";
        case 0x0A: return "GANONS_TOWER";
        case 0x0B: return "GERUDO_TRAINING_GROUND";
        case 0x0C: return "THIEVES_HIDEOUT";
        case 0x0D: return "INSIDE_GANONS_CASTLE";
        case 0x0E: return "GANONS_TOWER_COLLAPSING";
        case 0x0F: return "INSIDE_GANONS_CASTLE_COLLAPSING";
        case 0x10: return "TREASURE_BOX_SHOP";
        case 0x11: return "GOHMA_BOSS_ARENA";
        case 0x12: return "DODONGO_BOSS_ARENA";
        case 0x13: return "BARINADE_BOSS_ARENA";
        case 0x14: return "PHANTOM_GANON_BOSS_ARENA";
        case 0x15: return "VOLVAGIA_BOSS_ARENA";
        case 0x16: return "MORPHA_BOSS_ARENA";
        case 0x17: return "TWINROVA_BOSS_ARENA";
        case 0x18: return "BONGO_BONGO_BOSS_ARENA";
        case 0x19: return "GANONDORF_BOSS_ARENA";
        case 0x1A: return "TOWER_COLLAPSE_EXTERIOR";
        case 0x1B: return "MARKET_ENTRANCE_DAY";
        case 0x1C: return "MARKET_ENTRANCE_NIGHT";
        case 0x1D: return "MARKET_ENTRANCE_RUINS";
        case 0x1E: return "BACK_ALLEY_DAY";
        case 0x1F: return "BACK_ALLEY_NIGHT";
        case 0x20: return "MARKET_DAY";
        case 0x21: return "MARKET_NIGHT";
        case 0x22: return "MARKET_RUINS";
        case 0x23: return "TEMPLE_OF_TIME_EXTERIOR_DAY";
        case 0x24: return "TEMPLE_OF_TIME_EXTERIOR_NIGHT";
        case 0x25: return "TEMPLE_OF_TIME_EXTERIOR_RUINS";
        case 0x26: return "KNOW_IT_ALL_HOUSE";
        case 0x27: return "TWINS_HOUSE";
        case 0x28: return "MIDOS_HOUSE";
        case 0x29: return "SARIAS_HOUSE";
        case 0x2A: return "CARPENTER_BOSS_HOUSE";
        case 0x2B: return "BACK_ALLEY_HOUSE";
        case 0x2C: return "GRANNYS_POTION_SHOP";
        case 0x2D: return "KOKIRI_SHOP";
        case 0x2E: return "GORON_SHOP";
        case 0x2F: return "ZORA_SHOP";
        case 0x30: return "KAKARIKO_POTION_SHOP";
        case 0x31: return "MARKET_POTION_SHOP";
        case 0x32: return "BOMBCHU_SHOP";
        case 0x33: return "HAPPY_MASK_SHOP";
        case 0x34: return "LINKS_HOUSE";
        case 0x35: return "BACK_ALLEY_DOG_LADY_HOUSE";
        case 0x36: return "STABLE";
        case 0x37: return "GRAVEKEEPERS_HUT";
        case 0x38: return "IMPAS_HOUSE";
        case 0x39: return "LAKESIDE_LABORATORY";
        case 0x3A: return "CARPENTERS_TENT";
        case 0x3B: return "GRAVE_REDEAD";
        case 0x3C: return "GRAVE_FAIRY_FOUNTAIN";
        case 0x3D: return "GRAVE_ROYAL_TOMB";
        case 0x3E: return "DAMPES_GRAVE";
        case 0x3F: return "GREAT_FAIRY_FOUNTAIN_UPGRADE";
        case 0x40: return "SHOOTING_GALLERY";
        case 0x41: return "TEMPLE_OF_TIME";
        case 0x42: return "CHAMBER_OF_THE_SAGES";
        case 0x43: return "CASTLE_COURTYARD";
        case 0x44: return "GREAT_FAIRY_FOUNTAIN_SPELLS";
        case 0x45: return "GROTTOS";
        case 0x46: return "GRAVE_REDEAD_DUPLICATE";
        case 0x47: return "FAIRY_FOUNTAIN";
        case 0x48: return "BOMBCHU_BOWLING_ALLEY";
        case 0x49: return "RANCH_HOUSE";
        case 0x4A: return "GUARD_HOUSE";
        case 0x4B: return "GRANNYS_POTION_SHOP_2";
        case 0x4C: return "GANON_BLUE_WARP";
        case 0x4D: return "HOUSE_OF_SKULLTULA";
        case 0x4E: return "GERUDO_FORTRESS_INTERIOR_DUP";
        case 0x4F: return "FINAL_GANON_BATTLE_AREA";
        case 0x50: return "HOUSE_OF_SKULLTULA_DUP";
        case 0x51: return "HYRULE_FIELD";
        case 0x52: return "KAKARIKO_VILLAGE";
        case 0x53: return "GRAVEYARD";
        case 0x54: return "ZORAS_RIVER";
        case 0x55: return "KOKIRI_FOREST";
        case 0x56: return "SACRED_FOREST_MEADOW";
        case 0x57: return "LAKE_HYLIA";
        case 0x58: return "ZORAS_DOMAIN";
        case 0x59: return "ZORAS_FOUNTAIN";
        case 0x5A: return "GERUDO_VALLEY";
        case 0x5B: return "LOST_WOODS";
        case 0x5C: return "DESERT_COLOSSUS";
        case 0x5D: return "GERUDO_FORTRESS";
        case 0x5E: return "HAUNTED_WASTELAND";
        case 0x5F: return "HYRULE_CASTLE";
        case 0x60: return "DEATH_MOUNTAIN_TRAIL";
        case 0x61: return "DEATH_MOUNTAIN_CRATER";
        case 0x62: return "GORON_CITY";
        case 0x63: return "LON_LON_RANCH";
        case 0x64: return "OUTSIDE_GANONS_CASTLE";
        default:   return "UNKNOWN_SCENE";
    }
}

// ---------------------------------------------------------------------------
// RoomManifest — per-room static manifest writer.
//
// Each (scene, room) gets one JSON file at:
//   roommanifests/scene_<scene>_room_<room>.json
//
// Read-modify-write semantics: each visit's static-actor list overwrites
// staticActors[]; provenance updates; visitCount increments;
// cutscenesObserved / otrResourceHashes (populated by Phase B) are
// preserved across visits.
//
// Atomic write: temp file + rename, so a crash mid-write doesn't corrupt
// the existing file.
//
// In-memory buffers per (scene, room) accumulate static-actor entries
// during the spawn batch; flushed on OnSceneSpawnActors() (the natural
// "end of static spawn batch" signal).
// ---------------------------------------------------------------------------

namespace RoomManifest {

struct ActorEntry {
    int16_t actorId;
    uint16_t params;
    float posX, posY, posZ;
    int16_t category;
    std::string name;  // resolved at append time
};

using RoomKey = std::pair<int16_t, int16_t>;  // (scene, room)

static std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

// Per-(scene, room) buffer of static-actor entries, accumulated between
// OnActorSpawn calls and flushed on OnSceneSpawnActors.
static std::map<RoomKey, std::vector<ActorEntry>>& Buffer() {
    static std::map<RoomKey, std::vector<ActorEntry>> b;
    return b;
}

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static std::filesystem::path RoomManifestPath(int16_t sceneNum, int16_t roomNum) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "roommanifests/scene_%d_room_%d.json",
                  static_cast<int>(sceneNum), static_cast<int>(roomNum));
    return std::filesystem::path(buf);
}

// Ensures roommanifests/ directory exists. Best-effort; logs and returns
// false on failure (mirrors RoomNavData.cpp:201-207 convention).
static bool EnsureDirectory() {
    std::error_code ec;
    std::filesystem::create_directories("roommanifests", ec);
    if (ec) {
        SPDLOG_WARN("[SceneLog] EnsureDirectory: create_directories(roommanifests) failed: {}",
                    ec.message());
        return false;
    }
    return true;
}

// Atomically write `content` to `target`. Returns true on success.
// Pattern: write to temp file, rename to target. Rename is atomic on
// POSIX and on NTFS; protects against torn writes if the process dies
// mid-write.
static bool WriteAtomic(const std::filesystem::path& target, const std::string& content) {
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f.is_open()) {
            SPDLOG_WARN("[SceneLog] WriteAtomic: open temp file failed: {}", tmp.string());
            return false;
        }
        f << content;
        if (!f.good()) {
            SPDLOG_WARN("[SceneLog] WriteAtomic: write to temp file failed: {}", tmp.string());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        SPDLOG_WARN("[SceneLog] WriteAtomic: rename {} -> {} failed: {}",
                    tmp.string(), target.string(), ec.message());
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return false;
    }
    return true;
}

// Read the existing manifest file for (scene, room) if present and parse
// as JSON. Returns empty/default JSON on first visit or on parse failure.
static nlohmann::json ReadExisting(int16_t sceneNum, int16_t roomNum) {
    auto path = RoomManifestPath(sceneNum, roomNum);
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        return nlohmann::json::object();  // first visit
    }
    try {
        nlohmann::json j;
        f >> j;
        return j;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[SceneLog] ReadExisting: parse failed for {}: {} (treating as fresh)",
                    path.string(), e.what());
        return nlohmann::json::object();
    }
}

// Append a static-actor entry to the per-(scene, room) buffer.
// Called from OnActorSpawn when numSetupActors > 0.
static void AppendStaticActor(int16_t sceneNum, int16_t roomNum, const ActorEntry& entry) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) return;
    std::lock_guard<std::mutex> lock(Mutex());
    Buffer()[{sceneNum, roomNum}].push_back(entry);
}

// Flush the per-(scene, room) buffer to disk via read-modify-write.
// Reads existing JSON, overwrites staticActors[] and provenance with
// current visit's data, increments visitCount, preserves
// cutscenesObserved / otrResourceHashes.
static void FlushRoom(int16_t sceneNum, int16_t roomNum) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) return;
    if (!EnsureDirectory()) return;

    std::vector<ActorEntry> actors;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        auto it = Buffer().find({sceneNum, roomNum});
        if (it == Buffer().end() || it->second.empty()) {
            // No static actors observed — still write a manifest entry so
            // the file exists and visitCount increments. The room may be
            // intentionally empty (cutscene-only, traversal-only).
            actors.clear();
        } else {
            actors = std::move(it->second);
            it->second.clear();
        }
    }

    nlohmann::json j = ReadExisting(sceneNum, roomNum);

    // Top-level identity fields.
    j["schemaVersion"] = SCENELOG_SCHEMA_VERSION;
    j["scene"] = sceneNum;
    j["sceneName"] = GetSceneSymbolicName(sceneNum);
    j["room"] = roomNum;

    // Provenance — overwrite each visit; visitCount carries forward.
    int prevCount = 0;
    if (j.contains("provenance") && j["provenance"].contains("visitCount") &&
        j["provenance"]["visitCount"].is_number_integer()) {
        prevCount = j["provenance"]["visitCount"].get<int>();
    }
    j["provenance"] = {
        { "buildCommit",          static_cast<const char*>(gGitCommitHash) },
        { "buildVersion",         static_cast<const char*>(gBuildVersion) },
        { "buildBranch",          static_cast<const char*>(gGitBranch) },
        { "buildDate",            reinterpret_cast<const char*>(gBuildDate) },
        { "lastVisitedTimestamp", NowMs() },
        { "visitCount",           prevCount + 1 },
    };

    // Static actors — overwrite each visit (deterministic from scene/room data).
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : actors) {
        char idHex[8];
        char paramsHex[8];
        std::snprintf(idHex, sizeof(idHex), "0x%04X", static_cast<unsigned>(a.actorId) & 0xFFFFu);
        std::snprintf(paramsHex, sizeof(paramsHex), "0x%04X", static_cast<unsigned>(a.params) & 0xFFFFu);
        arr.push_back({
            { "actorId",        idHex },
            { "actorIdDecimal", static_cast<int>(a.actorId) & 0xFFFF },
            { "name",           a.name },
            { "params",         paramsHex },
            { "pos",            { static_cast<int>(a.posX),
                                  static_cast<int>(a.posY),
                                  static_cast<int>(a.posZ) } },
            { "category",       a.category },
        });
    }
    j["staticActors"] = std::move(arr);

    // Phase B fields — preserve if present, default to empty if missing.
    if (!j.contains("cutscenesObserved")) {
        j["cutscenesObserved"] = nlohmann::json::array();
    }
    if (!j.contains("otrResourceHashes")) {
        j["otrResourceHashes"] = nlohmann::json::object();
    }

    auto path = RoomManifestPath(sceneNum, roomNum);
    std::string content = j.dump(2);
    content += '\n';
    if (!WriteAtomic(path, content)) {
        SPDLOG_WARN("[SceneLog] FlushRoom: WriteAtomic failed for scene={} room={}",
                    sceneNum, roomNum);
    }
}

}  // namespace RoomManifest

// ---------------------------------------------------------------------------
// Hook handlers
// ---------------------------------------------------------------------------

static void OnActorSpawnLog(void* refActor) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    if (refActor == nullptr || gPlayState == nullptr) {
        return;
    }
    Actor* actor = static_cast<Actor*>(refActor);
    const char* actorName = GetActorSymbolicName(actor->id);
    int isStatic = gPlayState->numSetupActors > 0 ? 1 : 0;

    SPDLOG_INFO("[Spawn] scene={} room={} actor=0x{:04X} \"{}\" params=0x{:04X} pos=({:.0f},{:.0f},{:.0f}) cat={} setup={}",
                gPlayState->sceneNum,
                actor->room,
                static_cast<uint16_t>(actor->id),
                actorName,
                static_cast<uint16_t>(actor->params),
                actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                actor->category,
                isStatic);

    // Static-only: append to the (scene, room) buffer for flush at
    // OnSceneSpawnActors. Dynamic spawns (drops, projectiles, etc.) are
    // logged to SoH log above but not persisted to the per-room manifest
    // — the manifest is a static reference, not a session event log.
    if (isStatic) {
        RoomManifest::ActorEntry entry;
        entry.actorId = actor->id;
        entry.params = static_cast<uint16_t>(actor->params);
        entry.posX = actor->world.pos.x;
        entry.posY = actor->world.pos.y;
        entry.posZ = actor->world.pos.z;
        entry.category = actor->category;
        entry.name = actorName;
        RoomManifest::AppendStaticActor(gPlayState->sceneNum, actor->room, entry);
    }
}

static void OnTransitionEndLog(int16_t sceneNum) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    int16_t roomNum = (gPlayState != nullptr) ? gPlayState->roomCtx.curRoom.num : -1;
    uint16_t entrance = static_cast<uint16_t>(gSaveContext.entranceIndex);

    SPDLOG_INFO("[TransitionEnd] scene={} room={} entrance=0x{:04X}", sceneNum, roomNum, entrance);
}

static void OnSceneInitLog(int16_t sceneNum) {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    SPDLOG_INFO("[SceneInit] scene={}", sceneNum);
}

static void OnSceneSpawnActorsLog() {
    if (SceneLogLevel() < SCENE_LOG_LEVEL_PREFLIGHT) {
        return;
    }
    if (gPlayState == nullptr) {
        return;
    }
    int16_t sceneNum = gPlayState->sceneNum;
    int16_t roomNum = gPlayState->roomCtx.curRoom.num;

    SPDLOG_INFO("[SceneSpawnActors] scene={} room={}", sceneNum, roomNum);

    // Flush the per-(scene, room) buffer accumulated by OnActorSpawn calls
    // since the previous flush. This is the canonical "static spawn batch
    // is complete" signal in OoT's scene-load lifecycle.
    RoomManifest::FlushRoom(sceneNum, roomNum);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void RegisterSceneLog() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorSpawn>(OnActorSpawnLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnTransitionEnd>(OnTransitionEndLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>(OnSceneInitLog);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(OnSceneSpawnActorsLog);
}

static RegisterShipInitFunc registerSceneLog(RegisterSceneLog);
