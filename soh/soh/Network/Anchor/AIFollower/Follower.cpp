/**
 * AiFollower / Follower — implementation. Phase 1 commit 1: scaffolding.
 *
 * Empty stub. Subsequent commits move the AI Follower state machine,
 * helper methods, hook bodies, and tunable constants out of
 * HookHandlers.cpp into this file.
 *
 * This file's mere existence is the value-add of commit 1: verifies the
 * new directory under soh/soh/Network/Anchor/AiFollower/ is picked up by
 * the CMake GLOB_RECURSE in soh/CMakeLists.txt (after a reconfigure)
 * and that the namespace + ShipInit boot path work end-to-end. Future
 * move-commits drop into a working build infrastructure rather than
 * doing scaffolding + logic-move atomically (which is harder to
 * verify and harder to revert).
 */

#include "Follower.h"
#include "soh/ShipInit.hpp"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

namespace AnchorFollower {

void RegisterFollowerModule() {
    // Stub. Subsequent Phase 1 commits register OnGameFrameUpdate /
    // ShouldActorUpdate hooks here, mirroring (and eventually
    // replacing) the registrations currently performed inside
    // Anchor::RegisterHooks for follower-specific behaviour.
    SPDLOG_DEBUG("[AiFollower] Follower module scaffolded (no hooks yet)");
}

} // namespace AnchorFollower

// ShipInit hook — fires once at boot. Mirrors the pattern used by
// every other Anchor::Common module (NavTraits, ActorTrail,
// LeashRespawn, etc.).
static RegisterShipInitFunc registerFollowerModule(AnchorFollower::RegisterFollowerModule);
