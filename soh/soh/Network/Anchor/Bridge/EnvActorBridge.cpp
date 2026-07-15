// Refactor A.8 — EnvActor extern "C" shim bridge.
//
// Seven C-linkage facades for env-actor drop + destroy sync (grass
// cuts, breakable containers, etc.). Moved from HookHandlers.cpp on
// 2026-06-01 per Plans/A.8_design_review.md (DR-5) Stage 3.8.
//
// Domain: EnvActor sync — peer-cut routing through host arbitration,
// host-side drop-substitution bypass, and the synchronous destroy
// broadcast helper used by EnKusa / breakable containers / future
// regrow-class env actors.
//
// File-statics relocated here from HookHandlers.cpp:
//   - g_isHostingPeerEnvActorDrop  (file-static bool, exposed via
//     accessor for IsReceivingNetworkItemDrop's OR with the
//     ItemDrop-domain `g_isSpawningNetworkItemDrop`).
//   - g_isApplyingNetworkEnvActorDestroy  (file-static thread_local
//     bool, used as echo-suppress inside HandlePacket_EnvActorDestroy's
//     apply path).

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemyNetId.h"  // #243.7.2 — explicit (was transitive via Anchor.h)
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <libultraship/libultraship.h>
#include <libultraship/bridge/consolevariablebridge.h>  // [Diag] gEnhancements.PendingBugsDiag CVar gate

extern "C" {
#include "z64.h"
#include "macros.h"     // GET_PLAYER
#include "functions.h"  // Item_DropCollectible / Item_DropCollectibleRandom
#include "variables.h"  // gSaveContext
#include "src/overlays/actors/ovl_En_Kusa/z_en_kusa.h"  // EnKusa cast for collider.base.at
extern PlayState* gPlayState;
}

// Vanilla heart-vs-rupee substitution chain. Forward-decl matches the
// definition in soh/src/code/z_en_item00.c. Used by the peer pre-
// substitute path in Anchor_DropCollectibleEnvActor.
extern "C" s16 func_8001F404(s16 dropId);

// ============================================================================
// File-statics owned by this domain.
// ============================================================================

// Host-side gate: set true while HandlePacket_EnvActorDrop is calling
// Item_DropCollectible* on the host to fulfil a peer's grass-cut. The
// vanilla func_8001F404 substitution chain in z_en_item00.c (heart→
// rupee at full HP, etc.) keys on the LOCAL gSaveContext, so on the
// host this substitutes against host's state instead of the cutter's.
// Suppressing substitution here makes the host's broadcast carry the
// peer-intended drop type verbatim. Distinct from
// g_isSpawningNetworkItemDrop (ItemDropBridge domain) because on this
// path we DO want the resulting OnActorSpawn(EN_ITEM00) to broadcast
// ITEM_DROP_SYNC.
static bool g_isHostingPeerEnvActorDrop = false;

// Thread-local echo-suppress flag. Set inside
// HandlePacket_EnvActorDestroy's per-actor dispatch loop so
// Anchor_BroadcastEnvActorDestroy short-circuits when called as part
// of applying a received destroy (e.g. EnKusa_SetupCut chain). See
// the comment block above Anchor_BroadcastEnvActorDestroy below for
// the historical rationale (the original ledger-based guard broke
// bidirectional sync for cyclic env actors; thread-local replaces).
static thread_local bool g_isApplyingNetworkEnvActorDestroy = false;

