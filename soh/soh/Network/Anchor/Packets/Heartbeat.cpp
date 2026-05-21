#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

/**
 * HEARTBEAT — every client → all clients. Two-axis liveness signal.
 *
 * Why this exists (#194 follow-up):
 *   In log 301, P1 (host) got stuck in a hintnut dialog. The game thread
 *   stopped advancing but the relay's connection to P1 was still open.
 *   After ~40 s the relay's read-deadline expired and the host got
 *   force-disconnected. Peers had no signal during the 40 s window —
 *   they saw stale state with no way to distinguish "host frozen" from
 *   "everything fine, host just isn't sending updates this tick".
 *
 * Architecture:
 *   - Sent every kHeartbeatTxIntervalSec (2 s) from the NETWORK THREAD,
 *     not the game thread. TickHeartbeat() is called from
 *     ProcessOutgoingPackets which runs on Network::ReceiveFromServer's
 *     tight loop. So even when the game thread is fully frozen, the
 *     heartbeat keeps firing — keeping the relay's read-deadline alive
 *     AND letting peers see "client connection healthy".
 *
 *   - Carries gameFrameCounter — incremented from OnGameFrameUpdate on
 *     the GAME THREAD. If the game thread is frozen, the counter stops
 *     advancing. Peers receive heartbeats with the same gameFrameCounter
 *     for several ticks → they detect "game frozen but connected".
 *
 *   - Receive fast-path runs on the network thread directly (in
 *     OnIncomingJson), NOT through the standard incomingPacketQueue.
 *     This way a peer's frozen game thread doesn't delay updating its
 *     view of OTHER peers' liveness — the maps are written from the
 *     network thread regardless of whether the game thread is ticking.
 *
 * Detection thresholds:
 *   - IsClientLikelyFrozen: no heartbeat received for >5 s.
 *     Connection-level — peer disconnected or relay broke.
 *   - IsClientGameFrozen: heartbeats arriving but gameFrameCounter
 *     hasn't advanced in >3 s. Game-thread-level — peer frozen on a
 *     textbox, cutscene, pause, etc.
 *
 * Both getters return false for the local client (we cannot observe
 * our own freeze from inside the freeze) and for unknown clientIds.
 *
 * Future consumers (not implemented here, just on-the-record):
 *   - #191 voting-skip quorum exclusion: skip frozen peers in the
 *     all-pressed quorum so the timer doesn't have to elapse.
 *   - AI Player Follower (#169): surface "leader frozen" state instead of
 *     escalating through stuck-cycle / leash-timeout safety nets.
 *   - UI banner: "P3 may be frozen".
 *
 * Wire fields:
 *   sendTimeMs:        std::chrono::steady_clock at network-thread
 *                      send time, in milliseconds. Currently unused
 *                      by receivers (we use local-clock now() at rx
 *                      time instead — clock skew between hosts means
 *                      sender clocks can't be trusted directly).
 *                      Kept on-the-wire for future debugging /
 *                      latency-profiling use.
 *   gameFrameCounter:  monotonic counter, see above.
 *   schema:            1.
 *   quiet:             true (suppresses relay-side logging).
 */

