#include "soh/Network/Anchor/Common/SkelAnimeWire.h"

#include <algorithm>

#include <libultraship/log/luslog.h>
#include <nlohmann/json.hpp>

#include "soh/Network/Anchor/JsonConversions.hpp"

namespace SkelAnimeWire {

nlohmann::json SerializePoseTable(const Vec3s* table, uint8_t count) {
    nlohmann::json arr = nlohmann::json::array();
    if (table == nullptr) {
        return arr;
    }
    const uint8_t bounded = std::min(count, kHardCap);
    for (uint8_t i = 0; i < bounded; i++) {
        arr.push_back(table[i]);
    }
    return arr;
}

void DeserializePoseTable(Vec3s* dest,
                          uint8_t maxSlots,
                          const nlohmann::json& arr,
                          uint32_t netIdForDiag) {
    if (dest == nullptr || maxSlots == 0 || !arr.is_array()) {
        return;
    }
    const size_t incoming = arr.size();
    if (incoming > kHardCap) {
        SPDLOG_WARN("[SkelAnimeWire] Suspicious pose-table size {} for netId={} "
                    "(hard cap {}); clamping",
                    incoming, netIdForDiag, (int)kHardCap);
    }
    const size_t limit = std::min({(size_t)maxSlots,
                                   incoming,
                                   (size_t)kHardCap});
    for (size_t i = 0; i < limit; i++) {
        dest[i] = arr[i].get<Vec3s>();
    }
}

}  // namespace SkelAnimeWire
