#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/EnemyNetId.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Ta/z_en_ta.h"
extern PlayState* gPlayState;
}

/**
 * TALON_CASTLE_STATE — any-client → all-clients broadcast for the
 * Hyrule Castle child-timeline Talon's wake / talk / run-off state
 * machine. Closes the bug where a co-located peer sees Talon stay
 * asleep until the peer reloads the scene
 * (Claude/Analysis/talon_castle_wake_sync_2026-06-17.md).
 *
 * Architecture: vanilla Talon's wake transition fires when
 * Actor_ProcessTalkRequest succeeds with EXCH_ITEM_CHICKEN in the
 * exchange-item slot (z_en_ta.c:331-354). This only happens on the
 * machine of the player who's actually holding the Cucco — which
 * may not be the room host. The standard host-authoritative
 * ENEMY_STATE pipeline therefore cannot drive the transition on
 * other clients. We mirror MidoPostDekuLeave's any-client-emitter
 * shape, parameterised over an opaque state index so the entire
 * 12-state sequence (wake / talk / idle-awake / run-off / despawn)
 * propagates without one packet family per transition boundary.
 *
 * Wire fields:
 *   sceneNum     — sender's local scene at send time (Pillar E ValidateSameScene).
 *   stateIndex   — 0x00..0x0B per z_en_ta.c state map.
 *   timeline     — Pillar B linkAge match.
 *   targetTeamId — team-scoped per session_state.md "Pillar 0" Mido scope precedent.
 *
 * Forward-only apply: receivers reject lower state indices (newIdx <
 * currentIdx) to prevent a freshly-spawned late-joiner's "sleeping"
 * broadcast from regressing players who've already advanced past
 * wake. Receivers also no-op when newIdx == currentIdx — avoids
 * replaying audio / animation cues when the local state machine
 * already tick-advanced to match.
 *
 * Echo prevention: HandlePacket sets ext->netStateIndex = newIdx
 * BEFORE invoking EnTa_NetSync_ApplyState. The HookHandlers
 * OnActorUpdate poll driver compares EnTa_NetSync_GetStateIndex to
 * ext->netStateIndex and skips broadcast when equal, so a fresh
 * apply doesn't trigger a re-broadcast.
 */

void Anchor::SendPacket_TalonCastleState(uint8_t stateIndex) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }
    nlohmann::json payload;
    payload["type"]         = TALON_CASTLE_STATE;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["stateIndex"]   = (int)stateIndex;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[TalonCastleState] Sending sceneNum={} stateIndex={}",
                (int)gPlayState->sceneNum, (int)stateIndex);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TalonCastleState(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[TalonCastleState] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[TalonCastleState] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    int rawIdx = payload.value("stateIndex", -1);
    if (rawIdx < 0 || rawIdx > 0xFF) {
        SPDLOG_INFO("[TalonCastleState] Drop — invalid stateIndex {}", rawIdx);
        return;
    }
    uint8_t newIdx = (uint8_t)rawIdx;

    // Walk ACTORCAT_NPC. Talon's castle variant is unique per scene
    // (one EnTa actor in Hyrule Castle child) — same lookup shape as
    // MidoPostDekuLeave.cpp:99-111. Note: vanilla castle Talon's
    // placement params is 0xFFFF (-1 as s16), not 0. Receivers gate
    // on the senderscene match above (Pillar E ValidateSameScene)
    // and on actorId match here; EnTa_NetSync_ApplyState's own scene
    // check is the final safety net against cross-variant pollution.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (actor != nullptr) {
        if (actor->id == ACTOR_EN_TA) {
            EnTa* ta = (EnTa*)actor;

            uint8_t currentIdx = EnTa_NetSync_GetStateIndex(ta);
            if (currentIdx != 0xFF && newIdx < currentIdx) {
                SPDLOG_INFO("[TalonCastleState] Drop — forward-only gate: "
                            "local state {} >= received state {}",
                            (int)currentIdx, (int)newIdx);
                return;
            }
            if (currentIdx == newIdx) {
                // Already at target state — refresh ext bookkeeping so
                // the poll driver doesn't accidentally re-broadcast.
                EnemyNetId* extEq = const_cast<EnemyNetId*>(
                    ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
                if (extEq != nullptr) {
                    extEq->netStateIndex = (s16)newIdx;
                }
                SPDLOG_INFO("[TalonCastleState] No-op — local already at state {}",
                            (int)newIdx);
                return;
            }

            // Mark synced BEFORE applying so the next OnActorUpdate
            // poll sees equality and skips its broadcast.
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext != nullptr) {
                ext->netStateIndex = (s16)newIdx;
            }

            EnTa_NetSync_ApplyState(ta, gPlayState, newIdx);
            SPDLOG_INFO("[TalonCastleState] Applied state {} → {}",
                        (int)currentIdx, (int)newIdx);
            return;
        }
        actor = actor->next;
    }

    SPDLOG_INFO("[TalonCastleState] No castle Talon in scene {} — packet dropped",
                (int)gPlayState->sceneNum);
}
