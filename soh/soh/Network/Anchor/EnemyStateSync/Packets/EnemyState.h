#pragma once

// Pillar C2 Phase 4 — unified ENEMY_STATE wire format (v1 scaffold).
//
// Consolidates four legacy packet types into a single ENEMY_STATE packet:
//   - ENEMY_UPDATE   → ENEMY_STATE with phaseChanged=false (steady state)
//   - ENEMY_DEFEATED → ENEMY_STATE with phaseChanged=true, phase=DyingByLocal/Dead, optional killer
//   - ENEMY_SPAWN    → ENEMY_STATE with phaseChanged=true, phase=Alive, optional spawnInfo
//   - ENEMY_RESPAWN  → ENEMY_STATE with phaseChanged=true, phase=Regrowing
//
// Plan: Claude/Plans/pillar_c2_enemystatesync.md, Phase 4.
//
// COMMIT A SCOPE — wire scaffold only.
// - Type string registered in Anchor.h.
// - Schema entry registered in PacketSchemas.h.
// - Dispatch route in Anchor::OnIncomingJson.
// - Send/Handle declared and defined in EnemyState.cpp.
// - NO call site changes anywhere. Legacy ENEMY_UPDATE / ENEMY_DEFEATED /
//   ENEMY_SPAWN / ENEMY_RESPAWN continue to be the wire format on both ends.
//
// Commit B will swap emit sites to SendPacket_EnemyState and verify via
// Test 3. Commit C will retire the four legacy packet types and their
// .cpp files once B passes.

#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"

#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json_fwd.hpp>

extern "C" {
#include "z64.h"  // Actor, Vec3f, Vec3s
}

namespace EnemyStateSync {

// Optional payloads attached to phase-transition packets. The legacy
// packets each carried different supplemental data; the unified
// ENEMY_STATE puts them behind separate JSON keys included only when
// the relevant transition applies.

struct KillerInfo {
    uint32_t    clientId;  // 0 = host's own kill (no remote attribution)
    std::string teamId;    // sender team for Q I Tier 2 attribution
};

struct SpawnInfo {
    int16_t  actorId;
    uint16_t params;
    Vec3f    homePos;
};

}  // namespace EnemyStateSync
