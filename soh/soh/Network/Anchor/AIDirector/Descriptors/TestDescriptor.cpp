/**
 * TestDescriptor implementation. See header for design notes.
 */

#include "TestDescriptor.h"
#include "../Director.h"

#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64actor.h"  // ACTOR_EN_TEST via the inline DEFINE_ACTOR enum
}

namespace AnchorDirector {

bool TestDescriptor::IsEnabled() const {
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.TestDescriptorEnabled"), 0) != 0;
}

std::vector<SpawnProposal> TestDescriptor::ProposeSpawn(const Director& director,
                                                       const SessionView& view) {
    // Live-count cap.
    if (director.GetLiveCount(GetDescriptorId()) >= kMaxAlive) {
        return {};
    }

    // Pick the target player. Prefer the most-isolated; fall back to
    // first available. SessionView::IsValid already guaranteed by Tick.
    const PlayerSnapshot* target = view.MostIsolatedPlayer();
    if (target == nullptr) return {};

    // Skip targets without a loaded save or in cutscene (spawning during
    // a cutscene would be visually jarring even for a test).
    if (!target->isSaveLoaded || target->isInCutscene) {
        return {};
    }

    // Cooldown is per-(scene, room, descriptor). MsCooldownElapsed wraps
    // Anchor::MsToGameTicks so the 30-second window holds at any tick
    // rate.
    if (!director.MsCooldownElapsed(target->sceneNum, target->roomNum,
                                    GetDescriptorId(), kCooldownMs)) {
        return {};
    }

    // Spawn position: kSpawnOffsetXZ units in front of the target. Yaw
    // toward the target so the Stalfos faces the player on spawn-in.
    // Simple "+X" offset; nav-aware placement is step 13 (Pick-
    // SpawnPosition) when Invader needs it.
    SpawnProposal p;
    p.source       = this;
    p.sceneNum     = target->sceneNum;
    p.roomNum      = target->roomNum;
    p.worldPos.x   = target->worldPos.x + kSpawnOffsetXZ;
    p.worldPos.y   = target->worldPos.y;
    p.worldPos.z   = target->worldPos.z;
    p.yawTowards   = 0x8000;  // facing -X toward the player
    p.actorId      = ACTOR_EN_TEST;
    p.actorParams  = 0;
    p.variantId    = 0;
    p.priority     = DescriptorPriority::Ambient;  // low; preempted by real descriptors
    p.groupId      = 0;
    ++mProposalsOffered;
    return { p };
}

void TestDescriptor::OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
    ++mTotalRemoved;
    mLastRemovedNetId = netId;
    SPDLOG_INFO("[TestDescriptor] OnSpawnRemoved netId={} cause={} (proposals={} removed={})",
                netId, (int)cause, mProposalsOffered, mTotalRemoved);
}

std::string TestDescriptor::GetDebugSnapshotLine() const {
    return "proposals=" + std::to_string(mProposalsOffered) +
           " removed=" + std::to_string(mTotalRemoved) +
           " lastRemove=" + std::to_string(mLastRemovedNetId);
}

}  // namespace AnchorDirector