void Anchor::SendPacket_Heartbeat() {
    nlohmann::json payload;
    payload["type"]             = HEARTBEAT;
    payload["quiet"]            = true;
    payload["gameFrameCounter"] = (uint64_t)gameFrameCounter.load(std::memory_order_relaxed);
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count();
    payload["sendTimeMs"]       = (int64_t)nowMs;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_Heartbeat(nlohmann::json payload) {
    // Network-thread fast-path. Self-check: ignore our own heartbeats
    // even though the relay shouldn't echo them.
    uint32_t clientId = payload.value("clientId", (uint32_t)0);
    if (clientId == 0 || clientId == ownClientId) {
        return;
    }

    auto     now             = std::chrono::steady_clock::now();
    uint64_t newGameFrame    = (uint64_t)payload.value("gameFrameCounter", (uint64_t)0);

    std::lock_guard<std::mutex> lock(heartbeatMutex);
    lastHeartbeatRxByClientId[clientId] = now;

    auto itGameFrame = lastHeartbeatGameFrameByClientId.find(clientId);
    if (itGameFrame == lastHeartbeatGameFrameByClientId.end() ||
        itGameFrame->second != newGameFrame) {
        // Game frame counter advanced (or first heartbeat) → record the
        // new value and the rx-time it advanced AT. While the counter
        // stays the same across multiple heartbeats we leave the
        // timestamp untouched — that's how IsClientGameFrozen detects
        // "game frozen for >3 s".
        lastHeartbeatGameFrameByClientId[clientId]   = newGameFrame;
        lastHeartbeatGameFrameAtByClientId[clientId] = now;
    }
}

void Anchor::TickHeartbeat() {
    if (!isConnected) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - lastHeartbeatTx).count();
    if (elapsed < kHeartbeatTxIntervalSec) {
        return;
    }
    lastHeartbeatTx = now;
    SendPacket_Heartbeat();
}

bool Anchor::IsClientLikelyFrozen(uint32_t clientId) {
    if (clientId == 0 || clientId == ownClientId) {
        return false;
    }
    std::chrono::steady_clock::time_point lastRx;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        auto it = lastHeartbeatRxByClientId.find(clientId);
        if (it == lastHeartbeatRxByClientId.end()) {
            return false;  // never received a heartbeat from this client
        }
        lastRx = it->second;
    }
    auto elapsed = std::chrono::duration<float>(
                       std::chrono::steady_clock::now() - lastRx).count();
    bool frozen = elapsed > kClientFrozenThresholdSec;

    // Edge-log: log once on transition into / out of frozen state to
    // surface the change without per-tick spam.
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        bool wasFlagged = heartbeatFrozenFlaggedByClientId[clientId];
        if (frozen && !wasFlagged) {
            SPDLOG_WARN("[Heartbeat] Client {} likely frozen — no heartbeat for {:.1f}s",
                        clientId, elapsed);
            heartbeatFrozenFlaggedByClientId[clientId] = true;
        } else if (!frozen && wasFlagged) {
            SPDLOG_INFO("[Heartbeat] Client {} recovered — heartbeat flowing again",
                        clientId);
            heartbeatFrozenFlaggedByClientId[clientId] = false;
        }
    }
    return frozen;
}

bool Anchor::IsClientGameFrozen(uint32_t clientId) {
    if (clientId == 0 || clientId == ownClientId) {
        return false;
    }
    // If the connection itself is dead, "game frozen" is not a useful
    // distinct signal — the peer can't update either state.
    if (IsClientLikelyFrozen(clientId)) {
        return false;
    }
    std::chrono::steady_clock::time_point lastFrameAt;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        auto it = lastHeartbeatGameFrameAtByClientId.find(clientId);
        if (it == lastHeartbeatGameFrameAtByClientId.end()) {
            return false;
        }
        lastFrameAt = it->second;
    }
    auto elapsed = std::chrono::duration<float>(
                       std::chrono::steady_clock::now() - lastFrameAt).count();
    bool gameFrozen = elapsed > kClientGameFrozenThresholdSec;

    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        bool wasFlagged = heartbeatGameFrozenFlaggedByClientId[clientId];
        if (gameFrozen && !wasFlagged) {
            SPDLOG_WARN("[Heartbeat] Client {} game thread frozen — counter stale for {:.1f}s "
                        "(connection still alive)", clientId, elapsed);
            heartbeatGameFrozenFlaggedByClientId[clientId] = true;
        } else if (!gameFrozen && wasFlagged) {
            SPDLOG_INFO("[Heartbeat] Client {} game thread recovered", clientId);
            heartbeatGameFrozenFlaggedByClientId[clientId] = false;
        }
    }
    return gameFrozen;
}
