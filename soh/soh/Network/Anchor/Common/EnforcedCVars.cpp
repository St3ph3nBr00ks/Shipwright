#include "EnforcedCVars.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <cstring>

namespace AnchorCVarSync {

// Maps the project's CVAR_CHEAT(...) / CVAR_ENHANCEMENT(...) macro
// expansions to the matching MultiplayerCVars field. v1 uses a
// strcmp chain — 20 string comparisons per call is negligible at
// the read-site frequencies in question (most are per-frame at
// 60Hz, single-digit number of sites each). Convert to a
// std::unordered_map<std::string, std::function<...>> only if a
// profiler flags the chain as a hot spot.
//
// Add new entries here when the MultiplayerCVars struct grows; the
// helper falls through to the local CVar on any unknown name, so
// forgetting an entry produces a graceful "local takes effect"
// failure mode rather than a crash.

static const Anchor* GetActiveAnchor() {
    // Gate on isConnected (not isEnabled): between Enable() being
    // requested and the relay's HANDSHAKE-reply UPDATE_ROOM_STATE
    // arriving, roomState.cvars is still zero-initialised and would
    // return wrong defaults for the three non-zero-default fields
    // (timeTravel = 1, speedModifierValue = 1.0f,
    // bombTimerMultiplier = 1.0f). isConnected gates the window
    // such that we read enforced values only after the owner's
    // snapshot has arrived.
    if (Anchor::Instance == nullptr) return nullptr;
    if (!Anchor::Instance->isEnabled) return nullptr;
    if (!Anchor::Instance->isConnected) return nullptr;
    return Anchor::Instance;
}

int32_t GetEnforcedInt(const char* cvarName, int32_t localDefault) {
    const Anchor* anchor = GetActiveAnchor();
    if (anchor == nullptr) {
        return CVarGetInteger(cvarName, localDefault);
    }

    const MultiplayerCVars& cv = anchor->roomState.cvars;

    // Class A — drift causes desync/crash
    if (strcmp(cvarName, CVAR_ENHANCEMENT("TimeTravel")) == 0)        return cv.timeTravel;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("DamageMult")) == 0)        return cv.damageMult;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("FallDamageMult")) == 0)    return cv.fallDamageMult;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("VoidDamageMult")) == 0)    return cv.voidDamageMult;
    if (strcmp(cvarName, CVAR_CHEAT("FreezeTime")) == 0)              return cv.freezeTime;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("RandomizedEnemies")) == 0) return cv.randomizedEnemies;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("NewDrops")) == 0)          return cv.newDrops;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("HyperEnemies")) == 0)      return cv.hyperEnemies;

    // Class B — drift causes UX / fairness divergence
    if (strcmp(cvarName, CVAR_CHEAT("NoRestrictItems")) == 0)         return cv.noRestrictItems;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("BonkDamageMult")) == 0)    return cv.bonkDamageMult;
    if (strcmp(cvarName, CVAR_CHEAT("InfiniteAmmo")) == 0)            return cv.infiniteAmmo;
    if (strcmp(cvarName, CVAR_CHEAT("ClimbEverything")) == 0)         return cv.climbEverything;
    if (strcmp(cvarName, CVAR_CHEAT("HookshotEverything")) == 0)      return cv.hookshotEverything;
    if (strcmp(cvarName, CVAR_CHEAT("SuperTunic")) == 0)              return cv.superTunic;
    if (strcmp(cvarName, CVAR_CHEAT("TimelessEquipment")) == 0)       return cv.timelessEquipment;
    if (strcmp(cvarName, CVAR_CHEAT("ShieldTwoHanded")) == 0)         return cv.shieldTwoHanded;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("RemoveExplosiveLimit")) == 0) return cv.removeExplosiveLimit;
    if (strcmp(cvarName, CVAR_CHEAT("FireproofDekuShield")) == 0)     return cv.fireproofDekuShield;

    // Unknown CVar — caller is using the wrapper for a name we don't
    // enforce. Graceful fallback so misuse doesn't break behaviour.
    return CVarGetInteger(cvarName, localDefault);
}

