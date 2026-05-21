/**
 * Player Diagnostic Recorder — implementation.
 *
 * Mirrors FollowerRecorder's structure (lifecycle, file rotation,
 * JSONL output) but captures the LOCAL player (Link) instead of the
 * AI follower. Used to analyse manual navigation paths through
 * difficult geometry so the follower's logic can be improved to
 * match.
 */

#include "PlayerRecorder.h"
#include "soh/cvar_prefixes.h"
#include "soh/ShipInit.hpp"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

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
#include "macros.h"
extern PlayState* gPlayState;
}

#define CVAR_PR_ENABLED      CVAR_DEVELOPER_TOOLS("PlayerRecorder.Enabled")
#define CVAR_PR_CAPTURE_HZ   CVAR_DEVELOPER_TOOLS("PlayerRecorder.CaptureHz")
#define CVAR_PR_MAX_SECONDS  CVAR_DEVELOPER_TOOLS("PlayerRecorder.MaxSeconds")

// Schema versions:
//   v1 — initial release (frame, pos, rot, speedXZ, stick + button input,
//        full state1/2/3 flag bitmaps + decoded climb / ledge / crawl /
//        damage / talk / cutscene / input-disabled booleans, scene + room).
#define PR_SCHEMA_VERSION 1

namespace AnchorFollower {

namespace {

namespace fs = std::filesystem;

std::mutex& Mutex() {
    static std::mutex m;
    return m;
}

std::ofstream gFile;
std::string   gFilePath;
uint64_t      gStartMs         = 0;
uint64_t      gLastCaptureMs   = 0;
int           gCaptureCount    = 0;
int           gLinesSinceFlush = 0;
bool          gActive          = false;

constexpr int kFlushEveryNLines = 60;

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

fs::path LogsDir() {
    return fs::path(Ship::Context::GetAppDirectoryPath()) / "logs";
}

bool EnsureDirectory() {
    std::error_code ec;
    if (fs::exists(LogsDir(), ec)) return true;
    fs::create_directories(LogsDir(), ec);
    if (ec) {
        SPDLOG_WARN("[PlayerRecorder] EnsureDirectory failed: {}", ec.message());
        return false;
    }
    return true;
}

// Mirror FollowerRecorder's session-index discovery so player and
// follower recordings share the same N as the main log.
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
        if (!std::all_of(middle.begin(), middle.end(), ::isdigit)) continue;
        try {
            int n = std::stoi(middle);
            if (n > maxN) maxN = n;
        } catch (...) { /* malformed; skip */ }
    }
    return maxN;
}

bool OpenRecording() {
    if (!EnsureDirectory()) return false;
    uint64_t now = NowMs();
    int n = FindCurrentSessionLogIndex();
    std::string fileName;
    if (n >= 0) {
        fileName = "Ship of Harkinian " + std::to_string(n) + " player.log";
    } else {
        fileName = "Ship of Harkinian player " + std::to_string(now) + ".log";
        SPDLOG_WARN("[PlayerRecorder] No main-log index found; falling back to "
                    "timestamp filename: {}", fileName);
    }
    gFilePath = (LogsDir() / fileName).string();
    gFile.open(gFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!gFile.is_open()) {
        SPDLOG_WARN("[PlayerRecorder] Failed to open {}", gFilePath);
        return false;
    }
    gStartMs         = now;
    gLastCaptureMs   = 0;
    gCaptureCount    = 0;
    gLinesSinceFlush = 0;
    SPDLOG_INFO("[PlayerRecorder] Started recording to {}", gFilePath);
    return true;
}

