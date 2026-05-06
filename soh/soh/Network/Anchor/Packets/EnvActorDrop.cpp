#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
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
}

// Killer-attribution shim defined in HookHandlers.cpp. Forward-declared
// here so HandlePacket_EnvActorDrop can attribute the resulting
// ITEM_DROP_SYNC broadcast to the peer who cut the grass (rather than
// to the host who is merely running the drop on peer's behalf).
extern "C" void Anchor_BeginItemDropForKiller(uint32_t killerClientId);
extern "C" void Anchor_EndItemDrop(void);

/**
 * ENV_ACTOR_DROP — peer → host (targeted at room host).
 *
 * #193 Phase 4 v2 — peer notifies host that its local env actor (grass
 * shrub, etc.) was cut and would have dropped an item. Peer suppresses
 * the local drop; host runs the drop on its side; ITEM_DROP_SYNC fans
 * out via the existing Phase 2 broadcast pipeline.
 *
 * Eliminates the v1 simultaneous-cut double-drop: when both clients
 * cut their local copy of the same grass shrub, host's local Item_Drop*
 * run and host's broadcast spawn TWO drops on peer. Under v2, peer's
 * local drop is suppressed and host de-duplicates by checking whether
 * its local actor is still alive at receive time.
 *
 * Wire shape:
 *   {
 *     "type":               "ENV_ACTOR_DROP",
 *     "schema":             1,
 *     "sceneNum":           17,
 *     "timeline":           0,
 *     "targetClientId":     <effective room host>,
 *     "netId":              0xNNNNNNNN,
 *     "dropParam":          0xXX,           // ITEM00_* enum (single-item path)
 *     "dropParamForRandom": 0xXX,           // RNG group (Item_DropCollectibleRandom path; 0 = unused)
 *     "pos":                [x, y, z]
 *   }
 *
 * The two drop-param fields disambiguate which `Item_DropCollectible*`
 * entry point to call on host:
 *   dropParamForRandom != 0 → Item_DropCollectibleRandom (En_Kusa
 *                              TYPE_0/TYPE_2 path; param is the RNG
 *                              group << 4).
 *   dropParamForRandom == 0 → Item_DropCollectible (TYPE_1 path; param
 *                              is a specific ITEM00_* enum).
 *
 * Host-side dedup: walk the syncable actor categories for an actor
 * matching `netId`. If the actor is still alive on host (peer cut their
 * copy first), host runs the drop. If the actor is dead on host (host
 * already cut their own copy and produced a drop), the request is
 * silently dropped (host already handled it).
 */

void Anchor::SendPacket_EnvActorDrop(uint32_t netId, s16 dropParam,
                                     s16 dropParamForRandom, Vec3f pos) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;  // host doesn't notify itself

    uint32_t targetId = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    if (targetId == 0 || targetId == ownClientId) return;

    nlohmann::json payload;
    payload["type"]               = ENV_ACTOR_DROP;
    payload["targetClientId"]     = targetId;
    payload["sceneNum"]           = (int)gPlayState->sceneNum;
    payload["netId"]              = netId;
    payload["dropParam"]          = (int)dropParam;
    payload["dropParamForRandom"] = (int)dropParamForRandom;
    payload["pos"]                = nlohmann::json::array({ pos.x, pos.y, pos.z });
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[EnvActorDrop] Sending netId={} dropParam=0x{:02X} random=0x{:02X} target={}",
                netId, (int)dropParam, (int)dropParamForRandom, targetId);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnvActorDrop(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    // Host-only handler. The packet was targeted at us via targetClientId
    // — relay routing already filtered, but defence in depth.
    if (!::SceneAuthority::IsMyCurrentRoomHost()) return;

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[EnvActorDrop] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[EnvActorDrop] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    uint32_t netId              = (uint32_t)payload.value("netId", (uint32_t)0);
    s16      dropParam          = (s16)payload.value("dropParam", 0);
    s16      dropParamForRandom = (s16)payload.value("dropParamForRandom", 0);
    uint32_t senderClientId     = (uint32_t)payload.value("clientId", (uint32_t)0);

    if (netId == 0) {
        SPDLOG_WARN("[EnvActorDrop] Drop — netId == 0");
        return;
    }

    Vec3f pos = { 0.0f, 0.0f, 0.0f };
    if (payload.contains("pos") && payload["pos"].is_array() && payload["pos"].size() == 3) {
        pos.x = payload["pos"][0].get<float>();
        pos.y = payload["pos"][1].get<float>();
        pos.z = payload["pos"][2].get<float>();
    } else {
        SPDLOG_WARN("[EnvActorDrop] Drop — malformed pos field");
        return;
    }

    // Walk the syncable actor categories for the matching netId. If the
    // actor exists and is alive on host, run the drop. If it's already
    // dead (host cut their copy first and the drop already fired), skip
    // — this is the de-dup path that eliminates the simultaneous-cut
    // double-drop case.
    bool hostActorAlive = false;
    for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
        Actor* it = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
        while (it != nullptr) {
            if (it->update != nullptr) {
                const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(it);
                if (ext != nullptr && ext->netId == netId) {
                    hostActorAlive = true;
                    break;
                }
            }
            it = it->next;
        }
        if (hostActorAlive) break;
    }
    if (!hostActorAlive) {
        SPDLOG_INFO("[EnvActorDrop] netId={} — actor not alive on host (already cut or never spawned); skipping drop",
                    netId);
        return;
    }

    // Run the drop with peer's clientId as the killer attribution. The
    // OnActorSpawn(EN_ITEM00) hook reads g_pendingItemDropKillerClientId
    // and broadcasts ITEM_DROP_SYNC with that value; peer (the cutter)
    // gets the 3s exclusivity window on its own drop.
    Anchor_BeginItemDropForKiller(senderClientId);
    if (dropParamForRandom != 0) {
        Item_DropCollectibleRandom(gPlayState, NULL, &pos, dropParamForRandom);
        SPDLOG_INFO("[EnvActorDrop] netId={} ran Item_DropCollectibleRandom param=0x{:02X} killer={}",
                    netId, (int)dropParamForRandom, senderClientId);
    } else {
        Item_DropCollectible(gPlayState, &pos, dropParam);
        SPDLOG_INFO("[EnvActorDrop] netId={} ran Item_DropCollectible param=0x{:02X} killer={}",
                    netId, (int)dropParam, senderClientId);
    }
    Anchor_EndItemDrop();
}
