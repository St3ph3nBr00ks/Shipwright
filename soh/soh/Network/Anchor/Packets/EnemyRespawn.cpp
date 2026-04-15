#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_RESPAWN
 *
 * Sent by the host when its Karebaba actor completes the natural death cycle and
 * returns to a living state (OnActorUpdate host respawn detection fires). On
 * receipt, a non-host that is still counting down its DeadItemDrop/Dead timer
 * (200 frames each, running ~3× slower at VirtualBox ~20fps than native ~60fps)
 * skips immediately to Regrow so both clients complete the respawn within ~1s
 * of each other instead of ~10s apart.
 *
 * Send condition: host only, once per respawn cycle, from the existing Karebaba
 * respawn detection block in HookHandlers.cpp OnActorUpdate.
 *
 * Receive condition: non-host only. No-op if the actor is already past the death
 * cycle (pendingNaturalDeath is false or state is already Regrow/living).
 */

void Anchor::SendPacket_EnemyRespawn(uint32_t netId) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"]     = ENEMY_RESPAWN;
    payload["netId"]    = netId;
    payload["sceneNum"] = (s16)gPlayState->sceneNum;
    payload["quiet"]    = true;

    SPDLOG_INFO("[EnemyRespawn] Sending respawn for netId={}", netId);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyRespawn(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    // Both host and non-host can receive this packet:
    //   Host receives it when the non-host was the killer and its death cycle
    //   completed first (Fix 33) — host's actor has pendingNaturalDeath=true.
    //   Non-host receives it when the host was the killer (original Fix 32).
    // The pendingNaturalDeath guard below is sufficient to prevent spurious action.

    uint32_t netId = payload.value("netId",    (uint32_t)0);
    s16      scene = payload.value("sceneNum", (s16)0);

    if (!gPlayState || scene != (s16)gPlayState->sceneNum) return;

    // Search ACTORCAT_MISC first: the Karebaba lives there during DeadItemDrop
    // and Dead states (Actor_ChangeCategory moves it at SetupDeadItemDrop; it
    // moves back to ACTORCAT_ENEMY at the end of EnKarebaba_Regrow).
    Actor* actor = nullptr;
    {
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
        while (it) {
            const EnemyNetId* e = ObjectExtension::GetInstance().Get<EnemyNetId>(it);
            if (e && e->netId == netId) { actor = it; break; }
            it = it->next;
        }
    }
    // Fallback: already back in ACTORCAT_ENEMY (completed its cycle naturally
    // before the packet arrived, or packet was delayed).
    if (!actor) {
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
        while (it) {
            const EnemyNetId* e = ObjectExtension::GetInstance().Get<EnemyNetId>(it);
            if (e && e->netId == netId) { actor = it; break; }
            it = it->next;
        }
    }

    if (!actor) {
        SPDLOG_WARN("[EnemyRespawn] netId={} not found — ignoring", netId);
        return;
    }

    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (!ext || !ext->pendingNaturalDeath) {
        // Not in a network-driven death cycle, or already respawned — nothing to do.
        return;
    }

    if (actor->id != ACTOR_EN_KAREBABA) {
        // Future: extend this branch for other enemies with self-timed respawn cycles.
        return;
    }

    s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
    if (curState == 6 || curState == 8) {
        // DeadItemDrop (6) or Dead (8): skip remaining countdown, jump to Regrow.
        // EnKarebaba_SetupRegrowNet resets world.pos to home, clears parent, zeroes
        // params, then calls SetupRegrow. EnKarebaba_Regrow's 0→20 growth counter
        // will run on subsequent frames and call Actor_ChangeCategory back to
        // ACTORCAT_ENEMY naturally when it completes.
        SPDLOG_INFO("[EnemyRespawn] netId={} skipping to Regrow (was state={})", netId, curState);
        EnKarebaba_SetupRegrowNet((EnKarebaba*)actor);
    }
    // curState == 9 (already in Regrow) or living state (already back in ACTORCAT_ENEMY):
    // natural cycle is far enough along; no intervention needed.
}
