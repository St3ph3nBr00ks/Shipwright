#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
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
    PacketTimeline::SetTimelineField(payload);

    // Q I Tier 2 — kill attribution. Two paths depending on who is sending:
    //
    //  Host path: host owns the kill-feed truth. Look up the killer from
    //    lastDamagerByNetId (last DAMAGE_ENEMY sender wins) or fall back to
    //    ownClientId for a host-local kill with no prior remote damage.
    //    Broadcast the attributed packet to every online peer.
    //
    //  Non-host path: don't attribute locally — non-hosts have no way to
    //    prove the killer field couldn't be forged. Instead, send the kill
    //    targeted at the effective host only. The host reads the relay-
    //    enriched `clientId` field (set server-side from the TCP socket,
    //    not a payload field a client could set) to learn the true sender,
    //    then re-broadcasts to all other peers with attribution filled in.
    //    Costs ~one host roundtrip of latency on the kill propagating to
    //    other peers; trade-off accepted for tamper-proof attribution.
    if (::SceneAuthority::IsEffectiveHost()) {
        auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
        const uint32_t damager = bookkeeping.LookupDamager(netId);
        const uint32_t killerId = damager != 0 ? damager : ownClientId;  // 0 = local host kill, no remote damager recorded
        payload["killerClientId"] = killerId;
        if (killerId == ownClientId) {
            payload["killerTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        } else if (clients.contains(killerId)) {
            payload["killerTeamId"] = clients[killerId].teamId;
        }
        // Drop the entry now that the kill has been attributed; subsequent
        // hits on a respawned actor (same netId) start fresh.
        bookkeeping.ClearDamager(netId);

        SPDLOG_INFO("[EnemyDefeated] Host send for netId={} killerClientId={} killerTeamId={}",
                    netId,
                    payload.value("killerClientId", (uint32_t)0),
                    payload.value("killerTeamId", std::string("(unattributed)")));

        for (auto& [clientId, client] : clients) {
            if (client.online && client.isSaveLoaded && !client.self) {
                payload["targetClientId"] = clientId;
                SendJsonToRemote(payload);
            }
        }
    } else {
        // Non-host: route through host. The host fills in attribution from
        // the relay-enriched sender field and re-broadcasts to other peers.
        SPDLOG_INFO("[EnemyDefeated] Non-host route-to-host for netId={} (host will attribute and re-broadcast)",
                    netId);
        payload["targetClientId"] = effectiveHostClientId;
        SendJsonToRemote(payload);
    }
}