float GetEnforcedFloat(const char* cvarName, float localDefault) {
    const Anchor* anchor = GetActiveAnchor();
    if (anchor == nullptr) {
        return CVarGetFloat(cvarName, localDefault);
    }

    const MultiplayerCVars& cv = anchor->roomState.cvars;

    if (strcmp(cvarName, CVAR_CHEAT("SpeedModifier.Value")) == 0)     return cv.speedModifierValue;
    if (strcmp(cvarName, CVAR_CHEAT("BombTimerMultiplier")) == 0)     return cv.bombTimerMultiplier;

    return CVarGetFloat(cvarName, localDefault);
}

bool IsEnforced(const char* cvarName) {
    if (GetActiveAnchor() == nullptr) return false;

    // Integer-class enforced names
    if (strcmp(cvarName, CVAR_ENHANCEMENT("TimeTravel")) == 0)        return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("DamageMult")) == 0)        return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("FallDamageMult")) == 0)    return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("VoidDamageMult")) == 0)    return true;
    if (strcmp(cvarName, CVAR_CHEAT("FreezeTime")) == 0)              return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("RandomizedEnemies")) == 0) return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("NewDrops")) == 0)          return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("HyperEnemies")) == 0)      return true;
    if (strcmp(cvarName, CVAR_CHEAT("NoRestrictItems")) == 0)         return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("BonkDamageMult")) == 0)    return true;
    if (strcmp(cvarName, CVAR_CHEAT("InfiniteAmmo")) == 0)            return true;
    if (strcmp(cvarName, CVAR_CHEAT("ClimbEverything")) == 0)         return true;
    if (strcmp(cvarName, CVAR_CHEAT("HookshotEverything")) == 0)      return true;
    if (strcmp(cvarName, CVAR_CHEAT("SuperTunic")) == 0)              return true;
    if (strcmp(cvarName, CVAR_CHEAT("TimelessEquipment")) == 0)       return true;
    if (strcmp(cvarName, CVAR_CHEAT("ShieldTwoHanded")) == 0)         return true;
    if (strcmp(cvarName, CVAR_ENHANCEMENT("RemoveExplosiveLimit")) == 0) return true;
    if (strcmp(cvarName, CVAR_CHEAT("FireproofDekuShield")) == 0)     return true;

    // Float-class enforced names
    if (strcmp(cvarName, CVAR_CHEAT("SpeedModifier.Value")) == 0)     return true;
    if (strcmp(cvarName, CVAR_CHEAT("BombTimerMultiplier")) == 0)     return true;

    return false;
}

}  // namespace AnchorCVarSync

// extern "C" shims for C-decomp consumers (z_player.c etc.).
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
// HANDSHAKE-time PrepRoomState primes the relay with the owner's local CVar
// values on first connect. Beyond that, the relay only learns about changes
// when SendPacket_UpdateRoomState fires — which today only happens when the
// owner toggles a widget in the Network → Anchor admin pane (and, once
// Phase 3 lands, the Flotilla → Host Settings sidebar).
//
// Console-driven changes (`set gEnhancements.DamageMult 2`) and CVar-file
// reloads bypass the widget-callback path entirely, leaving the relay's
// cached roomState stale. A peer that disconnects and reconnects after a
// console change receives the *previous* broadcast value, not the current
// local value — which surfaced in field test log 376.
//
// This poll closes the gap: on the owner's frame update, once per second,
// snapshot the local CVars and compare to the last broadcast. Any drift
// triggers a fresh SendPacket_UpdateRoomState. The 1-second cadence keeps
// the bandwidth cost negligible while making console edits propagate
// within the same RTT envelope as widget toggles.

