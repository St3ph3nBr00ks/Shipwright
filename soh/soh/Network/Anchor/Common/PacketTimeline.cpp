#include "PacketTimeline.h"

extern "C" {
#include "z64.h"
extern SaveContext gSaveContext;
}

namespace PacketTimeline {

void SetTimelineField(nlohmann::json& payload) {
    payload["timeline"] = (uint8_t)gSaveContext.linkAge;
}

bool IsCrossTimelinePacket(const nlohmann::json& payload) {
    if (!payload.contains("timeline")) {
        return false;
    }
    uint8_t packetTimeline = payload["timeline"].get<uint8_t>();
    return packetTimeline != (uint8_t)gSaveContext.linkAge;
}

}  // namespace PacketTimeline
