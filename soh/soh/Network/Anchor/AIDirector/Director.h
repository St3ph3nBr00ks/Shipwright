/**
 * Director — host-authoritative spawn-decision substrate for Flotilla AI.
 *
 * Singleton that runs on every client; ticking work is gated to the global-
 * effective-host. Registered SpawnableEnemyDescriptors (Invader first,
 * future enemy types later) implement per-type spawn rules through the
 * interface in SpawnableEnemyDescriptor.h.
 *
 * Step 1 (this scaffold): empty registry, host-gated Tick body that
 * does no work, ledgers declared but not yet used, accessor surface
 * stubbed. Subsequent steps populate per Plans/ai_director_plan.md §9.
 *
 * See:
 *   - Plans/ai_director_plan.md — full design
 *   - Plans/ai_invader_plan.md  — first descriptor's design
 *
 * Naming: namespace `AnchorDirector` — flat single-namespace per project
 * convention.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "SpawnableEnemyDescriptor.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"  // Vec3f
}

namespace AnchorDirector {

// Per-player snapshot inside SessionView. Built fresh each Tick from the
// authoritative AnchorClient map; never aliased outside the tick.
//
// Fields sourced from `Anchor::Instance->clients[clientId]` unless noted.
// Fields marked "step 12+" are stubs in step 2 — they'll be wired when
// InvaderDescriptor first needs them; the columns exist in the struct
// now so descriptors can reference them without API churn later.
struct PlayerSnapshot {
    uint32_t     clientId;              // matches AnchorClient::clientId
    std::string  teamId;
    int16_t      sceneNum;
    int8_t       roomNum;
    Vec3f        worldPos;              // last PLAYER_UPDATE; few-frames stale for non-self
    bool         isLocal;               // clientId == Anchor::Instance->ownClientId
    bool         isSaveLoaded;
    bool         isInCutscene;          // csCtxState != CS_STATE_IDLE
    bool         isInvulnerable;        // invincibilityTimer != 0
    bool         followerActive;        // AI Follower mode is on for this client
    bool         isClimbing;            // peer's local Player is climbing (G1/G2)

    // step 12+ fields — populated lazily when a descriptor needs them.
    bool         currentRoomHasLiveBoss = false;
    int          framesInCurrentScene   = 0;
};

// Per-tick read-only snapshot of session state passed to descriptors.
// Built once at the top of Tick (BuildSessionView), shared across all
// descriptors via the ProposeSpawn second parameter. Never outlives Tick.
struct SessionView {
    std::vector<PlayerSnapshot> players;
    uint64_t globalFrameCounter = 0;   // Director-internal counter; host-side only
    uint32_t currentTickMs      = 50;  // Anchor::Instance->mAvgGameTickMs

    // Valid when at least one online player with a loaded save is present.
    // Tick early-exits on !IsValid().
    bool IsValid() const { return !players.empty(); }

    // --- Helpers (computed lazily; cheap O(N) over players) --------

    // True if any player is in the named scene.
    bool AnyPlayerInScene(int16_t sceneNum) const;

    // True if any player has !ShouldAdvanceWorldTime (cutscene / textbox /
    // freeze). Used by spawn-director predicates: don't spawn while
    // anyone is watching a cutscene.
    bool AnyPlayerInCutscene() const;

    // True if every player in this scene is also inside the boss room
    // (and the boss is alive). step 12+: returns false until the boss-
    // room registry plumbing lands. Descriptors that consult it earlier
    // will see "no team committed" — safe default.
    bool AllPlayersInBossRoom(int16_t sceneNum) const;

    // Player with greatest min-distance to any other player. Returns
    // nullptr in the trivial single-player case. Used by spawn-director
    // candidate-selection heuristics ("invade the isolated one").
    const PlayerSnapshot* MostIsolatedPlayer() const;

    // Lookup by clientId. Returns nullptr if not present.
    const PlayerSnapshot* PlayerByClientId(uint32_t clientId) const;
};

class Director {
public:
    // Singleton accessor. Director exists on every client; non-host
    // instances ignore Tick early-exit and receive DIRECTOR_STATE_SYNC
    // (step 5) so the debug panel can render cached host state.
    static Director& Instance();

    // --- Registration -------------------------------------------------

    // Take ownership of `descriptor` and push onto registry. Initialize()
    // runs on the next Tick (deferred so callers can register multiple
    // descriptors before any descriptor sees the Director). Returns the
    // raw pointer for callers that want a stable handle (the unique_ptr
    // remains owned by Director).
    SpawnableEnemyDescriptor* Register(std::unique_ptr<SpawnableEnemyDescriptor> descriptor);

    // --- Tick driver --------------------------------------------------

    // Called every OnGameFrameUpdate from Anchor::RegisterDirectorHooks.
    // Step 1: early-exits on non-host and on empty registry. With no
    // descriptors registered, the per-tick body is a no-op even on host.
    void Tick();

    // --- Accessors used by descriptors --------------------------------

    // Returns ticks elapsed since the last spawn for (scene, room, descId).
    // Returns INT_MAX when no spawn has been recorded — callers that compare
    // against a threshold (`framesSinceLast >= cooldownTicks`) get the
    // "trivially elapsed" answer on first-spawn.
    int  GetFramesSinceLastSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId) const;

    // Returns live count for this descriptor's spawned actors. Bumped by
    // RecordSpawn; decremented by OnEnemyRemoved.
    int  GetLiveCount(uint8_t descId) const;

    // Returns true when the (scene, room, descriptor) tuple's cooldown has
    // elapsed, i.e. (now - lastSpawnFrame) >= MsToGameTicks(ms). Internally
    // routes through Anchor::Instance->MsToGameTicks so the threshold
    // respects current measured tick rate.
    bool MsCooldownElapsed(int16_t sceneNum, int8_t roomNum, uint8_t descId, int ms) const;

    // --- Ledger update --------------------------------------------------

    // Record a successful spawn in all three ledgers atomically. Called by
    // Director's own ExecuteSpawn (step 5+ — wired after the ENEMY_SPAWN
    // schema bump). Step 3: method available for unit-testing the ledger
    // shape via TestDescriptor.
    //
    // - Stamps mLastSpawnFrameByKey with the current mGlobalFrameCounter.
    // - Bumps mLiveCountByDescriptor.
    // - Adds the netId → descriptorId mapping so OnEnemyRemoved routes
    //   the removal back to the right descriptor.
    //
    // Triggers a DIRECTOR_STATE_SYNC broadcast at the end (host only)
    // so peers' cached Director state stays current for migration.
    void RecordSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId, uint32_t netId);

    // --- Host migration snapshot (step 5) -------------------------------

    // Build a full snapshot of Director state for DIRECTOR_STATE_SYNC.
    // Includes ledgers + per-descriptor SerializeMigrationState() blobs.
    // Called by SendPacket_DirectorStateSync on host; safe to call at any
    // time (returns whatever state is currently present).
    nlohmann::json SerializeMigrationSnapshot() const;

    // Apply a received DIRECTOR_STATE_SYNC snapshot. Overwrites local
    // ledgers, mGlobalFrameCounter, and calls each known descriptor's
    // RestoreMigrationState with that descriptor's slice. Always safe
    // to call on a peer; the migration-hook OnDirectorHostMigrated fires
    // separately via OnHostMigrated when the host role transfers.
    void ApplyMigrationSnapshot(const nlohmann::json& snapshot);

    // --- Lifecycle hooks called by Anchor / packet handlers -----------

    // Called from ENEMY_DEFEATED dispatch when the removed actor was
    // director-spawned (mNetIdToDescriptor lookup succeeds). Routes to
    // the descriptor's OnSpawnRemoved + decrements live count.
    void OnEnemyRemoved(uint32_t netId, DefeatCause cause);

    // Called from Anchor when a curated DirectorEvent fires. Forwards to
    // every enabled descriptor's OnEvent.
    void NotifyEvent(const DirectorEventPayload& evt);

    // Called from Anchor when global-effective-host migration completes.
    // isNewHost=true on the incoming host (after DIRECTOR_STATE_SYNC has
    // populated ledgers); isNewHost=false on the outgoing host (just
    // before SerializeMigrationState).
    void OnHostMigrated(bool isNewHost);

    // --- Read-only access for the debug panel (step 8) ----------------

    const std::vector<std::unique_ptr<SpawnableEnemyDescriptor>>& GetDescriptors() const {
        return mDescriptors;
    }

private:
    Director();  // step 7: constructs default descriptor registry
    Director(const Director&) = delete;
    Director& operator=(const Director&) = delete;

    // Build a SessionView snapshot for this tick. Step 1 returns an
    // always-invalid view; step 2 walks players + DummyPlayers.
    SessionView BuildSessionView() const;

    // Execute a winning spawn proposal on the host:
    //   - Actor_Spawn locally; read assigned netId via EnemyNetId extension.
    //   - RecordSpawn (ledgers + DIRECTOR_STATE_SYNC broadcast).
    //   - SendPacket_EnemySpawn with descriptor identity.
    // Returns true on success; false if Actor_Spawn failed or netId
    // couldn't be read (rare — actor limit reached, non-EnemyNetId actor).
    bool ExecuteSpawn(const SpawnProposal& proposal);

    // Ledgers --------------------------------------------------------
    //
    // Key for cooldown ledger packs (sceneNum, roomNum, descriptorId)
    // into one uint32: scene<<16 | room<<8 | descId.
    std::unordered_map<uint32_t, int>     mLastSpawnFrameByKey;
    std::unordered_map<uint8_t,  int>     mLiveCountByDescriptor;
    std::unordered_map<uint32_t, uint8_t> mNetIdToDescriptor;

    // Registry --------------------------------------------------------
    std::vector<std::unique_ptr<SpawnableEnemyDescriptor>> mDescriptors;

    // Initialize() runs on first tick after descriptors are registered.
    // Reset to false on Register() so newly-registered descriptors get
    // Initialize'd next tick.
    bool mInitializedDescriptors = false;

    // Host-side tick counter. Increments at the top of Tick (after the
    // IsEffectiveHost gate) so it tracks "ticks while host" rather than
    // "ticks since boot". Cooldown ledger compares against this counter
    // via MsToGameTicks(ms). Persists across descriptor (un)registration.
    uint64_t mGlobalFrameCounter = 0;
};

}  // namespace AnchorDirector
