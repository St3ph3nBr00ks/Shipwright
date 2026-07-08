// Cutscene Catchup — ledger population + hook wiring.
//
// Plan: Claude/Plans/cutscene_late_join_plan.md
// Types: Common/CutsceneCatchup.h (data records + entry struct)
// Substrate: Anchor.h (ledger map, pendingCatchup state, seq counters)
//
// Data flow:
//     LEADER (room host)                    LATE-JOINER
//     ------------------                    -----------
//     OnActorSpawn during Play_InCsMode →   (in another scene / no state)
//       RecordSpawnedActor
//     OnFlagSet during Play_InCsMode →
//       RecordFlagSet
//     OnItemReceive during Play_InCsMode →
//       RecordItemGranted
//     Anchor_NotifyCutsceneMusicStart →
//       RecordMusicStart
//     TickCutsceneCatchup 1Hz →
//       UpdateFrameCounter
//       SnapshotCamera
//       SnapshotActors                      (scene entry)
//                                            OnSceneSpawnActors detects
//                                            mid-cutscene peer → sends
//                                            CUTSCENE_CATCHUP_REQUEST
//                                            (Phase 3 wiring)
//     HandlePacket_CutsceneCatchup-       ← CUTSCENE_CATCHUP_RESPONSE
//     Request → build ledger snapshot,      (Phase 3 wiring — receiver
//     send CUTSCENE_CATCHUP_RESPONSE         applies delta + seeks audio
//                                            + jumps csCtx.frames)
//
// This file (Phase 2B) implements the LEADER side: ledger lifecycle,
// Record*/Snapshot functions, hook registration. Phase 3 wires the
// wire packets + late-joiner apply path.
//
// SoC: leader-side data capture is a pure ledger operation — no wire I/O,
// no peer communication. Wire concerns live in the Phase 3 packet files.
//
// Rigor: 7 principles reviewed inline where non-trivial decisions land.

#include "soh/Network/Anchor/Common/CutsceneCatchup.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"    // GET_ACTIVE_CAM, GET_PLAYER
extern PlayState* gPlayState;
}

