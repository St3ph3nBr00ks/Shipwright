/**
 * AI Player Follower Diagnostic Recorder — implementation.
 *
 * Spec: Claude/Plans/follower_recorder_plan.md.
 */

#include "FollowerRecorder.h"
#include "Follower.h"
#include "../Anchor.h"
#include "../Common/DistanceMath.h"
#include "../Common/NavCVars.h"
#include "soh/cvar_prefixes.h"
#include "soh/ShipInit.hpp"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // NODE_CLIMB_ANY for subgoal-flag breakdown

#include "ship/Context.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

#define CVAR_FR_ENABLED      CVAR_DEVELOPER_TOOLS("FollowerRecorder.Enabled")
#define CVAR_FR_CAPTURE_HZ   CVAR_DEVELOPER_TOOLS("FollowerRecorder.CaptureHz")
#define CVAR_FR_MAX_SECONDS  CVAR_DEVELOPER_TOOLS("FollowerRecorder.MaxSeconds")

// Schema versions:
//   v1 — initial release (frame, follower state/pos/yaw, leader fields,
//         nav-path size + cursor, autonomousClimb flag, safety-net
//         counters, events array).
//   v2 — climb-surface diagnostics: subgoalFlags (NavNode flag bitmap of
//         current waypoint), computedClimbMask (resolved consumer
//         mask), climbNodesInPath (count of climb-surface waypoints
//         remaining from cursor to end-of-path).
//   v3 — pathFlagsAggregate (OR of remaining waypoint flags — see if
//         path INCLUDES intent anywhere downstream of the cursor) +
//         navPathWaypoints array (full per-waypoint position + flag
//         dump for path-routing investigations).
#define FR_SCHEMA_VERSION 3

