#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"
#include "soh/Network/Anchor/EnemyStateSync/Packets/EnemyState.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

// Pillar C2 Phase 4 Commit A — wire scaffold for the unified ENEMY_STATE
// packet. This file registers the Send/Handle entry points so the type is
// dispatchable end-to-end, but no call site emits or consumes it yet.
//
// Commit B will migrate the four legacy emit sites (ENEMY_UPDATE,
// ENEMY_DEFEATED, ENEMY_SPAWN, ENEMY_RESPAWN) onto SendPacket_EnemyState
// and switch the receive path. Commit C retires the legacy packet files
// and type strings.
//
// See Claude/Plans/pillar_c2_enemystatesync.md, Phase 4.

namespace {

// Stable wire string for the LifecyclePhase enum. Mirrors
// EnemyStateSync::LifecyclePhaseName but keeps the wire vocabulary
// pinned to this packet so a future rename of the diagnostic name
// can't silently break peers. Do NOT translate.
const char* PhaseToWire(EnemyStateSync::LifecyclePhase phase) {
    using EnemyStateSync::LifecyclePhase;
    switch (phase) {
        case LifecyclePhase::Alive:                return "Alive";
        case LifecyclePhase::DyingByLocal:         return "DyingByLocal";
        case LifecyclePhase::DyingByNetwork:       return "DyingByNetwork";
        case LifecyclePhase::AwaitingDeadItemDrop: return "AwaitingDeadItemDrop";
        case LifecyclePhase::Dead:                 return "Dead";
        case LifecyclePhase::Regrowing:            return "Regrowing";
    }
    return "Alive";
}

}  // namespace

void Anchor::SendPacket_EnemyState(uint32_t netId,
                                   Actor* actor,
                                   EnemyStateSync::LifecyclePhase phase,
                                   bool phaseChanged,
                                   const EnemyStateSync::KillerInfo* killer,
                                   const EnemyStateSync::SpawnInfo* spawnInfo) {
    if (!IsSaveLoaded()) return;
    if (gPlayState == nullptr) return;

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["netId"]        = netId;
    payload["sceneNum"]     = (s16)gPlayState->sceneNum;
    payload["phase"]        = PhaseToWire(phase);
    payload["phaseChanged"] = phaseChanged;
    PacketTimeline::SetTimelineField(payload);

    if (actor != nullptr) {
        payload["pos"]      = { actor->world.pos.x, actor->world.pos.y, actor->world.pos.z };
        payload["rot"]      = { actor->world.rot.x, actor->world.rot.y, actor->world.rot.z };
        payload["shapeRot"] = { actor->shape.rot.x, actor->shape.rot.y, actor->shape.rot.z };
        payload["health"]   = (s8)actor->colChkInfo.health;
        payload["scale"]    = { actor->scale.x, actor->scale.y, actor->scale.z };
    }

    if (killer != nullptr) {
        payload["killer"] = {
            { "clientId", killer->clientId },
            { "teamId",   killer->teamId   },
        };
    }

    if (spawnInfo != nullptr) {
        payload["spawnInfo"] = {
            { "actorId", spawnInfo->actorId },
            { "params",  spawnInfo->params  },
            { "homePos", { spawnInfo->homePos.x, spawnInfo->homePos.y, spawnInfo->homePos.z } },
        };
    }

    SPDLOG_DEBUG("[EnemyState] Send netId={} phase={} phaseChanged={}",
                 netId, PhaseToWire(phase), phaseChanged);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyState(nlohmann::json payload) {
    // Commit A — wire scaffold only. The four legacy handlers
    // (HandlePacket_EnemyUpdate / EnemyDefeated / EnemySpawn / EnemyRespawn)
    // remain the active receive path. This handler exists so the type is
    // dispatchable; Commit B will move per-phase behaviour into it.
    if (!IsSaveLoaded()) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const std::string phase = payload.value("phase", std::string("Alive"));
    const uint32_t    netId = payload.value("netId", (uint32_t)0);
    const bool        phaseChanged = payload.value("phaseChanged", false);

    SPDLOG_DEBUG("[EnemyState] Recv netId={} phase={} phaseChanged={}",
                 netId, phase, phaseChanged);
}
