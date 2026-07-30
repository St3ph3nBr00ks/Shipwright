/**
 * GravityAdapter — Phase 2 SUPERSEDED body (kept as callable helper).
 *
 * Original Phase 2 design applied gravity + terminal-velocity clamp
 * to an enhanced enemy when its descriptor's ShouldApplyGravity
 * returned true. Used vanilla `bgCheckFlags & 0x1` as the on-ground
 * signal. Field-test log 761 (2026-07-30) proved this dormant for
 * combat En_Sw: vanilla combat En_Sw never populates bgCheckFlags
 * (`bgFlags=0x0` on every diagnostic emission), so ShouldApplyGravity
 * — which reads those flags — never returns true, and TickGravity is
 * never reached.
 *
 * Superseded by the En_Sw enhanced state machine (Option B, per
 * Plans/en_sw_enhanced_state_machine_pilot.md). The state machine's
 * WallEdgeDrop + GroundPursue states absorb gravity + landing logic
 * directly, driving vertical motion under a state machine that owns
 * bgCheck-independent detection of wall-vs-floor state.
 *
 * Body reduced to early-return. Kept as a callable helper for future
 * enemy descriptors that DO participate in vanilla bgCheck and want
 * simple gravity semantics without a full state machine. Original
 * body preserved in git history.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before GravityAdapter.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "GravityAdapter.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt

#include <libultraship/bridge/consolevariablebridge.h>

namespace AnchorEnemyEnhancement {

bool TickGravity(EnemyEnhancementDescriptor& descriptor,
                 GravityAdapterState& state,
                 Actor* actor,
                 PlayState* play) {
    (void)state;
    if (actor == nullptr || play == nullptr) return false;

    // CVar gate preserved so callers that check the CVar independently
    // still get consistent enable/disable semantics. Routes through
    // AnchorCVarSync so peers honour host's enforced value.
    const char* cvarName = descriptor.GravityAwareCVar();
    if (cvarName != nullptr && AnchorCVarSync::GetEnforcedInt(cvarName, 0) == 0) {
        return false;
    }

    // SUPERSEDED by En_Sw enhanced state machine (Option B pilot). The
    // state machine's WallEdgeDrop state applies gravity directly when
    // an on-wall spider steps off a wall's edge; GroundPursue handles
    // the landed case. No consumer calls this helper today. See git
    // history for the previous full implementation.
    return false;
}

}  // namespace AnchorEnemyEnhancement