namespace CutsceneCatchup {

// ---- Internal helpers ------------------------------------------------

namespace {

// Are we currently the room host for the local player's scene? Only
// the room host builds ledgers — peers ignore all Record* calls.
// Mirror of the gate used by other host-authoritative Anchor systems.
bool IsLeader() {
    if (Anchor::Instance == nullptr) return false;
    if (!Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr) return false;
    return ::SceneAuthority::IsRoomHost(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
}

// Vanilla `Play_InCsMode` gate. Cheap; safe to call every hook fire.
// Reads csCtx.state + player state; both are game-thread only.
bool IsInCutscene() {
    if (gPlayState == nullptr) return false;
    return Play_InCsMode(gPlayState);
}

// The current active cutscene key (matches Anchor's cutsceneStartActive
// dedup key format: "<csKind>:<csKey>"). Returns empty string when no
// cutscene is active or when multiple are active (v1 doesn't support
// concurrent cutscenes — return empty to skip capture rather than
// arbitrarily pick one, which would fragment the ledger).
std::string ActiveKindKey() {
    if (Anchor::Instance == nullptr) return {};
    const auto& active = Anchor::Instance->cutsceneStartActive;
    if (active.size() != 1) {
        // 0: no cutscene active. Common — silent.
        // 2+: concurrent cutscenes. Rare + unsupported in v1; log at
        //     DEBUG so field-test surfaces if it happens.
        if (!active.empty()) {
            SPDLOG_DEBUG("[CutsceneCatchup] {} concurrent cutscenes — capture "
                         "skipped (v1 assumes single-cutscene sessions)",
                         active.size());
        }
        return {};
    }
    return *active.begin();
}

// Find (or lazily create) the ledger entry for the currently-active
// cutscene. Returns nullptr if there's no unambiguous active cutscene.
//
// KISS: entry lifecycle is driven implicitly by cutsceneStartActive —
// creating on-demand from the first Record* call rather than adding a
// separate "cutscene started, initialise ledger" broadcast hook keeps
// the coupling loose (either the leader is in a cutscene AND recording
// side effects, or nobody cares).
CutsceneCatchupEntry* GetOrCreateActiveEntry(const char* triggerContext = "unknown") {
    const std::string key = ActiveKindKey();
    if (key.empty()) return nullptr;

    auto& map = Anchor::Instance->cutsceneCatchupLedger;
    auto it = map.find(key);
    if (it != map.end()) return it->second.get();

    // Fix G — refuse ledger creation unless this cutscene was
    // originated locally by THIS client. `cutsceneStartActive` is
    // populated symmetrically by SendPacket_CutsceneStart (we
    // originated) AND HandlePacket_CutsceneStart (peer broadcast
    // arrived) AND HandlePacket_CutsceneFrameSync late-join hydration
    // (peer told us they're mid-cutscene). Only the first case
    // qualifies as leader — the sibling set
    // `cutsceneStartActiveLocalOrigin` records that origin.
    //
    // Without this gate, log 629 P2 became phantom leader for
    // 'deku_tree_intro:0' via IsRoomHost=1 + stale Play_InCsMode=1
    // (Player.stateFlags1 leftover from a prior savecontext
    // cutscene) + cutsceneStartActive populated from P1's broadcast.
    // See Analysis/cutscene_late_join_bugs_deep_analysis_2026-07-08.md
    // Bug 2 root-cause chain.
    if (Anchor::Instance->cutsceneStartActiveLocalOrigin.count(key) == 0) {
        SPDLOG_INFO("[CutsceneCatchup] Ledger create refused — cutscene "
                    "'{}' was not locally originated (peer-received START "
                    "populates cutsceneStartActive but never the local-"
                    "origin sibling set). trigger='{}'",
                    key, triggerContext);
        return nullptr;
    }

    // Fix F — refuse ledger creation while we have an outstanding
    // catchup REQUEST for the same key. Retained as belt-and-
    // suspenders — Fix G subsumes this in the common case, but Fix F
    // guards against a hypothetical race where we simultaneously
    // originate a local cutscene and REQUEST catchup for the same
    // key. Log 628 line 814 was the original trigger for this guard.
    if (Anchor::Instance->pendingCatchups.count(key) > 0) {
        SPDLOG_INFO("[CutsceneCatchup] Ledger create refused — pending "
                    "catchup REQUEST outstanding for key='{}' trigger='{}' "
                    "(we are a peer awaiting a leader's response, not "
                    "the leader ourselves)",
                    key, triggerContext);
        return nullptr;
    }

    // Parse "<csKind>:<csKey>" back out.
    std::string csKind;
    uint32_t    csKey = 0;
    {
        auto sep = key.find(':');
        if (sep == std::string::npos) {
            SPDLOG_WARN("[CutsceneCatchup] Malformed active key '{}' — skip",
                        key);
            return nullptr;
        }
        csKind = key.substr(0, sep);
        try {
            csKey = static_cast<uint32_t>(std::stoul(key.substr(sep + 1)));
        } catch (...) {
            csKey = 0;
        }
    }

    auto entry = std::make_unique<CutsceneCatchupEntry>();
    entry->csKind             = std::move(csKind);
    entry->csKey              = csKey;
    entry->sceneNum           = (int16_t)gPlayState->sceneNum;
    entry->roomNum            = (int8_t)gPlayState->roomCtx.curRoom.num;
    entry->linkAge            = (uint8_t)(gSaveContext.linkAge & 0x1);
    entry->startFrame         = (int32_t)gPlayState->csCtx.frames;
    entry->currentLeaderFrame = entry->startFrame;

    auto* raw = entry.get();
    map.emplace(key, std::move(entry));
    SPDLOG_INFO("[CutsceneCatchup] Ledger entry created key='{}' scene={} "
                "room={} linkAge={} startFrame={}",
                key, raw->sceneNum, raw->roomNum, raw->linkAge,
                raw->startFrame);

    // Diag 2 — leader/ledger sanity. Log the exact preconditions that
    // let this ledger entry be created, plus the caller context (which
    // Record* trigger fired first). If P2 became its own leader while
    // also holding a pending catchup request, this line makes the
    // conflict visible. Gated on gEnhancements.Anchor.CutsceneLateJoinDiag.
    // See Analysis/cutscene_ff_reset_and_mp_dialogue_deadline_2026-07-08.md
    // Bug 2 hypothesis.
    if (CVarGetInteger(CVAR_ENHANCEMENT("Anchor.CutsceneLateJoinDiag"), 0) != 0) {
        const bool isRoomHost = ::SceneAuthority::IsRoomHost(
            (int16_t)gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num,
            (uint8_t)(gSaveContext.linkAge & 0x1));
        const bool inCsMode = Play_InCsMode(gPlayState);
        const int  csState  = (int)gPlayState->csCtx.state;
        const int  csFrames = (int)gPlayState->csCtx.frames;
        const size_t pendingCatchups =
            Anchor::Instance ? Anchor::Instance->pendingCatchups.size() : 0;
        const size_t peerCount =
            Anchor::Instance ? Anchor::Instance->clients.size() : 0;
        SPDLOG_INFO("[CutsceneCatchup.diag] Ledger create trigger='{}' key='{}' "
                    "IsRoomHost={} Play_InCsMode={} csState={} csFrames={} "
                    "pendingCatchups={} peerCount={}",
                    triggerContext, key,
                    (int)isRoomHost, (int)inCsMode, csState, csFrames,
                    pendingCatchups, peerCount);
    }
    return raw;
}

}  // namespace

// ---- Public Record* API ---------------------------------------------

void RecordSpawnedActor(Actor* actor) {
    if (actor == nullptr) return;
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("RecordSpawnedActor");
    if (entry == nullptr) return;

    SpawnedActorRec rec{};
    rec.netId   = 0;                              // Phase 3 fills from EnemyNetId if present
    rec.actorId = (int16_t)actor->id;
    rec.params  = (int16_t)actor->params;
    rec.room    = (int8_t)actor->room;
    rec.pos     = actor->world.pos;
    rec.rot     = actor->world.rot;
    entry->spawnedActors.push_back(rec);
}

void RecordFlagSet(int16_t flagType, int16_t flag, int16_t sceneOrRoomNum) {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("RecordFlagSet");
    if (entry == nullptr) return;

    FlagSetRec rec{ flagType, flag, sceneOrRoomNum };
    entry->flagsSet.push_back(rec);
}

void RecordItemGranted(int16_t itemId, int32_t amount) {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("RecordItemGranted");
    if (entry == nullptr) return;

    ItemGrantRec rec{ itemId, amount };
    entry->itemsGranted.push_back(rec);
}

void RecordMusicStart(int32_t seqId) {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("RecordMusicStart");
    if (entry == nullptr) return;

    entry->music.seqId = seqId;
    entry->music.startedAt = std::chrono::steady_clock::now();
    entry->music.offsetFromStartMs = 0;
    SPDLOG_INFO("[CutsceneCatchup] Music captured seqId=0x{:X}", seqId);
}

// ---- Snapshot helpers (called from TickCutsceneCatchup at ~1Hz) -----

// Camera snapshot — cheap; grab active camera eye/at/fov each tick.
// Called at 1Hz so a late-joiner joining right after a snapshot gets
// at most 1s of camera staleness. Vanilla next-frame cutscene tick
// then interpolates the camera correctly from that starting position.
void SnapshotCamera() {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("SnapshotCamera");
    if (entry == nullptr) return;
    if (gPlayState == nullptr) return;

    Camera* cam = GET_ACTIVE_CAM(gPlayState);
    if (cam == nullptr) return;

    entry->camera.eye     = cam->eye;
    entry->camera.at      = cam->at;
    entry->camera.fov     = cam->fov;
    entry->camera.validAt = (int32_t)gPlayState->csCtx.frames;

    // Fix N + N.2 — capture the message textId + Link's world state at
    // the same 1 Hz cadence. Peer applies these post-fast-forward to
    // stop at exactly leader's textbox + place Link at leader's pos.
    // Cheap direct reads; no allocation. Ships in
    // CUTSCENE_CATCHUP_RESPONSE. See Analysis/cutscene_overshoot_and_
    // teleport_2026-07-08.md.
    entry->leaderMsgTextId = (uint16_t)gPlayState->msgCtx.textId;
    Player* leaderLink = GET_PLAYER(gPlayState);
    if (leaderLink != nullptr) {
        entry->leaderPlayerPos.x = leaderLink->actor.world.pos.x;
        entry->leaderPlayerPos.y = leaderLink->actor.world.pos.y;
        entry->leaderPlayerPos.z = leaderLink->actor.world.pos.z;
        entry->leaderPlayerYaw   = leaderLink->actor.shape.rot.y;
        entry->hasLeaderPlayerSnapshot = true;
    }
}

// Actor snapshot — walks ACTORCAT_NPC / _PROP / _BOSS categories and
// captures pose + anim state. Rationale (R12 in plan §7):
//
//   Vanilla NPCs during cutscenes have disableGameplayLogic = true and
//   advance only under cutscene command control. A late-joiner needs
//   them at the leader's frame-N state, not at their frame-0 initial
//   state. Since most cutscene-controlled actors are Demo_* overlays
//   NOT in IsSyncedWorldActor, we snapshot them here.
//
// v1 scope: only capture actors whose category matches the shortlist.
// Rate-limited to 1Hz via caller.
//
// Cannot verify: whether SkelAnime->curFrame is safe to read from an
// arbitrary NPC without knowing the actor's exact layout. Snapshot
// omits animFrame for now (0) and lets vanilla dispatcher regenerate
// on catchup apply. Actor position + rotation IS reproducible via
// world.pos + world.rot direct-write.
void SnapshotActors() {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("SnapshotActors");
    if (entry == nullptr) return;
    if (gPlayState == nullptr) return;

    entry->actors.clear();

    static constexpr uint8_t kCategories[] = {
        ACTORCAT_NPC, ACTORCAT_PROP, ACTORCAT_BOSS
    };
    for (uint8_t cat : kCategories) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            ActorStateSnapshot snap{};
            snap.netId         = 0;                       // Phase 3 populates if EnemyNetId present
            snap.actorId       = (int16_t)actor->id;
            snap.params        = (int16_t)actor->params;
            snap.room          = (int8_t)actor->room;
            snap.home          = actor->home.pos;
            snap.pos           = actor->world.pos;
            snap.rot           = actor->world.rot;
            snap.shapeRot      = actor->shape.rot;
            snap.animFrame     = 0;                       // v1: skip (see comment)
            snap.actionFuncKey = 0;                       // v1: skip
            entry->actors.push_back(snap);
            actor = actor->next;
        }
    }
}

