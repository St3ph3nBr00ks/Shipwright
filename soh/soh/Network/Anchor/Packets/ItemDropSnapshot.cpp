#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "z64.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

// Network-spawn gate helpers — implemented in HookHandlers.cpp.
// Forward-declared here so HandlePacket_ItemDropSnapshot can bracket
// each per-drop Actor_Spawn the same way HandlePacket_ItemDropSync
// does.
void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                       int64_t spawnTimeMs,
                                       const std::string& killerTeamId);
void Anchor_EndNetworkItemDropSpawn(void);

/**
 * ITEM_DROP_SNAPSHOT — host → joining peer (targeted).
 *
 * #193 Phase 5 — late-join replay of in-flight EN_ITEM00 drops.
 *
 * Trigger: when a peer transitions into the host's scene (UPDATE_CLIENT_STATE
 * shows their sceneNum just changed to match host's, OR they just loaded
 * a save into host's scene), the host walks its local ACTORCAT_MISC for
 * EN_ITEM00 actors with an `ItemDropNetId` extension and broadcasts the
 * list to the named peer. The peer spawns each drop locally with the
 * extension stamped from the snapshot, so the pickup gate works
 * immediately on its newly-arrived drops.
 *
 * Idempotency: the receive site short-circuits per-drop on a netId
 * collision with an already-spawned local drop (peer might have
 * received the same drop via ITEM_DROP_SYNC if the original broadcast
 * arrived before the joiner's UPDATE_CLIENT_STATE made it to host).
 *
 * Wire shape:
 *   {
 *     "type":           "ITEM_DROP_SNAPSHOT",
 *     "schema":         1,
 *     "sceneNum":       17,
 *     "timeline":       0,
 *     "targetClientId": <peer>,
 *     "drops": [
 *       {
 *         "netId":          0xNNNNNNNN,
 *         "params":         0x00,
 *         "pos":            [x, y, z],
 *         "killerClientId": <player>,
 *         "spawnTimeMs":    <hostMonotonic>
 *       },
 *       ...
 *     ]
 *   }
 *
 * Bandwidth: ~80 bytes JSON per drop. A scene rarely holds more than
 * a handful of drops at once (despawn timer ~11 s on transient items),
 * so total snapshot weight is < 1 KB even in worst case. Sent at most
 * once per peer scene-transition.
 */

void Anchor::SendPacket_ItemDropSnapshot(uint32_t targetClientId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!::SceneAuthority::IsEffectiveHost()) return;
    if (targetClientId == 0 || targetClientId == ownClientId) return;

    int16_t sceneNum = (int16_t)gPlayState->sceneNum;

    nlohmann::json drops = nlohmann::json::array();
    Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
    while (it != nullptr) {
        if (it->id == ACTOR_EN_ITEM00 && it->update != nullptr) {
            const ItemDropNetId* ext =
                ObjectExtension::GetInstance().Get<ItemDropNetId>(it);
            if (ext != nullptr && ext->netId != 0) {
                nlohmann::json entry;
                entry["netId"]          = ext->netId;
                entry["params"]         = (int)(it->params & 0xFF);
                entry["pos"]            = nlohmann::json::array(
                    { it->world.pos.x, it->world.pos.y, it->world.pos.z });
                entry["killerClientId"] = ext->killerClientId;
                entry["killerTeamId"]   = ext->killerTeamId;
                entry["spawnTimeMs"]    = ext->spawnTimeMs;
                drops.push_back(std::move(entry));
            }
        }
        it = it->next;
    }

    if (drops.empty()) {
        SPDLOG_DEBUG("[ItemDropSnapshot] target={} sceneNum={} — no live drops to replay",
                     targetClientId, (int)sceneNum);
        return;
    }

    nlohmann::json payload;
    payload["type"]           = ITEM_DROP_SNAPSHOT;
    payload["sceneNum"]       = (int)sceneNum;
    payload["targetClientId"] = targetClientId;
    payload["drops"]          = std::move(drops);
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[ItemDropSnapshot] target={} sceneNum={} drops={}",
                targetClientId, (int)sceneNum, (int)payload["drops"].size());

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ItemDropSnapshot(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[ItemDropSnapshot] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[ItemDropSnapshot] Drop — local scene {} != snapshot scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    if (!payload.contains("drops") || !payload["drops"].is_array()) {
        SPDLOG_WARN("[ItemDropSnapshot] Drop — malformed drops field");
        return;
    }

    int spawned    = 0;
    int duplicated = 0;
    for (const auto& entry : payload["drops"]) {
        uint32_t netId          = (uint32_t)entry.value("netId", (uint32_t)0);
        u8       params         = (u8)entry.value("params", 0);
        uint32_t killerClientId = (uint32_t)entry.value("killerClientId", (uint32_t)0);
        std::string killerTeamId = entry.value("killerTeamId", std::string{});
        int64_t  spawnTimeMs    = (int64_t)entry.value("spawnTimeMs", (int64_t)0);

        if (netId == 0) continue;
        if (!entry.contains("pos") || !entry["pos"].is_array() ||
            entry["pos"].size() != 3) {
            continue;
        }
        Vec3f pos;
        pos.x = entry["pos"][0].get<float>();
        pos.y = entry["pos"][1].get<float>();
        pos.z = entry["pos"][2].get<float>();

        // Idempotency: skip if an EnItem00 with this netId already
        // exists locally (peer received ITEM_DROP_SYNC before this
        // snapshot arrived, or scene transition race).
        bool already = false;
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
        while (it != nullptr) {
            if (it->id == ACTOR_EN_ITEM00 && it->update != nullptr) {
                const ItemDropNetId* existing =
                    ObjectExtension::GetInstance().Get<ItemDropNetId>(it);
                if (existing != nullptr && existing->netId == netId) {
                    already = true;
                    break;
                }
            }
            it = it->next;
        }
        if (already) {
            duplicated++;
            continue;
        }

        Anchor_BeginNetworkItemDropSpawn(netId, killerClientId, spawnTimeMs, killerTeamId);
        Actor* spawnedActor = Actor_Spawn(&gPlayState->actorCtx, gPlayState,
                                           ACTOR_EN_ITEM00, pos.x, pos.y, pos.z,
                                           0, 0, 0, (s16)params);
        Anchor_EndNetworkItemDropSpawn();
        if (spawnedActor != nullptr) {
            spawned++;
        }
    }

    SPDLOG_INFO("[ItemDropSnapshot] Replayed sceneNum={} spawned={} duplicates_skipped={}",
                (int)sceneNum, spawned, duplicated);
}
