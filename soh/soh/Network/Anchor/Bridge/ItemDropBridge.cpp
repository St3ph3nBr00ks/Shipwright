// Refactor A.8 — ItemDrop extern "C" shim bridge.
//
// Eight C-linkage facades + two C++ receive-side helpers + the
// shared file-statics that drive the item-drop network pipeline.
// Moved from HookHandlers.cpp on 2026-06-01 per Plans/A.8_design_review.md
// (DR-5) Stage 3.8.
//
// Domain: item-drop (EN_ITEM00) sync. Shims for killer attribution,
// per-player drop exclusion, invisible-decorative tagging, the
// receive-side substitution-bypass query, and the cross-bridge OR
// with EnvActorBridge's host-side substitution gate.
//
// File-statics live here (definitions); HookHandlers.cpp sees them
// via the `extern` declarations in Bridge/ItemDropBridgeState.h. The
// shared-header pattern is intentional — DR-5 §"Shared dependencies"
// flagged that a pure-accessor approach would mean ~10+ getter/setter
// pairs across this domain. The hooks in HookHandlers' OnActorSpawn
// (EN_ITEM00) path read/write these directly through the header.

#include "Bridge/ItemDropBridgeState.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyHostBookkeeping.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <libultraship/libultraship.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

// Cross-bridge accessor — read EnvActorBridge's
// `g_isHostingPeerEnvActorDrop` for the IsReceivingNetworkItemDrop OR.
extern "C" bool Anchor_IsHostingPeerEnvActorDrop(void);

// ============================================================================
// File-static definitions (declared `extern` in ItemDropBridgeState.h).
// ============================================================================

// #193 Phase 2 — Item-drop killer-attribution shim.
//
// Game thread is single-threaded so file-scope statics suffice. Begin
// records the killerClientId before vanilla `Item_DropCollectible*`
// fires `Actor_Spawn(ACTOR_EN_ITEM00)`. End clears.
//
// Killer resolution:
//   - Item_DropCollectibleRandom(play, fromActor, ...): if fromActor's
//     EnemyNetId has a recorded damager (host-side bookkeeping populated
//     by ENEMY_DEFEATED arrival from a peer), use that. Else fall back
//     to the local client's id. Covers both "host kills a peer's enemy"
//     (rare; host's own kill) and "peer kills via DAMAGE_ENEMY routed
//     to host" (the dominant MP path — Damager map is populated).
//   - Item_DropCollectible / Item_DropCollectible2 (no fromActor): caller
//     uses the no-arg form, killer = local client. Phase 4 will refine
//     for env-actor drops if needed.
//
// State scope: only one `Item_DropCollectible*` invocation can be active
// at a time on the game thread, so a single static suffices. Depth
// counter handles nested calls (Phase 2 + 4): when
// `Item_DropCollectibleRandom` fires its inner-loop recursive
// `Item_DropCollectible` (z_en_item00.c:1797/1799), the outer Begin's
// killer attribution must persist through the inner Begin/End — depth
// counter ensures only the outermost Begin sets state and only the
// outermost End clears it.
uint32_t    g_pendingItemDropKillerClientId = 0;
std::string g_pendingItemDropKillerTeamId;   // Phase 1 (spec Q2)
int64_t     g_pendingItemDropSpawnTimeMs    = 0;
int         g_pendingItemDropDepth          = 0;

// Receive-side gate: set true while HandlePacket_ItemDropSync is calling
// Actor_Spawn so the OnActorSpawn ACTOR_EN_ITEM00 hook knows not to
// re-broadcast the drop (it's already a network drop). Mirrors the
// `isSpawningNetworkActor` pattern from ENEMY_SPAWN.
bool     g_isSpawningNetworkItemDrop     = false;
uint32_t g_pendingNetworkItemDropNetId   = 0;

// Thread-local flag set by Anchor_SpawnSyncedStickDrop to tag the
// next OnActorSpawn(EN_ITEM00) as a "decorative invisible companion"
// drop: the visible visual is provided by the offering actor's own
// drawn stick (gDekuBabaStickDropDL); this EN_ITEM00 is the
// interactive pickup collider with draw=NULL. The flag is consumed
// (cleared) by the OnActorSpawn host path when stamping the deferred
// broadcast record.
bool g_pendingItemDropInvisibleDecorative = false;

