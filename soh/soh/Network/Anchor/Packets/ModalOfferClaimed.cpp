#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SyncedClaimableDrop.h"
#include "soh/Network/Anchor/Common/DropAdapters/DropAdapter.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * MODAL_OFFER_CLAIMED — host → all clients (team-broadcast).
 *
 * #193 Plan B follow-up to step 6. The correct dismissal trigger for
 * a modal-offer Drop: fired when host's player actually accepts the
 * offer, detected via the OnActorSpawn(EN_ITEM00 & 0x8000) hook on
 * host (the modal-completion phantom from `func_8083E4C4` in
 * z_player.c).
 *
 * Authority: host-only send. Peers never run their own modal offer
 * (ModalOfferAdapter suppresses it), so a phantom on a peer would be
 * a vanilla-path artifact we don't expect; gate the send on
 * SceneAuthority::IsMyCurrentRoomHost().
 *
 * Receive-side dismissal applies to peers (claimerClientId !=
 * ownClientId) only — host's offering actor is mid-cutscene with the
 * vanilla state machine cleaning it up; explicit Actor_Kill would
 * skip the death animation and stomp the get-item flow. The
 * MODAL_OFFER_CLAIMED relay echo on host still sets the Drop to
 * Resolved (idempotent state bookkeeping) without touching the
 * visual reps.
 *
 * Wire fields:
 *   offererNetId      — the offering actor's EnemyNetId.netId, by
 *                       ModalOfferAdapter convention also equal to
 *                       the Drop's dropId.
 *   claimerClientId   — the host's ownClientId (the host IS the
 *                       claimer; only host's player can accept the
 *                       modal — peers have it suppressed).
 *   targetTeamId      — team-scope routing.
 *   timeline          — Pillar B linkAge bit.
 */

void Anchor::SendPacket_ModalOfferClaimed(uint32_t offererEnemyNetId,
                                          uint32_t claimerClientId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]            = MODAL_OFFER_CLAIMED;
    payload["targetTeamId"]    = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["offererNetId"]    = offererEnemyNetId;
    payload["claimerClientId"] = claimerClientId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[ModalOfferClaimed] Sending offererNetId={} claimer={}",
                offererEnemyNetId, claimerClientId);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ModalOfferClaimed(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    uint32_t offererNetId    = (uint32_t)payload.value("offererNetId",    (uint32_t)0);
    uint32_t claimerClientId = (uint32_t)payload.value("claimerClientId", (uint32_t)0);

    if (offererNetId == 0) {
        SPDLOG_WARN("[ModalOfferClaimed] rx with offererNetId=0; dropping");
        return;
    }

    auto& reg = SyncedClaimableDrop::Registry::Instance();
    SyncedClaimableDrop::Drop* drop = reg.Find(offererNetId);
    if (drop == nullptr) {
        SPDLOG_DEBUG("[ModalOfferClaimed] rx offererNetId={} claimer={} — no matching Drop "
                     "(modal-offer adapter never allocated locally; ignored)",
                     offererNetId, claimerClientId);
        return;
    }
    if (drop->state == SyncedClaimableDrop::DropState::Resolved) {
        // Host's relay echo of its own broadcast, or duplicate. No-op.
        return;
    }

    // Skip the local visual-rep dismissal when this client is the
    // claimer. Vanilla state machine on the offering actor (mid get-
    // item cutscene) handles cleanup; an explicit Actor_Kill here
    // would race the cutscene and produce visual jank. Peers, whose
    // mirror actors are NOT in a cutscene and have their modal
    // suppressed, dispatch the adapter's DismissVisualRep to end the
    // stem visual immediately.
    const bool isLocalClaimer = (claimerClientId == ownClientId);
    if (!isLocalClaimer && drop->adapter != nullptr) {
        for (Actor* visualRep : drop->visualReps) {
            drop->adapter->DismissVisualRep(*drop, visualRep);
        }
    }

    drop->claimerClientId = claimerClientId;
    reg.TransitionTo(*drop, SyncedClaimableDrop::DropState::Resolved);

    SPDLOG_INFO("[ModalOfferClaimed] rx offererNetId={} claimer={} — Drop resolved, "
                "visual rep dismissal {} (adapter='{}')",
                offererNetId, claimerClientId,
                isLocalClaimer ? "skipped (local claimer)" : "dispatched",
                drop->adapter != nullptr ? drop->adapter->Name() : "(null)");
}
