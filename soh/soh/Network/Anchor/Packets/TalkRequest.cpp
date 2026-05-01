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
void EnHintnuts_SetupTalk(EnHintnuts* this_);
}

/**
 * TALK_REQUEST — peer → room host
 *
 * Architecture context (Hintnut sync):
 *   Hintnut state machine is host-authoritative. `Actor_ProcessTalkRequest`
 *   is local — only the client whose Link pressed A returns true. Without
 *   a peer-to-host bridge, peer initiates dialog locally (the OoT message
 *   system opens correctly), but peer's hintnut never transitions to Talk
 *   on host. Host keeps broadcasting Run; peer's rx-driver reverts peer's
 *   local Talk back to Run; peer's Run actionFunc give-up branch then
 *   fires SetupBurrow → hintnut "re-enters its nest" mid-dialog. Logs 214
 *   Bug 2.
 *
 *   Fix: peer detects local Actor_ProcessTalkRequest and fires this packet
 *   instead of running SetupTalk. Host receives and runs SetupTalk on its
 *   local actor → broadcasts state=7 (Talk) → peer's rx-driver applies
 *   SetupTalk on peer. Hintnut anim transitions to Talk on both clients
 *   in lockstep. Dialog UI on peer is unaffected (it's already open via
 *   the engine's local talk-offer path).
 *
 * Wire fields:
 *   netId           — target actor
 *   sceneNum        — scene scope (Pillar E ValidateSameScene)
 *   timeline        — Pillar B linkAge (set via PacketTimeline)
 *   targetClientId  — room host of the actor's (sceneNum, my-roomNum,
 *                     timeline). Falls back to global effective host.
 *
 * Double-apply guard: receiver no-ops if its local actor is already in
 * Talk (state 7) or past it (Leave 8 / Freeze 9 / BeginFreeze 10).
 */

void Anchor::SendPacket_TalkRequest(uint32_t targetNetId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = TALK_REQUEST;
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
    SPDLOG_INFO("[TalkRequest] Sent netId={} target={}", targetNetId, target);
}

void Anchor::HandlePacket_TalkRequest(nlohmann::json payload) {
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
        SPDLOG_WARN("[TalkRequest] No actor found for netId={}", netId);
        return;
    }

    switch (actor->id) {
        case ACTOR_EN_HINTNUTS: {
            EnHintnuts* h = (EnHintnuts*)actor;
            // Double-apply guard: ignore late peer requests if host has
            // already transitioned out of a "talkable" state. State 7
            // (Talk), 8 (Leave), 9 (Freeze), 10 (BeginFreeze) all mean
            // either "already talking" or "no longer eligible".
            s16 cur = EnHintnuts_GetStateIndex(h);
            const bool ineligible =
                (cur == 7) || (cur == 8) || (cur == 9) || (cur == 10);
            if (ineligible) {
                SPDLOG_INFO("[TalkRequest] netId={} guard: host already in state={}",
                            netId, (int)cur);
                return;
            }
            // Mirrors the Run actionFunc's talk-request branch
            // (z_en_hintnuts.c:420-421) — natural code path that would
            // fire if host's own local Link had pressed A.
            EnHintnuts_SetupTalk(h);
            SPDLOG_INFO("[TalkRequest] Applied to Hintnut netId={} (was state={})",
                        netId, (int)cur);
            break;
        }
        default:
            SPDLOG_WARN("[TalkRequest] Unhandled actor id={} for netId={}",
                        actor->id, netId);
            break;
    }
}
