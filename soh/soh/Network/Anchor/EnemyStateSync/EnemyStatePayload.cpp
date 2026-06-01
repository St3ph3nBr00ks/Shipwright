#include "EnemyStatePayload.h"

#include "soh/Network/Anchor/Anchor.h"                // Anchor::ENEMY_STATE
#include "soh/Network/Anchor/Common/PacketTimeline.h" // SetTimelineField

namespace EnemyStateSync {

nlohmann::json BuildBaseEnemyStatePayload(LifecyclePhase phase,
                                          uint32_t netId,
                                          bool phaseChanged) {
    nlohmann::json payload;
    payload["type"]         = Anchor::ENEMY_STATE;
    payload["phase"]        = LifecyclePhaseName(phase);
    payload["phaseChanged"] = phaseChanged;
    if (netId != 0) {
        payload["netId"] = netId;
    }
    PacketTimeline::SetTimelineField(payload);
    return payload;
}

}  // namespace EnemyStateSync
