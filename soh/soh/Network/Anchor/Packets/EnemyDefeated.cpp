#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_DEFEATED
 *
 * Sent by the host when an enemy fires OnEnemyDefeat (at the end of its death
 * animation, just before Actor_Kill). Non-host clients receive this and call
 * Actor_Kill on the matching local actor so the enemy disappears on all clients.
 *
 * Note: the non-host enemy will not play a death animation — it disappears
 * immediately. Full death animation sync is deferred to a later phase.
 */

void Anchor::SendPacket_EnemyDefeated(uint32_t netId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DEFEATED;
    payload["netId"] = netId;

    SPDLOG_INFO("[EnemyDefeated] Sending defeat for netId={}", netId);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);

    SPDLOG_INFO("[EnemyDefeated] Received defeat for netId={}", netId);

    // Walk every syncable actor category (shared list in Anchor.h) looking
    // for the netId match. Covers ENEMY + BOSS plus any actor that underwent
    // a runtime category transition (Karebaba→MISC, Armos→BG, etc.).
    for (size_t catIdx = 0; catIdx < kSyncableActorCategoriesCount; catIdx++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[catIdx]].head;
        while (actor != nullptr) {
            // Grab next before any actor mutation to avoid touching freed memory.
            Actor* next = actor->next;
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext != nullptr && ext->netId == netId) {
                // Host records this kill for join-time replay regardless of who killed it.
                if (roomState.ownerClientId == ownClientId) {
                    deadEnemiesByScene[gPlayState->sceneNum].insert(netId);
                }

                // Karebaba (ACTOR_EN_KAREBABA): let the natural death→respawn cycle
                // play out on non-host instead of calling Actor_Kill.  The actor stays
                // alive, runs Dying→DeadItemDrop→Dead→Regrow→Idle, and remains available
                // for the next ENEMY_UPDATE sync after respawn (Fix 24).
                //
                // If the Karebaba is already in a death cycle (defeatPacketSent = local
                // kill, or pendingNaturalDeath = prior network kill), ignore the duplicate.
                if (actor->id == ACTOR_EN_KAREBABA) {
                    if (ext->defeatPacketSent || ext->pendingNaturalDeath) {
                        SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} already dying — stacking kill for after respawn", netId);
                        // The actor is already mid-cycle; we cannot apply this kill now.
                        // Mark stalledKillPending so that when the current cycle completes
                        // and respawn detection fires in OnActorUpdate, it immediately
                        // re-triggers the death cycle instead of restoring the actor to
                        // live state (Fix 36 — stacked kill support).
                        ext->stalledKillPending = true;
                        // Also ensure pendingKillNetIds persists so that if the player
                        // exits and re-enters the room mid-cycle, SetupDeadItemDrop fires
                        // again on the fresh spawn (Fix 35).
                        pendingKillNetIds.insert(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} — triggering natural death cycle", netId);
                    EnKarebaba_SetupDyingNet((EnKarebaba*)actor);
                    ext->hasLocalDeath       = true;
                    ext->pendingNaturalDeath = true;
                    // Keep netId in pendingKillNetIds so that if P2 exits the room
                    // mid-cycle (OoT destroys the actor on room unload), the fresh
                    // spawn on re-entry will also be set to SetupDeadItemDrop via the
                    // pendingKillNetIds check in OnActorSpawn (Fix 35). Erased by the
                    // non-host respawn detection when the cycle completes.
                    pendingKillNetIds.insert(netId);
                    return;
                }

                SPDLOG_INFO("[EnemyDefeated] Killing actor id={} netId={}", actor->id, netId);
                // Guard against the OnActorKill hook (Fix 12) echoing ENEMY_DEFEATED
                // back to the network for this Actor_Kill call.
                isKillingNetworkActor = true;
                Actor_Kill(actor);
                isKillingNetworkActor = false;
                return;
            }
            actor = next;
        }
    }

    // Also check ACTORCAT_MISC: a Karebaba moves there during its DeadItemDrop/Dead
    // states.  If it is already in a natural death cycle, ignore — it will respawn.
    {
        Actor* misc = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
        while (misc != nullptr) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(misc));
            if (ext != nullptr && ext->netId == netId &&
                (ext->pendingNaturalDeath || ext->defeatPacketSent)) {
                SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} in ACTORCAT_MISC natural cycle — stacking kill for after respawn", netId);
                ext->stalledKillPending = true;
                // Ensure persistence across room re-entries (Fix 35).
                pendingKillNetIds.insert(netId);
                return;
            }
            misc = misc->next;
        }
    }

    SPDLOG_WARN("[EnemyDefeated] No actor found for netId={} — buffering as pendingKill (scene not loaded yet?)",
                netId);
    pendingKillNetIds.insert(netId);
}
