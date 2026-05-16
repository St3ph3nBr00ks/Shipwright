/**
 * SpawnableEnemyDescriptor — interface for enemy types registered with the
 * AI Director.
 *
 * The Director iterates registered descriptors each tick on the global-
 * effective-host, calls ProposeSpawn(), arbitrates the returned proposals,
 * and broadcasts ENEMY_SPAWN for the chosen group. Descriptors are
 * enemy-type-agnostic at the Director layer — Invader, future ambush
 * squads, future roaming mini-bosses all register through this surface.
 *
 * See Plans/ai_director_plan.md §2.4 for the full interface design rationale
 * and §10 for the design-decision log behind the per-method shape.
 *
 * Naming: namespace `AnchorDirector` — flat single-namespace per project
 * convention (Pitfall 11 in session_state.md: never nest under `Anchor`,
 * collides with the `class Anchor` declaration).
 */

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
}

namespace AnchorDirector {

class Director;
class SpawnableEnemyDescriptor;
struct SessionView;  // defined in Director.h

// Reserved descriptor IDs:
//   0   — sentinel "not director-spawned" (used in ENEMY_SPAWN payload)
//   1   — InvaderDescriptor
//   255 — TestDescriptor (dev-only)
// Other IDs are descriptor-private; uniqueness enforced at registration.

namespace DescriptorPriority {
    constexpr int Ambient  = 20;  // background flavor; preempted by anything else
    constexpr int Standard = 50;  // most spawns (Invader)
    constexpr int Reactive = 70;  // event-driven (ambush after chest-open, etc.)
    constexpr int Critical = 90;  // narrative / mini-boss tier
}

enum class DefeatCause : uint8_t {
    Kill          = 0,
    Leash         = 1,
    SceneExit     = 2,
    BossRoom      = 3,
    TeamCommitted = 4,
    HostMigration = 5,
};

enum class DirectorEvent : uint8_t {
    PlayerEnteredRoom    = 1,
    PlayerLeftRoom       = 2,
    PlayerKilledEnemy    = 3,
    PlayerTookDamage     = 4,
    PlayerOpenedChest    = 5,
    BossDefeated         = 6,
    CutsceneStarted      = 7,
    CutsceneEnded        = 8,
    SaveLoaded           = 9,
    SceneTransitionBegin = 10,
};

struct DirectorEventPayload {
    DirectorEvent type;
    uint32_t      clientId;       // matches AnchorClient::clientId (6-digit pseudo-random ID)
    int16_t       sceneNum;       // event-relevant scene; 0 if N/A
    int8_t        roomNum;        // event-relevant room; 0 if N/A
    nlohmann::json data;          // event-specific extras (chestId, victimNetId, etc.)
};

struct SpawnProposal {
    SpawnableEnemyDescriptor* source = nullptr;
    int16_t  sceneNum     = -1;
    int8_t   roomNum      = 0;
    Vec3f    worldPos     = { 0.0f, 0.0f, 0.0f };
    int16_t  yawTowards   = 0;
    int16_t  actorId      = 0;
    uint16_t actorParams  = 0;
    uint8_t  variantId    = 0;
    int      priority     = DescriptorPriority::Standard;
    int      groupId      = 0;  // multi-actor proposals share a groupId; 0 = solo
};

class SpawnableEnemyDescriptor {
public:
    virtual ~SpawnableEnemyDescriptor() = default;

    // --- Identity ----------------------------------------------------

    virtual uint8_t      GetDescriptorId() const = 0;
    virtual const char*  GetDebugName()    const = 0;

    // --- Lifecycle ---------------------------------------------------

    // One-time setup after registration. Director calls once per descriptor
    // before the first Tick that includes it. Override to precompute
    // scene-eligibility tables, register interest in event types, etc.
    virtual void Initialize(Director& director) { (void)director; }

    // Master gate. Director skips disabled descriptors entirely (no
    // ProposeSpawn, no event delivery, no debug-panel ticks).
    virtual bool IsEnabled() const = 0;