void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Pillar B Phase 1 — drop cross-timeline scene-scoped traffic.
    // Known Phase-1 limitation: when an effective host is in a different
    // timeline than the kill, the kill is dropped here instead of being
    // re-broadcast to peers in the killer's timeline. Cross-timeline
    // routing is left for a future phase (host needs to relay regardless
    // of its own timeline; design extension beyond Phase 1 scope).
    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
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

    // Host-routed attribution (paired with non-host send path above): when
    // the effective host receives an unattributed kill (killerClientId=0),
    // it fills in attribution from the relay-enriched sender field and
    // re-broadcasts to every peer except the original sender (who already
    // killed locally and doesn't need the echo).
    //
    // sentDefeatThisScene gates re-broadcast against the host's own local
    // kill of the same netId — if the host already sent the attributed
    // packet via OnEnemyDefeat, skip the duplicate. Inserts into the same
    // dedup set used by OnEnemyDefeat / OnActorKill so a follow-on local
    // OnActorKill on this netId is also suppressed.
    if (::SceneAuthority::IsEffectiveHost() && killerClientId == 0 &&
        !EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(netId)) {
        uint32_t senderId = payload.value("clientId", (uint32_t)0);
        if (senderId != 0) {
            EnemyStateSync::HostBookkeeping::Instance().ClaimDefeatBroadcast(netId);

            nlohmann::json rebroadcast;
            rebroadcast["type"]           = ENEMY_DEFEATED;
            rebroadcast["netId"]          = netId;
            rebroadcast["killerClientId"] = senderId;
            // Stamp our timeline. The IsCrossTimelinePacket filter above
            // already proved sender's timeline matches ours, so the re-
            // broadcast carries the correct timeline for downstream peers.
            PacketTimeline::SetTimelineField(rebroadcast);
            if (clients.contains(senderId)) {
                rebroadcast["killerTeamId"] = clients[senderId].teamId;
            }

            SPDLOG_INFO("[EnemyDefeated] Host re-broadcast for netId={} killerClientId={} (attributed from sender)",
                        netId, senderId);

            for (auto& [clientId, client] : clients) {
                if (client.online && client.isSaveLoaded && !client.self &&
                    clientId != senderId) {
                    rebroadcast["targetClientId"] = clientId;
                    SendJsonToRemote(rebroadcast);
                }
            }
        }
    }

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
                if (::SceneAuthority::IsEffectiveHost()) {
                    EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(gPlayState->sceneNum, netId);
                }

                // Karebaba (ACTOR_EN_KAREBABA): let the natural death→respawn cycle
                // play out on non-host instead of calling Actor_Kill.  The actor stays
                // alive, runs Dying→DeadItemDrop→Dead→Regrow→Idle, and remains available
                // for the next ENEMY_UPDATE sync after respawn (Fix 24).
                //
                // If the Karebaba is already in a death cycle (defeatPacketSent = local
                // kill, or pendingNaturalDeath = prior network kill), ignore the duplicate.
                if (actor->id == ACTOR_EN_KAREBABA) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Karebaba.dupDetect");
                    // C2 Phase 2 fix (regression in cc6a435b7): the merged
                    // HostBookkeeping defeat-broadcast set conflates "this
                    // actor is in a death cycle" (per-EnemyNetId state) with
                    // "host has re-broadcast in this scene visit" (per-scene
                    // dedup). On the host, the route-to-host re-broadcast at
                    // line 140 above just claimed the broadcast for this
                    // exact netId, so HasDefeatBroadcast would falsely trip
                    // dup-detect on the very first receive. The original
                    // ext->defeatPacketSent was never set by the receive
                    // path; the right post-Phase-1 check is "actor is in any
                    // death phase," which PhaseImpliesHasLocalDeath captures
                    // (DyingByLocal | DyingByNetwork | AwaitingDeadItemDrop
                    // | Dead). PhaseImpliesPendingNaturalDeath is a strict
                    // subset, so the unified predicate is enough.
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
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
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} — triggering natural death cycle", netId);
                    EnKarebaba_SetupDyingNet((EnKarebaba*)actor);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    // Keep netId in pendingKillNetIds so that if P2 exits the room
                    // mid-cycle (OoT destroys the actor on room unload), the fresh
                    // spawn on re-entry will also be set to SetupDeadItemDrop via the
                    // pendingKillNetIds check in OnActorSpawn (Fix 35). Erased by the
                    // non-host respawn detection when the cycle completes.
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
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
            if (ext != nullptr && ext->netId == netId) {
                EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Karebaba.MISC.dupDetect");
            }
            // Same regression fix as the ENEMY-category dup-detect above:
            // use PhaseImpliesHasLocalDeath instead of HasDefeatBroadcast
            // so the host's own route-to-host re-broadcast doesn't false-
            // positive this dup-detect on the FIRST receive.
            if (ext != nullptr && ext->netId == netId &&
                EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} in ACTORCAT_MISC natural cycle — duplicate, dedup only", netId);
                // Same rationale as the ACTORCAT_ENEMY already-dying branch above:
                // duplicate replay should not set stalledKillPending. Persist
                // pendingKillNetIds for room-exit/re-entry (Fix 35) only.
                EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                return;
            }
            misc = misc->next;
        }
    }

    SPDLOG_WARN("[EnemyDefeated] No actor found for netId={} — buffering as pendingKill (scene not loaded yet?)",
                netId);
    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);

    // Also record the kill in deadEnemiesByScene so the host's join-time replay
    // (HandlePacket_UpdateClientState) covers this enemy. Without this, kills
    // received while the host is in a different room of the kill's scene never
    // make it into the replay list and any new client joining mid-session
    // sees the enemy alive. Scene is decoded from the netId (low 15 bits of
    // the upper word; bit 31 is Pillar B timeline) — works regardless of
    // where the host is now.
    if (::SceneAuthority::IsEffectiveHost()) {
        int16_t sceneFromNetId = (int16_t)((netId >> 16) & 0x7FFF);
        EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(sceneFromNetId, netId);
    }
}
