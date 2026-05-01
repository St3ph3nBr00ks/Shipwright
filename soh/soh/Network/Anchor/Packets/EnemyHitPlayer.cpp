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
// Host-side handler lives in the actor — calls into z_en_goroiwa.c to apply
// the reverse-direction state transition without damaging host's local Link.
#include "overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_HIT_PLAYER — issue #153 Phase 2
 *
 * Non-host → host. Sent when a non-host's local Link is hit by its local
 * instance of an enemy whose state response to the hit must be authoritative
 * on the host (currently En_Goroiwa only).
 *
 * Why this is separate from DAMAGE_ENEMY:
 *   DAMAGE_ENEMY is "non-host damaged an enemy" — host accumulates damage and
 *   lets its authoritative copy die via the normal path.
 *   ENEMY_HIT_PLAYER is "enemy hit non-host" — the state that needs to sync
 *   is on the ENEMY (reverse direction, trigger Wait/fall), not the player.
 *   Damage to the player happens per-client on the client that was hit.
 *
 * Receive guard: only host processes. Host does NOT apply damage to its own
 * local Link — only the enemy's state transition.
 *
 * Double-apply guard: host's handler checks actionFunc; if the boulder has
 * already transitioned out of Roll this cycle (from host-local hit), the
 * packet is a no-op.
 */

void Anchor::SendPacket_EnemyHitPlayer(uint32_t netId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = ENEMY_HIT_PLAYER;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = netId;
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    // Pillar A Phase 2 — target the scene host of MY current scene
    // (which is the scene the actor I just hit lives in). Falls back
    // to the global effective host when no scene host is set, which
    // matches Phase 1 routing.
    const uint32_t target = ::SceneAuthority::GetSceneHostClientId(
        gPlayState->sceneNum, (uint8_t)(gSaveContext.linkAge & 0x1));
    payload["targetClientId"] = target;
    SendJsonToRemote(payload);
    SPDLOG_INFO("[EnemyHitPlayer] Sent netId={} target={}", netId, target);
}

void Anchor::HandlePacket_EnemyHitPlayer(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Pillar B Phase 1 — drop cross-timeline scene-scoped traffic.
    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    // Pillar A Phase 2 — only the scene host of the packet's scene
    // applies the hit-response. Other clients (including the global
    // effective host when it's not the scene host of this sceneNum)
    // ignore the packet so the authoritative state machine runs in
    // exactly one place.
    if (!::SceneAuthority::IsSceneHost(sceneNum, (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);

    // Walk every syncable actor category (shared list in Anchor.h).
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
        SPDLOG_WARN("[EnemyHitPlayer] No actor found for netId={}", netId);
        return;
    }

    switch (actor->id) {
        case ACTOR_EN_GOROIWA:
            EnGoroiwa_ProcessRemoteHit((EnGoroiwa*)actor, gPlayState);
            SPDLOG_INFO("[EnemyHitPlayer] Applied to Goroiwa netId={}", netId);
            break;
        default:
            // Other enemies: nothing to do yet. Future actors (e.g. bounce-back
            // props) would dispatch here.
            break;
    }
}