namespace AnchorFollower {

namespace {

namespace fs = std::filesystem;

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

// All file-scope state is guarded by Mutex(). Recording session lifecycle:
// inactive → CVar flips on → CaptureFrame opens file lazily on next tick →
// active → CVar flips off OR maxSeconds elapsed → file closed in next
// OnGameFrameUpdate tick.
std::ofstream            gFile;
std::string              gFilePath;
uint64_t                 gStartMs         = 0;
uint64_t                 gLastCaptureMs   = 0;
int                      gCaptureCount    = 0;
int                      gLinesSinceFlush = 0;
std::vector<std::string> gPendingEvents;
bool                     gActive          = false;

constexpr int kFlushEveryNLines = 60;     // ~4s at 15 Hz

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// SoH's logs/ directory, sibling to the main `Ship of Harkinian <N>.log`
// files. Resolved via the same AppDirectoryPath API that OTRGlobals.cpp:798
// uses for the main log, so portable + dev-shell-friendly + matches the
// path the user already knows from SPDLOG.
fs::path LogsDir() {
    return fs::path(Ship::Context::GetAppDirectoryPath()) / "logs";
}

bool EnsureDirectory() {
    std::error_code ec;
    if (fs::exists(LogsDir(), ec)) return true;
    fs::create_directories(LogsDir(), ec);
    if (ec) {
        SPDLOG_WARN("[FollowerRecorder] EnsureDirectory failed: {}", ec.message());
        return false;
    }
    return true;
}

// Find the index N of the current session's main log file. OTRGlobals.cpp:
// 803-836 picks `maxN + 1` at SoH launch and writes "Ship of Harkinian <N>.log"
// to logs/. By the time the recorder fires, that file exists and is the
// highest-numbered match in logs/. Scanning for the max N here gives us the
// session's index without needing to plumb it through libultraship.
//
// Returns -1 if no main-log file is found (recorder will fall back to a
// timestamp-based filename in that edge case).
int FindCurrentSessionLogIndex() {
    std::error_code ec;
    fs::path dir = LogsDir();
    if (!fs::exists(dir, ec)) return -1;

    const std::string prefix = "Ship of Harkinian ";
    const std::string suffix = ".log";
    int maxN = -1;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().generic_string();
        if (name.size() <= prefix.size() + suffix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        const std::string middle =
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (middle.empty()) continue;
        // Skip "<N> follower" filenames so a prior session's recorder log
        // doesn't inflate the index. Only digits-only middles are main logs.
        if (!std::all_of(middle.begin(), middle.end(), ::isdigit)) continue;
        try {
            int n = std::stoi(middle);
            if (n > maxN) maxN = n;
        } catch (...) { /* malformed; skip */ }
    }
    return maxN;
}

// Map followerAIState's underlying integer to a name. Lookup is by integer
// because Anchor::FollowerAIState is a PRIVATE nested enum — friend access is
// granted only to CaptureFrame, not to free helpers in this anonymous
// namespace. CaptureFrame casts the enum to int and passes it here; the
// integer values match the enum declaration order in Anchor.h:515.
//
// 2026-05-12 PM: RETURN removed (merged into FOLLOW). Integer values for
// CLIMBING and later states shifted down by 1 — old JSONL recordings
// produced before this commit will name the post-RETURN states
// incorrectly. Re-record any captures used as regression baselines.
const char* StateNameFromInt(int s) {
    switch (s) {
        case  0: return "IDLE";
        case  1: return "FOLLOW";
        case  2: return "STUCK";
        case  3: return "ENGAGE";
        case  4: return "ATTACK";
        case  5: return "CLIMBING";
        case  6: return "BLOCK";
        case  7: return "RANGED_ATTACK";
        case  8: return "STANDBY";
        case  9: return "COLLECT_ITEM";
    }
    return "UNKNOWN";
}

// Open a fresh recording file. Caller holds Mutex(). Filename mirrors the
// session's main log: "Ship of Harkinian <N> follower.log", same N as
// "Ship of Harkinian <N>.log" so a recording pairs visually with its main
// log. Falls back to a millisecond timestamp if N can't be discovered.
bool OpenRecording() {
    if (!EnsureDirectory()) return false;
    uint64_t now = NowMs();
    int n = FindCurrentSessionLogIndex();
    std::string fileName;
    if (n >= 0) {
        fileName = "Ship of Harkinian " + std::to_string(n) + " follower.log";
    } else {
        fileName = "Ship of Harkinian follower " + std::to_string(now) + ".log";
        SPDLOG_WARN("[FollowerRecorder] No main-log index found; falling back to "
                    "timestamp filename: {}", fileName);
    }
    gFilePath = (LogsDir() / fileName).string();
    gFile.open(gFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!gFile.is_open()) {
        SPDLOG_WARN("[FollowerRecorder] Failed to open {}", gFilePath);
        return false;
    }
    gStartMs         = now;
    gLastCaptureMs   = 0;
    gCaptureCount    = 0;
    gLinesSinceFlush = 0;
    gPendingEvents.clear();
    SPDLOG_INFO("[FollowerRecorder] Started recording to {}", gFilePath);
    return true;
}

// Caller holds Mutex().
void CloseRecording(const char* reason) {
    if (gFile.is_open()) {
        gFile.flush();
        gFile.close();
        SPDLOG_INFO("[FollowerRecorder] Stopped ({}); {} captures over {} ms; file: {}",
                    reason, gCaptureCount, NowMs() - gStartMs, gFilePath);
    }
    gFilePath.clear();
    gStartMs         = 0;
    gLastCaptureMs   = 0;
    gCaptureCount    = 0;
    gLinesSinceFlush = 0;
    gPendingEvents.clear();
}

int ClampHz(int hz) {
    if (hz < 1)   return 1;
    if (hz > 60)  return 60;
    return hz;
}

int ClampMaxSeconds(int s) {
    if (s < 1)     return 1;
    if (s > 3600)  return 3600;
    return s;
}

// Frame-tick maintenance hook. Manages activate/deactivate transitions and
// auto-stop on max-seconds timeout. Capture itself happens inside
// CaptureFrame from TickFollower; this hook only handles lifecycle.
void OnFrameTick() {
    bool cvarOn = CVarGetInteger(CVAR_FR_ENABLED, 0) != 0;
    std::lock_guard<std::mutex> lock(Mutex());

    if (cvarOn && !gActive) {
        if (OpenRecording()) {
            gActive = true;
        } else {
            // Open failure — flip CVar back so user sees the toggle revert
            // and the WARN above tells them why.
            CVarSetInteger(CVAR_FR_ENABLED, 0);
            CVarSave();
        }
        return;
    }

    if (!cvarOn && gActive) {
        CloseRecording("user-disabled");
        gActive = false;
        return;
    }

    if (gActive) {
        int maxSecs = ClampMaxSeconds(CVarGetInteger(CVAR_FR_MAX_SECONDS, 300));
        if ((NowMs() - gStartMs) >= static_cast<uint64_t>(maxSecs) * 1000ull) {
            CloseRecording("max-seconds reached");
            gActive = false;
            CVarSetInteger(CVAR_FR_ENABLED, 0);
            CVarSave();
        }
    }
}

} // namespace

bool IsRecorderActive() {
    std::lock_guard<std::mutex> lock(Mutex());
    return gActive;
}

void QueueRecorderEvent(std::string_view ev) {
    std::lock_guard<std::mutex> lock(Mutex());
    if (!gActive) return;
    gPendingEvents.emplace_back(ev);
}

void CaptureFrame(const FollowerFrameContext& ctx) {
    if (gPlayState == nullptr || ctx.player == nullptr) return;

    std::lock_guard<std::mutex> lock(Mutex());
    if (!gActive || !gFile.is_open()) return;

    // Hz throttle. CaptureHz is "captures per second" — convert to a
    // minimum-interval gate. CaptureFrame is called every game frame
    // (60 Hz typical) so this drops most calls.
    int hz = ClampHz(CVarGetInteger(CVAR_FR_CAPTURE_HZ, 15));
    uint64_t intervalMs = 1000ull / static_cast<uint64_t>(hz);
    uint64_t now = NowMs();
    if (gLastCaptureMs != 0 && (now - gLastCaptureMs) < intervalMs) return;
    gLastCaptureMs = now;

    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr) return;

