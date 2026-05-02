#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "z64.h"
#include "overlays/actors/ovl_En_Hintnuts/z_en_hintnuts.h"
extern PlayState* gPlayState;
}

extern "C" {
void EnHintnuts_SetupLeave(EnHintnuts* this_, PlayState* play);
}

/**
 * DIALOG_END — peer → room host
 *
 * Architecture context (Hintnut sync):
 *   `Message_GetState` is local — only the client whose Link initiated
 *   dialog sees TEXT_STATE_EVENT when the dialog closes. Without a peer-
 *   to-host bridge, peer fires SetupLeave locally (z_en_hintnuts.c:441)
 *   when its dialog ends; SetupLeave's body spawns a recovery heart via
 *   Actor_Spawn(ACTOR_EN_ITEM00, …, 0x3) and sets actionFunc to Leave.
 *
 *   Under host-authoritative sync, host's local Talk actionFunc never
 *   sees TEXT_STATE_EVENT (host's dialog never opened) — so host stays
 *   in Talk and keeps broadcasting state=7. Peer's rx-driver applies
 *   Talk back over peer's Leave, peer's Talk reads TEXT_STATE_EVENT
 *   again, fires SetupLeave again — heart-spawn loop runs every 50ms
 *   until the actor table overflows and the renderer crashes (logs 216:
 *   ~280 hearts in 14s, then exception 0xC0000005).
 *
 *   Fix: peer detects local TEXT_STATE_EVENT and fires this packet
 *   instead of running SetupLeave. Host receives and runs the
 *   canonical SetupLeave on its local actor → spawns the heart on host
 *   (replicates to peer through standard EnItem00 spawn flow) →
 *   broadcasts state=8 (Leave). Peer's rx-driver applies via
 *   ApplyNetState case 8 (inlined no-heart variant). Peer's local
 *   Leave actionFunc then drives Message_CloseTextbox + Actor_Kill at
 *   the natural run-off-stage moment.
 *
 * Wire fields:
 *   netId           — target actor
 *   sceneNum        — scene scope (Pillar E ValidateSameScene)
 *   timeline        — Pillar B linkAge (set via PacketTimeline)
 *   targetClientId  — room host of the actor's (sceneNum, my-roomNum,
 *                     timeline). Falls back to global effective host.
 *
 * Double-apply guard: receiver no-ops if its local actor is already in
 * Leave (8), Freeze (9), or BeginFreeze (10) — host already advanced
 * past Talk for this cycle.
 */

void Anchor::SendPacket_DialogEnd(uint32_t targetNetId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = DIALOG_END;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = targetNetId;
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    const uint32_t target = ::SceneAuthority::GetRoomHostClientId(
        gPlayState->sceneNum,
        (s8)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    payload["targetClientId"] = target;
    SendJsonToRemote(payload);
    SPDLOG_INFO("[DialogEnd] Sent netId={} target={}", targetNetId, target);
}

void Anchor::HandlePacket_DialogEnd(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    if (!::SceneAuthority::IsRoomHost(sceneNum,
                                       (s8)gPlayState->roomCtx.curRoom.num,
                                       (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);

    Actor* actor = nullptr;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (actor != nullptr) {
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext != nullptr && ext->netId == netId) {
                goto actor_found;
            }
            actor = actor->next;
        }
    }
    actor = nullptr;
actor_found:
    if (actor == nullptr) {
        SPDLOG_WARN("[DialogEnd] No actor found for netId={}", netId);
        return;
    }

    switch (actor->id) {
        case ACTOR_EN_HINTNUTS: {
            EnHintnuts* h = (EnHintnuts*)actor;
            // Double-apply guard: ignore late peer notifications if host
            // has already advanced past Talk. State 8 (Leave), 9
            // (Freeze), 10 (BeginFreeze) all mean "post-talk path
            // already running"; spurious peer dups (multiple in-flight
            // before host's Leave broadcast lands on peer) are no-op'd.
            s16 cur = EnHintnuts_GetStateIndex(h);
            const bool ineligible =
                (cur == 8) || (cur == 9) || (cur == 10);
            if (ineligible) {
                SPDLOG_INFO("[DialogEnd] netId={} guard: host already in state={}",
                            netId, (int)cur);
                return;
            }
            // Mirrors EnHintnuts_Talk's TEXT_STATE_EVENT branch
            // (z_en_hintnuts.c:440-441) — the natural code path that
            // would fire if host's own local Link had been in dialog.
            EnHintnuts_SetupLeave(h, gPlayState);
            SPDLOG_INFO("[DialogEnd] Applied to Hintnut netId={} (was state={})",
                        netId, (int)cur);
            break;
        }
        default:
            SPDLOG_WARN("[DialogEnd] Unhandled actor id={} for netId={}",
                        actor->id, netId);
            break;
    }
}