// #276 — thread-local parent-cull-context flag. Set by Obj_Mure2's
// distance-cull state (func_80B9A6F8 → ObjMure2_CleanupAndDie at
// z_obj_mure2.c:200-213) immediately before its Actor_Kill loop
// over actorSpawnPtrList children. The OnActorKill hook in
// HookHandlers.cpp reads this flag via Anchor_IsObjMure2CullingChildren
// and suppresses the ENEMY_DEFEATED broadcast for the cull-class kills.
//
// Each peer's local Obj_Mure2 makes the same distance-based cull
// decision INDEPENDENTLY (vanilla state machine fires on local
// player's distance, not host's). So host's broadcast of these cull
// kills arrives at peer as 12 pendingKill WARN lines (log 524
// 19:17:33.187 / 19:17:49.247) — peer has no live actors to clean up.
//
// IMPORTANT — distinct from En_Kusa's cut-state sync path. The legit
// "player chopped grass" case routes through EnKusa_SetupCut (line 455-
// 473 in z_en_kusa.c), which fires Anchor_BroadcastEnvActorDestroy
// via the ENV_ACTOR_DESTROY packet family. TYPE_0 grass (params&3 == 0)
// uses Actor_Kill directly at z_en_kusa.c:347 WITHOUT this thread-local
// being set, so its ENEMY_DEFEATED broadcast still fires correctly.
// Only the parent-driven cull burst is suppressed.
static thread_local bool g_isObjMure2CullingChildren = false;

// Accessor for the ItemDropBridge domain's IsReceivingNetworkItemDrop
// shim, which ORs this flag with its own g_isSpawningNetworkItemDrop.
// Kept as a function rather than `extern` linkage so the underlying
// static stays internal-linkage and the cross-bridge contract is a
// single named call.
extern "C" bool Anchor_IsHostingPeerEnvActorDrop(void) {
    return g_isHostingPeerEnvActorDrop;
}

// #276 — Obj_Mure2 cull-context bracket helpers, called from vanilla
// z_obj_mure2.c around the Actor_Kill loop in ObjMure2_CleanupAndDie.
// Begin/End must be balanced (single-threaded; vanilla actor update
// runs on the game thread). HookHandlers.cpp OnActorKill checks the
// flag via Anchor_IsObjMure2CullingChildren below.
extern "C" void Anchor_BeginObjMure2Cull(void) {
    g_isObjMure2CullingChildren = true;
}
extern "C" void Anchor_EndObjMure2Cull(void) {
    g_isObjMure2CullingChildren = false;
}
extern "C" bool Anchor_IsObjMure2CullingChildren(void) {
    return g_isObjMure2CullingChildren;
}

// ============================================================================
// Shim definitions.
// ============================================================================