// #193 Phase 4 v3 exclusion list — per-player drop sources that
// must NOT round-trip through the sync pipeline. Each client
// triggers these locally on their own player-driven event (playing
// Sun's Song for Shot_Sun, the diving game for En_Ex_Ruppy, etc.);
// broadcasting host's drop would replicate it on peer where peer is
// ALSO independently triggering their own, producing duplicate
// pickups, and the peer-local-death-drop suppressor would wrongly
// kill peer's correct local drop on the assumption it would be
// replaced by host's.
//
// The shim wraps the call site: Anchor_BeginItemDropLocalOnly()
// sets a thread-local flag, the OnActorSpawn(EN_ITEM00) hook reads
// it and returns early (no broadcast on host, no suppression on
// peer), Anchor_EndItemDropLocalOnly() clears it. Pairs with
// Item_DropCollectible* and Actor_Spawn(EN_ITEM00, ...).
//
// z_player.c:7287 modal-completion phantom and z_en_ex_ruppy.c:351
// dive-game ruppies ALREADY route through the 0x8000 modal-phantom
// filter (HookHandlers.cpp:1804). Shot_Sun's drop at z_shot_sun.c:
// 179 does NOT — it's a regular Item_DropCollectible(MAGIC_LARGE)
// at a fixed scripted position. This shim is the remaining
// exclusion mechanism.
bool g_isLocalOnlyItemDrop = false;

// Deferred-broadcast queue (nut trajectory desync fix, log 281).
//
// `Item_DropCollectible(play, &pos, params)` sets the spawned EN_ITEM00's
// post-spawn random `world.rot.y` AT z_en_item00.c:1625, AFTER our
// OnActorSpawn(EN_ITEM00) hook has already fired. Broadcasting from the
// hook captures rot.y == 0, so each receiver runs its own local
// Rand_CenteredFloat for the trajectory and the nut/ammo lands in a
// different XZ spot per client. Sticks via the modal-offer path are
// unaffected (visual rep is the synced enemy actor itself, not an
// EN_ITEM00).
//
// Defer the broadcast: OnActorSpawn pushes a PendingItemDropBroadcast
// record; Anchor_EndItemDrop (which fires at z_en_item00.c:1639, AFTER
// the rotation assignment) drains the queue and sends ITEM_DROP_SYNC
// with the now-correct rot.y. `Item_DropCollectibleRandom`'s inner loop
// spawns multiple drops under the same outer Begin/End pair — all of
// them are drained together at the depth==0 End.
std::vector<PendingItemDropBroadcast> g_pendingItemDropBroadcasts;

// ============================================================================
// Shim definitions.
// ============================================================================

extern "C" void Anchor_SetPendingItemDropInvisibleDecorative(bool flag) {
    g_pendingItemDropInvisibleDecorative = flag;
}

extern "C" void Anchor_BeginItemDropLocalOnly(void) {
    g_isLocalOnlyItemDrop = true;
}
extern "C" void Anchor_EndItemDropLocalOnly(void) {
    g_isLocalOnlyItemDrop = false;
}

extern "C" void Anchor_BeginItemDrop(Actor* fromActor) {
    // Inner / nested call: keep outer's killer attribution intact.
    if (g_pendingItemDropDepth++ > 0) {
        return;
    }
    g_pendingItemDropSpawnTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!Anchor::Instance) {
        g_pendingItemDropKillerClientId = 0;
        return;
    }
    // Default: local client owns the kill (host's own kill OR an env actor
    // cut by local Link).
    g_pendingItemDropKillerClientId = Anchor::Instance->ownClientId;
    if (fromActor != nullptr) {
        const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(fromActor);
        if (ext != nullptr) {
            uint32_t damager = EnemyStateSync::HostBookkeeping::Instance().LookupDamager(ext->netId);
            if (damager != 0) {
                g_pendingItemDropKillerClientId = damager;
            }
        }
    }
}

extern "C" void Anchor_EndItemDrop(void) {
    if (g_pendingItemDropDepth > 0) {
        --g_pendingItemDropDepth;
    }
    // Only clear at the outermost End.
    if (g_pendingItemDropDepth == 0) {
        g_pendingItemDropKillerClientId = 0;
        g_pendingItemDropSpawnTimeMs = 0;

        // Drain deferred broadcasts — at this point Item_DropCollectible
        // (or _Random's loop) has finished setting each spawned actor's
        // world.rot.y, so the broadcast now carries the same rotation
        // both sides will use.
        if (!g_pendingItemDropBroadcasts.empty() &&
            Anchor::Instance != nullptr && Anchor::Instance->isConnected) {
            for (const auto& p : g_pendingItemDropBroadcasts) {
                if (p.actor == nullptr || p.actor->update == nullptr) continue;
                const s16 rotY = (s16)p.actor->world.rot.y;
                Anchor::Instance->SendPacket_ItemDropSync(
                    p.itemNetId, (u8)p.resolvedType,
                    p.actor->world.pos,
                    p.killerClientId, p.spawnTimeMs,
                    /*offererEnemyNetId=*/ 0u,
                    /*rotY=*/             rotY,
                    /*invisibleDecorative=*/ p.invisibleDecorative);
                SPDLOG_INFO("[ItemDropSync] Host broadcast (deferred) netId={} type=0x{:02X} "
                            "pos=({:.0f},{:.0f},{:.0f}) rotY={} killer={} spawnTimeMs={} "
                            "invisibleDecorative={}",
                            p.itemNetId, (int)p.resolvedType,
                            p.actor->world.pos.x, p.actor->world.pos.y, p.actor->world.pos.z,
                            (int)rotY, p.killerClientId, (long long)p.spawnTimeMs,
                            p.invisibleDecorative ? "true" : "false");
            }
        }
        g_pendingItemDropBroadcasts.clear();
    }
}

