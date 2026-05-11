/**
 * NavCVars — central manifest of every gEnhancements.Nav.* CVar.
 *
 * One declaration per CVar. Replaces the seven independent
 * `#define CVAR_NAV_ENABLED CVAR_ENHANCEMENT("Nav.Enabled")` declarations
 * that previously lived inside every Common/*.cpp gate module.
 *
 * Each consumer's `IsXxxEnabled()` helper becomes one line:
 *     return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kXxx);
 *
 * This is the only file that needs to know the literal "gEnhancements.Nav.*"
 * string. New nav features add a constant here, not a `#define` in their
 * own .cpp. Plan: Plans/follower_nav_refactor_2026-05-11.md §R2.
 */
#pragma once

#include "soh/cvar_prefixes.h"

namespace AnchorNavCVars {

// Master gate. When 0, every per-feature gate returns false even if its
// own toggle is on. Default off per Flotilla vanilla-altering policy.
inline constexpr const char* kEnabled =
    CVAR_ENHANCEMENT("Nav.Enabled");

// Per-feature toggles. Each is read in addition to kEnabled. Default off;
// individual features ship behind their own opt-in.
inline constexpr const char* kActorTrail =
    CVAR_ENHANCEMENT("Nav.ActorTrail");
inline constexpr const char* kAiFollowerConsumer =
    CVAR_ENHANCEMENT("Nav.AiFollowerConsumer");
inline constexpr const char* kClimbableSurfaces =
    CVAR_ENHANCEMENT("Nav.ClimbableSurfaces");
inline constexpr const char* kEdgeAvoidance =
    CVAR_ENHANCEMENT("Nav.EdgeAvoidance");
inline constexpr const char* kGroundFollowing =
    CVAR_ENHANCEMENT("Nav.GroundFollowing");
inline constexpr const char* kLeashRespawn =
    CVAR_ENHANCEMENT("Nav.LeashRespawn");
inline constexpr const char* kRoomNavConsumer =
    CVAR_ENHANCEMENT("Nav.RoomNavConsumer");
inline constexpr const char* kTargetSelection =
    CVAR_ENHANCEMENT("Nav.TargetSelection");
inline constexpr const char* kVerticalTeleport =
    CVAR_ENHANCEMENT("Nav.VerticalTeleport");

// True iff the master kEnabled CVar is non-zero.
bool IsMasterEnabled();

// True iff master AND the named feature CVar are both non-zero.
// `featureCVar` should be one of the k* constants above.
bool IsFeatureEnabled(const char* featureCVar);

} // namespace AnchorNavCVars