// #193 Phase 4 v2 — env-actor drop wrapper, replaces direct
// `Item_DropCollectible(play, pos, params)` calls in env-actor
// destroy paths (En_Kusa, etc.). Behavioural matrix:
//
//   Disconnected:       call vanilla Item_DropCollectible.
//   Connected, host:    call vanilla Item_DropCollectible. The
//                       OnActorSpawn(EN_ITEM00) hook broadcasts
//                       ITEM_DROP_SYNC.
//   Connected, peer,
//   actor has netId:    suppress local drop; send ENV_ACTOR_DROP to
//                       host. Host runs the drop and broadcasts; peer
//                       receives the broadcast and spawns the drop.
//   Connected, peer,
//   actor has no netId: call vanilla (env actor not in the synced
//                       allowlist — fall back to v1 behaviour). Should
//                       not occur once Phase 4 v2 admits all four
//                       env-actor IDs to IsSyncedWorldActor.
//
// #193 Phase 4 v3 — return type changed from `void` to `EnItem00*` so
// capture-and-modify call sites (Bg_Haka_Tubo, Bg_Spot18_Basket — set
// velocity.y / shape.rot.y on the spawned actor) keep compiling. On
// the peer-suppress path the wrapper returns NULL; the caller's
// `if (collectible != NULL)` post-modify branch becomes a no-op,
// which is a cosmetic-only effect (peer's broadcast-spawned drops
// don't inherit the fan-out velocity but land at the right position
// and are pickable).
//
// #193 Phase 4 v3 race-D mitigation — peer-side scene check before
// sending. If host has left the scene (or no host found in clients
// map), the ENV_ACTOR_DROP would either be applied on host's wrong
// scene or silently dropped at the receive guard. Falling back to a
// local Item_DropCollectible preserves a visible drop on this client;
// same shape as the offline branch above (single client, single drop,
// no broadcast). Strictly better than silent loss.
//
// Policy 3 (cutter's vanilla wins) — peer pre-substitutes the dropId
// against PEER'S gSaveContext via func_8001F404 BEFORE sending. Host's
// HandlePacket_EnvActorDrop brackets the resulting Item_DropCollectible
// with Anchor_BeginHostingPeerEnvActorDrop so substitution is bypassed
// on the host side. Net effect: the cutter's local single-player
// vanilla outcome (heart-vs-rupee, adult-stick→rupee, no-bow→cancel,
// etc.) is broadcast verbatim to everyone.
extern "C" EnItem00* Anchor_DropCollectibleEnvActor(PlayState* play, Actor* envActor,
                                                    Vec3f* pos, s16 params) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        return Item_DropCollectible(play, pos, params);
    }
    if (::SceneAuthority::IsMyCurrentRoomHost()) {
        return Item_DropCollectible(play, pos, params);
    }
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(envActor);
    if (ext == nullptr || ext->netId == 0) {
        return Item_DropCollectible(play, pos, params);
    }
    // Race-D peer-side scene check.
    uint32_t hostId = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    auto hostIt = Anchor::Instance->clients.find(hostId);
    if (hostIt == Anchor::Instance->clients.end() ||
        hostIt->second.sceneNum != (s16)gPlayState->sceneNum) {
        return Item_DropCollectible(play, pos, params);
    }
    // Policy 3: pre-substitute on peer side against peer's state.
    // Mirrors Item_DropCollectible's own pre-spawn substitution path
    // (z_en_item00.c:1622-1631). The 0x8000 param-bit (also called
    // param8000 in z_en_item00.c) means "skip substitution"; preserve
    // that semantic by forwarding the raw params unchanged.
    s16 paramBody = params & 0x00FF;
    s16 paramFlags = params & ~0x00FF;
    if (!(params & 0x8000)) {
        s16 resolved = func_8001F404(paramBody);
        if (resolved == -1 || resolved == 0xFF) {
            // Peer's vanilla would have suppressed the drop entirely
            // (e.g. arrows when no bow owned). No broadcast — match
            // single-player behaviour exactly.
            return nullptr;
        }
        paramBody = resolved;
    }
    s16 resolvedParams = (s16)((paramFlags & 0xFF00) | (paramBody & 0x00FF));
    if (resolvedParams != params) {
        SPDLOG_INFO("[EnvActorDrop] peer pre-substituted params=0x{:04X} -> 0x{:04X} (cutter HP {}/{})",
                    (int)(uint16_t)params, (int)(uint16_t)resolvedParams,
                    (int)gSaveContext.health, (int)gSaveContext.healthCapacity);
    }
    // [Diag] pending-bugs 2026-07-15 — delayed-rupee bug diagnostic.
    // Log the cutter identity so future recurrences can be traced back
    // to the actor that struck the grass. When AC_HIT fires on
    // EN_KUSA's collider, `collider.base.at` points to the AT-collider's
    // attached actor (the striker). If that pointer is null, the strike
    // came from something other than a standard AT-vs-AC collision
    // (e.g. a `Actor_HasParent` lift-throw exit path). Also log local
    // player distance + weapon state so we can rule out player-side
    // action. See Claude/Analysis/delayed_green_rupee_from_grass_log717_2026-07-15.md.
    if (CVarGetInteger("gEnhancements.PendingBugsDiag", 0)) {
        Actor* atActor = nullptr;
        if (envActor->id == ACTOR_EN_KUSA) {
            atActor = ((EnKusa*)envActor)->collider.base.at;
        }
        Player* localLink = GET_PLAYER(gPlayState);
        float dx = envActor->world.pos.x - localLink->actor.world.pos.x;
        float dy = envActor->world.pos.y - localLink->actor.world.pos.y;
        float dz = envActor->world.pos.z - localLink->actor.world.pos.z;
        float dist3D = sqrtf(dx*dx + dy*dy + dz*dz);
        SPDLOG_INFO("[EnvActorDrop.diag] cutter envActor.id={} netId={} envActor.pos=({:.0f},{:.0f},{:.0f}) "
                    "at.id={} at.pos=({:.0f},{:.0f},{:.0f}) at.cat={} "
                    "localLink.pos=({:.0f},{:.0f},{:.0f}) dist3D={:.0f} "
                    "heldItemAction={} stateFlags1=0x{:08X} meleeWeaponState={}",
                    envActor->id, ext->netId,
                    envActor->world.pos.x, envActor->world.pos.y, envActor->world.pos.z,
                    atActor ? (int)atActor->id : -1,
                    atActor ? atActor->world.pos.x : 0.0f,
                    atActor ? atActor->world.pos.y : 0.0f,
                    atActor ? atActor->world.pos.z : 0.0f,
                    atActor ? (int)atActor->category : -1,
                    localLink->actor.world.pos.x, localLink->actor.world.pos.y, localLink->actor.world.pos.z,
                    dist3D,
                    (int)localLink->heldItemAction,
                    (unsigned)localLink->stateFlags1,
                    (int)localLink->meleeWeaponState);
    }

    // Peer with a synced env actor and host in scope: notify host,
    // suppress local drop.
    Anchor::Instance->SendPacket_EnvActorDrop(ext->netId, resolvedParams, /*forRandom=*/0, *pos);
    return nullptr;
}