    // FollowerFrameContext only carries `play` and `player` populated by
    // the OnGameFrameUpdate caller (Follower.cpp:3550-3552); leader fields
    // remain default-zero because TickFollower's body resolves the leader
    // via local lookups. Mirror that resolution here so the recorder gets
    // ground-truth data instead of capturing zeros.
    Actor* leaderActor = nullptr;
    if (anchor->followerLeaderClientId != 0) {
        Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (cand != nullptr) {
            if (cand->id == ACTOR_EN_OE2 &&
                anchor->GetDummyPlayerClientId(cand) == anchor->followerLeaderClientId) {
                leaderActor = cand;
                break;
            }
            cand = cand->next;
        }
    }
    Vec3f leaderPos = (leaderActor != nullptr) ? leaderActor->world.pos
                                                : Vec3f{ 0.0f, 0.0f, 0.0f };
    s8   leaderRoomLive = -1;
    bool leaderClimbing = false;
    bool leaderCrawling = false;
    if (anchor->followerLeaderClientId != 0) {
        auto it = anchor->clients.find(anchor->followerLeaderClientId);
        if (it != anchor->clients.end()) {
            leaderRoomLive = it->second.curRoomNum;
            leaderClimbing = it->second.isClimbing;
            leaderCrawling = it->second.isCrawling;
        }
    }
    s8   ourRoomLive   = (s8)gPlayState->roomCtx.curRoom.num;
    bool roomsDiffer   = (leaderRoomLive != -1 && ourRoomLive != -1 &&
                          leaderRoomLive != ourRoomLive);

    const Vec3f& fp = ctx.player->actor.world.pos;
    f32 distXZ  = (leaderActor != nullptr) ? AnchorDist::DistXZ(leaderPos, fp) : 0.0f;
    f32 distXYZ = (leaderActor != nullptr) ? AnchorDist::Dist3D(leaderPos, fp) : 0.0f;

    nlohmann::json j;
    j["schema"]            = FR_SCHEMA_VERSION;
    j["frame"]             = anchor->followerTickCounter;
    j["tMs"]               = static_cast<uint64_t>(now - gStartMs);
    j["scene"]             = static_cast<int>(gPlayState->sceneNum);
    j["room"]              = static_cast<int>(ourRoomLive);
    j["transitionTrigger"] = static_cast<int>(gPlayState->transitionTrigger);

    j["leaderClientId"] = static_cast<uint64_t>(anchor->followerLeaderClientId);
    j["leaderResolved"] = (leaderActor != nullptr) ? 1 : 0;
    j["leaderRoom"]     = static_cast<int>(leaderRoomLive);
    j["leaderClimbing"] = leaderClimbing ? 1 : 0;
    j["leaderCrawling"] = leaderCrawling ? 1 : 0;
    j["roomsDiffer"]    = roomsDiffer ? 1 : 0;
    j["leaderPos_f"]    = nlohmann::json::array({ leaderPos.x, leaderPos.y, leaderPos.z });

    j["followerPos_f"]       = nlohmann::json::array({ fp.x, fp.y, fp.z });
    j["followerYaw"]         = static_cast<int>(ctx.player->actor.shape.rot.y);
    j["followerStateFlags1"] = static_cast<uint64_t>(ctx.player->stateFlags1);
    j["followerState"]       = StateNameFromInt(static_cast<int>(anchor->followerAIState));
    j["stateFramesIn"]       = anchor->followerStateFrames;
    j["stuckFrames"]         = anchor->followerStuckFrames;

    j["distXZ_f"]  = distXZ;
    j["distXYZ_f"] = distXYZ;

    bool pathPresent = !anchor->followerNavPath.Empty();
    j["navPathPresent"] = pathPresent ? 1 : 0;
    j["navPathLen"]     = static_cast<uint64_t>(anchor->followerNavPath.waypoints.size());
    j["navPathCursor"]  = static_cast<uint64_t>(anchor->followerNavPath.cursorIdx);

