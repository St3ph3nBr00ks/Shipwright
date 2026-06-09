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

    // -----------------------------------------------------------------
    // OnActorSpawn — owner-side: tag the actor + broadcast HORSE_SPAWN.
    // Receiver-side (peer horse coming through Anchor_SpawnPeerHorse) is
    // guarded by isSpawningNetworkActor — the EnemyStateSync pattern
    // already gates that flag, but defensive re-check here keeps the
    // horse path independent.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorSpawn>(
        [](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (!IsHorseSyncCandidate(actor)) return;
            if (Anchor::Instance == nullptr) return;
            if (!Anchor::Instance->isConnected) return;
            if (Anchor::Instance->isSpawningNetworkActor) return;
            if (gPlayState == nullptr) return;

            // Tag the freshly-spawned horse as owner-local.
            HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (ext == nullptr) return;
            if (ext->netId != 0) return;  // already tagged; skip

            const uint32_t owner   = Anchor::Instance->ownClientId;
            const int16_t  sceneId = (int16_t)gPlayState->sceneNum;
            const int16_t  params  = (int16_t)actor->params;
            ext->netId         = Anchor::MakeHorseNetId(owner, sceneId, params);
            ext->ownerClientId = owner;
            ext->isPeerOwned   = false;

            Anchor::Instance->SendPacket_HorseSpawn(
                ext->netId, params, sceneId, actor->world.pos,
                actor->shape.rot.y);
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
        [](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (actor == nullptr || actor->id != ACTOR_EN_HORSE) return;
            if (Anchor::Instance == nullptr) return;
            if (!Anchor::Instance->isConnected) return;
            if (!Anchor::IsHorseSyncEnabled()) return;

            HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (ext == nullptr || ext->netId == 0) return;

            if (ext->isPeerOwned) {
                // Mount-side gate — peer horses are unmountable by the
                // local player. Setting playerControlled forces
                // EnHorse_GetMountSide to return 0 at z_en_horse.c:3643+.
                ((EnHorse*)actor)->playerControlled = 1;
                return;
            }

            // Owner-side throttle. Send every kHorseStateMs (100ms = 10Hz
            // nominal), or immediately on actionState change.
            constexpr int kHorseStateMs = 100;
            const int32_t currentAction = (int32_t)((EnHorse*)actor)->action;
            const uint64_t now =
                Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
            const int      intervalTicks =
                Anchor::Instance->MsToGameTicks(kHorseStateMs);
            if (intervalTicks <= 0) return;
            const bool actionEdge = (currentAction != sLastSentOwnerActionState);
            const bool intervalElapsed =
                now >= Anchor::Instance->mHorseStateLastFrame + (uint64_t)intervalTicks;

            if (actionEdge || intervalElapsed) {
                Anchor::Instance->SendPacket_HorseState(actor);
                Anchor::Instance->mHorseStateLastFrame = now;
                sLastSentOwnerActionState = currentAction;
            }
        });

    // -----------------------------------------------------------------
    // OnActorKill — owner emits HORSE_DESPAWN. Peer-side Actor_Kill
    // routed through KillNetworkActorSilently in HorseDespawn handler
    // suppresses this hook from re-broadcasting (isKillingNetworkActor
    // bracket pattern — see Anchor.cpp KillNetworkActorSilently impl).
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnActorKill>(
        [](void* refActor) {
            Actor* actor = static_cast<Actor*>(refActor);
            if (actor == nullptr || actor->id != ACTOR_EN_HORSE) return;
            if (Anchor::Instance == nullptr) return;
            if (!Anchor::Instance->isConnected) return;
            if (Anchor::Instance->isKillingNetworkActor) return;
            if (!Anchor::IsHorseSyncEnabled()) return;

            HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(actor);
            if (ext == nullptr || ext->netId == 0) return;
            if (ext->isPeerOwned) return;  // peer horse cleanup happens via packet

            constexpr uint8_t kReasonNaturalDespawn = 0;
            Anchor::Instance->SendPacket_HorseDespawn(ext->netId, kReasonNaturalDespawn);
        });

    // -----------------------------------------------------------------
    // OnSceneSpawnActors — clear stale mPeerHorses cache. Same shape as
    // NPC Follower's scene-transition pointer cleanup: cached Actor*
    // pointers become dangling after scene reload, and reading
    // dangling->update is UB.
    // -----------------------------------------------------------------
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        []() {
            if (Anchor::Instance == nullptr) return;
            Anchor::Instance->mPeerHorses.clear();
            sLastSentOwnerActionState = -1;
            Anchor::Instance->mHorseStateLastFrame = 0;
        });
}
