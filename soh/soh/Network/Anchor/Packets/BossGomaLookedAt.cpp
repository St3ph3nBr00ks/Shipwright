#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
extern PlayState* gPlayState;
}

/**
 * BOSS_GOMA_LOOKED_AT — peer → room host
 *
 * Architecture context (issue #67 Boss_Goma intro):
 *   `BossGoma_Encounter` case 3 (z_boss_goma.c:799) gates the boss-fall
 *   transition on `actor.projectedPos` falling within a screen-space
 *   frustum check. `projectedPos` is computed once per frame from each
 *   client's local active camera. Without this bridge, only HOST's
 *   camera contributes to `lookedAtFrames` — peer can be standing in
 *   the boss room and pointing the camera up at Goma all day, and the
 *   fight never starts because host's camera doesn't satisfy the
 *   check.
 *
 *   Two compounding timing problems the earlier `lookedAtFrames++`
 *   approach hit (log 293, 2026-05-05):
 *     1. Vanilla case 3 has an else-branch (`this->lookedAtFrames = 0`,
 *        z_boss_goma.c:820) that resets the counter EVERY frame the
 *        local frustum check fails. The receive handler's `++` was
 *        clobbered the next frame.
 *     2. Peer's case 3 only fires for ~16 frames before peer's local
 *        threshold (lookedAtFrames > 15) trips and peer's
 *        `BossGoma_SetupEncounterState4` runs locally. Peer stops
 *        sending packets after that. Host's case 1-2 intro cutscene
 *        runs autonomously for 228 frames (~11s), so by the time
 *        host reaches case 3 the packet stream has long ended.
 *        Receiver's per-frame `++` would have nothing to consume.
 *
 *   Fix: maintain a STICKY frame accumulator on the boss's
 *   `EnemyNetId::bossGomaPeerLookFrames`. Increment on every received
 *   packet regardless of host's current actionState. case 3 in
 *   z_boss_goma.c reads + clears via `Anchor_BossGomaConsumePeerLookFrames`
 *   each frame and merges the consumed count into `lookedAtFrames`
 *   AFTER the if/else block — the else-reset can't clobber it. The
 *   accumulator persists across host's autonomous cutscene window so
 *   the peer-signal arriving 11s early still drives the transition.
 *
 *   Vanilla appearance preserved: case 3 still requires a 15+ frame
 *   "look" period before SetupEncounterState4 (peer's per-frame
 *   sender hits this within ~16 packets). Case 4 cinematic (Goma's
 *   eye-roll → fall from ceiling) plays normally on host because
 *   host transitions through case 4-130 itself; ENEMY_STATE
 *   broadcasts host's authoritative world.pos so peer's visual
 *   matches once host catches up.
 *
 * Wire fields:
 *   netId           — Boss_Goma actor netId
 *   sceneNum        — scene scope (Pillar E ValidateSameScene)
 *   timeline        — Pillar B linkAge (set via PacketTimeline)
 *   targetClientId  — room host of the actor's (sceneNum, my-roomNum,
 *                     timeline). Falls back to global effective host.
 *
 * Receive behaviour:
 *   actionState == 0 — peer's notification is also treated as the
 *     encounter-start signal (fallback when host's local trigger
 *     hasn't fired yet). Host force-advances to case 1, mirroring
 *     the trigger-zone branch in `BossGoma_Encounter` case 0
 *     (z_boss_goma.c:711-712). Intro cutscene runs normally.
 *   any actionState — increment `bossGomaPeerLookFrames` (clamped).
 *     case 3 consumes-and-merges into lookedAtFrames each frame.
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

    BossGoma*    boss   = (BossGoma*)actor;
    EnemyNetId*  mut    = const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
    const bool   firstSignalThisEncounter = (mut != nullptr) && !mut->bossGomaPeerSignaled;

    // Sticky flag — case 3 in z_boss_goma.c consumes-and-clears via
    // Anchor_BossGomaConsumePeerSignaled. Persists across host's
    // case 1-2 cutscene window (~228 frames) so peer's packet —
    // which can land ~11s before host enters case 3 — isn't lost.
    if (mut != nullptr) {
        mut->bossGomaPeerSignaled = true;
    }

    if (firstSignalThisEncounter) {
        SPDLOG_INFO("[BossGomaLookedAt] rx netId={} actionState={} (first peer signal "
                    "this encounter)",
                    netId, (int)boss->actionState);
    }

    if (boss->actionState == 0) {
        // Peer is signaling encounter start. Host's case 0 was waiting
        // for the room-entrance trigger zone, but peer has already
        // reached case 3 locally — that's sufficient evidence to kick
        // off the door-close cutscene on host. Mirrors the trigger-zone
        // branch at z_boss_goma.c:711-712.
        if (firstSignalThisEncounter) {
            SPDLOG_INFO("[BossGomaLookedAt] rx → starting door-close cutscene "
                        "(actionState 0→1)");
        }
        Player_SetCsActionWithHaltedActors(gPlayState, &boss->actor, 8);
        boss->actionState = 1;
    } else if (boss->actionState == 3) {
        // Host has finished the door-close cutscene and is sitting in
        // the look-puzzle wait. Vanilla case 3 needs the player's local
        // camera frustum-check to pass for >15 frames before advancing —
        // but on host the player isn't moving the camera (P2 is the
        // active player). Fire the eye-roll cinematic directly.
        // Consume the flag so case 3's next-frame check no-ops.
        SPDLOG_INFO("[BossGomaLookedAt] rx → firing SetupEncounterState4 directly "
                    "(host actionState=3, peer-signaled)");
        if (mut != nullptr) {
            mut->bossGomaPeerSignaled = false;
        }
        BossGoma_SetupEncounterState4(boss, gPlayState);
    }
    // actionState 1, 2 (cutscene playing) — flag set above; case 3
    // will pick it up when the cutscene finishes.
    // actionState >= 4 (cinematic playing or fight in progress) —
    // already past the look-puzzle gate; flag is harmless.
}
