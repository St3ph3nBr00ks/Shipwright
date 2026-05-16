/**
 * InvaderDescriptor — Director-side spawn-decision adapter for the
 * future AI Invader (ACTOR_EN_INVADER, ACTORCAT_NPC, hostile-NPC
 * sibling of the NPC Follower).
 *
 * **Step 11 scope**: scaffold only.
 *   - Registered alongside TestDescriptor in Director::Director().
 *   - IsEnabled reads gEnhancements.AI.Invaders.Enabled AND
 *     gEnhancements.Nav.Enabled (master gate per plan §3 / §2.3 —
 *     Nav substrate is a hard dependency for any future spawn-
 *     position selection).
 *   - ProposeSpawn returns empty vector unconditionally — no spawn
 *     logic yet. Validates the two-descriptor Director path
 *     (arbitration loop, registry iteration, debug-panel rendering).
 *
 * **Step 12 scope** (not in this file yet): eligibility predicates
 * from ai_invader_plan.md §7.1 — live-count cap, cooldown, scene-
 * flag check, boss-room-with-live-boss check, cutscene check,
 * minimum time-in-scene. Returns a proposal with placeholder
 * spawn position when all gates pass.
 *
 * **Step 13 scope**: PickSpawnPosition consumes RoomNavData to
 * pick walkable candidate nodes filtered by player-distance,
 * player-LOS, room boundaries.
 *
 * **Step 15 scope**: hand off to feature/ai-invader branch where
 * the actual ACTOR_EN_INVADER scaffolding lives. The descriptor
 * remains in the Director's registry; what changes is the actor
 * id the ExecuteSpawn call gets. Combat AI is blocked on #208
 * (follower state-machine formal design pass).
 *
 * **Translation guide reference**:
 * Claude/Plans/actor_feature_translation_guide.md catalogues the
 * NPC Follower → NPC Invader feature translation patterns. The
 * Invader actor (when built in step 15) clones NPC Follower's
 * structure with two diffs: targets players instead of leader,
 * and damage routing is host-authoritative (race B).
 *
 * Reserved descriptor id 1 per SpawnableEnemyDescriptor.h.
 */

#pragma once

#include "../SpawnableEnemyDescriptor.h"

#include <unordered_map>

namespace AnchorDirector {

// Forward-decls for types referenced in method signatures below.
// SessionView is already forward-declared in SpawnableEnemyDescriptor.h
// (used by ProposeSpawn); PlayerSnapshot is added here because
// IsValidTarget / PickValidTarget reference it but the parent header
// doesn't. Both are defined in Director.h — including Director.h here
// would create a circular include (Director.h → InvaderDescriptor.h
// via the Director's registry construction).
struct PlayerSnapshot;

// Per-Invader runtime state. Keyed on netId in mActiveInvaders.
// Replaces the earlier scalar mLastSpawnPos / mLastSpawnNetId
// approach now that MaxAlive can be > 1 and each invader carries
// its own sticky target / orphan timer / pending-follow state.
//
// Phase 1 scope: targetClientId (sticky), lastKnownScene/Room (for
// orphan-detection), orphanFrames (60s timeout). Phase 2 adds
// outOfSightFrames + cantReachFrames once the real ACTOR_EN_INVADER
// has the signals (#208-blocked).
struct InvaderRuntimeState {
    Vec3f    lastSpawnPos      = { 0.0f, 0.0f, 0.0f };
    uint32_t targetClientId    = 0;     // sticky — 0 means "needs initial target"
    int16_t  lastKnownSceneNum = 0;
    int8_t   lastKnownRoomNum  = 0;
    int      orphanFrames      = 0;     // ticks since any team-player was in (scene, room)
    bool     pendingFollowSpawn = false; // Phase 2 lifecycle: true between PlayerEnteredRoom event and respawn
    int      followGraceFrames = 0;     // countdown for follow-spawn grace period
    Vec3f    pendingFollowPos  = { 0.0f, 0.0f, 0.0f };  // captured entrance pos
    int16_t  pendingFollowScene = 0;
    int8_t   pendingFollowRoom  = 0;
};

class InvaderDescriptor : public SpawnableEnemyDescriptor {
public:
    // --- Identity ---
    uint8_t      GetDescriptorId() const override { return 1; }
    const char*  GetDebugName()    const override { return "Invader"; }

    // --- Lifecycle ---
    bool IsEnabled() const override;

    // --- Per-tick proposal (scaffold: empty) ---
    std::vector<SpawnProposal> ProposeSpawn(const Director& director,
                                            const SessionView& view) override;

    // --- Dev-only force spawn (panel button) ---
    std::vector<SpawnProposal> BuildForcedProposal(const Director& director,
                                                   const SessionView& view) override;

    // --- Spawn-executed callback ---
    void OnSpawnExecuted(uint32_t netId, const Vec3f& worldPos) override;

    // --- Spawn-removed cleanup ---
    void OnSpawnRemoved(uint32_t netId, DefeatCause cause) override;

    // --- Reactive events (Phase 1 §7.5 scene-following) ---
    void OnEvent(const DirectorEventPayload& evt) override;

