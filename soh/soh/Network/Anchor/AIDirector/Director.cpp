/**
 * Director — implementation. Plans/ai_director_plan.md §9.
 *
 * Step 1 (landed): empty registry, host-gated Tick body, ledgers declared
 * but unused, lifecycle hooks stubbed.
 * Step 2 (landed): BuildSessionView walks Anchor::clients into a per-tick
 * PlayerSnapshot vector + helper methods (AnyPlayerInScene /
 * AnyPlayerInCutscene / MostIsolatedPlayer / PlayerByClientId).
 * AllPlayersInBossRoom / currentRoomHasLiveBoss / framesInCurrentScene
 * stay stubbed until step 12+ (InvaderDescriptor needs them).
 * Step 3 (landed): ledger plumbing — RecordSpawn writes all three ledgers;
 * GetFramesSinceLastSpawn returns ticks-elapsed (was returning raw stored
 * frame); MsCooldownElapsed wired through Anchor::MsToGameTicks. ENEMY_-
 * DEFEATED routes to Director::OnEnemyRemoved on host from both the local
 * OnEnemyDefeat hook (HookHandlers.cpp) and the HandlePacket_EnemyDefeated
 * receive path (EnemyState.cpp).
 * Step 4 (landed): ENEMY_STATE schema 5 — director identity fields on
 * dynamic-spawn packets (Common/PacketSchemas.h + EnemyState.cpp).
 * Step 5 (landed): DIRECTOR_STATE_SYNC packet plus SerializeMigrationSnapshot
 * / ApplyMigrationSnapshot. Host broadcasts on every RecordSpawn / OnEnemy-
 * Removed; peers cache; OnBecameEffectiveHost fires Director::OnHostMigrated
 * so descriptors run their migration hook.
 * Step 6 (landed): event-forwarding plumbing — DirectorEvent::Player-
 * EnteredRoom / PlayerLeftRoom / SaveLoaded fire from UCS receive
 * (UpdateClientState.cpp) on peer transitions; PlayerKilledEnemy +
 * BossDefeated fire from OnEnemyDefeat (HookHandlers.cpp) and Player-
 * KilledEnemy also fires from HandlePacket_EnemyDefeated (EnemyState.cpp)
 * with killerClientId attribution. Other events (PlayerTookDamage,
 * PlayerOpenedChest, CutsceneStarted/Ended, SceneTransitionBegin) wire
 * when a descriptor first needs them — same pattern: build
 * DirectorEventPayload, call NotifyEvent.
 * Step 7 (landed): proposal-arbitration loop in Tick — collects proposals
 * from enabled descriptors, picks highest-priority (ties → oldest last-
 * spawn-frame), runs ExecuteSpawn. ExecuteSpawn calls Actor_Spawn,
 * reads assigned netId from EnemyNetId, RecordSpawn (broadcasts
 * DIRECTOR_STATE_SYNC), SendPacket_EnemySpawn with descriptor identity.
 * TestDescriptor registered in the Director constructor — gated by
 * gEnhancements.AI.Director.TestDescriptorEnabled CVar.
 * Step 8+: ImGui debug panel.
 *
 * Hook registration lives in Anchor::RegisterDirectorHooks (HookHandlers.cpp
 * — sibling to the existing RegisterFollowerHooks call). The OnGameFrameUpdate
 * lambda there forwards to Director::Instance().Tick() when isConnected.
 */

#include "Director.h"
#include "Descriptors/InvaderDescriptor.h"
#include "Descriptors/TestDescriptor.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

#include <libultraship/bridge/consolevariablebridge.h>  // CVarGetInteger
#include <libultraship/libultraship.h>  // SPDLOG_*

#include "../Anchor.h"
#include "../Common/SceneAuthority.h"
#include "soh/ActorDB.h"  // ActorDB::Instance->RetrieveEntry — pre-spawn object lookup
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"  // CVAR_ENHANCEMENT

