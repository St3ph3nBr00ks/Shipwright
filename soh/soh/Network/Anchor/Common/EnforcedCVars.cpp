#include "EnforcedCVars.h"
#include "EnforcedCVarRegistry.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <cstring>

namespace AnchorCVarSync {

static const Anchor* GetActiveAnchor() {
    // Gate on isConnected (not isEnabled): between Enable() being
    // requested and the relay's HANDSHAKE-reply UPDATE_ROOM_STATE
    // arriving, the enforced maps are still empty and the wrapper
    // would return local-default fallback. Gating on isConnected
    // closes a brief window between Enable() and the maps being
    // populated by the relay's snapshot reply.
    if (Anchor::Instance == nullptr) return nullptr;
    if (!Anchor::Instance->isEnabled) return nullptr;
    if (!Anchor::Instance->isConnected) return nullptr;
    return Anchor::Instance;
}

int32_t GetEnforcedInt(const char* cvarName, int32_t localDefault) {
    const Anchor* anchor = GetActiveAnchor();
    if (anchor == nullptr) return CVarGetInteger(cvarName, localDefault);
    const auto& map = anchor->roomState.enforcedInts;
    auto it = map.find(cvarName);
    if (it != map.end()) return it->second;
    // Unknown CVar (not in the host's broadcast) — fall back to local
    // so non-enforced reads behave like vanilla.
    return CVarGetInteger(cvarName, localDefault);
}

float GetEnforcedFloat(const char* cvarName, float localDefault) {
    const Anchor* anchor = GetActiveAnchor();
    if (anchor == nullptr) return CVarGetFloat(cvarName, localDefault);
    const auto& map = anchor->roomState.enforcedFloats;
    auto it = map.find(cvarName);
    if (it != map.end()) return it->second;
    return CVarGetFloat(cvarName, localDefault);
}

bool IsEnforced(const char* cvarName) {
    if (GetActiveAnchor() == nullptr) return false;
    for (const auto& e : kEnforcedInts) {
        if (std::strcmp(e.cvarName, cvarName) == 0) return true;
    }
    for (const auto& e : kEnforcedFloats) {
        if (std::strcmp(e.cvarName, cvarName) == 0) return true;
    }
    return false;
}

void OnCvarsReceivedDispatch() {
    // Re-fire RegisterShipInitFunc listeners attached to every enforced
    // CVar name. COND_HOOK / COND_VB_SHOULD blocks inside the
    // registered functions re-evaluate against the wrapper's new
    // return value (i.e. host's authoritative value just deserialised
    // into roomState.enforced{Ints,Floats}).
    for (const auto& e : kEnforcedInts) {
        ShipInit::Init(e.cvarName);
    }
    for (const auto& e : kEnforcedFloats) {
        ShipInit::Init(e.cvarName);
    }
}

}  // namespace AnchorCVarSync

// extern "C" shims for C-decomp consumers.
extern "C" int32_t Anchor_GetEnforcedInt(const char* cvarName, int32_t localDefault) {
    return AnchorCVarSync::GetEnforcedInt(cvarName, localDefault);
}

extern "C" float Anchor_GetEnforcedFloat(const char* cvarName, float localDefault) {
    return AnchorCVarSync::GetEnforcedFloat(cvarName, localDefault);
}

// ============================================================================
// Owner-side auto-broadcast of local CVar changes
// ============================================================================
//
// HANDSHAKE-time PrepRoomState primes the relay with the owner's local
// values on first connect. Beyond that, the relay only learns about
// changes when SendPacket_UpdateRoomState fires — admin pane / Host
// Settings widget callbacks fire it immediately via
// TriggerOwnerBroadcastNow(); the auto-poll below catches console set,
// file reloads, and any other write path that bypasses widget
// callbacks. Closes the log-376 reconnect-race: relay cache stays
// current with owner's console edits.

