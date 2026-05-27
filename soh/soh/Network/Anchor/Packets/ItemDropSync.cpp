#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"  // kSyncableActorCategories — Plan B step 6
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SyncedClaimableDrop.h"  // Plan B step 6
#include "soh/Network/Anchor/Common/DropAdapters/GroundDropAdapter.h"  // Plan B step 3
#include "soh/Network/Anchor/Common/DropAdapters/ModalOfferAdapter.h"  // Plan B step 6
#include "soh/ObjectExtension/ObjectExtension.h"  // EnemyNetId lookup — Plan B step 6
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "z64.h"
extern PlayState* gPlayState;
}

// Phase 2 receive-side gate — implemented in HookHandlers.cpp (file-scope
// statics). Forward-declared here so HandlePacket_ItemDropSync can bracket
// its `Actor_Spawn` call with the network-spawn flag, ensuring the
// `OnActorSpawn(ACTOR_EN_ITEM00)` hook stamps the host-supplied netId
// onto the receiver's local extension and skips re-broadcast.
void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                       int64_t spawnTimeMs);
void Anchor_EndNetworkItemDropSpawn(void);

/**
 * ITEM_DROP_SYNC — host → all clients (team-broadcast).
 *
 * #193 Phase 1 — packet plumbing only. The OnActorSpawn(ACTOR_EN_ITEM00)
 * sender hook + receiver Actor_Spawn + ItemDropNetId extension stamping
 * land in Phase 2 (next session). For now, Send/Handle are stubs that
 * exercise the wire format and dispatch chain.
 *
 * Wire fields:
 *   sceneNum        — sender's scene at send time (Pillar E ValidateSameScene).
 *   roomNum         — sender's room at send time.
 *   timeline        — Pillar B linkAge bit.
 *   netId           — unique per-drop identifier (item-scoped, not actor-scoped).
 *   params          — ITEM00_* enum value (post-resolution; ITEM00_FLEXIBLE
 *                     should never reach this point — the host's local
 *                     EnItem00_Init resolves it before broadcast).
 *   pos             — world-space spawn position.
 *   killerClientId  — player who triggered the drop. 0 = unattributed
 *                     (no killer-exclusivity window).
 *   spawnTimeMs     — host's monotonic clock at drop time. Receivers
 *                     compare against `now` for the killer-exclusive
 *                     grace window. Sender clocks aren't trusted for
 *                     freshness comparisons (see Heartbeat for the same
 *                     issue) but ARE used for ordering / deduplication.
 *   targetTeamId    — team-scope routing.
 *
 * Allowlist (Q7, host-side filter at sender): only transient drops
 * (rupees, hearts, ammo, magic, sticks, nuts, seeds). Progression items
 * (heart pieces, heart containers, small keys, tunics, shields) keep
 * per-player semantics — each client spawns its own copy via the
 * vanilla scripted path; this packet is not sent for them.
 */

