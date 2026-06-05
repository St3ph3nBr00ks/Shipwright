#pragma once

// Settings Sync v2 — host-authoritative read-side wrapper, registry-driven.
//
// Reads route through `Anchor::Instance->roomState.enforcedInts` /
// `enforcedFloats` maps when a session is connected. Falls back to
// CVarGetInteger / CVarGetFloat on disconnect or unknown name. The
// set of CVars considered "enforced" is defined by
// Common/EnforcedCVarRegistry.{h,cpp} — adding a new enforced CVar
// is one entry in the registry; this wrapper, the
// sender/receiver, the auto-broadcast snapshot, the dispatch helper,
// and the snapshot diff all iterate the same registry.
//
// Plans/settings_sync_design.md (architecture + gate behaviour).
// Research/settings_sync_audit.md (scope + Class A/B/C split).
//
// Usage from C++ (Anchor + soh/ Enhancements + cheat handlers):
//     if (AnchorCVarSync::GetEnforcedInt(CVAR_CHEAT("InfiniteAmmo"), 0)) { ... }
//
// Usage from C decomp (z_player.c, z_actor.c, etc.) — declarations
// for the C-linkage shims live in soh/include/functions.h so every
// TU that includes global.h sees them transparently:
//     if (Anchor_GetEnforcedInt(CVAR_CHEAT("InfiniteAmmo"), 0)) { ... }

#include <libultraship/libultraship.h>

namespace AnchorCVarSync {

// Returns host's enforced value when a session is connected AND the
// named CVar is present in `roomState.enforcedInts` (i.e. the host
// actually broadcast it); otherwise delegates to
// CVarGetInteger(cvarName, localDefault). Safe to call any time —
// null-Anchor / pre-connect / map-miss paths all fall back.
int32_t GetEnforcedInt(const char* cvarName, int32_t localDefault);

// Float variant. Same gate / fallback shape.
float GetEnforcedFloat(const char* cvarName, float localDefault);

// Predicate for UI: true when the named CVar IS in the enforced
// registry AND a session is connected. Used by the Phase 3 Flotilla
// → Host Settings sidebar's gate logic.
bool IsEnforced(const char* cvarName);

// Triggers ShipInit::Init(cvarName) for every enforced CVar after the
// receive side has updated roomState's enforced maps. This re-fires
// any RegisterShipInitFunc listeners gated on those names, which in
// turn re-evaluate their COND_HOOK / COND_VB_SHOULD blocks against
// the wrapper's new return value. Without this, peer-side hooks
// (InfiniteAmmo refill, FreezeTime, HyperEnemies, etc.) stay
// registered/unregistered according to peer's local CVar — never
// re-evaluated when host's value changes over the wire.
//
// Call from HandlePacket_UpdateRoomState AFTER updating
// roomState.enforced{Ints,Floats} so the listeners' wrapper reads
// return host's authoritative value.
void OnCvarsReceivedDispatch();

// Owner-side helper for UI widget callbacks. Snapshots local enforced
// CVars, broadcasts via SendPacket_UpdateRoomState, and updates the
// auto-poll's last-broadcast baseline so the next 1-second tick
// doesn't double-broadcast the same state.
//
// No-op when not owner / not connected — safe to call from any
// widget that targets an enforced CVar without per-call gating.
void TriggerOwnerBroadcastNow();

}  // namespace AnchorCVarSync

// C-linkage shims for OoT decomp consumers (z_player.c etc.).
// Decls also forward-declared in soh/include/functions.h.
#ifdef __cplusplus
extern "C" {
#endif

int32_t Anchor_GetEnforcedInt(const char* cvarName, int32_t localDefault);
float   Anchor_GetEnforcedFloat(const char* cvarName, float localDefault);

#ifdef __cplusplus
}
#endif
