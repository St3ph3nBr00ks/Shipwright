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

// Per-actor hit-response helper for En_Hintnuts (file-static — only
// reachable through this packet's dispatch). Mirrors the natural code
// path inside EnHintnuts_ColliderCheck for the nutsball-hit branch:
// runs both HitByScrubProjectile1 (BG transition for non-puzzle scrubs
// or the third-correct-hit case) and HitByScrubProjectile2 (puzzle-
// counter advance + BeginFreeze / BeginRun branch). On host these two
// drive the authoritative state transition; the resulting state change
// reaches peers through the standard ENEMY_STATE pipeline.
//
// Both functions are non-static in z_en_hintnuts.c so we can reach
// them via the actor's header.
extern "C" {
void EnHintnuts_HitByScrubProjectile1(EnHintnuts* this_, PlayState* play);
void EnHintnuts_HitByScrubProjectile2(EnHintnuts* this_);
}

/**
 * PROJECTILE_HIT_ENEMY — peer → room host
 *
 * Architecture context (Hintnut sync):
 *   Hintnut state machine is host-authoritative. Each client spawns its
 *   own En_Nutsball at frame 6 of the synced ThrowNut animation, aimed
 *   at its own local Link. Reflect physics is per-client (each client's
 *   shield reflects the local nutball). When a peer's local reflected
 *   nutball lands on a peer's local hintnut, the peer is NOT permitted
 *   to transition the hintnut locally (would fight host's authoritative
 *   state machine). Instead the peer fires this packet; host applies
 *   the hit-response on its local actor; the resulting state change
 *   propagates back to peers via standard ENEMY_STATE.
 *
 *   Visible cost: ~50ms RTT between "peer sees nutball impact" and
 *   "peer's hintnut transitions to BeginFreeze / BeginRun". Same RTT
 *   shape as DAMAGE_ENEMY and ENEMY_HIT_PLAYER.
 *
 * Wire fields:
 *   netId             — target enemy
 *   sceneNum          — scene scope (Pillar E ValidateSameScene)
 *   timeline          — Pillar B linkAge (set via PacketTimeline)
 *   projectileActorId — what hit (currently always ACTOR_EN_NUTSBALL;
 *                       reserved for future projectile types)
 *   targetClientId    — room host of the actor's (sceneNum, my-roomNum,
 *                       timeline). Falls back to global effective host.
 *
 * Double-apply guard: receiver no-ops if the host's local actor has
 * already transitioned out of an "accepts-hit" state for this cycle
 * (peer hit + host hit collide).
 */

void Anchor::SendPacket_ProjectileHitEnemy(uint32_t targetNetId, s16 projectileActorId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]              = PROJECTILE_HIT_ENEMY;
    payload["sceneNum"]          = gPlayState->sceneNum;
    payload["netId"]             = targetNetId;
    payload["projectileActorId"] = (int)projectileActorId;
    payload["quiet"]             = true;
    PacketTimeline::SetTimelineField(payload);

    // Route to the actor's room host. The actor lives in MY current
    // room (the local nutball just hit a local enemy in this client's
    // actor list), so MY current room is the actor's room.
    const uint32_t target = ::SceneAuthority::GetRoomHostClientId(
        gPlayState->sceneNum,
        (s8)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    payload["targetClientId"] = target;
    SendJsonToRemote(payload);
    SPDLOG_INFO("[ProjectileHitEnemy] Sent netId={} projectileActorId={} target={}",
                targetNetId, (int)projectileActorId, target);
}

void Anchor::HandlePacket_ProjectileHitEnemy(nlohmann::json payload) {
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

    // Only the room host of (sceneNum, my-current-roomNum, timeline)
    // owns the authoritative state machine for actors in that scope.
    if (!::SceneAuthority::IsRoomHost(sceneNum,
                                       (s8)gPlayState->roomCtx.curRoom.num,
                                       (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;
    }

    uint32_t netId             = payload.value("netId", (uint32_t)0);
    s16      projectileActorId = (s16)payload.value("projectileActorId", (int)0);

    // Walk every syncable actor category for the target.
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
        SPDLOG_WARN("[ProjectileHitEnemy] No actor found for netId={}", netId);
        return;
    }

    switch (actor->id) {
        case ACTOR_EN_HINTNUTS: {
            EnHintnuts* h = (EnHintnuts*)actor;
            // Double-apply guard: if host already transitioned out of an
            // "accepts hit" state (Stand / ThrowNut / LookAround / Wait),
            // the simultaneous host-local + peer hit case is already
            // resolved. State 9 (Freeze), 10 (BeginFreeze), 5 (BeginRun),
            // 6 (Run), 7 (Talk), 8 (Leave) all mean "hit was already
            // processed this cycle"; ignore the late peer notification.
            s16 cur = EnHintnuts_GetStateIndex(h);
            const bool alreadyResponded =
                (cur == 5) || (cur == 6) || (cur == 7) ||
                (cur == 8) || (cur == 9) || (cur == 10);
            if (alreadyResponded) {
                SPDLOG_INFO("[ProjectileHitEnemy] netId={} guard: host already in state={}",
                            netId, (int)cur);
                return;
            }
            // Mirrors EnHintnuts_ColliderCheck's nutball-hit branch
            // (z_en_hintnuts.c:516-518) — the natural code path that
            // would fire if host's own local nutball had landed.
            EnHintnuts_HitByScrubProjectile1(h, gPlayState);
            EnHintnuts_HitByScrubProjectile2(h);
            SPDLOG_INFO("[ProjectileHitEnemy] Applied to Hintnut netId={} (was state={})",
                        netId, (int)cur);
            break;
        }
        default:
            SPDLOG_WARN("[ProjectileHitEnemy] Unhandled actor id={} for netId={}",
                        actor->id, netId);
            break;
    }
    (void)projectileActorId;  // reserved for future per-projectile dispatch
}