void Anchor::SendPacket_ItemDropSync(uint32_t itemNetId, u8 itemParams,
                                     Vec3f pos, uint32_t killerClientId,
                                     int64_t spawnTimeMs,
                                     uint32_t offererEnemyNetId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]              = ITEM_DROP_SYNC;
    payload["targetTeamId"]      = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["sceneNum"]          = (int)gPlayState->sceneNum;
    payload["roomNum"]           = (int)gPlayState->roomCtx.curRoom.num;
    payload["netId"]             = itemNetId;
    payload["params"]            = (int)itemParams;
    payload["pos"]               = nlohmann::json::array({ pos.x, pos.y, pos.z });
    payload["killerClientId"]    = killerClientId;
    payload["spawnTimeMs"]       = spawnTimeMs;
    // Plan B step 6 — nonzero signals a modal-offer drop. Peer uses
    // this to find its mirror of the offering actor (Karebaba /
    // Dekubaba stem) by EnemyNetId and register it as a visual rep,
    // instead of spawning an EN_ITEM00. Zero (default) for normal
    // ground drops.
    payload["offererEnemyNetId"] = offererEnemyNetId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[ItemDropSync] Sending netId={} params=0x{:02X} pos=({:.0f},{:.0f},{:.0f}) "
                "killer={} sceneNum={} offererNetId={}",
                itemNetId, (int)itemParams, pos.x, pos.y, pos.z,
                killerClientId, (int)gPlayState->sceneNum, offererEnemyNetId);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ItemDropSync(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[ItemDropSync] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[ItemDropSync] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    uint32_t itemNetId       = (uint32_t)payload.value("netId", (uint32_t)0);
    u8       itemParams      = (u8)payload.value("params", 0);
    uint32_t killerClientId  = (uint32_t)payload.value("killerClientId", (uint32_t)0);
    int64_t  spawnTimeMs     = (int64_t)payload.value("spawnTimeMs", (int64_t)0);

    Vec3f pos = { 0.0f, 0.0f, 0.0f };
    if (payload.contains("pos") && payload["pos"].is_array() && payload["pos"].size() == 3) {
        pos.x = payload["pos"][0].get<float>();
        pos.y = payload["pos"][1].get<float>();
        pos.z = payload["pos"][2].get<float>();
    } else {
        SPDLOG_WARN("[ItemDropSync] Drop — malformed pos field");
        return;
    }

    // Plan B step 6 — modal-offer drop branch. Host broadcasts these
    // when ModalOfferAdapter::OfferGetItemNearby fires (Karebaba /
    // Dekubaba stem). `offererEnemyNetId != 0` signals "this is a
    // modal-offer drop, not a ground drop" — peer must NOT spawn an
    // EN_ITEM00 (the visual rep is the offering actor itself).
    uint32_t offererEnemyNetId = (uint32_t)payload.value("offererEnemyNetId", (uint32_t)0);
    if (offererEnemyNetId != 0) {
        uint32_t authorityClientIdForOffer = (uint32_t)payload.value("clientId", (uint32_t)0);

        // Find peer's mirror of the offering actor by EnemyNetId. Walk
        // every synced-actor category. The offering actor must already
        // exist locally (synced via ENEMY_SPAWN / ENEMY_STATE) by the
        // time the modal-offer broadcast arrives.
        Actor* mirror = nullptr;
        for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
            Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
            while (a != nullptr) {
                const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                if (ext != nullptr && ext->netId == offererEnemyNetId) {
                    mirror = a;
                    break;
                }
                a = a->next;
            }
            if (mirror != nullptr) break;
        }

        if (mirror == nullptr) {
            SPDLOG_WARN("[ItemDropSync] modal-offer dropId={} offererNetId={} — no peer actor "
                        "with matching EnemyNetId; mirror-rep registration skipped (peer's "
                        "ENEMY_DEFEATED handler will still terminate the actor naturally)",
                        itemNetId, offererEnemyNetId);
            return;
        }

        // Register peer's mirror actor in the SyncedClaimableDrop
        // registry. The Drop's dropId == offererEnemyNetId by
        // convention (set by ModalOfferAdapter on host).
        SyncedClaimableDrop::Registry& reg = SyncedClaimableDrop::Registry::Instance();
        SyncedClaimableDrop::Drop* drop = reg.Find(itemNetId);
        if (drop == nullptr) {
            drop = reg.AllocateDrop(itemNetId, authorityClientIdForOffer,
                                    (int16_t)itemParams, pos,
                                    sceneNum, /*roomNum=*/ 0, /*linkAge=*/ 0,
                                    killerClientId, spawnTimeMs);
        }
        if (drop != nullptr) {
            if (drop->adapter == nullptr) {
                drop->adapter = SyncedClaimableDrop::ModalOfferAdapter::GetInstance();
            }
            reg.RegisterVisualRep(itemNetId, mirror);
        }

        SPDLOG_INFO("[ItemDropSync] rx modal-offer dropId={} offererNetId={} mirror=found "
                    "type=0x{:02X}",
                    itemNetId, offererEnemyNetId, (int)itemParams);
        return;
    }

    // Idempotency: walk ACTORCAT_MISC for an existing EN_ITEM00 with a
    // matching ItemDropNetId extension. Skip if found (we already
    // spawned this drop, e.g. duplicate broadcast or self-echo).
    Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
    while (it != nullptr) {
        if (it->id == ACTOR_EN_ITEM00 && it->update != nullptr) {
            const ItemDropNetId* existing =
                ObjectExtension::GetInstance().Get<ItemDropNetId>(it);
            if (existing != nullptr && existing->netId == itemNetId) {
                SPDLOG_DEBUG("[ItemDropSync] Drop — netId={} already spawned locally",
                             itemNetId);
                return;
            }
        }
        it = it->next;
    }

    // Spawn the receiver's local EN_ITEM00 with the host's params.
    //
    // Use vanilla `Item_DropCollectible` (NOT raw `Actor_Spawn`) so the
    // peer's drop runs the same bouncy-fall animation as host's: post-
    // spawn velocity setup (`velocity.y = 8.0f`, `speedXZ = 2.0f`,
    // `gravity = -0.9f`), random rotation, scale-grow, and the
    // `func_8001E304` actionFunc that drives the falling/bouncing.
    //
    // Without this, peer's drop appears static at the exact spawn
    // position (raw Actor_Spawn → Init runs default actionFunc, no
    // velocity setup) — observed in field log 2026-05-06.
    //
    // `Anchor_BeginNetworkItemDropSpawn` increments the shim's depth
    // counter so the inner `Anchor_BeginItemDrop(NULL)` call inside
    // Item_DropCollectible sees depth>0 and skips overwriting the
    // host-supplied killer/spawnTime state. The OnActorSpawn EN_ITEM00
    // hook reads `g_isSpawningNetworkItemDrop = true` and routes to
    // the receive-side branch (stamp extension, skip broadcast).
    Anchor_BeginNetworkItemDropSpawn(itemNetId, killerClientId, spawnTimeMs);
    EnItem00* spawned = Item_DropCollectible(gPlayState, &pos, (s16)itemParams);
    Anchor_EndNetworkItemDropSpawn();

    if (spawned == nullptr) {
        SPDLOG_WARN("[ItemDropSync] Item_DropCollectible failed for netId={} type=0x{:02X}",
                    itemNetId, (int)itemParams);
        return;
    }

    SPDLOG_INFO("[ItemDropSync] rx netId={} type=0x{:02X} pos=({:.0f},{:.0f},{:.0f}) killer={}",
                itemNetId, (int)itemParams, pos.x, pos.y, pos.z, killerClientId);

    // Plan B step 3 — populate the SyncedClaimableDrop registry from
    // the peer-side spawn. Pairs with the host-side population in
    // HookHandlers.cpp OnActorSpawn(EN_ITEM00). The sender's clientId
    // (relay-injected `clientId` field) is the drop's authority — host
    // who broadcast this drop.
    //
    // Note: roomNum and linkAge are not currently in the packet
    // schema (per design doc §6 — schema unchanged for v1). Default
    // to 0; these fields are not yet consumed by any code path. If
    // future adapters need them they can be added to the wire format.
    uint32_t authorityClientId = (uint32_t)payload.value("clientId", (uint32_t)0);
    SyncedClaimableDrop::GroundDropAdapter::GetInstance()->RegisterSpawn(
        itemNetId,
        authorityClientId,
        (int16_t)itemParams,
        pos,
        sceneNum,
        /*roomNum=*/  0,
        /*linkAge=*/  0,
        killerClientId,
        spawnTimeMs,
        &spawned->actor);
}
