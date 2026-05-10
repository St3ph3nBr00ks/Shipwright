/**
 * AI Follower Diagnostic Recorder — implementation.
 *
 * Spec: Claude/Plans/follower_recorder_plan.md.
 */

#include "FollowerRecorder.h"
#include "Follower.h"
#include "../Anchor.h"
#include "soh/cvar_prefixes.h"
#include "soh/ShipInit.hpp"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

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

#define FR_SCHEMA_VERSION 1

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

fs::path RecordingsDir() {
    return fs::path("roommanifests") / "follower_recordings";
}

bool EnsureDirectory() {
    std::error_code ec;
    if (fs::exists(RecordingsDir(), ec)) return true;
    fs::create_directories(RecordingsDir(), ec);
    if (ec) {
        SPDLOG_WARN("[FollowerRecorder] EnsureDirectory failed: {}", ec.message());
        return false;
    }
    return true;
}

// Map followerAIState's underlying integer to a name. Lookup is by integer
// because Anchor::FollowerAIState is a PRIVATE nested enum — friend access is
// granted only to CaptureFrame, not to free helpers in this anonymous
// namespace. CaptureFrame casts the enum to int and passes it here; the
// integer values match the enum declaration order in Anchor.h:515.
const char* StateNameFromInt(int s) {
    switch (s) {
        case  0: return "IDLE";
        case  1: return "FOLLOW";
        case  2: return "STUCK";
        case  3: return "ENGAGE";
        case  4: return "ATTACK";
        case  5: return "RETURN";
        case  6: return "CLIMBING";
        case  7: return "BLOCK";
        case  8: return "RANGED_ATTACK";
        case  9: return "STANDBY";
        case 10: return "COLLECT_ITEM";
    }
    return "UNKNOWN";
}

// Open a fresh recording file. Caller holds Mutex(). Filename uses ms-since-
// epoch — unique per session, no platform-specific time formatting needed.
bool OpenRecording() {
    if (!EnsureDirectory()) return false;
    uint64_t now = NowMs();
    gFilePath = (RecordingsDir() / ("follower_" + std::to_string(now) + ".jsonl")).string();
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

    nlohmann::json j;
    j["schema"]            = FR_SCHEMA_VERSION;
    j["frame"]             = anchor->followerTickCounter;
    j["tMs"]               = static_cast<uint64_t>(now - gStartMs);
    j["scene"]             = static_cast<int>(gPlayState->sceneNum);
    j["room"]              = static_cast<int>(gPlayState->roomCtx.curRoom.num);
    j["transitionTrigger"] = static_cast<int>(gPlayState->transitionTrigger);

    j["leaderClientId"] = static_cast<uint64_t>(anchor->followerLeaderClientId);
    j["leaderRoom"]     = static_cast<int>(ctx.leaderRoom);
    j["leaderClimbing"] = ctx.leaderIsClimbing ? 1 : 0;
    j["leaderCrawling"] = ctx.leaderIsCrawling ? 1 : 0;
    j["roomsDiffer"]    = ctx.roomsDiffer ? 1 : 0;
    j["leaderPos_f"]    = nlohmann::json::array({ ctx.leaderPos.x, ctx.leaderPos.y, ctx.leaderPos.z });

    const Vec3f& fp = ctx.player->actor.world.pos;
    j["followerPos_f"]       = nlohmann::json::array({ fp.x, fp.y, fp.z });
    j["followerYaw"]         = static_cast<int>(ctx.player->actor.shape.rot.y);
    j["followerStateFlags1"] = static_cast<uint64_t>(ctx.player->stateFlags1);
    j["followerState"]       = StateNameFromInt(static_cast<int>(anchor->followerAIState));
    j["stateFramesIn"]       = anchor->followerStateFrames;
    j["stuckFrames"]         = anchor->followerStuckFrames;

    j["distXZ_f"]  = ctx.distToLeaderXZ;
    j["distXYZ_f"] = std::sqrt(ctx.distToLeaderSq);

    bool pathPresent = !anchor->followerNavPath.Empty();
    j["navPathPresent"] = pathPresent ? 1 : 0;
    j["navPathLen"]     = static_cast<uint64_t>(anchor->followerNavPath.waypoints.size());
    j["navPathCursor"]  = static_cast<uint64_t>(anchor->followerNavPath.cursorIdx);

    j["g10LeashFrames"]    = anchor->followerOverrunFrames;
    j["g14CloseFailFrames"] = anchor->followerCloseFailFrames;
    j["g15HangFrames"]     = anchor->followerHangFrames;
    j["g12StuckCycles"]    = anchor->followerStuckCycleCount;
    j["postTeleportHold"]  = anchor->followerPostTeleportFrames;
    j["autonomousClimb"]   = anchor->followerAutonomousClimb ? 1 : 0;

    j["navEnabled"]        = CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0) != 0 ? 1 : 0;
    j["consumerEnabled"]   = CVarGetInteger(CVAR_ENHANCEMENT("Nav.AiFollowerConsumer"), 0) != 0 ? 1 : 0;

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
