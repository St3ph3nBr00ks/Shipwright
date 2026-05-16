/**
 * TestDescriptor implementation. See header for design notes.
 */

#include "TestDescriptor.h"
#include "../Director.h"

#include "soh/cvar_prefixes.h"

#include <imgui.h>  // ImGui::Button for RenderDebugUI
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "z64actor.h"  // ACTOR_EN_TEST via the inline DEFINE_ACTOR enum
}

namespace AnchorDirector {

namespace {
// Gates the per-tick "why no proposal" SPDLOGs behind the
// gEnhancements.AI.Director.LogProposals CVar so production logs
// stay quiet. Throttling (mTicksSinceLog ≥ 100) still applies on
// top — CVar on + throttle elapsed = log fires.
inline bool IsLoggingProposals() {
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.LogProposals"), 0) != 0;
}
}  // namespace

bool TestDescriptor::IsEnabled() const {
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.TestDescriptorEnabled"), 0) != 0;
}

std::vector<SpawnProposal> TestDescriptor::ProposeSpawn(const Director& director,
                                                       const SessionView& view) {
    // Step 9 diagnostic — throttled rejection reason log. ~100 ticks @
    // 20fps = ~5s. Remove once Director is field-validated.
    ++mTicksSinceLog;
    const bool shouldLog = (mTicksSinceLog >= 100) && IsLoggingProposals();
    auto markLog = [&]() { mTicksSinceLog = 0; };

    // Live-count cap.
    const int liveCount = director.GetLiveCount(GetDescriptorId());
    if (liveCount >= kMaxAlive) {
        if (shouldLog) {
            SPDLOG_INFO("[TestDescriptor] no proposal: liveCount={} cap={}",
                        liveCount, kMaxAlive);
            markLog();
        }
        return {};
    }

    // Pick the target player. Prefer the most-isolated; fall back to
    // first available. SessionView::IsValid already guaranteed by Tick.
    const PlayerSnapshot* target = view.MostIsolatedPlayer();
    if (target == nullptr) {
        if (shouldLog) {
            SPDLOG_INFO("[TestDescriptor] no proposal: no target player "
                        "(view.players={})", view.players.size());
            markLog();
        }
        return {};
    }

    // Skip targets without a loaded save or in cutscene (spawning during
    // a cutscene would be visually jarring even for a test).
    if (!target->isSaveLoaded || target->isInCutscene) {
        if (shouldLog) {
            SPDLOG_INFO("[TestDescriptor] no proposal: target invalid "
                        "(clientId={} save={} cutscene={})",
                        target->clientId, target->isSaveLoaded, target->isInCutscene);
            markLog();
        }
        return {};
    }

    // Cooldown is per-(scene, room, descriptor). MsCooldownElapsed wraps
    // Anchor::MsToGameTicks so the 30-second window holds at any tick
    // rate.
    if (!director.MsCooldownElapsed(target->sceneNum, target->roomNum,
                                    GetDescriptorId(), kCooldownMs)) {
        if (shouldLog) {
            const int framesSince = director.GetFramesSinceLastSpawn(
                target->sceneNum, target->roomNum, GetDescriptorId());
            SPDLOG_INFO("[TestDescriptor] no proposal: cooldown "
                        "(scene={} room={} framesSinceLast={})",
                        (int)target->sceneNum, (int)target->roomNum, framesSince);
            markLog();
        }
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
    // STALFOS_TYPE_1 (=1). Stalfos params 0 is STALFOS_TYPE_INVISIBLE —
    // visible only with Lens of Truth — which is why the field-test
    // 2026-05-15 in Bottom of the Well saw a ghost-but-physical Stalfos
    // (model present, AI active, hit sparkles fire, but actor's own
    // draw suppressed by the invisible-type flag). Type 1 is the
    // standard visible Stalfos.
    p.actorParams  = 1;
    p.variantId    = 0;
    p.priority     = DescriptorPriority::Ambient;  // low; preempted by real descriptors
    p.groupId      = 0;
    ++mProposalsOffered;
    if (IsLoggingProposals()) {
        SPDLOG_INFO("[TestDescriptor] OFFERED proposal: target=client{} scene={} room={} "
                    "playerPos=({:.0f},{:.0f},{:.0f}) spawnPos=({:.0f},{:.0f},{:.0f}) offset={:.0f}u",
                    target->clientId, (int)target->sceneNum, (int)target->roomNum,
                    target->worldPos.x, target->worldPos.y, target->worldPos.z,
                    p.worldPos.x, p.worldPos.y, p.worldPos.z,
                    kSpawnOffsetXZ);
    }
    markLog();
    return { p };
}

std::vector<SpawnProposal> TestDescriptor::BuildForcedProposal(const Director& director,
                                                               const SessionView& view) {
    // Bypasses live-count cap, cooldown, target-invalid checks — the
    // user explicitly asked for a spawn via the dev button.
    (void)director;
    const PlayerSnapshot* target = view.MostIsolatedPlayer();
    if (target == nullptr) {
        return {};  // No players → can't spawn meaningfully.
    }

    SpawnProposal p;
    p.source       = this;
    p.sceneNum     = target->sceneNum;
    p.roomNum      = target->roomNum;
    p.worldPos.x   = target->worldPos.x + kSpawnOffsetXZ;
    p.worldPos.y   = target->worldPos.y;
    p.worldPos.z   = target->worldPos.z;
    p.yawTowards   = 0x8000;
    p.actorId      = ACTOR_EN_TEST;
    p.actorParams  = 1;  // STALFOS_TYPE_1 visible variant
    p.variantId    = 0;
    p.priority     = DescriptorPriority::Ambient;
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

void TestDescriptor::RenderDebugUI(const Director& director) {
    (void)director;
    if (ImGui::Button("Force Spawn TestDescriptor")) {
        // Bypasses cooldown/cap/gates. RecordSpawn still increments the
        // live count + broadcasts state, so DIRECTOR_STATE_SYNC remains
        // consistent. Use sparingly during step 14 testing.
        Director::Instance().ForceSpawn(GetDescriptorId());
    }
}

}  // namespace AnchorDirector
