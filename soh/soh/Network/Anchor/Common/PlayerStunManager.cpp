/**
 * PlayerStunManager — implementation. See PlayerStunManager.h for
 * scope + design rationale (GH #333 A1-A15).
 *
 * Phase 4a: local state + local player input suppression + escape
 * counter. No wire broadcast yet — ApplyStun / ClearStun update local
 * map only. Phase 4b wires PLAYER_STUN_APPLIED / _CLEARED packets;
 * this file's function bodies will be augmented (not restructured)
 * to call SendPacket_* alongside the local write.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates
// land in C++ linkage before overlay headers open extern "C" blocks.
#include "soh/Network/Anchor/Anchor.h"

#include "PlayerStunManager.h"
// Anchor.h (included above) exposes Anchor::Instance->GetDummyPlayerClientId
// — no separate DummyPlayer header exists (DummyPlayer.cpp uses forward-
// decls in Anchor.h).
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace AnchorPlayerStun {

namespace {

// Sidecar map — per-client stun state.
std::unordered_map<uint32_t, StunEntry> sStunStates;

// Wall-clock ms helper (matches session_state.md project convention
// for absolute-time comparisons — vs game ticks which vary with
// framerate).
uint64_t NowMs(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Cardinal from stick x/y (0=neutral, 1=up, 2=right, 3=down, 4=left).
// Deadzone check first — if inside deadzone, neutral. Otherwise pick
// the dominant axis.
uint8_t CardinalFromStick(int8_t sx, int8_t sy) {
    const int ax = (sx < 0) ? -sx : sx;
    const int ay = (sy < 0) ? -sy : sy;
    if (ax < kStickDeadzone && ay < kStickDeadzone) return 0;
    if (ay >= ax) {
        return (sy > 0) ? 1 : 3;  // N64 convention: +y = up
    }
    return (sx > 0) ? 2 : 4;
}

// Resolve an Actor* to a clientId (own or DummyPlayer). Returns
// UINT32_MAX if not a player-family actor. Uses Anchor helpers.
uint32_t ResolveActorToClientId(Actor* actor) {
    if (actor == nullptr || Anchor::Instance == nullptr) return UINT32_MAX;
    // Local Link path
    if (gPlayState != nullptr && GET_PLAYER(gPlayState) == (Player*)actor) {
        return Anchor::Instance->ownClientId;
    }
    // DummyPlayer path — Anchor::GetDummyPlayerClientId returns 0 for
    // non-DummyPlayer, so we distinguish via a probe on actor category.
    // Direct approach: DummyPlayer actors are ACTOR_EN_OE2 with our
    // Update func; use the Anchor helper.
    const uint32_t maybe = Anchor::Instance->GetDummyPlayerClientId(actor);
    if (maybe != 0) return maybe;
    return UINT32_MAX;
}

// Purge escape-event timestamps older than the rolling window.
void PurgeStaleEscapePresses(StunEntry& entry, uint64_t nowMs) {
    while (!entry.escapePressesMs.empty() &&
           entry.escapePressesMs.front() + kEscapeWindowMs < nowMs) {
        entry.escapePressesMs.pop_front();
    }
}

// Push a new escape event timestamp; return true if threshold met.
bool RecordEscapeEvent(StunEntry& entry, uint64_t nowMs) {
    entry.escapePressesMs.push_back(nowMs);
    // Cap deque size defensively (in case >threshold arrive in one
    // frame from a chording jitter).
    while (entry.escapePressesMs.size() > (size_t)(kEscapeThreshold * 2)) {
        entry.escapePressesMs.pop_front();
    }
    return entry.escapePressesMs.size() >= (size_t)kEscapeThreshold;
}

}  // namespace

// ---- Public API --------------------------------------------------------

bool IsClientStunned(uint32_t clientId) {
    return sStunStates.find(clientId) != sStunStates.end();
}

bool IsActorStunned(Actor* playerActor) {
    const uint32_t cid = ResolveActorToClientId(playerActor);
    if (cid == UINT32_MAX) return false;
    return IsClientStunned(cid);
}

void ApplyStun(uint32_t clientId, uint32_t sourceEnSwNetId) {
    // Idempotent (A15): if already stunned, no-op. New source spider
    // does NOT refresh the timer or override sourceNetId.
    if (IsClientStunned(clientId)) {
        SPDLOG_INFO("[PlayerStun] ApplyStun clientId={} src={} — already stunned, no-op",
                    clientId, sourceEnSwNetId);
        return;
    }

    StunEntry entry;
    entry.sourceEnSwNetId = sourceEnSwNetId;
    entry.appliedAtMs     = NowMs();
    sStunStates[clientId] = entry;

    SPDLOG_INFO("[PlayerStun] ApplyStun clientId={} src={} appliedAtMs={}",
                clientId, sourceEnSwNetId, entry.appliedAtMs);

    // Phase 4b will add: SendPacket_PlayerStunApplied(clientId, sourceEnSwNetId);
    // when called on host with own or peer as victim.
}

void ClearStun(uint32_t clientId, ClearReason reason) {
    auto it = sStunStates.find(clientId);
    if (it == sStunStates.end()) return;  // Idempotent — no-op

    SPDLOG_INFO("[PlayerStun] ClearStun clientId={} reason={} heldMs={}",
                clientId, (int)reason, NowMs() - it->second.appliedAtMs);

    sStunStates.erase(it);

    // Phase 4b will add: SendPacket_PlayerStunCleared(clientId, reason);
    // when called on the ownership-appropriate client (per A13 matrix).
}

void OnSceneInit(void) {
    if (!sStunStates.empty()) {
        SPDLOG_INFO("[PlayerStun] OnSceneInit — clearing {} stunned client(s)",
                    sStunStates.size());
    }
    sStunStates.clear();
}

void OnClientDisconnect(uint32_t clientId) {
    ClearStun(clientId, ClearReason::Disconnect);
}

void Tick(PlayState* play) {
    if (Anchor::Instance == nullptr) return;
    if (sStunStates.empty()) return;  // Common case — early bail

    const uint64_t nowMs = NowMs();
    const uint32_t localClientId = Anchor::Instance->ownClientId;

    // 10s hard-cap sweep. Collect victims first to avoid iterator
    // invalidation from ClearStun's erase.
    std::vector<uint32_t> toClearTimeout;
    for (auto& kv : sStunStates) {
        if (nowMs - kv.second.appliedAtMs >= kStunTimeoutMs) {
            toClearTimeout.push_back(kv.first);
        }
    }
    for (uint32_t cid : toClearTimeout) {
        ClearStun(cid, ClearReason::Timeout);
    }

    // Cutscene / scene-change local clear (A10) — applies to the
    // LOCAL player only. Each client independently detects its own
    // cutscene entry / scene change.
    if (IsClientStunned(localClientId) && play != nullptr) {
        if (Play_InCsMode(play)) {
            ClearStun(localClientId, ClearReason::Cutscene);
        }
    }

    // Local input tracking + escape-event detection. Only applies
    // when the LOCAL player is stunned.
    auto it = sStunStates.find(localClientId);
    if (it == sStunStates.end()) return;
    if (play == nullptr) return;

    StunEntry& entry = it->second;
    PurgeStaleEscapePresses(entry, nowMs);

    // Read current controller state. play->state.input[0] is the
    // primary controller pre-consumed by Player.
    const OSContPad& cur = play->state.input[0].cur;

    const uint8_t curA = (cur.button & BTN_A) ? 1 : 0;
    const uint8_t curB = (cur.button & BTN_B) ? 1 : 0;
    const int8_t  curSX = cur.stick_x;
    const int8_t  curSY = cur.stick_y;
    const uint8_t curCard = CardinalFromStick(curSX, curSY);

    // A press: released → pressed edge
    if (curA == 1 && entry.prevAButton == 0) {
        if (RecordEscapeEvent(entry, nowMs)) {
            ClearStun(localClientId, ClearReason::Self);
            return;  // entry is now invalid
        }
    }
    // B press: same
    if (curB == 1 && entry.prevBButton == 0) {
        if (RecordEscapeEvent(entry, nowMs)) {
            ClearStun(localClientId, ClearReason::Self);
            return;
        }
    }
    // Stick release-to-deflect: prev was neutral, cur is deflected
    if (entry.prevCardinal == 0 && curCard != 0) {
        if (RecordEscapeEvent(entry, nowMs)) {
            ClearStun(localClientId, ClearReason::Self);
            return;
        }
    }
    // Cardinal change: prev was deflected, cur is a DIFFERENT
    // deflection (up→down, left→right, etc.)
    else if (entry.prevCardinal != 0 && curCard != 0 &&
             curCard != entry.prevCardinal) {
        if (RecordEscapeEvent(entry, nowMs)) {
            ClearStun(localClientId, ClearReason::Self);
            return;
        }
    }

    entry.prevAButton  = curA;
    entry.prevBButton  = curB;
    entry.prevStickX   = curSX;
    entry.prevStickY   = curSY;
    entry.prevCardinal = curCard;
}

}  // namespace AnchorPlayerStun

// ---- C-linkage bridge --------------------------------------------------

extern "C" int Anchor_PlayerStun_IsActorStunned(Actor* playerActor) {
    return AnchorPlayerStun::IsActorStunned(playerActor) ? 1 : 0;
}

extern "C" int Anchor_PlayerStun_IsClientStunned(uint32_t clientId) {
    return AnchorPlayerStun::IsClientStunned(clientId) ? 1 : 0;
}

extern "C" void Anchor_PlayerStun_ApplyStun(uint32_t victimClientId,
                                              uint32_t sourceEnSwNetId) {
    AnchorPlayerStun::ApplyStun(victimClientId, sourceEnSwNetId);
}

extern "C" void Anchor_PlayerStun_ClearStun(uint32_t victimClientId,
                                              uint8_t reason) {
    AnchorPlayerStun::ClearStun(victimClientId,
                                  (AnchorPlayerStun::ClearReason)reason);
}