    // --- Per-tick proposal -------------------------------------------

    // Return 0..N spawn proposals. Most descriptors return 0 or 1; multi-
    // actor descriptors (ambush squad) return N sharing a groupId. The
    // Director honors a chosen group all-or-none under arbitration.
    //
    // `view` is a per-tick read-only snapshot of session state (player
    // positions / scene / cutscene status / etc.) built once per Tick and
    // shared across all descriptors. Never outlive the call.
    virtual std::vector<SpawnProposal> ProposeSpawn(const Director& director,
                                                    const SessionView& view) = 0;

    // Dev-only "force spawn" — return a proposal bypassing the
    // descriptor's normal eligibility gates (cooldown, cap, cutscene,
    // scene blacklist, etc.). Called from Director::ForceSpawn via the
    // debug panel's per-descriptor "Force Spawn" button.
    //
    // Descriptors that don't want to support force-spawn return empty
    // (default). Descriptors that do should still gracefully handle
    // edge cases (no target, no nav graph) — they may use a more
    // permissive fallback (e.g. spawn at the local player's position)
    // since the user explicitly asked for a spawn.
    virtual std::vector<SpawnProposal> BuildForcedProposal(const Director& director,
                                                          const SessionView& view) {
        (void)director; (void)view;
        return {};
    }

    // --- Spawn-executed callback -------------------------------------

    // Fires when a proposal from this descriptor was successfully
    // executed (Actor_Spawn returned non-null, RecordSpawn updated the
    // ledgers, ENEMY_SPAWN broadcast). Lets the descriptor record
    // landed-spawn state (netId, final position) without needing
    // RecordSpawn coupling. Default: no-op.
    virtual void OnSpawnExecuted(uint32_t netId, const Vec3f& worldPos) {
        (void)netId; (void)worldPos;
    }

    // --- Spawn-removed cleanup ---------------------------------------

    // Fires when a spawned actor of this descriptor is removed (defeated,
    // despawned, scene-vacated, leashed out). Director relays ENEMY_DEFEATED
    // to this hook + updates its own live-count ledger.
    virtual void OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
        (void)netId; (void)cause;
    }

    // --- Reactive events ---------------------------------------------

    // Director forwards events from the curated DirectorEvent set, sourced
    // from existing Anchor hooks. Descriptors override only the events they
    // care about. Fires on the host only (events are host-authoritative).
    virtual void OnEvent(const DirectorEventPayload& evt) { (void)evt; }

    // --- Host migration ----------------------------------------------

    // Per-descriptor state for DIRECTOR_STATE_SYNC. Default empty.
    virtual nlohmann::json SerializeMigrationState() const { return {}; }
    virtual void RestoreMigrationState(const nlohmann::json& j) { (void)j; }

    // Fired AFTER state-restore on the new host (isNewHost=true), and
    // BEFORE state-serialize on the old host (isNewHost=false). Use for
    // rebuilding host-perspective caches or for "I am authority now"
    // telemetry. Default no-op.
    virtual void OnDirectorHostMigrated(bool isNewHost) { (void)isNewHost; }

    // Fallback for migration-failure recovery. Director calls this when
    // DIRECTOR_STATE_SYNC was lost; descriptor walks the world to recompute
    // its live count by inspecting ACTORCAT_ENEMY actors. Default returns 0.
    virtual int RecomputeLiveCountFromWorld() { return 0; }

    // --- Debug surface -----------------------------------------------

    // Render the descriptor's own ImGui block inside the AI Director debug
    // panel. Default: nothing (Director shows count + cooldown for all
    // descriptors unconditionally).
    virtual void RenderDebugUI(const Director& director) { (void)director; }

    // One-line snapshot of descriptor-private state for the realtime log
    // feed. Director prepends "[descriptorName]" automatically.
    virtual std::string GetDebugSnapshotLine() const { return ""; }
};

}  // namespace AnchorDirector
