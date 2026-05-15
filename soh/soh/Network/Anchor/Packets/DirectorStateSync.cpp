/**
 * DIRECTOR_STATE_SYNC — host → all peers
 *
 * Broadcasts the AI Director's full ledger state plus per-descriptor
 * migration blobs so that any peer becoming the next global-effective-
 * host has fresh state to resume spawn decisions.
 *
 * Sent by `AnchorDirector::Director::RecordSpawn` and `OnEnemyRemoved`
 * after every ledger mutation. Frequency scales with spawn activity;
 * payload is small (live entries × ~16 bytes JSON). Marked `quiet` so
 * the relay doesn't log per-packet bookkeeping for it.
 *
 * Receive side caches into the local Director's ledgers regardless of
 * host status — peers always carry a copy. The descriptor migration
 * notification (`OnDirectorHostMigrated(true)`) fires separately when
 * the local client actually becomes effective host, via
 * `Anchor::OnBecameEffectiveHost`.
 *
 * Plan: Plans/ai_director_plan.md §2.8.
 * Schema: 1 (PacketSchemas.h).
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/AIDirector/Director.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

void Anchor::SendPacket_DirectorStateSync() {
    // Host-only: peers' Director state mirrors the host's view but is
    // not authoritative — they shouldn't broadcast their own snapshots.
    // The host-side gate is here (rather than in Director) so the
    // single-player path (no peers) also no-ops cleanly.
    if (!::SceneAuthority::IsEffectiveHost()) {
        return;
    }

    // No peers online → broadcast is wasted bandwidth.
    bool anyPeerOnline = false;
    for (auto& [clientId, client] : clients) {
        if (client.online && !client.self) {
            anyPeerOnline = true;
            break;
        }
    }
    if (!anyPeerOnline) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = DIRECTOR_STATE_SYNC;
    payload["snapshot"] = AnchorDirector::Director::Instance().SerializeMigrationSnapshot();
    payload["quiet"]    = true;  // suppress relay-side per-packet bookkeeping logs
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_DirectorStateSync(nlohmann::json payload) {
    // Always apply on receive, regardless of host status. Peers cache the
    // snapshot for future migration; the local Director's Tick is gated
    // on IsEffectiveHost so cached ledgers don't cause unintended work
    // until the client actually becomes host.
    //
    // The authoritative host will never receive its own broadcast (it's
    // the sender, and the relay doesn't echo) — defensive guard anyway.
    if (::SceneAuthority::IsEffectiveHost()) {
        return;
    }
    if (!payload.contains("snapshot")) {
        return;
    }
    AnchorDirector::Director::Instance().ApplyMigrationSnapshot(payload["snapshot"]);
}