    // Climb-surface diagnostics (climb_surface_nav_grid_plan post-Stage-8
    // investigation). subgoalFlags lets us see whether the current
    // waypoint sits on a climb-surface node — the trigger condition for
    // Stage 6's FOLLOW→CLIMBING engagement. computedClimbMask lets us
    // confirm the resolved permission mask reaches the consumer
    // correctly. climbNodesInPath counts climb-surface waypoints in the
    // remaining path so a "path goes through climb but cursor hasn't
    // reached one yet" case is visible.
    j["subgoalFlags"]      = pathPresent
        ? static_cast<uint64_t>(anchor->followerNavPath.CurrentSubgoalFlags())
        : (uint64_t)0;
    j["computedClimbMask"] = static_cast<uint64_t>(
        anchor->followerNavPath.computedClimbMask);
    if (pathPresent) {
        size_t climbCount = 0;
        uint32_t flagsAggregate = 0;  // OR of all remaining waypoint flags
        for (size_t i = anchor->followerNavPath.cursorIdx;
             i < anchor->followerNavPath.waypointFlags.size(); i++) {
            const uint32_t f = anchor->followerNavPath.waypointFlags[i];
            flagsAggregate |= f;
            if (f & ::AnchorNavRoom::NODE_CLIMB_ANY) {
                climbCount++;
            }
        }
        j["climbNodesInPath"] = static_cast<uint64_t>(climbCount);
        // Aggregate of remaining-waypoint flags. Tells us whether the
        // path INCLUDES drop / jump / climb intent anywhere downstream
        // of the cursor — independent of whether the current subgoal
        // happens to carry it. Critical for diagnosing "BFS routed
        // through anchor but follower stalled before reaching the
        // tagged waypoint" vs "BFS never used the anchor at all".
        j["pathFlagsAggregate"] = static_cast<uint64_t>(flagsAggregate);
        // Full waypoint dump (positions + per-waypoint flags). High-fidelity
        // diagnostic for path-routing investigations. ~30-100 bytes per
        // waypoint × N-waypoint paths × capture frequency. Worst-case
        // 64-waypoint path × 30Hz × 100 bytes = ~200KB/s of recorder data;
        // tolerable for diagnostic sessions.
        nlohmann::json wpts = nlohmann::json::array();
        for (size_t i = 0; i < anchor->followerNavPath.waypoints.size(); i++) {
            const Vec3f& p = anchor->followerNavPath.waypoints[i];
            uint32_t f = (i < anchor->followerNavPath.waypointFlags.size())
                ? anchor->followerNavPath.waypointFlags[i] : (uint32_t)0;
            wpts.push_back({
                {"i", static_cast<uint64_t>(i)},
                {"x", p.x}, {"y", p.y}, {"z", p.z},
                {"f", static_cast<uint64_t>(f)},
            });
        }
        j["navPathWaypoints"] = wpts;
    } else {
        j["climbNodesInPath"]   = (uint64_t)0;
        j["pathFlagsAggregate"] = (uint64_t)0;
        j["navPathWaypoints"]   = nlohmann::json::array();
    }

    j["g10LeashFrames"]    = anchor->followerOverrunFrames;
    j["g14CloseFailFrames"] = anchor->followerCloseFailFrames;
    j["g15HangFrames"]     = anchor->followerHangFrames;
    j["g12StuckCycles"]    = anchor->followerStuckCycleCount;
    j["postTeleportHold"]  = anchor->followerPostTeleportFrames;
    j["autonomousClimb"]   = anchor->followerAutonomousClimb ? 1 : 0;

    j["navEnabled"]        = CVarGetInteger(AnchorNavCVars::kEnabled, 0) != 0 ? 1 : 0;
    j["consumerEnabled"]   = CVarGetInteger(AnchorNavCVars::kAiFollowerConsumer, 0) != 0 ? 1 : 0;

    if (gPendingEvents.empty()) {
        j["events"] = nlohmann::json::array();
    } else {
        j["events"] = gPendingEvents;
        gPendingEvents.clear();
    }

    gFile << j.dump() << '\n';
    ++gCaptureCount;
    if (++gLinesSinceFlush >= kFlushEveryNLines) {
        gFile.flush();
        gLinesSinceFlush = 0;
    }
}

void RegisterFollowerRecorder() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrameTick);
}

} // namespace AnchorFollower

static RegisterShipInitFunc registerFollowerRecorder(AnchorFollower::RegisterFollowerRecorder);
