#include "PacketSchemas.h"
#include "soh/Network/Anchor/Anchor.h"

namespace Anchor {

bool PeerSupportsField(uint32_t clientId, const std::string& packetType,
                       int requiredSchema) {
    if (!::Anchor::Instance) return true;  // pre-init: assume support

    auto it = ::Anchor::Instance->clients.find(clientId);
    if (it == ::Anchor::Instance->clients.end()) {
        // Unknown peer; default to true (legacy peers handle missing
        // fields via deserialise-to-default, which is safe).
        return true;
    }

    const auto& peerMap = it->second.peerMaxSchema;
    auto sit = peerMap.find(packetType);
    if (sit == peerMap.end()) {
        // Peer hasn't reported this packet type → legacy or pre-Pillar-F.
        // Default to true; receiver tolerates missing fields.
        return true;
    }
    return sit->second >= requiredSchema;
}

}  // namespace Anchor