void UpdateFrameCounter() {
    if (!IsLeader() || !IsInCutscene()) return;
    auto* entry = GetOrCreateActiveEntry("UpdateFrameCounter");
    if (entry == nullptr) return;
    if (gPlayState == nullptr) return;
    entry->currentLeaderFrame = (int32_t)gPlayState->csCtx.frames;
}

// ---- Lifecycle ------------------------------------------------------

void ClearEntry(const std::string& kindKey) {
    if (Anchor::Instance == nullptr) return;
    auto& map = Anchor::Instance->cutsceneCatchupLedger;
    auto it = map.find(kindKey);
    if (it == map.end()) return;
    SPDLOG_INFO("[CutsceneCatchup] Ledger entry cleared key='{}' "
                "(frames={}, spawns={}, flags={}, items={}, actors={})",
                kindKey, it->second->currentLeaderFrame,
                it->second->spawnedActors.size(),
                it->second->flagsSet.size(),
                it->second->itemsGranted.size(),
                it->second->actors.size());
    map.erase(it);
}

void ClearAll() {
    if (Anchor::Instance == nullptr) return;
    Anchor::Instance->cutsceneCatchupLedger.clear();
}

// ---- Reconciliation ------------------------------------------------
// Sync the ledger's set of active entries to the set in
// cutsceneStartActive. Called from TickCutsceneCatchup. Handles the
// case where a CUTSCENE_END fired but the leader never populated a
// ledger entry (unusual — happens if the cutscene had zero side-
// effects) and the case where a cutscene has been active for a while
// but Record* never fired (silent + no ledger entry — nothing to do).
void ReconcileLifecycle() {
    if (Anchor::Instance == nullptr) return;
    auto& ledger = Anchor::Instance->cutsceneCatchupLedger;
    const auto& active = Anchor::Instance->cutsceneStartActive;

    // Remove ledger entries whose cutscene is no longer active.
    for (auto it = ledger.begin(); it != ledger.end();) {
        if (active.count(it->first) == 0) {
            SPDLOG_INFO("[CutsceneCatchup] Ledger entry auto-cleared key='{}' "
                        "(cutsceneStartActive dropped it)",
                        it->first);
            it = ledger.erase(it);
        } else {
            ++it;
        }
    }
    // Note: we don't proactively CREATE entries here — Record*/Snapshot
    // functions create on-demand. This preserves the "no ledger without
    // side effects" invariant.
}

}  // namespace CutsceneCatchup