    // --- Per-tick lifecycle scan (Phase 1 §7.5) ---
    void OnTick(Director& director, const SessionView& view) override;

    // --- Debug surface ---
    std::string GetDebugSnapshotLine() const override;
    void RenderDebugUI(const Director& director) override;

    // --- In-world debug-draw accessor ---
    // Returns the last successfully-executed spawn's world position, or
    // nullopt if no spawn has happened this session. Used by the
    // DirectorDebugDraw render hook to render an in-world marker.
    //
    // LastSpawnSceneNum returns the scene the spawn happened in (-1 = no
    // spawn yet). DirectorDebugDraw gates the marker render on this
    // matching gPlayState->sceneNum so the post doesn't ghost-render in
    // a different scene at the same world coords (log 197 symptom —
    // Inside Deku Tree spawn at (17,-4,327), then user teleported to
    // Castle Town where (17,-4,327) maps to a visible spot, causing
    // the marker to appear in town with no actual Invader).
    bool HasLastSpawn() const { return mHasLastSpawn; }
    const Vec3f& LastSpawnPos() const { return mLastSpawnPos; }
    int16_t LastSpawnSceneNum() const { return mLastSpawnSceneNum; }

    // Default tunables. CVar overrides take precedence at read time;
    // these are the fallback values per ai_invader_plan.md §3. Public
    // so the .cpp's anonymous-namespace ReadMaxAlive / ReadCooldownMs
    // helpers can reference them without private-access trouble
    // (Pitfall 16 — anon-namespace functions are not class members).
    static constexpr int  kDefaultMaxAlive       = 1;        // per plan §3 (range 1..4)
    static constexpr int  kDefaultCooldownMs     = 90 * 1000; // 90s per plan §3 (range 30..600)

private:
    // Counters surfaced via the debug panel. Like TestDescriptor, the
    // descriptor doesn't get a "spawn executed" callback today — so we
    // track proposalsOffered (non-empty ProposeSpawn results) rather
    // than actual spawn count. Live count is queried from the Director.
    int      mProposalsOffered  = 0;
    int      mTotalRemoved      = 0;
    uint32_t mLastRemovedNetId  = 0;

    // Per-Invader runtime state. Keyed on netId. Populated by
    // OnSpawnExecuted, erased by OnSpawnRemoved, tracked per-tick by
    // ProposeSpawn (sticky target re-evaluation, orphan timer).
    std::unordered_map<uint32_t, InvaderRuntimeState> mActiveInvaders;

    // Scalar mirror of the most-recently-spawned invader's position
    // for the in-world red-marker overlay. Updated on every
    // OnSpawnExecuted; never cleared so the marker persists across
    // kills (intentional — useful for finding orphan locations).
    // With MaxAlive>1 this just tracks whichever spawn fired last;
    // future enhancement could render markers for ALL active invaders.
    Vec3f    mLastSpawnPos      = { 0.0f, 0.0f, 0.0f };
    uint32_t mLastSpawnNetId    = 0;
    bool     mHasLastSpawn      = false;  // false until first OnSpawnExecuted
    int16_t  mLastSpawnSceneNum = -1;     // scene of last spawn; gates DebugDraw render

    // Step 12 diagnostic — throttles per-tick rejection-reason SPDLOGs
    // to ~once per 5s at 20fps. Mirrors TestDescriptor's pattern;
    // gated by gEnhancements.AI.Director.LogProposals.
    int      mTicksSinceLog    = 0;

    // --- Phase 1 helpers ---

    // Phase 1 §7.5 valid-target predicate. A player is a valid target
    // when: online + save loaded + not in cutscene + not in boss room
    // with live boss + not in blacklisted scene + alive (proxy: save
    // loaded). The first three are read from PlayerSnapshot directly;
    // boss-room is stubbed false (step 12 deferral); blacklist consults
    // IsSceneFlaggedNoInvaders. Used by ProposeSpawn (initial target
    // pick) + the per-tick lifecycle scan (sticky-target re-evaluate).
    bool IsValidTarget(const PlayerSnapshot& p) const;

    // Returns the best valid target from the view, or nullptr if none
    // exists. "Best" today is most-isolated (same as MostIsolatedPlayer)
    // filtered through IsValidTarget. When all players are invalid
    // (everyone in boss room / cutscene / blacklisted scene / dead),
    // returns nullptr.
    const PlayerSnapshot* PickValidTarget(const SessionView& view) const;

    // Phase 1 §7.5 PERSISTENT-target predicate — same as IsValidTarget
    // but excludes the CUTSCENE invalidator. Used by the all-unavailable
    // despawn check so transient cutscenes (Kakariko entry, Lon Lon
    // Ranch entry, etc.) don't kill in-flight follow-spawns or freshly-
    // spawned Invaders sitting through an entry sequence. Cutscene is
    // a transient state; only permanent unavailability (blacklist
    // scene, boss room, save not loaded) should immediately despawn.
    bool IsPersistentTarget(const PlayerSnapshot& p) const;
};

}  // namespace AnchorDirector
