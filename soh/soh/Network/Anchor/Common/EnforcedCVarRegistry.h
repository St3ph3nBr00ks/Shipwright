#pragma once

// Settings-sync v2 — registry-driven manifest of enforced CVars.
//
// Single source of truth: every enforced CVar lives in either kEnforcedInts
// or kEnforcedFloats. Adding a new enforced CVar is one entry in the
// registry — the wrapper, sender, receiver, ShipInit dispatch, and the
// snapshot/diff machinery all iterate the same vector. The Flotilla →
// Host Settings sidebar widgets stay hand-written so authors keep
// control over labels, tooltips, and combo maps; they reference the
// same CVar names so they bind to the same underlying value.
//
// Wire format: each enforced CVar appears as a JSON key/value pair
// under `state.cvars` in UPDATE_ROOM_STATE. Keys are the full CVar
// macro expansion (e.g. "gEnhancements.DamageMult") — this changed
// from the v1 short-name convention ("damageMult"). Schema bumped
// 2 → 3 to reflect the wire-format break. Old v1 receivers will
// fail to find their expected short keys and gracefully fall back to
// local CVars (degradation, not crash).
//
// Classification reference (`Research/settings_sync_audit.md`):
// - Class A — drift causes desync / crash
// - Class B — drift causes UX / fairness divergence
// - Class C explicitly excluded — local preferences (text speed,
//   HUD scale, animation playback, controls, audio, fonts, etc.)

#include <cstdint>
#include <vector>

namespace AnchorCVarSync {

struct IntCVarEntry {
    const char* cvarName;     // Full macro expansion, e.g. CVAR_ENHANCEMENT("DamageMult")
    int32_t     defaultValue;
};

struct FloatCVarEntry {
    const char* cvarName;
    float       defaultValue;
};

extern const std::vector<IntCVarEntry>   kEnforcedInts;
extern const std::vector<FloatCVarEntry> kEnforcedFloats;

}  // namespace AnchorCVarSync
