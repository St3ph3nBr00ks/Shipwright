#pragma once

// Cutscene Catch-up ledger (Option B v1) — data types for the leader-
// side accumulation of observable side effects during a cutscene, plus
// the wire snapshot the late-joiner receives via CUTSCENE_CATCHUP_-
// RESPONSE.
//
// Plan: Claude/Plans/cutscene_late_join_plan.md
// Findings that shape this design:
//   - Analysis/cutscene_family_catchup_survey_2026-07-07.md (R11/R12
//     — vanilla dispatcher uses frame-equality for one-shot commands;
//     ledger must capture ALL observable state or naïve frame-jump
//     silently drops item awards, flag sets, particles).
//   - Analysis/soh_audio_seek_investigation_2026-07-07.md (Q4 — audio
//     seek exists via AudioLoad_SyncInitSeqPlayerSkipTicks; ledger
//     records seqId + startedAt so the late-joiner can seek to the
//     correct offset).

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"        // Vec3f, Vec3s
#include "variables.h"  // ActorId etc.
}

namespace CutsceneCatchup {

// ---- Delta records: observable side effects during cutscene ---------

// Actor spawned mid-cutscene by a script command or hook. Late-joiner
// re-spawns via existing Actor_Spawn path (bracketed with the same
// isSpawningNetworkActor gate the ENEMY_SPAWN path uses to avoid
// double-broadcast).
struct SpawnedActorRec {
    uint32_t netId;       // 0 if no EnemyNetId extension (rare)
    int16_t  actorId;
    int16_t  params;
    int8_t   room;
    Vec3f    pos;
    Vec3s    rot;
};

// Flag set mid-cutscene. Bracketed with g_isApplyingNetworkFlag on
// late-joiner apply (same pathway the SET_FLAG packet uses).
struct FlagSetRec {
    int16_t  flagType;         // FLAG_SCENE_SWITCH / _CHEST / _CLEAR / ...
    int16_t  flag;
    int16_t  sceneOrRoomNum;
};

// Item granted mid-cutscene (rare; most cutscene item awards happen
// via CUTSCENE_END path, but some scripts do fire GIVE_ITEM mid-cs).
// Bracketed with g_isApplyingNetworkItemGrant on apply.
struct ItemGrantRec {
    int16_t  itemId;
    int32_t  amount;
};

// ---- Snapshot records: reproducible state at leader-frame ------------

// Music command state — replay via Anchor_SeekBGMToMs helper.
struct MusicSnapshot {
    int32_t seqId = -1;      // -1 = no music command fired since start
    std::chrono::steady_clock::time_point startedAt;
    // Offset from cutscene start (0 if music started with the cutscene).
    int32_t offsetFromStartMs = 0;
};

// Camera state at leader-frame — apply directly to gPlayState->view
// or active camera on catchup. Optional (vanilla next-frame cutscene
// tick may re-set anyway; snapshot avoids one frame of jitter).
struct CameraSnapshot {
    Vec3f eye;
    Vec3f at;
    float fov;
    int32_t validAt = 0;     // csCtx.frames when snapshot was taken
};

// Actor-cutscene-state snapshot for cutscene-controlled actors that
// aren't in IsSyncedWorldActor (vanilla Demo_* overlays etc.). Empty
// vector = late-joiner relies on next-frame vanilla tick to correct
// actor state (acceptable for actors that re-init from world state
// each frame; risky for state-machine actors — expand in v1.1 as
// per-actor need is identified).
struct ActorStateSnapshot {
    uint32_t netId;         // For synced actors; 0 for legacy actors
                            // (identify by (actorId, params, room, home.pos))
    int16_t  actorId;
    int16_t  params;
    int8_t   room;
    Vec3f    home;          // used as identity key for legacy actors
    Vec3f    pos;
    Vec3s    rot;
    Vec3s    shapeRot;
    int32_t  animFrame;     // SkelAnime->curFrame if available
    int32_t  actionFuncKey; // per-actor state-machine key (0 = default)
};

// ---- Ledger entry: one per active cutscene on leader ----------------

struct CutsceneCatchupEntry {
    // Identity
    std::string csKind;                        // e.g. "deku_tree_intro"
    uint32_t    csKey     = 0;                 // per-family key
    int16_t     sceneNum  = -1;
    int8_t      roomNum   = -1;
    uint8_t     linkAge   = 0;

    // Frame counter accounting
    int32_t     startFrame        = 0;         // csCtx.frames at CUTSCENE_START
    int32_t     currentLeaderFrame = 0;        // updated each tick

    // Delta accumulators
    std::vector<SpawnedActorRec>    spawnedActors;
    std::vector<FlagSetRec>         flagsSet;
    std::vector<ItemGrantRec>       itemsGranted;

    // Snapshots
    MusicSnapshot                   music;
    CameraSnapshot                  camera;
    std::vector<ActorStateSnapshot> actors;
};

// ---- Public API — Phase 2B (implementation in CutsceneCatchup.cpp) --

// Leader-side capture. Each function is a no-op unless the local
// client is (a) the room host and (b) inside a cutscene. Callers
// don't need to gate; these gate internally for KISS at call sites.
void RecordSpawnedActor(struct Actor* actor);
void RecordFlagSet(int16_t flagType, int16_t flag, int16_t sceneOrRoomNum);
void RecordItemGranted(int16_t itemId, int32_t amount);
void RecordMusicStart(int32_t seqId);

// Called at ~1Hz from TickCutsceneCatchup (Phase 3):
void SnapshotCamera();
void SnapshotActors();
void UpdateFrameCounter();

// Lifecycle. Ledger entries are reconciled against
// Anchor::cutsceneStartActive each tick (ReconcileLifecycle), but
// explicit clear is provided for CUTSCENE_END packet-receive paths.
void ClearEntry(const std::string& kindKey);
void ClearAll();
void ReconcileLifecycle();

}  // namespace CutsceneCatchup

// z_demo.c calls this from Cutscene_Command_PlayBGM when the script
// starts a music sequence. Phase 4 wires the call site.
extern "C" void Anchor_NotifyCutsceneMusicStart(int seqId);