// #193 Phase 4 v2 — sibling for `Item_DropCollectibleRandom` from
// env-actor sites (En_Kusa TYPE_0/TYPE_2 path passes a "drop group"
// param shifted up by 4, NOT a specific ITEM00_*). Same behavioural
// matrix as Anchor_DropCollectibleEnvActor; routes the random param
// through `dropParamForRandom` so the host re-dispatches via
// `Item_DropCollectibleRandom`. Phase 4 v3 race-D mitigation
// applies symmetrically. Random spawns don't have a single-actor
// return-value capture pattern, so the wrapper stays `void`.
//
// Policy 3 known gap (2026-05-31): the random path on host runs the
// random-table RNG pick + FLEXIBLE substitution chain + final
// func_8001F404 against HOST's gSaveContext. The host-side bracket
// `Anchor_BeginHostingPeerEnvActorDrop` skips the final func_8001F404
// but NOT the FLEXIBLE chain at z_en_item00.c:1755-1801, which keys on
// host's health/magic/ammo/rupees. Full policy 3 for the random path
// would require either:
//   (a) sending cutter's gSaveContext shadow with the packet so host
//       swaps state during the call, or
//   (b) peer pre-resolves the full chain locally (dry-run helper).
// Affects EnKusa TYPE_0/TYPE_2, Obj_Mure, and other random-table env
// actors. Single-drop EnKusa TYPE_1 (the most common heart drop path)
// IS fully policy-3 compliant via Anchor_DropCollectibleEnvActor.
extern "C" void Anchor_DropCollectibleRandomEnvActor(PlayState* play, Actor* envActor,
                                                     Vec3f* pos, s16 dropGroupParams) {
    // [Diag] pending-bugs 2026-07-15 — magic-jar bug diagnostic. Log
    // envActor.world.pos + pos param at entry so we can identify whether
    // pos is coming from envActor.world.pos (expected) or is being
    // overridden. See Claude/Analysis/magic_jar_unexplained_pickup_2026-07-15.md.
    if (CVarGetInteger("gEnhancements.PendingBugsDiag", 0)) {
        SPDLOG_INFO("[EnvActor.diag] DropRandomCall envActor.id={} envActor.world.pos=({:.0f},{:.0f},{:.0f}) pos_param=({:.0f},{:.0f},{:.0f}) dropGroupParams=0x{:02X}",
                    envActor ? envActor->id : -1,
                    envActor ? envActor->world.pos.x : 0.0f,
                    envActor ? envActor->world.pos.y : 0.0f,
                    envActor ? envActor->world.pos.z : 0.0f,
                    pos ? pos->x : 0.0f,
                    pos ? pos->y : 0.0f,
                    pos ? pos->z : 0.0f,
                    (int)dropGroupParams);
    }
    if (!Anchor::Instance || !Anchor::Instance->isConnected) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    if (::SceneAuthority::IsMyCurrentRoomHost()) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(envActor);
    if (ext == nullptr || ext->netId == 0) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    // Race-D peer-side scene check (see Anchor_DropCollectibleEnvActor).
    uint32_t hostId = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    auto hostIt = Anchor::Instance->clients.find(hostId);
    if (hostIt == Anchor::Instance->clients.end() ||
        hostIt->second.sceneNum != (s16)gPlayState->sceneNum) {
        Item_DropCollectibleRandom(play, NULL, pos, dropGroupParams);
        return;
    }
    Anchor::Instance->SendPacket_EnvActorDrop(ext->netId, /*dropParam=*/0,
                                               dropGroupParams, *pos);
}

