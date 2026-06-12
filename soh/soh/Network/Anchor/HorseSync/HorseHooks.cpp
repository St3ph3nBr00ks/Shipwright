/*
 * Horse-sync hook registrations: OnActorSpawn / OnActorUpdate /
 * OnActorKill / OnSceneSpawnActors. Pulled into Anchor::RegisterHooks
 * via a single call site so HookHandlers.cpp stays small.
 *
 * Plan: Plans/horse_sync_plan.md §"Spawn paths" + §"Despawn paths" +
 * §"Suppress local AI on peer-owned horses".
 *
 * Owner-authoritative model: each client's local OnActorSpawn for its
 * own in-scope ACTOR_EN_HORSE instance broadcasts HORSE_SPAWN to peers.
 * OnActorUpdate emits HORSE_STATE at ~10 Hz with critical-edge bypass
 * on actionState change. OnActorKill emits HORSE_DESPAWN. Peers'
 * receivers materialize replicas via Anchor_SpawnPeerHorse; those
 * replicas' OnActor* hooks skip the owner-side broadcast (gated on
 * HorseNetId::isPeerOwned) but DO suppress local AI side-effects
 * (vanilla EnHorse_GetMountSide returns 0 when playerControlled is
 * already true — re-using that gate prevents local Link from
 * accidentally mounting a peer's horse).
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/HorseNetId.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/cvar_prefixes.h"  // CVAR_ENHANCEMENT — #262 diagnostic gate

#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
}

namespace {

// Triple-gate admission predicate per Plans/horse_sync_plan.md.
// CVar gate is checked first (cheap short-circuit when sync is off).
bool IsHorseSyncCandidate(Actor* actor) {
    if (actor == nullptr) return false;
    if (actor->id != ACTOR_EN_HORSE) return false;
    if (actor->params == 7 || actor->params == 8) return false;  // cutscene + HBA
    if (!Anchor::IsHorseSyncEnabled()) return false;
    return true;
}

// Cached prior actionState on the local owner's horse for critical-edge
// detection. A single slot is sufficient for v1 (owner only ever has one
// mounted Epona at a time in regular gameplay; carrier instances at Lon
// Lon Ranch are rare and would just cause one extra HORSE_STATE per
// switch — benign).
int32_t sLastSentOwnerActionState = -1;

}  // namespace

void Anchor::RegisterHorseHooks() {

    // Lambdas use [this] capture so the body has implicit member-access
    // through `this` — required for isSpawningNetworkActor and
    // isKillingNetworkActor which live in the private block (Pitfall 16).
    // `this` is a copy of the singleton pointer at registration time and
    // stays valid for the program lifetime (Anchor::Instance is never
    // re-assigned after first construction).

    // -----------------------------------------------------------------
    // OnActorSpawn — owner-side: tag the actor + broadcast HORSE_SPAWN.
    // Receiver-side (peer horse coming through Anchor_SpawnPeerHorse) is
    // guarded by isSpawningNetworkActor — the EnemyStateSync pattern
    // already gates that flag, but defensive re-check here keeps the
    // horse path independent.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorSpawn>(
        [this](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (!IsHorseSyncCandidate(actor)) return;
            if (!isConnected) return;
            if (isSpawningNetworkActor) return;
            if (gPlayState == nullptr) return;

            // Tag the freshly-spawned horse as owner-local.
            //
            // Fix C (2026-06-09): the original code did `Get` + early-
            // return on null. But Get returns null until something has
            // called Set (ObjectExtension.h:49) — and nothing ever does
            // for owner-spawned vanilla horses. So this hook silently
            // bailed for every vanilla Epona, HORSE_SPAWN never fired,
            // peers never saw the owner's mount. Discovered via the
            // title-screen Phase 2 field test (log 461 — P2 didn't see
            // P1's vanilla title-cutscene Epona). Fixed by skipping only
            // when the extension is already tagged (re-spawn case);
            // otherwise create + tag via Set.
            HorseNetId* existing =
                ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (existing != nullptr && existing->netId != 0) return;  // already tagged

            const uint32_t owner   = ownClientId;
            const int16_t  sceneId = (int16_t)gPlayState->sceneNum;
            const int16_t  params  = (int16_t)actor->params;
            const uint32_t netId =
                Anchor::MakeHorseNetId(owner, sceneId, params);

            ObjectExtension::GetInstance().Set<HorseNetId>(actor, HorseNetId{
                netId,
                owner,
                /*isPeerOwned=*/ false,
            });

            // #264 diag — log the actor's pos at the moment of tag/spawn
            // broadcast. Pair this with the [UpdateClientState.diag]
            // re-emit log to detect if world.pos drifted between tag
            // time and re-emit time (Hypothesis A — stale world.pos).
            if (CVarGetInteger(CVAR_ENHANCEMENT("DebugHorseSyncDiag"), 0) != 0) {
                SPDLOG_INFO("[HorseHooks.diag] OnActorSpawn tagged horse: "
                            "netId={} owner={} scene={} params={} "
                            "world.pos=({:.1f},{:.1f},{:.1f}) "
                            "home.pos=({:.1f},{:.1f},{:.1f}) "
                            "shape.rot.y={}",
                            netId, owner, (int)sceneId, (int)params,
                            actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                            actor->home.pos.x, actor->home.pos.y, actor->home.pos.z,
                            (int)actor->shape.rot.y);
            }

            SendPacket_HorseSpawn(netId, params, sceneId,
                                   actor->world.pos, actor->shape.rot.y);
        });

    // -----------------------------------------------------------------
    // OnActorUpdate — owner-side: emit HORSE_STATE at ~10 Hz + critical-
    // edge on actionState change. Peer-side: lock playerControlled so
    // local mount eligibility (EnHorse_GetMountSide returns 0 when
    // playerControlled is true) blocks accidental local mounts. Local
    // AI decisions on peer horses are otherwise inert because the
    // synced actionState immediately re-overrides any local choice.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorUpdate>(
        [this](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (actor == nullptr || actor->id != ACTOR_EN_HORSE) return;
            if (!isConnected) return;
            if (!Anchor::IsHorseSyncEnabled()) return;

            HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (ext == nullptr || ext->netId == 0) return;

            if (ext->isPeerOwned) {
                // Mount-side gate + animation enablement.
                //
                // Original v1 design used `playerControlled = 1` here to
                // make vanilla EnHorse_GetMountSide return 0 (mount
                // blocked). Field test 2026-06-11 (log 507, issue #257)
                // revealed this triggers EnHorse_MountDismount's freeze
                // branch at z_en_horse.c:3034-3039 (when playerControlled
                // becomes true while Actor_NotMounted is also true —
                // always true on a peer-owned horse because no local
                // Player is actually mounted), which sets action=FROZEN
                // and animationIdx=IDLE. Once frozen, the freeze
                // self-perpetuates and the horse never animates +
                // riderPos never updates → peer Link sat frozen at frame
                // 0 of gallop, Y oscillated as riderPos stayed at the
                // saddle limb pose.
                //
                // Title-screen Phase 3 hit the same wall and solved it
                // by clearing playerControlled = 0 so vanilla state
                // machine ticks SkelAnime normally. We mirror that here
                // for the gameplay path, and use the ENHORSE_UNRIDEABLE
                // flag instead for mount-side gating — see
                // EnHorse_MountDismount at z_en_horse.c:3025:
                //
                //   if (mountSide != 0 && !(stateFlags & ENHORSE_UNRIDEABLE)
                //       && player->rideActor == NULL) {
                //       Actor_SetRideActor(...);
                //   }
                //
                // The flag short-circuits the mount before SetRideActor,
                // without poisoning the action state. Set every tick
                // because vanilla state setup helpers clear it in some
                // transitions (e.g., EnHorse_Idle clear at line 1813).
                //
                // Title-screen branch retained — its tighter behaviour
                // (playerControlled=0 only) was tuned to its cutscene
                // animation requirements; gameplay now matches.
                EnHorse* en = (EnHorse*)actor;
                en->playerControlled = 0;
                en->stateFlags |= ENHORSE_UNRIDEABLE;
                return;
            }

            // Owner-side throttle. Send every kHorseStateMs (100ms = 10Hz
            // nominal), or immediately on actionState change.
            constexpr int kHorseStateMs = 100;
            const int32_t currentAction = (int32_t)((EnHorse*)actor)->action;
            const uint64_t now =
                gameFrameCounter.load(std::memory_order_relaxed);
            const int      intervalTicks = MsToGameTicks(kHorseStateMs);
            if (intervalTicks <= 0) return;
            const bool actionEdge = (currentAction != sLastSentOwnerActionState);
            const bool intervalElapsed =
                now >= mHorseStateLastFrame + (uint64_t)intervalTicks;

            if (actionEdge || intervalElapsed) {
                SendPacket_HorseState(actor);
                mHorseStateLastFrame = now;
                sLastSentOwnerActionState = currentAction;
            }
        });

    // -----------------------------------------------------------------
    // OnActorDestroy — owner emits HORSE_DESPAWN. #262 — was previously
    // OnActorKill which never fires for scene-cleanup despawns: vanilla
    // scene cleanup calls Actor_Delete (z_actor.c:3505), which fires
    // OnActorDestroy at z_actor.c:3513. Actor_Kill is reserved for
    // explicit in-actor kill paths (z_actor.c:1202) and is NOT what
    // scene transitions use. OnActorDestroy is the FIRST line of
    // Actor_Delete body so it fires for every actor destruction
    // regardless of trigger (scene cleanup, explicit Actor_Kill +
    // delete cascade, etc.).
    //
    // Peer-side cleanup still routes through HandlePacket_HorseDespawn's
    // KillNetworkActorSilently call which brackets isKillingNetworkActor;
    // this hook bails on that flag to avoid echo-broadcasting peer
    // despawns we triggered ourselves.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorDestroy>(
        [this](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (actor == nullptr || actor->id != ACTOR_EN_HORSE) return;
            if (!isConnected) return;
            if (isKillingNetworkActor) return;
            if (!Anchor::IsHorseSyncEnabled()) return;

            HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (ext == nullptr || ext->netId == 0) return;
            if (ext->isPeerOwned) return;  // peer horse cleanup happens via packet

            constexpr uint8_t kReasonNaturalDespawn = 0;
            SendPacket_HorseDespawn(ext->netId, kReasonNaturalDespawn);

            if (CVarGetInteger(CVAR_ENHANCEMENT("DebugHorseSyncDiag"), 0) != 0) {
                SPDLOG_INFO("[HorseHooks.diag] OnActorDestroy fired HORSE_DESPAWN "
                            "netId={} ownerClientId={} (scene transition or actor delete)",
                            ext->netId, ext->ownerClientId);
            }
        });

    // -----------------------------------------------------------------
    // OnSceneSpawnActors — clear stale mPeerHorses cache. Same shape as
    // NPC Follower's scene-transition pointer cleanup: cached Actor*
    // pointers become dangling after scene reload, and reading
    // dangling->update is UB.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        [this]() {
            mPeerHorses.clear();
            sLastSentOwnerActionState = -1;
            mHorseStateLastFrame = 0;
        });
}