extern "C" {
#include "variables.h"
#include "functions.h"  // Actor_Spawn
#include "macros.h"     // GET_PLAYER macro (lives in macros.h, NOT functions.h/z64.h)
#include "z64.h"
#include "z64cutscene.h"  // CS_STATE_IDLE

// Object_Spawn has no public header — defined only in src/code/z_scene.c.
// Forward-decl here for the pre-spawn object preload in ExecuteSpawn.
// Returns the object slot index; we ignore the return value, only need
// the side-effect of queuing the object load.
s32 Object_Spawn(ObjectContext* objectCtx, s16 objectId);
}

// gPlayState lives in z_play.c with C linkage. Use the unambiguous
// `extern "C"` form so the symbol resolves without depending on
// transitive C-linkage declarations from other headers (the bare
// `extern PlayState* gPlayState;` form used elsewhere in Anchor/
// only works because z64.h or similar transitively provides it
// inside an extern "C" block — fragile under different include
// orders). See SohModals.cpp:12 / ResourceManagerHelpers.cpp:18
// for the same robust pattern.
extern "C" PlayState* gPlayState;

namespace AnchorDirector {

namespace {
    // Cooldown-ledger key packing. Keeps the three identity bits in a
    // single uint32 so std::unordered_map<uint32_t, int> handles the
    // ledger without nested maps.
    inline uint32_t MakeCooldownKey(int16_t sceneNum, int8_t roomNum, uint8_t descId) {
        return (static_cast<uint32_t>(static_cast<uint16_t>(sceneNum)) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(roomNum))   << 8)
             |  static_cast<uint32_t>(descId);
    }
}  // namespace

Director& Director::Instance() {
    static Director sInstance;
    return sInstance;
}

Director::Director() {
    // Step 7: register the dev-only TestDescriptor. Permanently present
    // so the debug panel can show its state; gated to no-op by its own
    // IsEnabled() reading the gEnhancements.AI.Director.TestDescriptor-
    // Enabled CVar. Production builds carry the registration cost (one
    // virtual table entry + one per-tick IsEnabled hash-map miss) — well
    // under measurable.
    Register(std::make_unique<TestDescriptor>());

    // Step 11: register the InvaderDescriptor scaffold. ProposeSpawn
    // returns empty unconditionally — no spawn logic until step 12
    // (eligibility predicates) + step 13 (PickSpawnPosition) +
    // step 15 (real ACTOR_EN_INVADER actor). Permanently present so
    // the debug panel surfaces the descriptor row even when the
    // master Invaders.Enabled CVar is off.
    Register(std::make_unique<InvaderDescriptor>());
}

SpawnableEnemyDescriptor* Director::Register(std::unique_ptr<SpawnableEnemyDescriptor> descriptor) {
    if (!descriptor) {
        return nullptr;
    }
    SpawnableEnemyDescriptor* raw = descriptor.get();
    mDescriptors.push_back(std::move(descriptor));
    // Run Initialize on the next Tick. Deferring lets callers register
    // multiple descriptors back-to-back without any descriptor seeing the
    // Director mid-registration.
    mInitializedDescriptors = false;
    return raw;
}

