#pragma once

// Packet schema versioning (Pillar F).
//
// Every packet's JSON includes a top-level `schema` integer field
// (default 1 for current packets at Pillar F rollout). Receivers read
// schema to decode optional fields safely. Adding a field bumps the
// packet's local schema by 1; senders clamp the optional field on
// peer's `peerMaxSchema[packetType] >= N`.
//
// See Claude/Plans/pillar_f_schema_versioning.md.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Anchor {

// Current local schema per packet type. Bump when adding a new field.
// Add new packet types here as they're introduced.
inline const std::unordered_map<std::string, int> kPacketSchemas = {
    {"ALL_CLIENT_STATE", 1},
    {"DAMAGE_ENEMY", 1},
    {"DAMAGE_PLAYER", 1},
    {"DISABLE_ANCHOR", 1},
    {"ENEMY_DEFEATED", 1},
    {"ENEMY_HIT_PLAYER", 1},
    {"ENEMY_RESPAWN", 1},
    {"ENEMY_SPAWN", 1},
    {"ENEMY_UPDATE", 1},
    {"ENTRANCE_DISCOVERED", 1},
    {"GAME_COMPLETE", 1},
    {"GIVE_ITEM", 1},
    {"HANDSHAKE", 1},
    {"OCARINA_SFX", 1},
    {"PLAYER_SFX", 1},
    {"PLAYER_UPDATE", 1},
    {"REQUEST_TEAM_STATE", 1},
    {"REQUEST_TELEPORT", 1},
    {"SCENE_TRANSITION_HANDOFF", 1},
    {"SERVER_MESSAGE", 1},
    {"SET_CHECK_STATUS", 1},
    {"SET_FLAG", 1},
    {"TELEPORT_TO", 1},
    {"UNSET_FLAG", 1},
    {"UPDATE_BEANS_COUNT", 1},
    {"UPDATE_CLIENT_STATE", 1},
    {"UPDATE_DUNGEON_ITEMS", 1},
    {"UPDATE_ROOM_STATE", 1},
    {"UPDATE_TEAM_STATE", 1},
};

// Returns the current local schema for a packet type, or 1 if unknown
// (defensive default — unknown packets ride at the baseline schema).
inline int GetPacketSchema(const std::string& packetType) {
    auto it = kPacketSchemas.find(packetType);
    return it != kPacketSchemas.end() ? it->second : 1;
}

// Returns true when the peer's HANDSHAKE-recorded maxSchema for a packet
// type is at least the required schema. Sender uses this to decide
// whether to include schema-N optional fields.
//
// Defaults to true when the peer's maxSchema is unknown (e.g. legacy
// peer pre-Pillar-F): the sender should attempt to include the field
// rather than silently strip it; if the receiver doesn't understand,
// it deserialises to default which is safe.
bool PeerSupportsField(uint32_t clientId, const std::string& packetType,
                       int requiredSchema);

}  // namespace Anchor
