#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"  // kSyncableActorCategories
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyHostBookkeeping.h"  // ClaimDefeatBroadcast
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "functions.h"   // Actor_Kill
#include "macros.h"
#include "z64.h"
extern PlayState* gPlayState;

// Per-actor receive-side destruction helpers. Each one transitions
// the local actor into the right post-destroy state. Default for
// actors without a specialized helper: Actor_Kill. See
// HandlePacket_EnvActorDestroy dispatch below.
void Anchor_ApplyEnKusaCut(Actor* thisx);  // z_en_kusa.c
}

/**
 * ENV_ACTOR_DESTROY — any client → all (team-broadcast).
 *
 * Sent when a synced env actor is destroyed locally — including cut
 * events that DON'T call Actor_Kill on the sender side (e.g.
 * ENKUSA_TYPE_1 grass that transitions to a cut-stub state without
 * dying). Receivers find their local matching actor by EnemyNetId
 * and Actor_Kill it so the destruction propagates cross-client.
 *
 * Idempotent — duplicate broadcasts (simultaneous-cut race) no-op
 * on the receive side because the second arrival finds the actor
 * already gone (`update == nullptr`) and skips.
 *
 * Wire fields:
 *   sceneNum          — sender's scene (cross-scene gate).
 *   timeline          — Pillar B linkAge bit.
 *   actorNetId        — EnemyNetId.netId of the destroyed actor.
 *   actorId           — actor type (informational; not strictly
 *                       needed since netId encodes it, but helpful
 *                       for diagnostics).
 *   destroyerClientId — relay-injected `clientId` carries this
 *                       redundantly. Diagnostic-only.
 *
 * Plan: `Claude/Plans/env_actor_destroy_sync.md`.
 */

void Anchor::SendPacket_EnvActorDestroy(uint32_t actorNetId, s16 actorId) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = ENV_ACTOR_DESTROY;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["actorNetId"]   = actorNetId;
    payload["actorId"]      = (int)actorId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[EnvActorDestroy] Sending actorNetId={} actorId={} sceneNum={}",
                actorNetId, (int)actorId, (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnvActorDestroy(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_DEBUG("[EnvActorDestroy] rx — local scene {} != sender scene {}",
                     (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    uint32_t actorNetId = (uint32_t)payload.value("actorNetId", (uint32_t)0);
    if (actorNetId == 0) {
        SPDLOG_WARN("[EnvActorDestroy] rx — actorNetId == 0");
        return;
    }

    // Claim the dedup ledger entry FIRST so any actor-specific
    // destroy helper invoked below (e.g. EnKusa_SetupCut) that
    // internally calls Anchor_BroadcastEnvActorDestroy will see the
    // netId already claimed and skip re-broadcasting. Prevents an
    // echo loop where peer's receipt of destruction triggers peer's
    // own cut transition which would otherwise broadcast back.
    EnemyStateSync::HostBookkeeping::Instance().ClaimDefeatBroadcast(actorNetId);

    // Walk synced actor categories for the matching netId. Same
    // category set as the existing receive-side actor scans
    // (kSyncableActorCategories includes ENEMY/BOSS/PROP/BG/NPC/
    // SWITCH/ITEMACTION/MISC — env actors are spread across PROP/BG/
    // MISC depending on actor).
    for (size_t ci = 0; ci < kSyncableActorCategoriesCount; ++ci) {
        Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[ci]].head;
        while (a != nullptr) {
            const EnemyNetId* ext =
                ObjectExtension::GetInstance().Get<EnemyNetId>(a);
            if (ext != nullptr && ext->netId == actorNetId &&
                a->update != nullptr) {
                // Per-actor receive-side dispatch. Most env actors
                // get vanilla Actor_Kill — they don't have a
                // distinct cut-stub state. En_Kusa is special: its
                // TYPE_1 / TYPE_2 variants have a cut-stub state
                // that should be visible on peer to match the
                // sender's vanilla appearance + regrowth timer.
                if (a->id == ACTOR_EN_KUSA) {
                    SPDLOG_INFO("[EnvActorDestroy] rx actorNetId={} — applying EnKusa cut "
                                "transition (preserves cut-stub state + regrowth timer)",
                                actorNetId);
                    Anchor_ApplyEnKusaCut(a);
                } else {
                    SPDLOG_INFO("[EnvActorDestroy] rx actorNetId={} — Actor_Kill local copy "
                                "(actor id={} cat={})",
                                actorNetId, a->id, (int)a->category);
                    Actor_Kill(a);
                }
                return;  // Idempotent: stop after first match.
            }
            a = a->next;
        }
    }

    SPDLOG_DEBUG("[EnvActorDestroy] rx actorNetId={} — no live local actor found "
                 "(already destroyed, despawned, or not in synced categories)",
                 actorNetId);
}