void Director::Tick() {
    // Director runs on every client but only the global-effective-host
    // actually decides spawns. Non-host clients still receive
    // DIRECTOR_STATE_SYNC (step 5) so their cached state is visible in
    // the debug panel, but Tick early-exits here.
    if (!::SceneAuthority::IsEffectiveHost()) {
        return;
    }

    // One-shot proof the host-gated Tick is firing. Gated behind
    // gEnhancements.AI.Director.LogProposals so production logs stay
    // quiet; flip the CVar on when investigating "why isn't the
    // Director doing anything?" — sees this line confirms the hook
    // chain is alive.
    static bool sLoggedFirstHostTick = false;
    if (!sLoggedFirstHostTick &&
        CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.LogProposals"), 0) != 0) {
        SPDLOG_INFO("[Director] First host Tick — descriptors={} (will arbitrate proposals)",
                    mDescriptors.size());
        sLoggedFirstHostTick = true;
    }

    // Host-side tick counter. Incremented unconditionally after the
    // IsEffectiveHost gate so the cooldown ledger reads consistent
    // values regardless of empty-registry / invalid-view early-exits.
    ++mGlobalFrameCounter;

    // No descriptors registered → nothing to do. Step 1 always lands here
    // because nothing has called Register() yet. Step 7 lands TestDescriptor;
    // step 11 lands InvaderDescriptor.
    if (mDescriptors.empty()) {
        return;
    }

    // First-tick initialization for newly-registered descriptors.
    if (!mInitializedDescriptors) {
        for (auto& d : mDescriptors) {
            d->Initialize(*this);
        }
        mInitializedDescriptors = true;
    }

    // Build the per-tick view. Empty / invalid (no players with save
    // loaded) → skip proposal work this tick.
    SessionView view = BuildSessionView();
    if (!view.IsValid()) {
        return;
    }

    // Step 7: collect proposals from enabled descriptors, arbitrate,
    // ExecuteSpawn on the winner. Single group per tick max (plan §3).
    // Multi-actor group support is in the wire — step 7 still single-
    // actor since TestDescriptor returns one proposal at a time.
    std::vector<SpawnProposal> proposals;
    for (auto& d : mDescriptors) {
        if (!d->IsEnabled()) continue;
        auto descProposals = d->ProposeSpawn(*this, view);
        for (auto& p : descProposals) {
            // Defensive: ensure source is set so post-arbitration code
            // can read descriptor identity off the proposal.
            if (p.source == nullptr) p.source = d.get();
            proposals.push_back(std::move(p));
        }
    }
    if (proposals.empty()) return;

    // Arbitrate: highest priority wins; ties broken by oldest last-
    // spawn-frame for the proposal's (scene, room, descId) tuple
    // (most overdue gets to spawn).
    size_t winnerIdx = 0;
    for (size_t i = 1; i < proposals.size(); ++i) {
        const SpawnProposal& w = proposals[winnerIdx];
        const SpawnProposal& c = proposals[i];
        if (c.priority > w.priority) {
            winnerIdx = i;
            continue;
        }
        if (c.priority < w.priority) continue;
        // Tiebreak by oldest last-spawn-frame.
        int wFrames = GetFramesSinceLastSpawn(w.sceneNum, w.roomNum,
                                              w.source->GetDescriptorId());
        int cFrames = GetFramesSinceLastSpawn(c.sceneNum, c.roomNum,
                                              c.source->GetDescriptorId());
        if (cFrames > wFrames) winnerIdx = i;
    }
    ExecuteSpawn(proposals[winnerIdx]);
}