// ---- Hook registration + extern "C" music entry point --------------

namespace {

void OnActorSpawnCutsceneCatchup(void* refActor) {
    CutsceneCatchup::RecordSpawnedActor(static_cast<Actor*>(refActor));
}

// GameInteractor's OnFlagSet fires with (flagType, flag). The
// sceneOrRoomNum comes from the caller context; ledger records the
// current sceneNum as a defensive default (peers will re-derive on
// apply if needed).
void OnFlagSetCutsceneCatchup(int16_t flagType, int16_t flag) {
    const int16_t sceneNum = (gPlayState != nullptr)
        ? (int16_t)gPlayState->sceneNum : (int16_t)-1;
    CutsceneCatchup::RecordFlagSet(flagType, flag, sceneNum);
}

void OnItemReceiveCutsceneCatchup(GetItemEntry itemEntry) {
    CutsceneCatchup::RecordItemGranted((int16_t)itemEntry.itemId, 1);
}

// ---- Post-hoc safety net -------------------------------------------
//
// Catches any cutscene-entry site we haven't gated (SoH enhancements,
// future upstream merges, or gate sites we missed in the audit). If
// pendingCatchups is non-empty AND csCtx.state has left IDLE, we force
// it back to IDLE. Logs at WARN so field-test surfaces which entry site
// leaked past the gate.
//
// See Analysis/cutscene_entry_gate_design_2026-07-07.md §4.3.
void OnGameFrameUpdate_CutsceneCatchupSafetyNet() {
    if (::Anchor::Instance == nullptr) return;
    if (::Anchor::Instance->pendingCatchups.empty()) return;
    if (gPlayState == nullptr) return;
    if (gPlayState->csCtx.state == CS_STATE_IDLE) return;
    // Fix K — respect active fast-forward. When catchupFastForwardTarget
    // > 0 we are DELIBERATELY driving vanilla forward toward the
    // leader's frame via TickCutsceneCatchup's fast-forward loop; the
    // safety net's "vanilla leaked past the entry gate" premise is
    // inapplicable. Log 631 showed this race resetting csCtx.state to
    // IDLE and destroying fast-forward's progress in a tight loop with
    // the FRAME_SYNC direct-request path. See
    // Analysis/cutscene_fast_forward_stall_and_race_2026-07-08.md.
    if (::Anchor::Instance->catchupFastForwardTarget > 0) return;

    // Vanilla entered a cutscene despite our pendingCatchup — a gate
    // was missed. Log the leaked state + force IDLE. The pending
    // catchup response (when it arrives) will still apply the delta;
    // in the meantime the local cutscene is suppressed. Race-lost
    // cases fall through to Skip-and-Apply naturally.
    SPDLOG_WARN("[CutsceneCatchup] Safety net fired — csCtx.state={} "
                "frames={} despite pending catchup for '{}'; forcing IDLE",
                (int)gPlayState->csCtx.state,
                (int)gPlayState->csCtx.frames,
                ::Anchor::Instance->pendingCatchups.begin()->first);
    gPlayState->csCtx.state = CS_STATE_IDLE;
    gPlayState->csCtx.frames = 0;
}

void RegisterCutsceneCatchup() {
    // OnActorSpawn matches the enemy-sync pattern (HookHandlers.cpp:963).
    // Fires before actor->init(), so pos/rot reflect the spawn call's
    // arguments — which is what the late-joiner needs to reproduce the
    // spawn locally via Actor_Spawn.
    GameInteractor::Instance
        ->RegisterGameHook<GameInteractor::OnActorSpawn>(
            OnActorSpawnCutsceneCatchup);
    GameInteractor::Instance
        ->RegisterGameHook<GameInteractor::OnFlagSet>(
            OnFlagSetCutsceneCatchup);
    GameInteractor::Instance
        ->RegisterGameHook<GameInteractor::OnItemReceive>(
            OnItemReceiveCutsceneCatchup);
    GameInteractor::Instance
        ->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
            OnGameFrameUpdate_CutsceneCatchupSafetyNet);
}

static RegisterShipInitFunc initFunc(RegisterCutsceneCatchup, {});

}  // namespace

// ---- extern "C" entry point for z_demo.c music capture -------------
// z_demo.c's Cutscene_Command_PlayBGM (~line 446) fires when the
// cutscene script starts a music sequence. Phase 4 adds a one-line
// call site there; this shim routes it into the leader's ledger.
extern "C" void Anchor_NotifyCutsceneMusicStart(int seqId) {
    CutsceneCatchup::RecordMusicStart((int32_t)seqId);
}
