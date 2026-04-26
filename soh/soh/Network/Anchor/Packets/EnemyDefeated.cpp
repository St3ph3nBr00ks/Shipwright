#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"  // CVAR_REMOTE_ANCHOR for local-host TeamId lookup
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

    // Q I Tier 2 — kill attribution. The host's lastDamagerByNetId map records
    // the most recent DAMAGE_ENEMY sender for each netId. If this kill is
    // being broadcast by the host (host's own player or a remote-via-host kill
    // that bottomed out the actor's HP), look up the killer:
    //   - Map hit  → killer is the recorded damager (covers all DAMAGE_ENEMY
    //                paths regardless of who landed the killing blow locally;
    //                last-DAMAGE_ENEMY-wins per design doc Q I Tier 2)
    //   - Map miss → killer is the host (host's own player damaged the enemy
    //                via local collision without any prior remote DAMAGE_ENEMY)
    //
    // killerTeamId is derived from clients[killerId].teamId at send time so
    // the receiver doesn't need its own team-table lookup. Q C grace-window
    // and future scoreboard work consume these fields off the wire directly.
    //
    // Non-host senders (the rare path where a non-host hits an enemy and lands
    // a killing blow before the host applies the DAMAGE_ENEMY) skip
    // attribution — only the host owns the lastDamagerByNetId map.
    if (roomState.ownerClientId == ownClientId) {
        uint32_t killerId;
        auto it = lastDamagerByNetId.find(netId);
        if (it != lastDamagerByNetId.end()) {
            killerId = it->second;
        } else {
            killerId = ownClientId;  // local host kill, no remote damager recorded
        }
        payload["killerClientId"] = killerId;
        if (killerId == ownClientId) {
            payload["killerTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        } else if (clients.contains(killerId)) {
            payload["killerTeamId"] = clients[killerId].teamId;
        }
        // Drop the entry now that the kill has been attributed; subsequent
        // hits on a respawned actor (same netId) start fresh.
        if (it != lastDamagerByNetId.end()) {
            lastDamagerByNetId.erase(it);
        }
    }

    SPDLOG_INFO("[EnemyDefeated] Sending defeat for netId={} killerClientId={} killerTeamId={}",
                netId,
                payload.value("killerClientId", (uint32_t)0),
                payload.value("killerTeamId", std::string("(unattributed)")));

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

    // Pillar E note: ValidateSameScene intentionally not added here.
    // ENEMY_DEFEATED is cross-scene tolerant by design — when the host
    // receives a kill while in a different scene than where the kill
    // happened, the actor lookup naturally fails and the kill is recorded
    // in deadEnemiesByScene[sceneFromNetId] (high 16 bits of netId) for
    // future scene-entry replay. A ValidateSameScene drop here would
    // skip that record and the kill would never replay.

    uint32_t netId = payload.value("netId", (uint32_t)0);

    // Q I Tier 2 — kill attribution. Schema-2 senders include killerClientId
    // and killerTeamId. Tier 2 is plumbed-but-no-UI: we log the values for
    // diagnostic visibility but otherwise no consumer reads them on receive.
    // Future Tier 3 work (kill feed, scoreboard) and Q C grace window
    // (3s drop pickup lockout to killer's team) will read off the wire here.
    uint32_t    killerClientId = payload.value("killerClientId", (uint32_t)0);
    std::string killerTeamId   = payload.value("killerTeamId", std::string{});

    SPDLOG_INFO("[EnemyDefeated] Received defeat for netId={} killerClientId={} killerTeamId={}",
                netId, killerClientId,
                killerTeamId.empty() ? "(unattributed)" : killerTeamId);

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
                        SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} already dying — duplicate, dedup only", netId);
                        // The actor is already mid-cycle. In practice this branch is
                        // only reached via duplicate delivery — typically the host's
                        // deadEnemiesByScene replay redelivering a kill the actor has
                        // already started processing locally. Karebaba in DeadItemDrop
                        // moves to ACTORCAT_MISC and is not damage-able, so a genuine
                        // "stacked kill" can't occur during the cycle.
                        //
                        // Previously we set stalledKillPending=true here so the respawn
                        // detector would re-kill on the next live state. That fired
                        // incorrectly on the duplicate-replay case: P2 leaves room, P1
                        // kills, P2 re-enters → P2 receives the original packet (buffered
                        // as pendingKill, dies via Fix 38) and then a replay packet from
                        // P1 → "already dying" → stalled kill set → Karebaba respawns
                        // and is immediately re-killed mid-regrowth.
                        //
                        // Keep pendingKillNetIds.insert for Fix 35 (room-exit/re-entry
                        // persistence). Don't set stalledKillPending.
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
                SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} in ACTORCAT_MISC natural cycle — duplicate, dedup only", netId);
                // Same rationale as the ACTORCAT_ENEMY already-dying branch above:
                // duplicate replay should not set stalledKillPending. Persist
                // pendingKillNetIds for room-exit/re-entry (Fix 35) only.
                pendingKillNetIds.insert(netId);
                return;
            }
            misc = misc->next;
        }
    }

    SPDLOG_WARN("[EnemyDefeated] No actor found for netId={} — buffering as pendingKill (scene not loaded yet?)",
                netId);
    pendingKillNetIds.insert(netId);

    // Also record the kill in deadEnemiesByScene so the host's join-time replay
    // (HandlePacket_UpdateClientState) covers this enemy. Without this, kills
    // received while the host is in a different room of the kill's scene never
    // make it into the replay list and any new client joining mid-session
    // sees the enemy alive. Scene is decoded from the netId (high 16 bits per
    // the netId encoding scheme) — works regardless of where the host is now.
    if (roomState.ownerClientId == ownClientId) {
        int16_t sceneFromNetId = (int16_t)((netId >> 16) & 0xFFFF);
        deadEnemiesByScene[sceneFromNetId].insert(netId);
    }
}