bool Director::ExecuteSpawn(const SpawnProposal& proposal) {
    if (gPlayState == nullptr || proposal.source == nullptr) {
        return false;
    }
    if (proposal.actorId == 0) {
        SPDLOG_WARN("[Director] ExecuteSpawn rejected: actorId=0 from descriptor '{}'",
                    proposal.source->GetDebugName());
        return false;
    }

    // Pre-spawn object preload. OoT actors defer their init() until
    // their required object is loaded; if the object isn't in the
    // scene's object list when Actor_Spawn fires, init() never runs
    // and the actor exists as an invisible-but-physically-present
    // ghost (its collider works, hit effects fire, but it has no
    // model). Field-test signature 2026-05-15: spawned EN_TEST in
    // Bottom of the Well room 0 (no native object_st), Stalfos
    // killed Link with invisible body and visible sword-hit
    // sparkles.
    //
    // Object_Spawn is idempotent for already-loaded objects (returns
    // the existing slot index). For new objects, it queues an async
    // DMA load; the actor's deferred-init loop picks the object up
    // a few frames later, then init() runs and the model appears.
    // Net visual: actor pops in a frame or two late instead of never.
    const auto& dbEntry = ActorDB::Instance->RetrieveEntry(proposal.actorId);
    if (dbEntry.entry.valid && dbEntry.entry.objectId > 0) {
        Object_Spawn(&gPlayState->objectCtx, (s16)dbEntry.entry.objectId);
    }

    // Spawn the actor locally on host. The OnActorSpawn hook will assign
    // a deterministic netId via the existing EnemyNetId extension path.
    //
    // isSpawningDirectorActor flag suppresses the OnActorSpawn auto-
    // broadcast of ENEMY_SPAWN — we send our own below with descriptor
    // identity in the schema-5 fields. Without this guard every Director
    // spawn produced two packets (auto director=0/0 + explicit
    // director=descId/variantId).
    Anchor::Instance->isSpawningDirectorActor = true;
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState,
                                 proposal.actorId,
                                 proposal.worldPos.x,
                                 proposal.worldPos.y,
                                 proposal.worldPos.z,
                                 /*rotX=*/0,
                                 proposal.yawTowards,
                                 /*rotZ=*/0,
                                 proposal.actorParams);
    Anchor::Instance->isSpawningDirectorActor = false;
    if (spawned == nullptr) {
        SPDLOG_WARN("[Director] ExecuteSpawn: Actor_Spawn failed for actorId={} descriptor='{}' "
                    "(actor limit, invalid id, or object not loaded)",
                    proposal.actorId, proposal.source->GetDebugName());
        return false;
    }

    // Read the netId assigned by OnActorSpawn. Synced actors get an
    // EnemyNetId extension; non-synced ones won't and we abort the
    // bookkeeping (the actor is still in the world — it just won't be
    // tracked by the Director). Tests should pick synced actor ids.
    const EnemyNetId* extConst = ObjectExtension::GetInstance().Get<EnemyNetId>(spawned);
    if (extConst == nullptr || extConst->netId == 0) {
        SPDLOG_WARN("[Director] ExecuteSpawn: spawned actorId={} has no EnemyNetId — "
                    "Director will not track this spawn (descriptor='{}')",
                    proposal.actorId, proposal.source->GetDebugName());
        return true;  // actor exists; bookkeeping skipped
    }
    const uint32_t netId = extConst->netId;

    // Ledger update + DIRECTOR_STATE_SYNC broadcast (handled internally).
    RecordSpawn(proposal.sceneNum, proposal.roomNum,
                proposal.source->GetDescriptorId(), netId);

    // Wire-format broadcast: peers spawn the suppressed-local-AI replica
    // via standard ENEMY_SPAWN pipeline. Carries director identity in
    // the schema-5 mandatory fields (step 4).
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendPacket_EnemySpawn(
            spawned,
            proposal.source->GetDescriptorId(),
            proposal.variantId,
            proposal.groupId);
    }

    SPDLOG_INFO("[Director] ExecuteSpawn ok: descriptor='{}' actorId={} netId={} "
                "pos=({:.1f},{:.1f},{:.1f}) variant={} group={}",
                proposal.source->GetDebugName(), proposal.actorId, netId,
                proposal.worldPos.x, proposal.worldPos.y, proposal.worldPos.z,
                proposal.variantId, proposal.groupId);
    return true;
}

int Director::GetFramesSinceLastSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId) const {
    uint32_t key = MakeCooldownKey(sceneNum, roomNum, descId);
    auto it = mLastSpawnFrameByKey.find(key);
    if (it == mLastSpawnFrameByKey.end()) {
        return INT_MAX;  // never spawned → cooldown trivially elapsed
    }
    const uint64_t storedFrame = static_cast<uint64_t>(it->second);
    if (mGlobalFrameCounter < storedFrame) {
        // Defensive: shouldn't happen (host counter monotonic since boot).
        // Could surface during host migration if a stale entry crossed.
        return 0;
    }
    const uint64_t delta = mGlobalFrameCounter - storedFrame;
    return (delta > static_cast<uint64_t>(INT_MAX))
               ? INT_MAX
               : static_cast<int>(delta);
}

