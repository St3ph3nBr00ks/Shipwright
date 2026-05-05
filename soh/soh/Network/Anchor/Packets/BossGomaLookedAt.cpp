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
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
extern PlayState* gPlayState;
}

/**
 * BOSS_GOMA_LOOKED_AT — peer → room host
 *
 * Architecture context (issue #67 Boss_Goma intro):
 *   `BossGoma_Encounter` case 3 (z_boss_goma.c:783) gates the boss-fall
 *   transition on `actor.projectedPos` falling within a screen-space
 *   frustum check. `projectedPos` is computed once per frame from each
 *   client's local active camera. Without this bridge, only HOST's
 *   camera contributes to `lookedAtFrames` — peer can be standing in
 *   the boss room and pointing the camera up at Goma all day, and the
 *   fight never starts because host's camera doesn't satisfy the
 *   check.
 *
 *   Peer's `BossGoma_Encounter` case 3 still runs every frame on peer
 *   (state-synced from host's actionState). Its projectedPos check
 *   uses peer's camera. When the check passes, peer fires
 *   `Anchor_NotifyBossGomaLookedAt` (HookHandlers.cpp). Host receives
 *   and bumps its local Goma's `lookedAtFrames` — combined with host's
 *   own local check, threshold (15) is reached when EITHER side has
 *   been looking long enough.
 *
 *   Peer's local `lookedAtFrames++` write is harmless: peer's
 *   actionFunc is force-synced from host's actionState, so peer's
 *   local SetupEncounterState4 transition is reverted by sync. Only
 *   host's counter advances the fight; peer's counter is observer-
 *   side bookkeeping that gets overwritten anyway.
 *
 * Wire fields:
 *   netId           — Boss_Goma actor netId
 *   sceneNum        — scene scope (Pillar E ValidateSameScene)
 *   timeline        — Pillar B linkAge (set via PacketTimeline)
 *   targetClientId  — room host of the actor's (sceneNum, my-roomNum,
 *                     timeline). Falls back to global effective host.
 *
 * Receive guard: only applies when host's local Goma is in actionState 3
 * (the look-at window). Spurious notifications during other states are
 * dropped — peer might still send during state transitions because
 * peer's actionState is one frame behind host's.
 */

void Anchor::SendPacket_BossGomaLookedAt(uint32_t bossNetId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = BOSS_GOMA_LOOKED_AT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = bossNetId;
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    const uint32_t target = ::SceneAuthority::GetRoomHostClientId(
        gPlayState->sceneNum,
        (s8)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    payload["targetClientId"] = target;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_BossGomaLookedAt(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
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
                goto goma_found;
            }
            actor = actor->next;
        }
    }
    actor = nullptr;
goma_found:
    if (actor == nullptr || actor->id != ACTOR_BOSS_GOMA) {
        return;
    }

    BossGoma* boss = (BossGoma*)actor;
    // Only count peer's look during the actual look-at-Goma window
    // (actionState 3). Other states' lookedAtFrames is unused.
    if (boss->actionState == 3) {
        boss->lookedAtFrames++;
    }
}
