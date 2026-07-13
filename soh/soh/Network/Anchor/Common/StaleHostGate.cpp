#include "StaleHostGate.h"

#include <chrono>

// Include Anchor.h FIRST so libultraship + nlohmann templates are
// processed with C++ linkage; the transitive re-inclusion inside any
// downstream extern "C" block then no-ops via include guards
// (Pitfall 40).
#include "soh/Network/Anchor/Anchor.h"

#include "soh/Network/Anchor/EnemyNetId.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"

namespace EnemyStateSync {

uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ShouldDeferToPeerLocalAI(int16_t actorId,
                              const EnemyNetId* ext,
                              uint64_t nowMs) {
    if (ext == nullptr) {
        return false;
    }
    // Boss encounters are strictly host-driven — the scripted
    // state machine can't tolerate peer-side divergence during a
    // brief silence. Keep the standard sync policy in force.
    if (IsSyncedBossActor(actorId)) {
        return false;
    }
    // No broadcast ever received — no basis to judge staleness.
    // Fall through to caller's normal apply decision.
    if (ext->lastStateReceiveMs == 0) {
        return false;
    }
    // steady_clock is monotonic; guard against wall-clock skew via
    // an unsigned-underflow-safe compare rather than subtraction.
    if (nowMs <= ext->lastStateReceiveMs) {
        return false;
    }
    return (nowMs - ext->lastStateReceiveMs) > kHostStalenessThresholdMs;
}

void RecordStateReceive(EnemyNetId* ext, uint64_t nowMs) {
    if (ext == nullptr) {
        return;
    }
    ext->lastStateReceiveMs = nowMs;
}

}  // namespace EnemyStateSync
