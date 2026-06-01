#pragma once

// Refactor A.11 — shared preamble factory for ENEMY_STATE-family Anchor
// packets. Five SendPacket_* functions (Alive / DyingByLocal / Regrowing /
// Spawn / Removed) share an identical type/phase/phaseChanged/netId/
// PacketTimeline preamble. See Plans/decoupling_gap_audit_2026-05-16.md
// §17.3 / §20.3 for the motivation and Plans/refactor_sequenced_landing_plan_
// 2026-05-29.md Stage 3.6.

#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace EnemyStateSync {

// Build the common preamble for an ENEMY_STATE-family packet payload.
// Sets type=ENEMY_STATE, phase (via LifecyclePhaseName), phaseChanged,
// netId (skipped if 0), and applies PacketTimeline::SetTimelineField.
// Callers add sceneNum and phase-specific fields after.
//
// Pass netId=0 for SendPacket_EnemySpawn whose payload uses actorId in
// place of netId on the wire (the optional netId field is then set
// later in the function from the actor's EnemyNetId extension).
nlohmann::json BuildBaseEnemyStatePayload(LifecyclePhase phase,
                                          uint32_t netId,
                                          bool phaseChanged = true);

}  // namespace EnemyStateSync