// #193 Phase 4 v2 — explicit-killer override. Used by
// `HandlePacket_EnvActorDrop` to attribute the host-side
// `Item_DropCollectible*` to the peer who cut the grass (peer's
// clientId carried in the packet's relay-injected `clientId` field).
// Pairs with the standard `Anchor_EndItemDrop`.
extern "C" void Anchor_BeginItemDropForKiller(uint32_t killerClientId) {
    if (g_pendingItemDropDepth++ > 0) {
        return;  // nested; outer scope wins
    }
    g_pendingItemDropSpawnTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_pendingItemDropKillerClientId = killerClientId;
}

// #231 / heart-vs-rupee desync — exposed for z_en_item00.c so the
// vanilla func_8001F404 substitution chain (heart→rupee at full HP,
// adult-stick→rupee, child-seeds→arrows, no-bow→cancel, etc.) can
// be skipped on receivers. Each substitution branches on the LOCAL
// client's gSaveContext (health, inventory, age) and produces a
// type-desync between host and peer when their game state differs.
// The host's broadcast carries the authoritative type — receivers
// must spawn that type verbatim, even if vanilla code would have
// substituted it locally.
//
// ORs the ItemDrop-domain `g_isSpawningNetworkItemDrop` (set during
// the ITEM_DROP_SYNC receive path) with EnvActorBridge's
// `g_isHostingPeerEnvActorDrop` (set during host's processing of a
// peer's EnvActorDrop). Both paths suppress func_8001F404; the
// flags are deliberately separate because the EnvActorDrop path
// must still broadcast (it's the host RE-substituting against host
// state that we suppress), whereas the ITEM_DROP_SYNC receive path
// must NOT re-broadcast.
extern "C" bool Anchor_IsReceivingNetworkItemDrop(void) {
    return g_isSpawningNetworkItemDrop || Anchor_IsHostingPeerEnvActorDrop();
}

// ============================================================================
// Receive-side helpers (C++ linkage). Called from
// HandlePacket_ItemDropSync + HandlePacket_ItemDropSnapshot to bracket
// the local Actor_Spawn so the OnActorSpawn EN_ITEM00 hook stamps the
// extension with the host's broadcast netId/killer/spawnTimeMs and
// suppresses re-broadcast.
//
// Increments the shim's depth counter so any inner Anchor_BeginItemDrop
// call (e.g., from a vanilla Item_DropCollectible invocation made
// during the receive-side re-spawn for animation-replication) sees
// depth>0 and bails out without overwriting the broadcast-supplied
// killer/spawnTime state. Pairs symmetrically with
// Anchor_EndNetworkItemDropSpawn.
// ============================================================================

void Anchor_BeginNetworkItemDropSpawn(uint32_t netId, uint32_t killerClientId,
                                       int64_t spawnTimeMs,
                                       const std::string& killerTeamId) {
    g_isSpawningNetworkItemDrop      = true;
    g_pendingNetworkItemDropNetId    = netId;
    g_pendingItemDropKillerClientId  = killerClientId;
    g_pendingItemDropKillerTeamId    = killerTeamId;
    g_pendingItemDropSpawnTimeMs     = spawnTimeMs;
    g_pendingItemDropDepth++;
}

void Anchor_EndNetworkItemDropSpawn(void) {
    if (g_pendingItemDropDepth > 0) {
        --g_pendingItemDropDepth;
    }
    g_isSpawningNetworkItemDrop      = false;
    g_pendingNetworkItemDropNetId    = 0;
    if (g_pendingItemDropDepth == 0) {
        g_pendingItemDropKillerClientId  = 0;
        g_pendingItemDropKillerTeamId.clear();
        g_pendingItemDropSpawnTimeMs     = 0;
    }
}