namespace {

constexpr int kPollIntervalFrames = 60;  // ~1 second @ 60 Hz; throttle.

MultiplayerCVars sLastBroadcastSnapshot{};
bool sBaselineCaptured = false;
int  sFramesSinceCheck = 0;

MultiplayerCVars SnapshotLocalEnforcedCVars() {
    MultiplayerCVars s{};
    s.timeTravel           = CVarGetInteger(CVAR_ENHANCEMENT("TimeTravel"), 1);
    s.damageMult           = CVarGetInteger(CVAR_ENHANCEMENT("DamageMult"), 0);
    s.fallDamageMult       = CVarGetInteger(CVAR_ENHANCEMENT("FallDamageMult"), 0);
    s.voidDamageMult       = CVarGetInteger(CVAR_ENHANCEMENT("VoidDamageMult"), 0);
    s.freezeTime           = CVarGetInteger(CVAR_CHEAT("FreezeTime"), 0);
    s.randomizedEnemies    = CVarGetInteger(CVAR_ENHANCEMENT("RandomizedEnemies"), 0);
    s.newDrops             = CVarGetInteger(CVAR_ENHANCEMENT("NewDrops"), 0);
    s.hyperEnemies         = CVarGetInteger(CVAR_ENHANCEMENT("HyperEnemies"), 0);
    s.noRestrictItems      = CVarGetInteger(CVAR_CHEAT("NoRestrictItems"), 0);
    s.bonkDamageMult       = CVarGetInteger(CVAR_ENHANCEMENT("BonkDamageMult"), 0);
    s.infiniteAmmo         = CVarGetInteger(CVAR_CHEAT("InfiniteAmmo"), 0);
    s.climbEverything      = CVarGetInteger(CVAR_CHEAT("ClimbEverything"), 0);
    s.hookshotEverything   = CVarGetInteger(CVAR_CHEAT("HookshotEverything"), 0);
    s.speedModifierValue   = CVarGetFloat(CVAR_CHEAT("SpeedModifier.Value"), 1.0f);
    s.superTunic           = CVarGetInteger(CVAR_CHEAT("SuperTunic"), 0);
    s.timelessEquipment    = CVarGetInteger(CVAR_CHEAT("TimelessEquipment"), 0);
    s.bombTimerMultiplier  = CVarGetFloat(CVAR_CHEAT("BombTimerMultiplier"), 1.0f);
    s.shieldTwoHanded      = CVarGetInteger(CVAR_CHEAT("ShieldTwoHanded"), 0);
    s.removeExplosiveLimit = CVarGetInteger(CVAR_ENHANCEMENT("RemoveExplosiveLimit"), 0);
    s.fireproofDekuShield  = CVarGetInteger(CVAR_CHEAT("FireproofDekuShield"), 0);
    return s;
}

bool EnforcedCVarsEqual(const MultiplayerCVars& a, const MultiplayerCVars& b) {
    return std::memcmp(&a, &b, sizeof(MultiplayerCVars)) == 0;
}

void OnFrameUpdateAutoSyncCVars() {
    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || !anchor->isEnabled || !anchor->isConnected) {
        // Reset on disconnect — next reconnect's HANDSHAKE re-primes the
        // relay, so we want to re-capture baseline rather than carry stale
        // state from the prior session.
        sBaselineCaptured = false;
        sFramesSinceCheck = 0;
        return;
    }
    if (anchor->roomState.ownerClientId != anchor->ownClientId) {
        // Non-owner — never broadcasts cvars. Effective-host migration
        // freezes the block per the strict-host rule (design doc §2).
        return;
    }

    if (++sFramesSinceCheck < kPollIntervalFrames) return;
    sFramesSinceCheck = 0;

    MultiplayerCVars current = SnapshotLocalEnforcedCVars();

    if (!sBaselineCaptured) {
        // First poll after connect / ownership acquisition. HANDSHAKE's
        // PrepRoomState already primed the relay with these exact values
        // — record the baseline without re-broadcasting.
        sLastBroadcastSnapshot = current;
        sBaselineCaptured = true;
        return;
    }

    if (EnforcedCVarsEqual(current, sLastBroadcastSnapshot)) return;

    sLastBroadcastSnapshot = current;
    anchor->SendPacket_UpdateRoomState();
    SPDLOG_INFO("[SettingsSync] Local enforced CVar drift detected — broadcast triggered.");
}

void RegisterEnforcedCVarsAutoSync() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        []() { OnFrameUpdateAutoSyncCVars(); });
}

static RegisterShipInitFunc gInitAutoSync(RegisterEnforcedCVarsAutoSync);

}  // namespace