// Bracket the host-side Item_DropCollectible* call in
// HandlePacket_EnvActorDrop so func_8001F404 substitution is skipped
// against host's gSaveContext. The peer's intended drop type is
// re-broadcast verbatim via the normal ITEM_DROP_SYNC pipeline.
extern "C" void Anchor_BeginHostingPeerEnvActorDrop(void) {
    g_isHostingPeerEnvActorDrop = true;
}

extern "C" void Anchor_EndHostingPeerEnvActorDrop(void) {
    g_isHostingPeerEnvActorDrop = false;
}

// Env-actor destruction broadcast helper. Called from each env
// actor's cut/destroy state transition (e.g. EnKusa's cut-stub
// transition for ENKUSA_TYPE_1, or any env actor's Destroy
// callback when destruction doesn't naturally route through
// Actor_Kill).
//
// Echo suppression: when this helper is called as part of
// applying a network-received destroy (HandlePacket_EnvActorDestroy
// invokes Anchor_ApplyEnKusaCut → EnKusa_SetupCut →
// Anchor_BroadcastEnvActorDestroy), the thread-local
// `g_isApplyingNetworkEnvActorDestroy` flag is set and the helper
// short-circuits. Prevents the echo back to the originator and
// further broadcast loops between peers.
//
// Earlier design (commit 0beb6bdf6) used HostBookkeeping's
// ClaimDefeatBroadcast ledger as the echo guard. That broke
// bidirectional sync for cyclic env actors: once a client RECEIVED
// a destroy for netId N (claiming N in the ledger), the client
// could never broadcast for N within the same scene visit — even
// after N regrew and the local player cut it again. Removed in
// favour of the thread-local flag below.
//
// Short-circuits on:
//   - disconnected (no broadcast needed in solo).
//   - actor has no EnemyNetId (not a synced actor — ignore).
//   - netId 0 (assignment didn't complete — ignore).
//   - g_isApplyingNetworkEnvActorDestroy (echo-suppress while
//     applying a received destroy).
//
// Plan: Claude/Plans/env_actor_destroy_sync.md §3.3.

extern "C" void Anchor_BeginNetworkEnvActorDestroy(void) {
    g_isApplyingNetworkEnvActorDestroy = true;
}
extern "C" void Anchor_EndNetworkEnvActorDestroy(void) {
    g_isApplyingNetworkEnvActorDestroy = false;
}

extern "C" void Anchor_BroadcastEnvActorDestroy(Actor* envActor) {
    if (envActor == nullptr) return;
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return;

    EnemyNetId* ext =
        const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(envActor));
    if (ext == nullptr || ext->netId == 0) return;

    // Transition phase unconditionally — both sender and receiver
    // need ext->phase == Dead so subsequent assocActor proximity
    // walks at pickup time can match this actor. Done BEFORE the
    // echo-suppress short-circuit (which only blocks the network
    // SendPacket, not state-tracking).
    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::Dead);

    if (g_isApplyingNetworkEnvActorDestroy) {
        // Receive-side application — phase already transitioned;
        // don't echo the broadcast back to the originator.
        return;
    }

    SPDLOG_INFO("[EnvActorDestroy] broadcasting actor id={} netId={} (cat={} pos=({:.0f},{:.0f},{:.0f}))",
                envActor->id, ext->netId, (int)envActor->category,
                envActor->world.pos.x, envActor->world.pos.y, envActor->world.pos.z);
    Anchor::Instance->SendPacket_EnvActorDestroy(ext->netId, envActor->id);
}
