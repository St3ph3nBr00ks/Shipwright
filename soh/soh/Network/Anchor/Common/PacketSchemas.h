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

namespace PacketSchemas {

// Current local schema per packet type. Bump when adding a new field.
// Add new packet types here as they're introduced.
inline const std::unordered_map<std::string, int> kPacketSchemas = {
    {"ALL_CLIENT_STATE", 1},
    {"DAMAGE_ENEMY", 3},  // bump-history: 2 (#174/#175 damageEffect+atHitEffect), 3 (Pillar B timeline).
    {"DAMAGE_PLAYER", 1},
    {"DISABLE_ANCHOR", 1},
    {"ENEMY_HIT_PLAYER", 2},  // bumped 2026-04-26 (Pillar B): timeline field.
    {"ENEMY_STATE", 4},  // bump-history: 1 (Pillar C2 Phase 4 — unified enemy lifecycle), 2 (Generic NPC State Sync Phase 0 — added "scope" field), 3 (per-torch lit-state sync — added "syokudaiLitTimer" field), 4 (KB-44 — added "bossGomaChildrenState" array field for Boss_Goma larva death tracking sync).
    {"ENTRANCE_DISCOVERED", 1},
    {"GAME_COMPLETE", 1},
    {"GIVE_ITEM", 1},
    {"HANDSHAKE", 1},
    {"HEARTBEAT", 1},  // #194 follow-up — every-client liveness signal sent from network thread.
    {"ITEM_DROP_SYNC", 1},  // #193 Phase 1 — host-fanout of EnItem00 drops with killer attribution.
    {"ITEM_COLLECTED", 1},  // #193 Phase 1 / race A — host arbitration broadcast; receivers Actor_Kill or apply pickup based on winner clientId.
    {"ITEM_PICKUP_REQUEST", 1},  // #193 race A mitigation — peer→host pickup arbitration request.
    {"ITEM_DROP_SNAPSHOT", 1},  // #193 Phase 5 — late-join replay of in-flight EnItem00 drops.
    {"ENV_ACTOR_DROP", 1},      // #193 Phase 4 v2 — peer→host env-actor cut drop request.
    {"OCARINA_SFX", 1},
    {"PLAYER_SFX", 1},
    {"PLAYER_UPDATE", 2},  // bumped for unk_860 (Deku-Stick burning timer) — drives remote flame VFX.
    {"REQUEST_TEAM_STATE", 1},
    {"REQUEST_TELEPORT", 1},
    {"SCENE_ACTOR_NETIDS", 1},  // KB-18 (#177) Option 4 — host-authoritative netId snapshot.
    {"SCENE_TRANSITION_HANDOFF", 2},  // bumped 2026-04-26 (Pillar B): timeline field.
    {"BOSS_EXIT_TEAM_WARP", 1},        // Team co-warp on synced boss-room exit.
    {"BOSS_GOMA_LOOKED_AT", 1},        // Peer→host bridge for Boss_Goma intro state-3 trigger.
    {"MIDO_POST_DEKU_LEAVE", 1},       // Dialog→peers broadcast for Mido's post-Deku-Tree confrontation walk-away.
    {"CUTSCENE_TEXT_ADVANCE", 1},      // #191 Peer→host vote-skip for cutscene-internal textboxes.
    {"CUTSCENE_TEXT_ADVANCED", 1},     // #191 Host→all broadcast when textbox advance triggers.
    {"SERVER_MESSAGE", 1},
    {"SET_CHECK_STATUS", 1},
    {"SET_FLAG", 1},
    {"TELEPORT_TO", 2},  // bumped 2026-04-27 (Pillar B Phase 5): linkAge + sceneNum for cross-timeline teleport.
    {"UNSET_FLAG", 1},
    {"UPDATE_BEANS_COUNT", 1},
    {"UPDATE_CLIENT_STATE", 1},
    {"UPDATE_DUNGEON_ITEMS", 1},
    {"UPDATE_ROOM_STATE", 1},
    {"UPDATE_TEAM_STATE", 1},
    {"WORLD_FLAG_SET", 1},        // Pillar C v1
    {"WORLD_FLAG_UNSET", 1},      // Pillar C v1 unset symmetry
    {"WORLD_STATE_REQUEST", 1},   // Pillar C v1 late-join
    {"WORLD_STATE_SNAPSHOT", 1},  // Pillar C v1 late-join
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

}  // namespace PacketSchemas