int Director::GetLiveCount(uint8_t descId) const {
    auto it = mLiveCountByDescriptor.find(descId);
    return (it == mLiveCountByDescriptor.end()) ? 0 : it->second;
}

bool Director::MsCooldownElapsed(int16_t sceneNum, int8_t roomNum, uint8_t descId, int ms) const {
    if (ms <= 0) {
        return true;  // no cooldown requested
    }
    const int framesSince = GetFramesSinceLastSpawn(sceneNum, roomNum, descId);
    if (framesSince == INT_MAX) {
        return true;  // never spawned
    }
    if (Anchor::Instance == nullptr) {
        return true;  // host gate already caught this; defensive only
    }
    const int requiredTicks = Anchor::Instance->MsToGameTicks(ms);
    return framesSince >= requiredTicks;
}

void Director::RecordSpawn(int16_t sceneNum, int8_t roomNum, uint8_t descId, uint32_t netId) {
    const uint32_t key = MakeCooldownKey(sceneNum, roomNum, descId);
    // mGlobalFrameCounter is uint64_t but the ledger holds int. The counter
    // increments at most once per tick — overflow would take >68 years at
    // 1000 Hz. Storing as int is fine; INT_MAX cap is a far horizon.
    mLastSpawnFrameByKey[key] = (mGlobalFrameCounter > static_cast<uint64_t>(INT_MAX))
                                    ? INT_MAX
                                    : static_cast<int>(mGlobalFrameCounter);
    ++mLiveCountByDescriptor[descId];
    if (netId != 0) {
        mNetIdToDescriptor[netId] = descId;
    }

    // Step 5: broadcast updated state to peers so any migration target
    // has a fresh snapshot. Host-side only; non-host shouldn't be calling
    // RecordSpawn at all, but the SendPacket itself gates on host.
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendPacket_DirectorStateSync();
    }
}

void Director::OnEnemyRemoved(uint32_t netId, DefeatCause cause) {
    auto it = mNetIdToDescriptor.find(netId);
    if (it == mNetIdToDescriptor.end()) {
        return;  // not director-spawned
    }
    uint8_t descId = it->second;

    for (auto& d : mDescriptors) {
        if (d->GetDescriptorId() == descId) {
            d->OnSpawnRemoved(netId, cause);
            break;
        }
    }

    int& count = mLiveCountByDescriptor[descId];
    count = std::max(0, count - 1);

    mNetIdToDescriptor.erase(it);

    // Step 5: broadcast updated state to peers so any migration target
    // has a fresh snapshot. Host-side only.
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->SendPacket_DirectorStateSync();
    }
}

void Director::NotifyEvent(const DirectorEventPayload& evt) {
    for (auto& d : mDescriptors) {
        if (!d->IsEnabled()) {
            continue;
        }
        d->OnEvent(evt);
    }
}

void Director::OnHostMigrated(bool isNewHost) {
    for (auto& d : mDescriptors) {
        d->OnDirectorHostMigrated(isNewHost);
    }
}

