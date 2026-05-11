/**
 * LeashRespawn — implementation.
 *
 * Per plan §10. Host-only override of synced-enemy spawn positions
 * on scene re-entry. The registry stores the last-known authoritative
 * position per netId; OnSceneEnter walks the freshly-spawned syncable
 * enemies and substitutes the registered position when a player's
 * recent trail places them near the recorded location.
 *
 * Migration corner case: registry is per-host. New host on migration
 * has empty registry → graceful fallback to vanilla home.pos.
 */

#include "LeashRespawn.h"

#include "ActorSyncHelpers.h"   // IsSyncableActor / kSyncableActorCategories
#include "ActorTrail.h"         // GetWaypointBefore / TrailWaypoint
#include "DistanceMath.h"
#include "NavCVars.h"
#include "NavTraits.h"
#include "SceneAuthority.h"     // IsMyCurrentRoomHost
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------

namespace {

// How close a player must have been to the registered position for
// the override to fire. >600u = the player likely never engaged this
// enemy → keep vanilla home.pos behaviour.
constexpr float kPlayerProximityRadiusSq = 600.0f * 600.0f;

// How far back to look in the player's trail for the proximity check.
// 1 second of recent wall-clock history.
constexpr uint32_t kPlayerTrailLookbackMs = 1000;

// Singleton registry. Host-only writers; OnSceneEnter is the reader.
std::unordered_map<uint32_t, Vec3f>& Registry() {
    static std::unordered_map<uint32_t, Vec3f> map;
    return map;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API — registry mutation.
// ---------------------------------------------------------------------------

void RecordKnownPosition(uint32_t netId, const Vec3f& pos) {
    if (netId == 0) return;
    Registry()[netId] = pos;
}

void ForgetKnownPosition(uint32_t netId) {
    if (netId == 0) return;
    Registry().erase(netId);
}

void ClearAllKnownPositions() {
    Registry().clear();
}

bool IsLeashRespawnEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kLeashRespawn);
}

// ---------------------------------------------------------------------------
// Public API — scene-enter override.
// ---------------------------------------------------------------------------

void OnSceneEnter(int16_t sceneNum, PlayState* play) {
    if (play == nullptr) return;
    if (!IsLeashRespawnEnabled()) return;

    // Host-only — non-hosts have no authority to override positions.
    if (!::SceneAuthority::IsMyCurrentRoomHost()) return;

    // Was-a-player-near-here check via player trail. We pull only the
    // local player's trail here for v1; remote DummyPlayers contribute
    // to the same trail map per ActorTrail::Tick rules, but we limit
    // the proximity test to the local player to keep the predicate
    // cheap. A future refinement can expand to all online clients
    // without changing the API.
    const ActorTrail& trail = ActorTrail::GetInstance();

    // Walk every freshly-spawned syncable enemy actor; if its netId
    // is in the registry AND eligibleForLeashRespawn is true AND a
    // local-player trail waypoint within the lookback is within range,
    // override world.pos.
    for (uint8_t cat : kSyncableActorCategories) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            Actor* next = actor->next;
            if (IsSyncableActor(actor) && actor->update != nullptr) {
                const NavTraits& traits = GetTraitsForActor(actor->id);
                if (traits.eligibleForLeashRespawn) {
                    const EnemyNetId* ext =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
                    if (ext != nullptr && ext->netId != 0) {
                        auto it = Registry().find(ext->netId);
                        if (it != Registry().end()) {
                            // Confirm a player was nearby in their
                            // recent trail history.
                            TrailWaypoint wp{};
                            const TrailKey playerKey = TrailKeyForPlayer(0);
                            if (trail.GetWaypointBefore(playerKey, kPlayerTrailLookbackMs, wp) &&
                                AnchorDist::Dist3DSq(wp.pos, it->second) < kPlayerProximityRadiusSq) {
                                actor->world.pos = it->second;
                                actor->prevPos   = it->second;
                                SPDLOG_INFO(
                                    "[LeashRespawn] netId={} scene={} "
                                    "world.pos overridden to ({:.0f},{:.0f},{:.0f}) "
                                    "(player wp dist² {:.0f})",
                                    ext->netId, (int)sceneNum,
                                    it->second.x, it->second.y, it->second.z,
                                    std::sqrt(AnchorDist::Dist3DSq(wp.pos, it->second)));
                            }
                        }
                    }
                }
            }
            actor = next;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle hooks.
// ---------------------------------------------------------------------------

namespace {

void OnSceneSpawnActorsBridge() {
    if (gPlayState == nullptr) return;
    OnSceneEnter((int16_t)gPlayState->sceneNum, gPlayState);
}

void OnExitGameClear(int32_t /*fileNum*/) {
    ClearAllKnownPositions();
}

}  // anonymous namespace

void RegisterLeashRespawn() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        OnSceneSpawnActorsBridge);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnExitGame>(
        OnExitGameClear);
}

} // namespace AnchorNav

static RegisterShipInitFunc registerLeashRespawn(AnchorNav::RegisterLeashRespawn);