void CloseRecording(const char* reason) {
    if (gFile.is_open()) {
        gFile.flush();
        gFile.close();
        SPDLOG_INFO("[PlayerRecorder] Stopped ({}); {} captures over {} ms; file: {}",
                    reason, gCaptureCount, NowMs() - gStartMs, gFilePath);
    }
    gFilePath.clear();
    gStartMs         = 0;
    gLastCaptureMs   = 0;
    gCaptureCount    = 0;
    gLinesSinceFlush = 0;
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

// Capture one frame of player state to JSONL. Returns silently if not
// active or if no player actor exists yet (title screen / loading).
void CaptureFrame() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (!gActive || !gFile.is_open()) return;
    if (gPlayState == nullptr) return;

    Player* player = (Player*)GET_PLAYER(gPlayState);
    if (player == nullptr) return;

    int hz = ClampHz(CVarGetInteger(CVAR_PR_CAPTURE_HZ, 15));
    uint64_t intervalMs = 1000ull / static_cast<uint64_t>(hz);
    uint64_t now = NowMs();
    if (gLastCaptureMs != 0 && (now - gLastCaptureMs) < intervalMs) return;
    gLastCaptureMs = now;

    nlohmann::json j;
    j["schema"]   = PR_SCHEMA_VERSION;
    j["frame"]    = (int64_t)gPlayState->state.frames;
    j["tMs"]      = (int64_t)(now - gStartMs);
    j["scene"]    = (int16_t)gPlayState->sceneNum;
    j["room"]     = (int8_t)gPlayState->roomCtx.curRoom.num;

    const Vec3f& p = player->actor.world.pos;
    j["pos"] = nlohmann::json::array({ p.x, p.y, p.z });

    const Vec3s& sr = player->actor.shape.rot;
    j["shape_rot"] = nlohmann::json::array({ (int)sr.x, (int)sr.y, (int)sr.z });

    const Vec3s& wr = player->actor.world.rot;
    j["world_rot"] = nlohmann::json::array({ (int)wr.x, (int)wr.y, (int)wr.z });

    j["speedXZ"] = (float)player->actor.speedXZ;

    // Input. play->state.input[0] is the local player's controller.
    Input* in = &gPlayState->state.input[0];
    j["stick_x"]      = (int)in->cur.stick_x;
    j["stick_y"]      = (int)in->cur.stick_y;
    j["button"]       = (uint32_t)in->cur.button;
    j["button_press"] = (uint32_t)in->press.button;

    // Full state-flag bitmaps for diff inspection.
    j["state1"] = (uint32_t)player->stateFlags1;
    j["state2"] = (uint32_t)player->stateFlags2;
    j["state3"] = (uint32_t)player->stateFlags3;

    // Decoded booleans for the navigation-relevant states.
    j["onLadder"]     = (player->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER)  ? 1 : 0;
    j["hangingLedge"] = (player->stateFlags1 & PLAYER_STATE1_HANGING_OFF_LEDGE)? 1 : 0;
    j["climbingLedge"]= (player->stateFlags1 & PLAYER_STATE1_CLIMBING_LEDGE)   ? 1 : 0;
    j["crawling"]     = (player->stateFlags2 & PLAYER_STATE2_CRAWLING)         ? 1 : 0;
    j["damaged"]      = (player->stateFlags1 & PLAYER_STATE1_DAMAGED)          ? 1 : 0;
    j["talking"]      = (player->stateFlags1 & PLAYER_STATE1_TALKING)          ? 1 : 0;
    j["inCutscene"]   = (player->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE)      ? 1 : 0;
    j["inputDisabled"]= (player->stateFlags1 & PLAYER_STATE1_INPUT_DISABLED)   ? 1 : 0;

    // actionFunc as an opaque pointer-sized identifier. Lets the user
    // see "actionFunc changed" via address-diff between frames without
    // requiring debug-name lookup tables (which OoT doesn't ship in
    // release builds).
    uintptr_t actionFunc = reinterpret_cast<uintptr_t>(player->actionFunc);
    j["actionFunc"] = (uint64_t)actionFunc;

    gFile << j.dump() << '\n';
    ++gCaptureCount;
    if (++gLinesSinceFlush >= kFlushEveryNLines) {
        gFile.flush();
        gLinesSinceFlush = 0;
    }
}

// Frame-tick driver. Manages lifecycle (start/stop on CVar flip,
// auto-stop on max-seconds) and invokes CaptureFrame.
void OnFrameTick() {
    bool cvarOn = CVarGetInteger(CVAR_PR_ENABLED, 0) != 0;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        if (cvarOn && !gActive) {
            if (OpenRecording()) {
                gActive = true;
            } else {
                CVarSetInteger(CVAR_PR_ENABLED, 0);
                CVarSave();
            }
        } else if (!cvarOn && gActive) {
            CloseRecording("user-disabled");
            gActive = false;
        } else if (gActive) {
            int maxSecs = ClampMaxSeconds(CVarGetInteger(CVAR_PR_MAX_SECONDS, 300));
            if ((NowMs() - gStartMs) >= static_cast<uint64_t>(maxSecs) * 1000ull) {
                CloseRecording("max-seconds reached");
                gActive = false;
                CVarSetInteger(CVAR_PR_ENABLED, 0);
                CVarSave();
            }
        }
    }
    CaptureFrame();
}

} // namespace

bool IsPlayerRecorderActive() {
    std::lock_guard<std::mutex> lock(Mutex());
    return gActive;
}

void RegisterPlayerRecorder() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrameTick);
}

} // namespace AnchorFollower

static RegisterShipInitFunc registerPlayerRecorder(AnchorFollower::RegisterPlayerRecorder);