SessionView Director::BuildSessionView() const {
    SessionView view;
    view.globalFrameCounter = mGlobalFrameCounter;

    // Defensive: Tick already early-exits on non-host via SceneAuthority,
    // and RegisterDirectorHooks only registers the hook when connected,
    // but we hit this path from the debug panel too (step 8) where Anchor
    // state may still be torn down.
    if (Anchor::Instance == nullptr) {
        return view;
    }
    view.currentTickMs = Anchor::Instance->mAvgGameTickMs;

    view.players.reserve(Anchor::Instance->clients.size());
    for (const auto& [clientId, client] : Anchor::Instance->clients) {
        if (!client.online) continue;

        const bool isLocal = (clientId == Anchor::Instance->ownClientId);

        PlayerSnapshot snap{};
        snap.clientId        = clientId;
        snap.teamId          = client.teamId;
        snap.isLocal         = isLocal;
        snap.followerActive  = client.followerActive;
        snap.isClimbing      = client.isClimbing;

        if (isLocal) {
            // The local client's entry in Anchor::Instance->clients is only
            // touched by RecomputeEffectiveHost / handshake bookkeeping — it
            // never receives the per-frame "live state" fields a peer gets
            // via PLAYER_UPDATE / UPDATE_CLIENT_STATE. So fields like
            // isSaveLoaded / sceneNum / worldPos stay at their struct
            // defaults (false / 0 / origin) on the local entry forever.
            // Read directly from the live game state instead.
            snap.isSaveLoaded   = Anchor::Instance->IsSaveLoaded();
            if (gPlayState != nullptr) {
                snap.sceneNum     = (int16_t)gPlayState->sceneNum;
                snap.roomNum      = (int8_t)gPlayState->roomCtx.curRoom.num;
                snap.isInCutscene = (gPlayState->csCtx.state != CS_STATE_IDLE);
                Player* localPlayer = GET_PLAYER(gPlayState);
                if (localPlayer != nullptr) {
                    snap.worldPos       = localPlayer->actor.world.pos;
                    snap.isInvulnerable = (localPlayer->invincibilityTimer != 0);
                }
            }
        } else {
            snap.sceneNum       = client.sceneNum;
            snap.roomNum        = client.curRoomNum;
            snap.worldPos       = client.posRot.pos;
            snap.isSaveLoaded   = client.isSaveLoaded;
            snap.isInCutscene   = (client.csCtxState != CS_STATE_IDLE);
            snap.isInvulnerable = (client.invincibilityTimer != 0);
        }

        // step 12+ fields left at struct-default values until the
        // boss-room / scene-residency plumbing lands. Documented in
        // SessionView::AllPlayersInBossRoom + the descriptor that
        // first needs framesInCurrentScene.

        view.players.push_back(std::move(snap));
    }

    return view;
}

// ---------------------------------------------------------------------------
// SessionView helpers.
// ---------------------------------------------------------------------------

bool SessionView::AnyPlayerInScene(int16_t sceneNum) const {
    for (const auto& p : players) {
        if (p.sceneNum == sceneNum) return true;
    }
    return false;
}

bool SessionView::AnyPlayerInCutscene() const {
    for (const auto& p : players) {
        if (p.isInCutscene) return true;
    }
    return false;
}

bool SessionView::AllPlayersInBossRoom(int16_t sceneNum) const {
    // step 12+: needs the boss-room registry + per-room boss-alive
    // lookup. Stubbed to false (= "not committed") so descriptors that
    // gate spawns on this predicate behave conservatively (i.e. they
    // never assume team is committed when we can't yet prove it).
    (void)sceneNum;
    return false;
}

const PlayerSnapshot* SessionView::MostIsolatedPlayer() const {
    if (players.size() < 2) {
        return players.empty() ? nullptr : &players.front();
    }

    const PlayerSnapshot* best = nullptr;
    float bestMinDistSq = -1.0f;

    for (size_t i = 0; i < players.size(); ++i) {
        float minDistSq = std::numeric_limits<float>::infinity();
        for (size_t j = 0; j < players.size(); ++j) {
            if (i == j) continue;
            // Cross-scene players are infinitely far apart for isolation
            // purposes — a player alone in their own scene counts as
            // "most isolated" by default.
            if (players[i].sceneNum != players[j].sceneNum) continue;

            const float dx = players[i].worldPos.x - players[j].worldPos.x;
            const float dy = players[i].worldPos.y - players[j].worldPos.y;
            const float dz = players[i].worldPos.z - players[j].worldPos.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < minDistSq) minDistSq = distSq;
        }
        if (minDistSq > bestMinDistSq) {
            bestMinDistSq = minDistSq;
            best = &players[i];
        }
    }
    return best;
}