namespace {

constexpr int kPollIntervalFrames = 60;  // ~1 second @ 60 Hz; throttle.

// Captured local snapshot at last broadcast. Empty until the first
// poll-cycle baseline is captured after connect.
std::unordered_map<std::string, int32_t> sLastBroadcastInts;
std::unordered_map<std::string, float>   sLastBroadcastFloats;
bool sBaselineCaptured = false;
int  sFramesSinceCheck = 0;

void SnapshotLocalEnforcedCVars(
    std::unordered_map<std::string, int32_t>& outInts,
    std::unordered_map<std::string, float>& outFloats)
{
    outInts.clear();
    outFloats.clear();
    for (const auto& e : AnchorCVarSync::kEnforcedInts) {
        outInts[e.cvarName] = CVarGetInteger(e.cvarName, e.defaultValue);
    }
    for (const auto& e : AnchorCVarSync::kEnforcedFloats) {
        outFloats[e.cvarName] = CVarGetFloat(e.cvarName, e.defaultValue);
    }
}

bool SnapshotsEqual(
    const std::unordered_map<std::string, int32_t>& aInts,
    const std::unordered_map<std::string, float>& aFloats,
    const std::unordered_map<std::string, int32_t>& bInts,
    const std::unordered_map<std::string, float>& bFloats)
{
    if (aInts.size() != bInts.size() || aFloats.size() != bFloats.size()) return false;
    for (const auto& [k, v] : aInts) {
        auto it = bInts.find(k);
        if (it == bInts.end() || it->second != v) return false;
    }
    for (const auto& [k, v] : aFloats) {
        auto it = bFloats.find(k);
        if (it == bFloats.end() || it->second != v) return false;
    }
    return true;
}

void OnFrameUpdateAutoSyncCVars() {
    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || !anchor->isEnabled || !anchor->isConnected) {
        // Reset on disconnect — next reconnect's HANDSHAKE re-primes the
        // relay, so we want to re-capture baseline rather than carry
        // stale state from the prior session.
        sBaselineCaptured = false;
        sFramesSinceCheck = 0;
        return;
    }
    if (anchor->roomState.ownerClientId != anchor->ownClientId) {
        // Non-owner — never broadcasts. Effective-host migration
        // freezes the block per the strict-host rule.
        return;
    }

    if (++sFramesSinceCheck < kPollIntervalFrames) return;
    sFramesSinceCheck = 0;

    std::unordered_map<std::string, int32_t> curInts;
    std::unordered_map<std::string, float>   curFloats;
    SnapshotLocalEnforcedCVars(curInts, curFloats);

    if (!sBaselineCaptured) {
        // First poll after connect / ownership acquisition. HANDSHAKE's
        // PrepRoomState already primed the relay with these exact
        // values — record the baseline without re-broadcasting.
        sLastBroadcastInts = curInts;
        sLastBroadcastFloats = curFloats;
        sBaselineCaptured = true;
        return;
    }

    if (SnapshotsEqual(curInts, curFloats, sLastBroadcastInts, sLastBroadcastFloats)) return;

    sLastBroadcastInts = curInts;
    sLastBroadcastFloats = curFloats;
    anchor->SendPacket_UpdateRoomState();
    SPDLOG_INFO("[SettingsSync] Local enforced CVar drift detected — broadcast triggered.");
}

void RegisterEnforcedCVarsAutoSync() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() { OnFrameUpdateAutoSyncCVars(); });
}

static RegisterShipInitFunc gInitAutoSync(RegisterEnforcedCVarsAutoSync);

}  // namespace

// Reopened so the public helper can touch the anonymous-namespace
// snapshot statics. See v1 module structure for the pattern rationale.
namespace AnchorCVarSync {

void TriggerOwnerBroadcastNow() {
    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || !anchor->isEnabled || !anchor->isConnected) return;
    if (anchor->roomState.ownerClientId != anchor->ownClientId) return;

    // Update the auto-poll's baseline BEFORE broadcasting so the next
    // 1-second tick observes no drift and skips its own redundant
    // broadcast.
    SnapshotLocalEnforcedCVars(sLastBroadcastInts, sLastBroadcastFloats);
    sBaselineCaptured = true;
    anchor->SendPacket_UpdateRoomState();
}

}  // namespace AnchorCVarSync
