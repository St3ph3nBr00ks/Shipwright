#pragma once

// Settings Sync v1 — host-authoritative read-side wrapper for the
// 20 enforced gameplay CVars (Class A drift-causes-desync, Class B
// drift-causes-unfairness). When connected to an active session, the
// returned value is sourced from `Anchor::Instance->roomState.cvars`
// (which is owner-authoritative per RoomState::ownerClientId — strict
// host, NOT effective host). When disconnected, falls back to the
// local `CVarGetInteger` / `CVarGetFloat` so single-player and
// menu-preview reads return the user's local preferences.
//
// Plans/settings_sync_design.md (architecture + gate behaviour).
// Research/settings_sync_audit.md (the 20-CVar scope + read-site list).
//
// Usage from C++ (Anchor + soh/ Enhancements + cheat handlers):
//     if (AnchorCVarSync::GetEnforcedInt(CVAR_CHEAT("InfiniteAmmo"), 0)) { ... }
//
// Usage from C decomp (z_player.c, z_actor.c, etc.) — declare a
// forward extern at the top of the .c file (see Pitfall 7 in
// CLAUDE.md / session_state.md) and call the `Anchor_*` shim:
//     extern int Anchor_GetEnforcedInt(const char* cvarName, int localDefault);
//     ...
//     if (Anchor_GetEnforcedInt(CVAR_CHEAT("InfiniteAmmo"), 0)) { ... }

#include <libultraship/libultraship.h>

namespace AnchorCVarSync {

// Returns host's enforced value when a session is connected AND the
// named CVar is in the enforced set; otherwise delegates to
// CVarGetInteger(cvarName, localDefault). Safe to call any time —
// null-Anchor / pre-connect / unknown-CVar paths all fall back.
int32_t GetEnforcedInt(const char* cvarName, int32_t localDefault);

// Float variant for SpeedModifier.Value + BombTimerMultiplier.
float GetEnforcedFloat(const char* cvarName, float localDefault);

// Predicate for UI: true when the named CVar IS in the enforced set
// AND there's a live connection that would override the local value.
// Used by the Phase 3 Flotilla → Host Settings sidebar's owner-vs-
// peer gate logic.
bool IsEnforced(const char* cvarName);

// Triggers ShipInit::Init(cvarName) for every enforced CVar after the
// receive side has updated roomState.cvars. This re-fires any
// RegisterShipInitFunc listeners gated on those names, which in turn
// re-evaluate their COND_HOOK / COND_VB_SHOULD blocks against the
// wrapper's new return value. Without this, peer-side hooks
// (InfiniteAmmo's per-frame refill, FreezeTime's freeze, HyperEnemies'
// double-tick, etc.) stay registered or unregistered according to
// peer's local CVar — never re-evaluated when host's value changes
// over the wire.
//
// Call from HandlePacket_UpdateRoomState AFTER updating
// roomState.cvars so the listeners' wrapper reads return host's
// authoritative value.
void OnCvarsReceivedDispatch();

}  // namespace AnchorCVarSync

// C-linkage shims for OoT decomp consumers (z_player.c etc.). The
// const char* parameter is the same string the surrounding code
// would pass to CVarGetInteger — typically the CVAR_CHEAT(...) /
// CVAR_ENHANCEMENT(...) macro expansion.
#ifdef __cplusplus
extern "C" {
#endif

int32_t Anchor_GetEnforcedInt(const char* cvarName, int32_t localDefault);
float   Anchor_GetEnforcedFloat(const char* cvarName, float localDefault);

#ifdef __cplusplus
}
#endif