const PlayerSnapshot* SessionView::PlayerByClientId(uint32_t clientId) const {
    for (const auto& p : players) {
        if (p.clientId == clientId) return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Host-migration snapshot (step 5).
// ---------------------------------------------------------------------------
//
// Wire format (compact arrays-of-pairs keep the JSON small):
//
//   {
//     "globalFrame":  <uint64>,
//     "lastSpawnFrames": [[<uint32 key>, <int frame>], ...],
//     "liveCounts":     [[<uint8 descId>, <int count>], ...],
//     "netIdToDesc":    [[<uint32 netId>, <uint8 descId>], ...],
//     "descState":      {"<descId>": <opaque blob>, ...}
//   }
//
// The packet envelope (type, schema, etc.) is added by
// SendPacket_DirectorStateSync; this method returns just the data payload.

nlohmann::json Director::SerializeMigrationSnapshot() const {
    nlohmann::json snapshot;
    snapshot["globalFrame"] = mGlobalFrameCounter;

    nlohmann::json lastSpawnFrames = nlohmann::json::array();
    for (const auto& [key, frame] : mLastSpawnFrameByKey) {
        lastSpawnFrames.push_back(nlohmann::json::array({key, frame}));
    }
    snapshot["lastSpawnFrames"] = lastSpawnFrames;

    nlohmann::json liveCounts = nlohmann::json::array();
    for (const auto& [descId, count] : mLiveCountByDescriptor) {
        liveCounts.push_back(nlohmann::json::array({descId, count}));
    }
    snapshot["liveCounts"] = liveCounts;

    nlohmann::json netIdToDesc = nlohmann::json::array();
    for (const auto& [netId, descId] : mNetIdToDescriptor) {
        netIdToDesc.push_back(nlohmann::json::array({netId, descId}));
    }
    snapshot["netIdToDesc"] = netIdToDesc;

    // Per-descriptor opaque state. Key as string for stable JSON-object
    // representation; descriptor ids are small uint8 → string conversion
    // is unambiguous.
    nlohmann::json descState = nlohmann::json::object();
    for (const auto& d : mDescriptors) {
        nlohmann::json blob = d->SerializeMigrationState();
        if (!blob.is_null() && !blob.empty()) {
            descState[std::to_string(d->GetDescriptorId())] = std::move(blob);
        }
    }
    snapshot["descState"] = descState;

    return snapshot;
}

void Director::ApplyMigrationSnapshot(const nlohmann::json& snapshot) {
    if (!snapshot.is_object()) {
        return;
    }

    // Replace, don't merge. The host is authoritative; whatever local
    // ledger entries existed are stale by definition.
    mLastSpawnFrameByKey.clear();
    mLiveCountByDescriptor.clear();
    mNetIdToDescriptor.clear();

    if (snapshot.contains("globalFrame")) {
        mGlobalFrameCounter = snapshot["globalFrame"].get<uint64_t>();
    }

    if (snapshot.contains("lastSpawnFrames") && snapshot["lastSpawnFrames"].is_array()) {
        for (const auto& pair : snapshot["lastSpawnFrames"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mLastSpawnFrameByKey[pair[0].get<uint32_t>()] = pair[1].get<int>();
        }
    }

    if (snapshot.contains("liveCounts") && snapshot["liveCounts"].is_array()) {
        for (const auto& pair : snapshot["liveCounts"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mLiveCountByDescriptor[pair[0].get<uint8_t>()] = pair[1].get<int>();
        }
    }

    if (snapshot.contains("netIdToDesc") && snapshot["netIdToDesc"].is_array()) {
        for (const auto& pair : snapshot["netIdToDesc"]) {
            if (!pair.is_array() || pair.size() != 2) continue;
            mNetIdToDescriptor[pair[0].get<uint32_t>()] = pair[1].get<uint8_t>();
        }
    }

    if (snapshot.contains("descState") && snapshot["descState"].is_object()) {
        const auto& descState = snapshot["descState"];
        for (auto& d : mDescriptors) {
            const std::string key = std::to_string(d->GetDescriptorId());
            if (descState.contains(key)) {
                d->RestoreMigrationState(descState[key]);
            }
        }
    }
}

}  // namespace AnchorDirector
