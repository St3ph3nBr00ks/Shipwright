#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncScope.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/SkelAnimeWire.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/Network/Anchor/EnemyStateSync/EnemyHostBookkeeping.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
// Issue #153 — En_Goroiwa carries extra path-state fields in the steady-state branch.
#include "overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
// KB-08 / #7 — En_Dekubaba carries actionState for state-machine sync.
#include "overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
// boss_goma_sync_plan.md §7 / KB-26 — En_Goma (Larva) carries
// actionState for egg-hatch state-machine sync.
#include "overlays/actors/ovl_En_Goma/z_en_goma.h"
// #135 / en_dekunuts_sync_plan.md — Mad Scrub state-machine sync.
#include "overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
// En_Hintnuts (Inside Deku Tree Compound Room) — separate actor from
// En_Dekunuts. Same conceptual "Mad Scrub" but distinct id (0x0192).
#include "overlays/actors/ovl_En_Hintnuts/z_en_hintnuts.h"
// #90 / en_st_sync_plan_v2.md — Skulltula state-machine sync.
#include "overlays/actors/ovl_En_St/z_en_st.h"
// #148 / en_sw_sync_plan.md — Skullwalltula state-machine sync.
#include "overlays/actors/ovl_En_Sw/z_en_sw.h"
// Boss-fight trigger sync — minimal Encounter -> FloorMain bridge.
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
// Push-block bidirectional sync — host needs to apply received pos when peer
// is the local pusher, and peers need to suppress apply when they're the
// local pusher (so received echoes don't overwrite their own progress).
#include "overlays/actors/ovl_Obj_Oshihiki/z_obj_oshihiki.h"
// Per-torch lit-state sync — host-authoritative `litTimer` so partial
// multi-torch puzzle progress is visible across clients.
#include "overlays/actors/ovl_Obj_Syokudai/z_obj_syokudai.h"
extern PlayState* gPlayState;
}

// Pillar C2 Phase 4 Commit C — consolidated ENEMY_STATE handler.
//
// All four legacy packet types (ENEMY_UPDATE, ENEMY_DEFEATED, ENEMY_SPAWN,
// ENEMY_RESPAWN) are now expressed as phases of the single ENEMY_STATE
// wire packet. The four legacy `Packets/Enemy{Update,Defeated,Spawn,
// Respawn}.cpp` files are retired; their bodies live below as per-phase
// helpers reached by HandlePacket_EnemyState's phase dispatcher. The
// emit-site signatures (SendPacket_EnemyUpdate, SendPacket_EnemyDefeated,
// SendPacket_EnemySpawn, SendPacket_EnemyRespawn) remain — call sites in
// HookHandlers.cpp continue to invoke them; only the wire-level type
// label has changed.
//
// See Claude/Plans/pillar_c2_enemystatesync.md, Phase 4.

// ---------------------------------------------------------------------------
// File-local helpers — last-sent cache (steady-state #60), threshold maths,
// extras gathering. Originally lived in Packets/EnemyUpdate.cpp.
// ---------------------------------------------------------------------------

namespace {

struct EnemyUpdateExtras {
    bool hasKarebaba         = false;
    s16  karebabaActionState = 0;
    s16  karebabaActorParams = 0;

    bool hasGoroiwa          = false;
    s16  goroiwaActionState  = 0;
    s16  goroiwaCurWp        = 0;
    s16  goroiwaNextWp       = 0;
    s16  goroiwaPathDir      = 0;
    u8   goroiwaFlags        = 0;

    // KB-08 / #7 — En_Dekubaba state-machine sync.
    bool hasDekubaba         = false;
    s16  dekubabaActionState = 0;
    // Bug 2 follow-on (small-Dekubaba mid-Lunge head float, log 162) —
    // world.pos.y is computed each frame from
    // home.pos.y - sin(stemSectionAngle[0..2]) * 20 * size, so without
    // syncing the three stem angles the non-host's head visual diverges
    // from the host whenever the Dekubaba is animating (Grow, Lunge,
    // PullBack, Recover, Hit). Six bytes per packet.
    s16  dekubabaStemAngles[3] = { 0, 0, 0 };

    // Plan §7 / KB-26 — En_Goma (Larva) state-machine sync.
    bool hasEnGoma         = false;
    s16  enGomaActionState = 0;

    // #135 / en_dekunuts_sync_plan.md §8 — Mad Scrub state-machine sync.
    // animFlagAndTimer is synced to anchor the projectile-spawn frame-6
    // check on the receiver (mitigates frame-skip race where non-host's
    // Animation_OnFrame(6.0f) misses if its anim timer landed off-frame).
    bool hasDekunuts             = false;
    s16  dekunutsActionState     = 0;
    s16  dekunutsAnimFlagAndTimer = 0;

    // En_Hintnuts state-machine sync (Inside Deku Tree Compound Room
    // puzzle scrubs). Same animFlagAndTimer trick as Dekunuts to anchor
    // ThrowNut's projectile-spawn frame-6 check.
    bool hasHintnuts             = false;
    s16  hintnutsActionState     = 0;
    s16  hintnutsAnimFlagAndTimer = 0;
    // sPuzzleCounter sync. Each client's local hits advance their own
    // counter independently; sync keeps both clients on the same value
    // so the puzzle's branch logic (HitByScrubProjectile1 BG transition,
    // SetupFreeze -3→-4 reset bump) resolves identically. Last-writer-
    // wins via simple overwrite — racing-hit divergence converges within
    // a few frames at the next broadcast.
    s16  hintnutsPuzzleCounter   = 0;

    // #90 / en_st_sync_plan_v2.md §3 — En_St state-machine sync.
    bool hasEnSt         = false;
    s16  enStActionState = 0;

    // #148 / en_sw_sync_plan.md §3 — En_Sw state-machine sync.
    bool hasEnSw         = false;
    s16  enSwActionState = 0;

    // Boss_Goma — minimal Encounter -> FloorMain bridge so any player
    // triggering the fight starts it on every client. Plus stunned/
    // damaged state buckets and eye/vulnerability gate fields so peer's
    // local BossGoma_UpdateHit (z_boss_goma.c:1823) gates damage in the
    // same frames as the host.
    bool hasBossGoma                  = false;
    s16  bossGomaActionState          = 0;
    s16  bossGomaEyeClosedTimer       = 0;
    s16  bossGomaInvincibilityFrames  = 0;
    s16  bossGomaVisualState          = 0;
    s16  bossGomaEyeState             = 0;
    // KB-44 — childrenGohmaState[3] gates the CeilingIdle exit. Slots:
    // 0 = not spawned, 1 = spawned/alive, -1 = dead. BossGoma_CeilingIdle
    // (z_boss_goma.c:1601-1622) only fires SetupFallJump (drop from
    // ceiling, fight resumes) when ALL three are -1; otherwise loops
    // back to MoveToCenter forever. Vanilla writes happen inside the
    // larva's own update — peer's larvae have parent=NULL (spawned via
    // Actor_Spawn, not SpawnAsChild) so peer can't write to its
    // BossGoma at all. Host-authoritative copy from host's BossGoma.
    s16  bossGomaChildrenState[3]     = {0, 0, 0};

    // Per-torch lit-state sync (Obj_Syokudai). Host-authoritative
    // `litTimer` for individual torches so partial multi-torch puzzle
    // progress is visible to peers (vanilla Flags_SetSwitch covers
    // "puzzle complete" but not "torch #2 of 4 lit").
    bool hasSyokudai     = false;
    s16  syokudaiLitTimer = 0;
};

// Snapshot of the last steady-state packet that actually went out (not
// skipped) for a given netId. Delta comparisons are always against this
// "last sent" snapshot, never against "last sampled" — otherwise sub-
// threshold drift accumulates without ever flushing.
struct EnemyUpdateLastSent {
    Vec3f    pos        = { 0, 0, 0 };
    Vec3s    rot        = { 0, 0, 0 };
    Vec3s    shapeRot   = { 0, 0, 0 };
    Vec3f    scale      = { 1.0f, 1.0f, 1.0f };
    s8       health     = 1;
    EnemyUpdateExtras extras;
    uint64_t jointHash  = 0;
    uint64_t lastSentMs = 0;
};

// File-scope cache. Cleared on scene-load (HookHandlers.cpp OnSceneSpawnActors)
// and on reconnect (Anchor.cpp OnConnected) via Anchor_ClearEnemyUpdateCache.
std::unordered_map<uint32_t, EnemyUpdateLastSent> sLastSentByNetId;

constexpr float kThresholdXZSq    = 4.0f;
constexpr float kThresholdY       = 2.0f;
constexpr s16   kRotThresholdS16  = 256;
constexpr float kScaleEpsilon     = 0.01f;

uint64_t NowMonotonicMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// FNV-1a over the skeleton's joint rotations plus (when present) morph
// rotations. A distinct separator mixed in when morphTable is present
// keeps an absent morphTable from colliding with a zero-filled one.
uint64_t HashLimbs(const SkelAnime* anime, uint8_t limbCount) {
    if (anime == nullptr || limbCount == 0) return 0;
    // Bound matches SkelAnimeWire::Serialize/Deserialize; the actor
    // struct allocates exactly limbCount Vec3s slots. See SkelAnimeWire.h.
    const uint8_t bounded = std::min(limbCount, SkelAnimeWire::kHardCap);
    uint64_t h = 14695981039346656037ULL;
    auto mix16 = [&h](uint16_t v) { h ^= (uint64_t)v; h *= 1099511628211ULL; };
    for (uint8_t i = 0; i < bounded; i++) {
        mix16((uint16_t)anime->jointTable[i].x);
        mix16((uint16_t)anime->jointTable[i].y);
        mix16((uint16_t)anime->jointTable[i].z);
    }
    if (anime->morphTable != nullptr) {
        mix16(0xFFFFu);
        for (uint8_t i = 0; i < bounded; i++) {
            mix16((uint16_t)anime->morphTable[i].x);
            mix16((uint16_t)anime->morphTable[i].y);
            mix16((uint16_t)anime->morphTable[i].z);
        }
    }
    return h;
}

EnemyUpdateExtras GatherExtras(Actor* actor) {
    EnemyUpdateExtras e;
    if (actor->id == ACTOR_EN_KAREBABA) {
        e.hasKarebaba         = true;
        e.karebabaActionState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
        e.karebabaActorParams = (s16)actor->params;
    } else if (actor->id == ACTOR_EN_GOROIWA) {
        EnGoroiwa* b         = (EnGoroiwa*)actor;
        e.hasGoroiwa         = true;
        e.goroiwaActionState = EnGoroiwa_GetStateIndex(b);
        e.goroiwaCurWp       = b->currentWaypoint;
        e.goroiwaNextWp      = b->nextWaypoint;
        e.goroiwaPathDir     = b->pathDirection;
        e.goroiwaFlags       = b->stateFlags;
    } else if (actor->id == ACTOR_EN_DEKUBABA) {
        EnDekubaba* baba       = (EnDekubaba*)actor;
        e.hasDekubaba          = true;
        e.dekubabaActionState  = EnDekubaba_GetStateIndex(baba);
        e.dekubabaStemAngles[0] = baba->stemSectionAngle[0];
        e.dekubabaStemAngles[1] = baba->stemSectionAngle[1];
        e.dekubabaStemAngles[2] = baba->stemSectionAngle[2];
    } else if (actor->id == ACTOR_EN_GOMA) {
        EnGoma* lg          = (EnGoma*)actor;
        e.hasEnGoma         = true;
        e.enGomaActionState = EnGoma_GetStateIndex(lg);
    } else if (actor->id == ACTOR_EN_DEKUNUTS) {
        EnDekunuts* d                 = (EnDekunuts*)actor;
        e.hasDekunuts                 = true;
        e.dekunutsActionState         = EnDekunuts_GetStateIndex(d);
        e.dekunutsAnimFlagAndTimer    = d->animFlagAndTimer;
    } else if (actor->id == ACTOR_EN_HINTNUTS) {
        EnHintnuts* h                 = (EnHintnuts*)actor;
        e.hasHintnuts                 = true;
        e.hintnutsActionState         = EnHintnuts_GetStateIndex(h);
        e.hintnutsAnimFlagAndTimer    = h->animFlagAndTimer;
        // sPuzzleCounter is global to all hintnuts in the scene; reading
        // from any one of them returns the same authoritative value.
        e.hintnutsPuzzleCounter       = EnHintnuts_GetPuzzleCounter();
    } else if (actor->id == ACTOR_EN_ST) {
        EnSt* st            = (EnSt*)actor;
        e.hasEnSt           = true;
        e.enStActionState   = EnSt_GetStateIndex(st);
    } else if (actor->id == ACTOR_EN_SW) {
        EnSw* sw            = (EnSw*)actor;
        e.hasEnSw           = true;
        e.enSwActionState   = EnSw_GetStateIndex(sw);
    } else if (actor->id == ACTOR_BOSS_GOMA) {
        BossGoma* bg                      = (BossGoma*)actor;
        e.hasBossGoma                     = true;
        e.bossGomaActionState             = BossGoma_GetStateIndex(bg);
        e.bossGomaEyeClosedTimer          = bg->eyeClosedTimer;
        e.bossGomaInvincibilityFrames     = bg->invincibilityFrames;
        e.bossGomaVisualState             = bg->visualState;
        e.bossGomaEyeState                = bg->eyeState;
        e.bossGomaChildrenState[0]        = bg->childrenGohmaState[0];
        e.bossGomaChildrenState[1]        = bg->childrenGohmaState[1];
        e.bossGomaChildrenState[2]        = bg->childrenGohmaState[2];
    } else if (actor->id == ACTOR_OBJ_SYOKUDAI) {
        ObjSyokudai* torch  = (ObjSyokudai*)actor;
        e.hasSyokudai       = true;
        e.syokudaiLitTimer  = torch->litTimer;
    }
    return e;
}

int RotDeltaAbs(s16 a, s16 b) {
    int d = (int)(int16_t)(a - b);
    return std::abs(d);
}

bool ExtrasDiffer(const EnemyUpdateExtras& cur, const EnemyUpdateExtras& prev) {
    if (cur.hasKarebaba != prev.hasKarebaba) return true;
    if (cur.hasKarebaba) {
        if (cur.karebabaActionState != prev.karebabaActionState) return true;
        if (cur.karebabaActorParams != prev.karebabaActorParams) return true;
    }
    if (cur.hasGoroiwa != prev.hasGoroiwa) return true;
    if (cur.hasGoroiwa) {
        if (cur.goroiwaActionState != prev.goroiwaActionState) return true;
        if (cur.goroiwaCurWp       != prev.goroiwaCurWp)       return true;
        if (cur.goroiwaNextWp      != prev.goroiwaNextWp)      return true;
        if (cur.goroiwaPathDir     != prev.goroiwaPathDir)     return true;
        if (cur.goroiwaFlags       != prev.goroiwaFlags)       return true;
    }
    if (cur.hasDekubaba != prev.hasDekubaba) return true;
    if (cur.hasDekubaba) {
        if (cur.dekubabaActionState != prev.dekubabaActionState) return true;
        // Stem angles drive head Y via trig — even small drift here
        // shows up as visible head float on the non-host.
        if (RotDeltaAbs(cur.dekubabaStemAngles[0], prev.dekubabaStemAngles[0]) >= kRotThresholdS16) return true;
        if (RotDeltaAbs(cur.dekubabaStemAngles[1], prev.dekubabaStemAngles[1]) >= kRotThresholdS16) return true;
        if (RotDeltaAbs(cur.dekubabaStemAngles[2], prev.dekubabaStemAngles[2]) >= kRotThresholdS16) return true;
    }
    if (cur.hasEnGoma != prev.hasEnGoma) return true;
    if (cur.hasEnGoma) {
        if (cur.enGomaActionState != prev.enGomaActionState) return true;
    }
    if (cur.hasDekunuts != prev.hasDekunuts) return true;
    if (cur.hasDekunuts) {
        if (cur.dekunutsActionState      != prev.dekunutsActionState)      return true;
        if (cur.dekunutsAnimFlagAndTimer != prev.dekunutsAnimFlagAndTimer) return true;
    }
    if (cur.hasHintnuts != prev.hasHintnuts) return true;
    if (cur.hasHintnuts) {
        if (cur.hintnutsActionState      != prev.hintnutsActionState)      return true;
        if (cur.hintnutsAnimFlagAndTimer != prev.hintnutsAnimFlagAndTimer) return true;
        if (cur.hintnutsPuzzleCounter    != prev.hintnutsPuzzleCounter)    return true;
    }
    if (cur.hasEnSt != prev.hasEnSt) return true;
    if (cur.hasEnSt) {
        if (cur.enStActionState != prev.enStActionState) return true;
    }
    if (cur.hasEnSw != prev.hasEnSw) return true;
    if (cur.hasEnSw) {
        if (cur.enSwActionState != prev.enSwActionState) return true;
    }
    if (cur.hasBossGoma != prev.hasBossGoma) return true;
    if (cur.hasBossGoma) {
        if (cur.bossGomaActionState         != prev.bossGomaActionState)         return true;
        if (cur.bossGomaEyeClosedTimer      != prev.bossGomaEyeClosedTimer)      return true;
        if (cur.bossGomaInvincibilityFrames != prev.bossGomaInvincibilityFrames) return true;
        if (cur.bossGomaVisualState         != prev.bossGomaVisualState)         return true;
        if (cur.bossGomaEyeState            != prev.bossGomaEyeState)            return true;
        if (cur.bossGomaChildrenState[0]    != prev.bossGomaChildrenState[0])    return true;
        if (cur.bossGomaChildrenState[1]    != prev.bossGomaChildrenState[1])    return true;
        if (cur.bossGomaChildrenState[2]    != prev.bossGomaChildrenState[2])    return true;
    }
    if (cur.hasSyokudai != prev.hasSyokudai) return true;
    if (cur.hasSyokudai) {
        // Only flag category transitions, not per-frame decrement.
        // Both clients run ObjSyokudai_Update locally and decrement
        // litTimer in lockstep; once the host's "lit transition" edge
        // (0 → positive) lands on peer, peer's local decrement keeps
        // pace without per-frame broadcasts. Categories:
        //   <= 0       — unlit (or burnt out)
        //   == -1      — permanently lit (puzzle complete)
        //   >  0       — burning, counting down
        auto bucket = [](s16 v) -> int {
            if (v == -1) return 2;     // permanently lit
            if (v >  0)  return 1;     // burning
            return 0;                  // unlit
        };
        if (bucket(cur.syokudaiLitTimer) != bucket(prev.syokudaiLitTimer)) return true;
    }
    return false;
}

bool AnyRotAxisExceeds(Vec3s cur, Vec3s prev, s16 threshold) {
    return RotDeltaAbs(cur.x, prev.x) >= threshold
        || RotDeltaAbs(cur.y, prev.y) >= threshold
        || RotDeltaAbs(cur.z, prev.z) >= threshold;
}

bool ScaleNear(Vec3f cur, Vec3f prev) {
    return fabsf(cur.x - prev.x) < kScaleEpsilon
        && fabsf(cur.y - prev.y) < kScaleEpsilon
        && fabsf(cur.z - prev.z) < kScaleEpsilon;
}

bool ShouldSkipEnemyUpdate(uint32_t netId,
                           const Actor* actor,
                           const EnemyNetId* ext,
                           const EnemyUpdateExtras& extras,
                           uint64_t nowMs,
                           uint64_t keepaliveMs) {
    auto it = sLastSentByNetId.find(netId);
    if (it == sLastSentByNetId.end()) return false;
    const EnemyUpdateLastSent& p = it->second;

    if ((nowMs - p.lastSentMs) >= keepaliveMs) return false;
    if (actor->colChkInfo.health != p.health) return false;
    if (ExtrasDiffer(extras, p.extras)) return false;

    uint64_t curHash = (ext != nullptr) ? HashLimbs(ext->skelAnime, ext->limbCount) : 0;
    if (curHash != p.jointHash) return false;

    float dx = actor->world.pos.x - p.pos.x;
    float dy = actor->world.pos.y - p.pos.y;
    float dz = actor->world.pos.z - p.pos.z;
    if ((dx * dx + dz * dz) >= kThresholdXZSq)                                return false;
    if (fabsf(dy) >= kThresholdY)                                             return false;
    if (AnyRotAxisExceeds(actor->world.rot, p.rot, kRotThresholdS16))         return false;
    if (AnyRotAxisExceeds(actor->shape.rot, p.shapeRot, kRotThresholdS16))    return false;
    if (!ScaleNear(actor->scale, p.scale))                                    return false;

    return true;
}

void UpdateLastSentCache(uint32_t netId,
                         const Actor* actor,
                         const EnemyNetId* ext,
                         const EnemyUpdateExtras& extras,
                         uint64_t nowMs) {
    EnemyUpdateLastSent& p = sLastSentByNetId[netId];
    p.pos        = actor->world.pos;
    p.rot        = actor->world.rot;
    p.shapeRot   = actor->shape.rot;
    p.scale      = actor->scale;
    p.health     = actor->colChkInfo.health;
    p.extras     = extras;
    p.jointHash  = (ext != nullptr) ? HashLimbs(ext->skelAnime, ext->limbCount) : 0;
    p.lastSentMs = nowMs;
}

}  // namespace

// Public — called on scene-load (HookHandlers.cpp OnSceneSpawnActors) and on
// reconnect (Anchor.cpp OnConnected) so a fresh peer sees a send on the
// next frame instead of waiting for the keepalive timer to elapse.
void Anchor_ClearEnemyUpdateCache() {
    sLastSentByNetId.clear();
}

// Public — drop a single netId from the dedup cache so the next
// SendPacket_EnemyUpdate(netId, ...) bypasses the no-delta filter and
// actually transmits. Used by #166's mid-boss late-join snapshot, where
// we WANT the packet to go out even if the steady-state cache thinks
// nothing has changed since the last broadcast (the joining peer hasn't
// seen any of those broadcasts yet).
void Anchor_ClearEnemyUpdateCacheForNetId(uint32_t netId) {
    sLastSentByNetId.erase(netId);
}

// ===========================================================================
// SEND SIDE — phase-specific senders (called from HookHandlers.cpp + others).
// All four set type=ENEMY_STATE and stamp the matching phase tag.
// ===========================================================================

// Phase=Alive, phaseChanged=false — steady-state per-frame update.
// Sent by the host every frame for each enemy actor in the current scene.
// Phase 5 #60: per-netId last-sent cache lets us skip no-op packets.
void Anchor::SendPacket_EnemyUpdate(uint32_t netId, Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only send if at least one other client is in the same scene AND room.
    s8 hostRoom = (s8)gPlayState->roomCtx.curRoom.num;
    bool hasRemoteInRoom = false;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.curRoomNum == hostRoom &&
            client.online && client.isSaveLoaded && !client.self) {
            hasRemoteInRoom = true;
            break;
        }
    }
    if (!hasRemoteInRoom) {
        return;
    }

    const bool     skipEnabled  = CVarGetInteger("gEnhancements.EnemyUpdateSkipEnabled", 1) != 0;
    const uint64_t keepaliveMs  = (uint64_t)std::max(1, CVarGetInteger("gEnhancements.EnemyUpdateKeepaliveMs", 250));

    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);

    EnemyUpdateExtras extras;
    uint64_t          nowMs = 0;
    if (skipEnabled) {
        extras = GatherExtras(actor);
        nowMs  = NowMonotonicMs();
        if (ShouldSkipEnemyUpdate(netId, actor, ext, extras, nowMs, keepaliveMs)) {
            SPDLOG_TRACE("[EnemyUpdate] Skipped netId={} (no-delta within {}ms)", netId, keepaliveMs);
            return;
        }
    } else {
        extras = GatherExtras(actor);
    }

    // State-machine TX logging — fires on the host once per delta. Local-
    // only (file write); zero relay impact. Compares current GatherExtras
    // result against the cached prev (if any). Adding a per-actor branch
    // here for new state-machine consumers keeps the log signal narrow:
    // only state changes for the actors we're actively debugging.
    {
        auto cacheIt = sLastSentByNetId.find(netId);
        const EnemyUpdateExtras* prevExtras =
            (cacheIt != sLastSentByNetId.end()) ? &cacheIt->second.extras : nullptr;
        if (extras.hasEnSw) {
            s16 prev = prevExtras && prevExtras->hasEnSw ? prevExtras->enSwActionState : -1;
            if (prev != extras.enSwActionState) {
                SPDLOG_INFO("[EnSw] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.enSwActionState);
            }
        }
        if (extras.hasEnSt) {
            s16 prev = prevExtras && prevExtras->hasEnSt ? prevExtras->enStActionState : -1;
            if (prev != extras.enStActionState) {
                SPDLOG_INFO("[EnSt] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.enStActionState);
            }
        }
        if (extras.hasDekunuts) {
            s16 prev = prevExtras && prevExtras->hasDekunuts ? prevExtras->dekunutsActionState : -1;
            if (prev != extras.dekunutsActionState) {
                SPDLOG_INFO("[EnDekunuts] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.dekunutsActionState);
            }
        }
        if (extras.hasHintnuts) {
            s16 prev = prevExtras && prevExtras->hasHintnuts ? prevExtras->hintnutsActionState : -1;
            if (prev != extras.hintnutsActionState) {
                SPDLOG_INFO("[EnHintnuts] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.hintnutsActionState);
            }
        }
        if (extras.hasDekubaba) {
            s16 prev = prevExtras && prevExtras->hasDekubaba ? prevExtras->dekubabaActionState : -1;
            if (prev != extras.dekubabaActionState) {
                SPDLOG_INFO("[EnDekubaba] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.dekubabaActionState);
            }
        }
        if (extras.hasKarebaba) {
            s16 prev = prevExtras && prevExtras->hasKarebaba ? prevExtras->karebabaActionState : -1;
            if (prev != extras.karebabaActionState) {
                SPDLOG_INFO("[EnKarebaba] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.karebabaActionState);
            }
        }
        if (extras.hasEnGoma) {
            s16 prev = prevExtras && prevExtras->hasEnGoma ? prevExtras->enGomaActionState : -1;
            if (prev != extras.enGomaActionState) {
                SPDLOG_INFO("[EnGoma] tx netId={} state={}→{}", netId,
                            (int)prev, (int)extras.enGomaActionState);
            }
        }
    }

    // Generic NPC State Sync Phase 0: read sync scope from the actor
    // and skip the emit entirely if scope is None. This preserves
    // bandwidth + avoids cross-team leak for actors that opt out.
    const AnchorSync::ActorSyncScope scope = AnchorSync::GetActorSyncScope(actor);
    if (scope == AnchorSync::ActorSyncScope::None) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["phase"]        = "Alive";
    payload["phaseChanged"] = false;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = netId;
    payload["pos"]      = actor->world.pos;
    payload["rot"]      = actor->world.rot;
    payload["shapeRot"] = actor->shape.rot;
    payload["health"]   = actor->colChkInfo.health;
    payload["scale"]    = actor->scale;
    payload["scope"]    = AnchorSync::ActorSyncScopeToString(scope);
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    if (extras.hasKarebaba) {
        payload["actionState"] = extras.karebabaActionState;
        payload["actorParams"] = extras.karebabaActorParams;
    }

    if (extras.hasGoroiwa) {
        payload["actionState"]    = extras.goroiwaActionState;
        payload["goroiwaCurWp"]   = (int)extras.goroiwaCurWp;
        payload["goroiwaNextWp"]  = (int)extras.goroiwaNextWp;
        payload["goroiwaPathDir"] = (int)extras.goroiwaPathDir;
        payload["goroiwaFlags"]   = (int)extras.goroiwaFlags;
    }

    // KB-08 / #7 — Dekubaba state-machine sync. Drives non-host
    // ApplyNetState to keep both clients on the same cycle phase.
    if (extras.hasDekubaba) {
        payload["actionState"] = extras.dekubabaActionState;
        // Bug 2 follow-on — three-element stem angle array drives the
        // animated head-tip Y position on the non-host.
        nlohmann::json stems = nlohmann::json::array();
        stems.push_back((int)extras.dekubabaStemAngles[0]);
        stems.push_back((int)extras.dekubabaStemAngles[1]);
        stems.push_back((int)extras.dekubabaStemAngles[2]);
        payload["dekubabaStems"] = stems;
    }

    // Plan §7 / KB-26 — En_Goma (Larva) state-machine sync. Drives non-
    // host EnGoma_ApplyNetState in HookHandlers' receive driver. Resolves
    // the egg-hatch desync where each client's local hatch timer advances
    // independently and host's egg appears hatched while non-host's egg
    // is still translating.
    if (extras.hasEnGoma) {
        payload["actionState"] = extras.enGomaActionState;
    }

    // #135 / en_dekunuts_sync_plan.md §8 — Mad Scrub state-machine sync.
    if (extras.hasDekunuts) {
        payload["actionState"]              = extras.dekunutsActionState;
        payload["dekunutsAnimFlagAndTimer"] = (int)extras.dekunutsAnimFlagAndTimer;
    }

    // En_Hintnuts state-machine sync (Inside Deku Tree Compound Room).
    if (extras.hasHintnuts) {
        payload["actionState"]              = extras.hintnutsActionState;
        payload["hintnutsAnimFlagAndTimer"] = (int)extras.hintnutsAnimFlagAndTimer;
        payload["hintnutsPuzzleCounter"]    = (int)extras.hintnutsPuzzleCounter;
    }

    // #90 / en_st_sync_plan_v2.md §3 — En_St state-machine sync.
    if (extras.hasEnSt) {
        payload["actionState"] = extras.enStActionState;
    }

    // #148 / en_sw_sync_plan.md §3 — En_Sw state-machine sync.
    if (extras.hasEnSw) {
        payload["actionState"] = extras.enSwActionState;
    }

    // Boss_Goma — minimal Encounter -> FloorMain bridge (any player can
    // trigger the fight on every client) + stunned/damaged buckets +
    // eye-vulnerability gate fields.
    if (extras.hasBossGoma) {
        payload["actionState"]                 = extras.bossGomaActionState;
        payload["bossGomaEyeClosedTimer"]      = (int)extras.bossGomaEyeClosedTimer;
        payload["bossGomaInvincibilityFrames"] = (int)extras.bossGomaInvincibilityFrames;
        payload["bossGomaVisualState"]         = (int)extras.bossGomaVisualState;
        payload["bossGomaEyeState"]            = (int)extras.bossGomaEyeState;
        payload["bossGomaChildrenState"]       = nlohmann::json::array({
            (int)extras.bossGomaChildrenState[0],
            (int)extras.bossGomaChildrenState[1],
            (int)extras.bossGomaChildrenState[2],
        });
    }

    // Per-torch lit-state sync (Obj_Syokudai).
    if (extras.hasSyokudai) {
        payload["syokudaiLitTimer"] = (int)extras.syokudaiLitTimer;
    }

    if (ext != nullptr && ext->skelAnime != nullptr && ext->limbCount > 0) {
        payload["jointTable"] = SkelAnimeWire::SerializePoseTable(
            ext->skelAnime->jointTable, ext->limbCount);
        if (ext->skelAnime->morphTable != nullptr) {
            auto morphs = SkelAnimeWire::SerializePoseTable(
                ext->skelAnime->morphTable, ext->limbCount);
            if (!morphs.empty()) {
                payload["morphTable"] = morphs;
            }
        }
    }

    SPDLOG_DEBUG("[EnemyUpdate] Send netId={} pos=({:.1f},{:.1f},{:.1f}) health={}",
                 netId, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                 (int)actor->colChkInfo.health);

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.curRoomNum == hostRoom &&
            client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }

    if (skipEnabled && isConnected) {
        UpdateLastSentCache(netId, actor, ext, extras, nowMs);
    }
}

// Phase=Alive, phaseChanged=true — dynamic spawn replication.
// Sent by the host when a runtime spawn occurs after the initial scene
// actor batch has loaded (En_Encount1, Peahat larvae, Floormaster split).
void Anchor::SendPacket_EnemySpawn(Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Generic NPC State Sync Phase 0: skip emit if actor scope is None.
    const AnchorSync::ActorSyncScope scope = AnchorSync::GetActorSyncScope(actor);
    if (scope == AnchorSync::ActorSyncScope::None) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["phase"]        = "Alive";
    payload["phaseChanged"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"]  = actor->id;
    payload["pos"]      = actor->home.pos;
    payload["rot"]      = actor->home.rot;
    payload["params"]   = actor->params;
    payload["scope"]    = AnchorSync::ActorSyncScopeToString(scope);
    PacketTimeline::SetTimelineField(payload);

    // Host-authoritative netId for dynamic spawns (#67-Gohma crash fix).
    // The host's OnActorSpawn assigned a collision-free netId via
    // EncodeUniqueDynamicNetId; carry it on the wire so the non-host's
    // local actor can adopt the same value rather than recomputing the
    // (potentially colliding) deterministic posHash. Receive-side override
    // lives in HandlePacket_EnemySpawn.
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext != nullptr && ext->netId != 0) {
        payload["netId"] = ext->netId;
    }

    SPDLOG_INFO("[EnemySpawn] Sending spawn actorId={} netId={} pos=({:.1f},{:.1f},{:.1f}) params={}",
                actor->id,
                payload.value("netId", (uint32_t)0),
                actor->home.pos.x, actor->home.pos.y, actor->home.pos.z, actor->params);

    SendJsonToRemote(payload);
}

// Phase=DyingByLocal, phaseChanged=true — defeat broadcast with attribution.
// Sent by any client when an enemy fires OnEnemyDefeat or Actor_Kill.
// Q I Tier 2: host attributes locally; non-host route-to-host with relay-
// enriched sender field for tamper-proof attribution.
void Anchor::SendPacket_EnemyDefeated(uint32_t netId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["phase"]        = "DyingByLocal";
    payload["phaseChanged"] = true;
    payload["netId"] = netId;
    PacketTimeline::SetTimelineField(payload);

    if (::SceneAuthority::IsMyCurrentRoomHost()) {
        auto& bookkeeping = EnemyStateSync::HostBookkeeping::Instance();
        const uint32_t damager = bookkeeping.LookupDamager(netId);
        const uint32_t killerId = damager != 0 ? damager : ownClientId;
        payload["killerClientId"] = killerId;
        if (killerId == ownClientId) {
            payload["killerTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        } else if (clients.contains(killerId)) {
            payload["killerTeamId"] = clients[killerId].teamId;
        }
        bookkeeping.ClearDamager(netId);

        SPDLOG_INFO("[EnemyDefeated] Host send for netId={} killerClientId={} killerTeamId={}",
                    netId,
                    payload.value("killerClientId", (uint32_t)0),
                    payload.value("killerTeamId", std::string("(unattributed)")));

        for (auto& [clientId, client] : clients) {
            if (client.online && client.isSaveLoaded && !client.self) {
                payload["targetClientId"] = clientId;
                SendJsonToRemote(payload);
            }
        }
    } else {
        SPDLOG_INFO("[EnemyDefeated] Non-host route-to-host for netId={} (host will attribute and re-broadcast)",
                    netId);
        payload["targetClientId"] = effectiveHostClientId;
        SendJsonToRemote(payload);
    }
}

// Phase=Regrowing, phaseChanged=true — Karebaba respawn-skip signal.
// Sent by the host when its Karebaba completes the natural death cycle and
// returns to a living state. Receivers still in DeadItemDrop/Dead skip
// immediately to Regrow so both clients converge within ~1s.
void Anchor::SendPacket_EnemyRespawn(uint32_t netId) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["phase"]        = "Regrowing";
    payload["phaseChanged"] = true;
    payload["netId"]    = netId;
    payload["sceneNum"] = (s16)gPlayState->sceneNum;
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[EnemyRespawn] Sending respawn for netId={}", netId);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

// ===========================================================================
// RECEIVE SIDE — per-phase handlers, dispatched by HandlePacket_EnemyState.
// All four called only from the dispatcher; not registered as packet types.
// ===========================================================================

// Phase=Alive, phaseChanged=false — steady-state per-frame update.
// Non-hosts apply pos/rot/health/skeleton from the host's authoritative
// state; host short-circuits (no self-apply) EXCEPT for push blocks
// (ACTOR_OBJ_OSHIHIKI), which use a bidirectional last-mover-wins pattern.
void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);
    Vec3f pos      = payload.value("pos", Vec3f{ 0, 0, 0 });
    Vec3s rot      = payload.value("rot", Vec3s{ 0, 0, 0 });
    Vec3s shapeRot = payload.value("shapeRot", rot);
    s8 health      = (s8)payload.value("health", 1);
    Vec3f scale    = payload.value("scale", Vec3f{ 1.0f, 1.0f, 1.0f });

    Actor* actor = nullptr;
    EnemyNetId* ext = nullptr;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (actor != nullptr) {
            ext = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext != nullptr && ext->netId == netId) {
                goto actor_found;
            }
            actor = actor->next;
        }
    }
    actor = nullptr;
actor_found:
    if (actor == nullptr) {
        // Suppress the warning on host: when a peer broadcasts a push-block
        // update for an actor in a different scene/room, the host's view
        // legitimately won't have that netId yet. The non-host warn fires
        // on real netId mismatches that block sync.
        //
        // Also suppress when we've already broadcast (or received-and-
        // recorded) a defeat for this netId (2026-05-15 log 118 fix).
        // After Actor_Kill removes the local actor, the host's in-flight
        // ENEMY_UPDATE packets that crossed our DEFEATED in the wire
        // produce a brief flurry of these "missing actor" lookups. Log
        // 118 showed 2-4 warnings per kill across three actors — pure
        // noise, the host stops sending within a few packets. The
        // HostBookkeeping::HasDefeatBroadcast set is populated by both
        // OnEnemyDefeat (local kill) and OnActorKill (network-driven
        // kill that completed locally), so a single check covers both
        // paths.
        if (!::SceneAuthority::IsEffectiveHost() &&
            !EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(netId)) {
            SPDLOG_WARN("[EnemyUpdate] No actor found for netId={} sceneNum={} — possible netId mismatch",
                        netId, sceneNum);
        }
        return;
    }

    // Push-block bidirectional sync: HandlePacket_EnemyUpdate runs on host
    // too for ACTOR_OBJ_OSHIHIKI so peer's pushes reach all clients. We
    // skip the apply when our local actor is currently in a push state —
    // otherwise a peer's stale echo would overwrite our in-progress local
    // push.
    const bool isPushBlock = (actor->id == ACTOR_OBJ_OSHIHIKI);
    if (isPushBlock) {
        ObjOshihiki* block = (ObjOshihiki*)actor;
        if ((block->stateFlags & (PUSHBLOCK_PUSH | PUSHBLOCK_FALL)) != 0) {
            return;
        }
    } else {
        // Self-apply skip: if the packet's sender is me, skip — don't
        // re-apply my own broadcast (relay may echo to sender for some
        // packet types). Sender id is stamped by the relay onto the
        // packet's `clientId` field, so it's authoritative regardless
        // of whether the sender was the room host at broadcast time.
        //
        // (Predecessor gate was "skip if I'm room host" — equivalent
        // under host-authoritative single-broadcaster sync but fragile
        // if authority shifts mid-flight or future packet types diverge
        // from the room-host-broadcasts-only pattern.)
        const uint32_t senderId = payload.value("clientId", (uint32_t)0);
        if (senderId != 0 && Anchor::Instance != nullptr &&
            senderId == Anchor::Instance->ownClientId) {
            return;
        }
    }

    // Receiver audio cue: when a remote pusher's ENEMY_STATE arrives and the
    // local actor isn't running its own ObjOshihiki_Push (which would emit
    // the rock-slide sound natively), play the same SFX so peer pushes
    // sound the same as local ones. Lower threshold rejects idle keepalives;
    // upper threshold rejects first-packet teleports (e.g. peer enters scene
    // and applies host's authoritative position which may differ from the
    // initial spawn pos by tens of units).
    //
    // Also keep home.pos tracking world.pos. ObjOshihiki_Push uses
    // home.pos as the base for the next 20-unit slide (z_obj_oshihiki.c:568)
    // and only commits home.pos = world.pos at push-complete. Without this
    // sync, a peer-pushed block that the local player later tries to push
    // would compute world.pos = stale_home.pos + smallOffset and visibly
    // snap backwards. Skipped during local push so we don't clobber the
    // local push baseline (concurrent-push degenerate case).
    if (isPushBlock) {
        const f32 dx = pos.x - actor->world.pos.x;
        const f32 dy = pos.y - actor->world.pos.y;
        const f32 dz = pos.z - actor->world.pos.z;
        const f32 distSq = dx * dx + dy * dy + dz * dz;
        // Push moves blocks ~1.5 units/frame at default speed. 0.25 < distSq
        // < 100 admits real push motion, rejects keepalives and teleports.
        if (distSq > 0.25f && distSq < 100.0f) {
            Audio_PlayActorSound2(actor, NA_SE_EV_ROCK_SLIDE - SFX_FLAG);
        }
        actor->home.pos = pos;
    }

    // Cache state unconditionally so OnActorUpdate can re-apply after the
    // enemy's own update() runs (Fix 4) and Karebaba respawn re-sync has
    // fresh values on revival. netHealth is NOT cached here — only updated
    // when we actually apply the value (multi-hit guard below).
    ext->hasNetState = true;
    ext->netPos      = pos;
    ext->netRot      = rot;
    ext->netShapeRot = shapeRot;
    ext->netScale    = scale;

    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyUpdate.applyGuard");
    if (!EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
        // ACTOR_FLAG_ATTACHED_TO_ARROW guard (cross-cutting).
        // When En_Arrow pins an enemy, the target's `Update` short-circuits
        // to a SkelAnime-only pass and position is driven by the parent
        // arrow. Overwriting world.pos here every packet fights the arrow-
        // driven local position, causing the pinned actor to teleport back
        // to the host's pre-pin location. Affected actors: En_St, En_Sw,
        // En_Bili, En_Bb, En_Crow, En_Firefly, En_Po_Sisters. Joint/morph/
        // rotation/health/scale still sync normally — only the world.pos
        // write is skipped while the actor is arrow-pinned.
        const bool arrowPinned = (actor->flags & ACTOR_FLAG_ATTACHED_TO_ARROW) != 0;
        // Animation-driven actors (Fix 7): `world.pos` is computed each
        // frame from the actor's own state machine, so external host
        // overrides cause visible drift / teardown faults.
        //   En_Dekubaba — head-tip from home.pos + stemSectionAngles.
        //   En_Karebaba — Spin state position from home.pos + shape.rot.
        // Boss_Goma is NOT in this group — its world.pos is the boss's
        // body-root location set by its actionFunc each frame (e.g.
        // CeilingMoveToCenter, FloorMain), not an animation-derived
        // sub-limb position. Without world.pos sync the host's climb /
        // ceiling-spawn locations don't reach peers (Bug A from log 47).
        //
        // En_Nutsball (Hintnut puzzle deku-nut projectile) is also
        // excluded. Per IsSyncedWorldActor design comment in
        // ActorSyncHelpers.cpp:44-49, the nutsball is peer-replicated:
        // each client spawns its own copy and runs Actor_MoveXZGravity
        // locally. Position-sync from host drags peer's local nut off
        // its trajectory toward peer's local Link, missing the
        // shield AT collider window — peer's reflect never registers
        // AT_BOUNCED. ENEMY_STATE traffic still flows for visibility
        // but world.pos is left to local physics. (Targeting fix for
        // EnHintnuts_ThrowNut at z_en_hintnuts.c spawns the nut toward
        // the local player so each client's reflect window is real.)
        const bool isAnimationDrivenPos = (actor->id == ACTOR_EN_DEKUBABA ||
                                           actor->id == ACTOR_EN_KAREBABA ||
                                           actor->id == ACTOR_EN_NUTSBALL);
        if (!isAnimationDrivenPos && !arrowPinned) {
            actor->world.pos = pos;
        }
        // shape.rot exclusion is split per-actor:
        //   - Karebaba: skip — its state-machine setup funcs (SetupAwaken /
        //     SetupUpright / SetupRetract) write shape.rot themselves; host-
        //     overwrite would fight the local action-func mid-frame.
        //   - Dekubaba: APPLY — shape.rot.y IS the lunge direction (set in
        //     EnDekubaba_Grow at z_en_dekubaba.c:612 via
        //     Anchor_GetNearestPlayerActor). Without sync, each client picks
        //     its own local-player target and the lunge diverges (KB-08
        //     residual; the state-machine sync alone doesn't cover this
        //     because the targeting math runs every frame on both clients
        //     against their own world view). Math_ApproachS only nudges by
        //     0xE38 per frame so re-applying host's value at ~20pps wins
        //     decisively against the local nudge.
        //   - Boss_Goma: APPLY — shape.rot is the body's facing direction,
        //     driven by Math_ApproachS in combat actionFuncs. Same rationale
        //     as Dekubaba: ~20pps host overwrite wins decisively over the
        //     local per-frame nudge, and without it each client's Goma faces
        //     a different direction (each client targets its own local
        //     player).
        if (actor->id != ACTOR_EN_KAREBABA) {
            actor->shape.rot = shapeRot;
        }
        actor->world.rot = rot;

        // Multi-hit guard for regular enemies (only apply network HP if it's
        // <= local HP, so peer's locally-dealt damage isn't undone). For boss
        // actors, REVERSE — host is the sole HP authority. Without this, peer's
        // local UpdateHit decrements peer's HP independently of host, peer races
        // to 0 HP, and peer fires OnBossDefeat from peer's local SetupDefeated
        // path (field test 273: Goma killed after just 1 cross-network hit
        // because peer's local HP path beat the host's authoritative HP).
        const bool forceNetHealth = IsSyncedBossActor(actor->id);
        if (forceNetHealth || health <= actor->colChkInfo.health) {
            actor->colChkInfo.health = health;
            ext->netHealth           = health;
            // Peer-side network-drive-dying signal — see EnemyNetId
            // declaration in Anchor.h for full rationale. Engages
            // Anchor_ShouldSuppress*Drop on the SAME frame health
            // reaches 0 from host's authoritative track, closing the
            // race against peer's vanilla actionFunc reading the same
            // health value and reaching Item_DropCollectible before
            // ENEMY_DEFEATED arrives. Gated on !IsMyCurrentRoomHost so
            // host's legitimate own-kill drop call isn't blocked by
            // its own outgoing/echoed ENEMY_STATE.
            if (health <= 0 && !::SceneAuthority::IsMyCurrentRoomHost()) {
                ext->networkDriveDying = true;
            }
        }
        actor->scale = scale;

        // Skip joint sync for En_Karebaba in active attack states (2/3/4/7):
        // the local state machine drives joints from its own timers; applying
        // the host's table causes flicker between out-of-phase poses.
        bool skipJoints = false;
        if (actor->id == ACTOR_EN_KAREBABA) {
            s16 localState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
            skipJoints = (localState == 2 || localState == 3 ||
                          localState == 4 || localState == 7);
        }
        if (!skipJoints && ext->skelAnime != nullptr && ext->limbCount > 0) {
            // Centralised pose-table writer enforces both the per-actor
            // slot limit and the global hard cap. See SkelAnimeWire.h
            // for the full rationale; #171 root cause was the previous
            // hand-rolled loop overrunning EnDekubaba's morphTable into
            // its adjacent boundFloor pointer.
            if (payload.contains("jointTable")) {
                SkelAnimeWire::DeserializePoseTable(
                    ext->skelAnime->jointTable, ext->limbCount,
                    payload["jointTable"], netId);
            }
            if (payload.contains("morphTable") && ext->skelAnime->morphTable != nullptr) {
                SkelAnimeWire::DeserializePoseTable(
                    ext->skelAnime->morphTable, ext->limbCount,
                    payload["morphTable"], netId);
            }
        }

        if (actor->id == ACTOR_EN_KAREBABA && payload.contains("actionState")) {
            ext->netStateIndex  = (s16)payload["actionState"].get<int>();
            ext->netActorParams = (s16)payload.value("actorParams", (int)0);
        }

        // KB-08 / #7 — cache Dekubaba host state so OnActorUpdate can call
        // EnDekubaba_ApplyNetState when local state diverges from net.
        if (actor->id == ACTOR_EN_DEKUBABA && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }

        // Plan §7 / KB-26 — cache En_Goma actionState. Drives
        // EnGoma_ApplyNetState in HookHandlers' non-host receive driver.
        if (actor->id == ACTOR_EN_GOMA && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }

        // #135 / en_dekunuts_sync_plan.md §8 — cache Mad Scrub
        // actionState + animFlagAndTimer. Driver block in HookHandlers
        // applies state via EnDekunuts_ApplyNetState; the timer is
        // written directly to anchor projectile-spawn frame-6 detection
        // on the receiver.
        if (actor->id == ACTOR_EN_DEKUNUTS && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }
        if (actor->id == ACTOR_EN_DEKUNUTS && payload.contains("dekunutsAnimFlagAndTimer")) {
            EnDekunuts* d = (EnDekunuts*)actor;
            d->animFlagAndTimer = (s16)payload["dekunutsAnimFlagAndTimer"].get<int>();
        }

        // En_Hintnuts — cache actionState + animFlagAndTimer. Mirrors
        // the Dekunuts receive path: driver block in HookHandlers applies
        // state via EnHintnuts_ApplyNetState; timer write here anchors
        // ThrowNut's projectile-spawn frame-6 detection on the receiver.
        if (actor->id == ACTOR_EN_HINTNUTS && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }
        if (actor->id == ACTOR_EN_HINTNUTS && payload.contains("hintnutsAnimFlagAndTimer")) {
            EnHintnuts* h = (EnHintnuts*)actor;
            h->animFlagAndTimer = (s16)payload["hintnutsAnimFlagAndTimer"].get<int>();
        }
        // sPuzzleCounter sync — last-writer-wins. Each client's local
        // hits advance their own counter; ENEMY_STATE broadcasts the
        // current value, receivers overwrite. Keeps the puzzle's branch
        // logic (HitByScrubProjectile1 BG transition gate, SetupFreeze
        // -3→-4 wrong-order reset bump) consistent across clients.
        if (actor->id == ACTOR_EN_HINTNUTS && payload.contains("hintnutsPuzzleCounter")) {
            EnHintnuts_SetPuzzleCounter((s16)payload["hintnutsPuzzleCounter"].get<int>());
        }

        // #90 / en_st_sync_plan_v2.md — cache En_St actionState.
        if (actor->id == ACTOR_EN_ST && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }
        // #148 / en_sw_sync_plan.md — cache En_Sw actionState.
        if (actor->id == ACTOR_EN_SW && payload.contains("actionState")) {
            ext->netStateIndex = (s16)payload["actionState"].get<int>();
        }
        // Boss_Goma — cache host actionState. Receive driver in
        // HookHandlers' OnActorUpdate non-host block invokes
        // BossGoma_BridgeToCombat when local Goma is in Encounter (0x00)
        // and host has progressed to combat (>= 0x01), plus
        // BossGoma_ApplyMinimalNetState for stunned/damaged transitions.
        // Eye-vulnerability gate fields are written DIRECTLY to the
        // local actor here so peer's BossGoma_UpdateHit gates damage
        // in the same frames as the host (z_boss_goma.c:1830).
        if (actor->id == ACTOR_BOSS_GOMA) {
            if (payload.contains("actionState")) {
                ext->netStateIndex = (s16)payload["actionState"].get<int>();
            }
            BossGoma* bg = (BossGoma*)actor;
            if (payload.contains("bossGomaEyeClosedTimer")) {
                bg->eyeClosedTimer = (s16)payload["bossGomaEyeClosedTimer"].get<int>();
            }
            if (payload.contains("bossGomaInvincibilityFrames")) {
                bg->invincibilityFrames = (s16)payload["bossGomaInvincibilityFrames"].get<int>();
            }
            if (payload.contains("bossGomaVisualState")) {
                bg->visualState = (s16)payload["bossGomaVisualState"].get<int>();
            }
            if (payload.contains("bossGomaEyeState")) {
                bg->eyeState = (s16)payload["bossGomaEyeState"].get<int>();
            }
            // KB-44 — childrenGohmaState[3] sync. Peer's larvae have
            // parent=NULL (spawned via Actor_Spawn from ENEMY_SPAWN
            // receive, not Actor_SpawnAsChild) so peer's local EnGoma
            // can never write to peer's BossGoma — the slot stays at
            // 1 (alive) forever, BossGoma_CeilingIdle's all-dead
            // exit condition (z_boss_goma.c:1604-1609) never fires,
            // and the boss loops on the ceiling indefinitely.
            // Mirror host's authoritative array.
            if (payload.contains("bossGomaChildrenState")) {
                const auto& arr = payload["bossGomaChildrenState"];
                if (arr.is_array() && arr.size() >= 3) {
                    bg->childrenGohmaState[0] = (s16)arr[0].get<int>();
                    bg->childrenGohmaState[1] = (s16)arr[1].get<int>();
                    bg->childrenGohmaState[2] = (s16)arr[2].get<int>();
                }
            }
        }
        // Bug 2 follow-on — apply host's stem angles directly to the local
        // actor so EnDekubaba_UpdateHeadPosition (called every frame inside
        // each actionFunc) computes the same head Y as the host. Written
        // before the actor's update() runs next frame; the actionFunc may
        // step them further but the per-packet re-write keeps drift bounded.
        if (actor->id == ACTOR_EN_DEKUBABA && payload.contains("dekubabaStems")) {
            const auto& stems = payload["dekubabaStems"];
            if (stems.is_array() && stems.size() >= 3) {
                EnDekubaba* baba = (EnDekubaba*)actor;
                baba->stemSectionAngle[0] = (s16)stems[0].get<int>();
                baba->stemSectionAngle[1] = (s16)stems[1].get<int>();
                baba->stemSectionAngle[2] = (s16)stems[2].get<int>();
            }
        }

        if (actor->id == ACTOR_EN_GOROIWA) {
            EnGoroiwa* boulder = (EnGoroiwa*)actor;
            if (payload.contains("actionState")) {
                ext->netStateIndex = (s16)payload["actionState"].get<int>();
            }
            if (payload.contains("goroiwaCurWp")) {
                ext->goroiwaCurrentWaypoint = (s16)payload["goroiwaCurWp"].get<int>();
                boulder->currentWaypoint    = ext->goroiwaCurrentWaypoint;
            }
            if (payload.contains("goroiwaNextWp")) {
                ext->goroiwaNextWaypoint = (s16)payload["goroiwaNextWp"].get<int>();
                boulder->nextWaypoint    = ext->goroiwaNextWaypoint;
            }
            if (payload.contains("goroiwaPathDir")) {
                ext->goroiwaPathDirection = (s16)payload["goroiwaPathDir"].get<int>();
                boulder->pathDirection    = ext->goroiwaPathDirection;
            }
            if (payload.contains("goroiwaFlags")) {
                ext->goroiwaFlags   = (u8)payload["goroiwaFlags"].get<int>();
                boulder->stateFlags = ext->goroiwaFlags;
            }
        }

        if (actor->id == ACTOR_OBJ_SYOKUDAI && payload.contains("syokudaiLitTimer")) {
            ObjSyokudai* torch    = (ObjSyokudai*)actor;
            const s16 prevLit     = torch->litTimer;
            const s16 incoming    = (s16)payload["syokudaiLitTimer"].get<int>();
            ext->syokudaiLitTimer = incoming;

            // Stale-zero guard. The bidirectional broadcast can fire BEFORE
            // the broadcaster's own incoming-lit packet has been processed
            // in the same frame, so the outbound payload carries the
            // broadcaster's pre-receive `litTimer == 0` even when the
            // broadcaster is about to apply a positive value. Without this
            // guard, the peer's receive overwrites its local-lit (positive)
            // back to 0, vanilla `ObjSyokudai_Update` then sees `litTimer == 0`
            // with the player's burning Deku Stick still in proximity and
            // re-enters the ignite branch — `sLitTorchCount++` fires AGAIN,
            // doubling the counter from a single user-action ignite. Field
            // test 282 shows this exact pattern: same `home=` position
            // increments twice in 50ms (one frame).
            //
            // The cost: a true burnout-zero broadcast won't propagate
            // through this field, but each client's own vanilla decrement
            // reaches 0 independently within a few frames at the same rate
            // — visible drift is sub-frame and self-correcting. Permanent-
            // lit (litTimer == -1) torches stay -1 because `prevLit != 0`
            // still holds.
            const bool incomingIsUnlit = (incoming == 0);
            const bool localIsLit      = (prevLit != 0);
            if (!(incomingIsUnlit && localIsLit)) {
                torch->litTimer = incoming;
            }

            // Multi-torch puzzle auto-complete on bidirectional sync.
            // Single-player code increments a static `sLitTorchCount`
            // only when the LOCAL Deku-Stick lights a torch — torches
            // lit via the network path don't bump it. So when P1 lights
            // 2 torches and P2 lights 2 in a 4-torch puzzle, neither
            // client's counter reaches 4 and `Flags_SetSwitch` never
            // fires.
            //
            // Fix: when a torch transitions unlit → lit via network
            // apply, scan the actor list for every Obj_Syokudai sharing
            // this torch's puzzle switchFlag; count puzzle-participant
            // torches that are currently lit; compare to the MAX
            // torchCount across the group. If count >= max, fire
            // `Flags_SetSwitch` directly. Replicates via WORLD_FLAG_SET.
            //
            // Refinement (issue #189): use MAX(torchCount) across the
            // group rather than the lit torch's torchCount alone, AND
            // restrict the scan to the same room. Two failure modes
            // closed:
            //   1. Mixed-torchCount room: if torch A has torchCount=1
            //      (single-torch trigger) and torch B has torchCount=2
            //      (gating torch with same switchFlag), reading the
            //      threshold from torch A under-counts and fires after
            //      one lit when vanilla wouldn't. Using MAX picks 2.
            //   2. Cross-room scan contamination: ACTORCAT_PROP is
            //      scene-wide, but switch flags are scene-scoped. If
            //      another room reuses the same switchFlag for an
            //      unrelated torch, we'd count it. Same-room filter
            //      restricts the scan to the lit torch's room, where
            //      vanilla's `sLitTorchCount` mechanism actually lives.
            //   3. Decorative always-lit torch contamination:
            //      `params & 0x400` torches with `torchCount = 0` would
            //      otherwise inflate the lit-count. Skipping
            //      `aTorchCount == 0` excludes them.
            //
            // Idempotent — Flags_SetSwitch is a no-op if already set.
            const bool wasUnlit  = (prevLit == 0);
            const bool isLit     = (torch->litTimer != 0);
            const s32 switchFlag = torch->actor.params & 0x3F;
            const s32 torchCount = (torch->actor.params >> 6) & 0xF;
            const s32 torchType  = torch->actor.params & 0xF000;
            const s8  litRoom    = torch->actor.room;
            if (wasUnlit && isLit && torchCount > 0 && torchType != 0 &&
                gPlayState != nullptr && !Flags_GetSwitch(gPlayState, switchFlag)) {
                int litInGroup = 0;
                int maxTorchCount = 0;
                Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head;
                while (a != nullptr) {
                    if (a->id == ACTOR_OBJ_SYOKUDAI &&
                        (a->params & 0x3F) == switchFlag &&
                        a->room == litRoom) {
                        const int aTorchCount = (a->params >> 6) & 0xF;
                        if (aTorchCount > 0) {
                            if (aTorchCount > maxTorchCount) {
                                maxTorchCount = aTorchCount;
                            }
                            if (((ObjSyokudai*)a)->litTimer != 0) {
                                litInGroup++;
                            }
                        }
                    }
                    a = a->next;
                }
                if (maxTorchCount > 0 && litInGroup >= maxTorchCount) {
                    Flags_SetSwitch(gPlayState, switchFlag);
                    SPDLOG_INFO("[Syokudai] Puzzle complete via network sync: "
                                "switchFlag={} room={} lit={}/{} (lit-torch torchCount={})",
                                switchFlag, (int)litRoom, litInGroup, maxTorchCount, torchCount);
                } else {
                    SPDLOG_INFO("[Syokudai] Lit transition observed but threshold not met: "
                                "switchFlag={} room={} lit={}/{} (lit-torch torchCount={})",
                                switchFlag, (int)litRoom, litInGroup, maxTorchCount, torchCount);
                }
            }
        }
    }

    SPDLOG_DEBUG("[EnemyUpdate] Applied netId={} pos=({:.1f},{:.1f},{:.1f}) health={}",
                 netId, pos.x, pos.y, pos.z, (int)health);
}

// Phase=Alive, phaseChanged=true — dynamic spawn replication.
// Non-host clients allow a single spawn through their suppression gate
// and let the host's canonical actor populate the local actor list.
void Anchor::HandlePacket_EnemySpawn(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)-1);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    s16    actorId = payload.value("actorId", (s16)0);
    Vec3f  pos     = payload["pos"].get<Vec3f>();
    Vec3s  rot     = payload["rot"].get<Vec3s>();
    s16    params  = payload.value("params", (s16)0);

    SPDLOG_INFO("[EnemySpawn] Received spawn actorId={} pos=({:.1f},{:.1f},{:.1f}) params={}",
                actorId, pos.x, pos.y, pos.z, params);

    // Idempotency guard for #186 — drop the spawn if an actor with this
    // netId already exists in any synced category. Without this, a
    // duplicate ENEMY_SPAWN (from the echo cascade or a legitimate
    // late-replay) creates a second instance of the same logical
    // actor; subsequent ENEMY_STATE updates pick whichever lookup hits
    // first and the duplicate accumulates garbage state until it
    // crashes the F3DEX interpreter. Layer 1 (host re-broadcast guard
    // in HookHandlers.cpp) is the primary closer; this layer is
    // defence in depth covering legit duplicate-delivery cases too.
    if (payload.contains("netId")) {
        uint32_t incomingNetId = payload["netId"].get<uint32_t>();
        for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
            Actor* existing = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
            while (existing != nullptr) {
                const EnemyNetId* ext =
                    ObjectExtension::GetInstance().Get<EnemyNetId>(existing);
                if (ext != nullptr && ext->netId == incomingNetId) {
                    SPDLOG_INFO("[EnemySpawn] Drop duplicate spawn — netId={} already "
                                "exists for actorId={} ptr={} (idempotency guard)",
                                incomingNetId, existing->id, (void*)existing);
                    return;
                }
                existing = existing->next;
            }
        }
    }

    isSpawningNetworkActor = true;
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId,
                                 pos.x, pos.y, pos.z,
                                 rot.x, rot.y, rot.z, params);
    isSpawningNetworkActor = false;

    if (spawned == nullptr) {
        SPDLOG_WARN("[EnemySpawn] Actor_Spawn failed for actorId={} — actor limit reached or invalid id",
                    actorId);
        return;
    }

    // Host-authoritative netId override (#67-Gohma crash fix).
    // The host's OnActorSpawn ran EncodeUniqueDynamicNetId and probed
    // for collision-free posHash bytes; non-host's deterministic local
    // compute would re-collide on tight clusters (12 Larvae in Gohma's
    // egg-throw). When the payload carries a netId, replace whatever
    // OnActorSpawn just stored with the host's authoritative value so
    // both clients agree on actor identity.
    if (payload.contains("netId")) {
        uint32_t hostNetId = payload["netId"].get<uint32_t>();
        EnemyNetId* ext = const_cast<EnemyNetId*>(
            ObjectExtension::GetInstance().Get<EnemyNetId>(spawned));
        if (ext != nullptr && ext->netId != hostNetId) {
            SPDLOG_INFO("[EnemySpawn] Override netId {} -> {} for actorId={} (host-authoritative)",
                        ext->netId, hostNetId, actorId);
            ext->netId = hostNetId;
        }
    }
}

// Phase=DyingByLocal, phaseChanged=true — defeat broadcast.
// Host re-broadcast (Q I Tier 2 attribution), Karebaba natural-cycle
// trigger, dedup, and pendingKill bookkeeping all live here.
void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Pillar B Phase 1 limitation: cross-timeline kills are dropped here
    // instead of being re-broadcast to peers in the killer's timeline.
    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    // Pillar E note: ValidateSameScene intentionally omitted — defeat
    // packets are cross-scene tolerant by design (deadEnemiesByScene
    // replay covers the late-load case).

    uint32_t netId = payload.value("netId", (uint32_t)0);

    uint32_t    killerClientId = payload.value("killerClientId", (uint32_t)0);
    std::string killerTeamId   = payload.value("killerTeamId", std::string{});

    SPDLOG_INFO("[EnemyDefeated] Received defeat for netId={} killerClientId={} killerTeamId={}",
                netId, killerClientId,
                killerTeamId.empty() ? "(unattributed)" : killerTeamId);

    // Host-routed attribution: when host receives unattributed kill, fill
    // in attribution from the relay-enriched sender field and re-broadcast.
    if (::SceneAuthority::IsEffectiveHost() && killerClientId == 0 &&
        !EnemyStateSync::HostBookkeeping::Instance().HasDefeatBroadcast(netId)) {
        uint32_t senderId = payload.value("clientId", (uint32_t)0);
        if (senderId != 0) {
            EnemyStateSync::HostBookkeeping::Instance().ClaimDefeatBroadcast(netId);

            nlohmann::json rebroadcast;
            rebroadcast["type"]           = ENEMY_STATE;
            rebroadcast["phase"]          = "DyingByLocal";
            rebroadcast["phaseChanged"]   = true;
            rebroadcast["netId"]          = netId;
            rebroadcast["killerClientId"] = senderId;
            PacketTimeline::SetTimelineField(rebroadcast);
            if (clients.contains(senderId)) {
                rebroadcast["killerTeamId"] = clients[senderId].teamId;
            }

            SPDLOG_INFO("[EnemyDefeated] Host re-broadcast for netId={} killerClientId={} (attributed from sender)",
                        netId, senderId);

            for (auto& [clientId, client] : clients) {
                if (client.online && client.isSaveLoaded && !client.self &&
                    clientId != senderId) {
                    rebroadcast["targetClientId"] = clientId;
                    SendJsonToRemote(rebroadcast);
                }
            }
        }
    }

    // Walk every syncable actor category looking for the netId match.
    // Covers ENEMY + BOSS plus runtime category transitions
    // (Karebaba→MISC, Armos→BG, etc.).
    for (size_t catIdx = 0; catIdx < kSyncableActorCategoriesCount; catIdx++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[catIdx]].head;
        while (actor != nullptr) {
            Actor* next = actor->next;
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext != nullptr && ext->netId == netId) {
                if (::SceneAuthority::IsMyCurrentRoomHost()) {
                    EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(gPlayState->sceneNum, netId);
                }

                // Init-pending guard. `actor->init != NULL` means the
                // actor's init function hasn't run yet — OoT defers it
                // for static actors when the object isn't loaded at
                // OnActorSpawn time (z_actor.c:1256-1268), running the
                // deferred init at z_actor.c:2633-2645. If we run a
                // per-actor SetupDyingNet here, the deferred init will
                // unconditionally rewrite `actionFunc` (every Init
                // function does this) and overwrite the dying state —
                // the actor stays alive after init.
                //
                // Symptom (P2 log 265, scene-respawn replay): three
                // Skullwalltulas re-spawn on scene re-entry; host's
                // replay arrives in the same sub-millisecond batch.
                // For 2/3 of them, init had already run by ~50ms before
                // the replay (SetupDyingNet wins). For the 3rd, init
                // ran ~50ms AFTER replay (init's SetupWait wins) — the
                // user saw 1 of 3 Skullwalltulas alive on re-entry.
                //
                // Fix: when init is still pending, skip the natural
                // death cycle and kill the actor directly. We're
                // replaying a past defeat — the death animation isn't
                // user-visible (the user wasn't there to see it the
                // first time). Karebaba already handles this case via
                // the AwaitingDeadItemDrop phase + OnActorInit hook
                // (Fix 38), so it's exempt from the generic guard.
                if (actor->init != NULL && actor->id != ACTOR_EN_KAREBABA) {
                    SPDLOG_INFO("[EnemyDefeated] netId={} actorId={} — init pending, instant kill (skip SetupDyingNet to avoid Init-overwrite race)",
                                netId, actor->id);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    isKillingNetworkActor = true;
                    Actor_Kill(actor);
                    isKillingNetworkActor = false;
                    return;
                }

                // Karebaba: let the natural death→respawn cycle play out
                // instead of calling Actor_Kill. Dedup against duplicate
                // delivery via PhaseImpliesHasLocalDeath.
                if (actor->id == ACTOR_EN_KAREBABA) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Karebaba.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} — triggering natural death cycle", netId);
                    EnKarebaba_SetupDyingNet((EnKarebaba*)actor);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // En_St: route through EnSt_SetupDyingNet so the
                // BounceAround → FinishBouncing → Die natural cycle plays.
                // Plan §6 / #90.
                if (actor->id == ACTOR_EN_ST) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.EnSt.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] EnSt netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] EnSt netId={} — triggering natural death cycle", netId);
                    EnSt_SetupDyingNet((EnSt*)actor, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // En_Sw: route through EnSw_SetupDyingNet — branches
                // internally on swType (combat vs gold variant). Plan §6 / #148.
                if (actor->id == ACTOR_EN_SW) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.EnSw.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] EnSw netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] EnSw netId={} — triggering natural death cycle", netId);
                    EnSw_SetupDyingNet((EnSw*)actor, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // Mad Scrub: route through EnDekunuts_SetupDyingNet so the
                // BeDamaged → Die natural cycle plays on the receiver
                // without echoing GameInteractor_ExecuteOnEnemyDefeat.
                // Plan §3 / #135.
                if (actor->id == ACTOR_EN_DEKUNUTS) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Dekunuts.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] Dekunuts netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] Dekunuts netId={} — triggering natural death cycle", netId);
                    EnDekunuts_SetupDyingNet((EnDekunuts*)actor, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // En_Dekubaba: route through SetupDyingNet so peer plays
                // the natural death animation. Two distinct paths handled
                // in the helper:
                //   PATH A: ShrinkDie → Deku Nut drop (small Babas + most
                //           kills, includes big-Baba final blow without
                //           StunnedVertical).
                //   PATH B: PrunedSomersault → DeadStickDrop → Deku Stick
                //           drop (big Babas struck mid-StunnedVertical
                //           with sword/boomerang).
                // Per-player consumables rule (Q 5.1): each peer drops
                // their own copy of the items. #89.
                if (actor->id == ACTOR_EN_DEKUBABA) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Dekubaba.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] Dekubaba netId={} already dying — duplicate, dedup only", netId);
                        // Peer-side dual-drop fix (field log 2026-05-06): peer's
                        // local OnEnemyDefeat fired before this packet arrived
                        // (state-machine sync drove health to 0; vanilla
                        // EnDekubaba_Hit → SetupShrinkDie at z_en_dekubaba.c:972),
                        // setting phase=DyingByLocal. Without upgrading to
                        // DyingByNetwork, Anchor_ShouldSuppressDekubabaDrop's
                        // PhaseImpliesPendingNaturalDeath check returns false
                        // and peer's ShrinkDie spawns a local EN_ITEM00 — then
                        // ITEM_DROP_SYNC adds a second one. Upgrading the phase
                        // here ensures the suppressor catches the next
                        // ShrinkDie tick (Math_StepToF gates the drop on the
                        // final shrink frame, which arrives after this packet
                        // in every observed timing).
                        if (ext->phase == EnemyStateSync::LifecyclePhase::DyingByLocal) {
                            EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                        }
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    // Issue #187: scene-respawn replay. When a peer leaves and
                    // returns to a scene, the host's deadEnemiesByScene list
                    // replays the defeat for the freshly-spawned Dekubaba.
                    // The fresh actor is in dormant Wait state (state 0) —
                    // never activated, never grew, never lunged.
                    // SetupDyingNet's ShrinkDie path (z_en_dekubaba.c:1456-
                    // 1461) sets actionFunc=ShrinkDie and the FastChompAnim,
                    // but ShrinkDie's preconditions don't hold for a Wait-
                    // state actor (no stem, no growth state) — actor stays
                    // alive in a half-init'd dying state. Visible bug:
                    // peer hits the Dekubaba, doesn't take damage, looks
                    // like vanilla "dormant Dekubaba is invulnerable from
                    // a distance."
                    //
                    // Fix: detect dormant state via GetStateIndex == 0 and
                    // Actor_Kill directly. Death animation is skipped — the
                    // peer already saw it in the original kill before
                    // leaving the scene. On re-entry the semantically-
                    // correct outcome is "this enemy is dead, period."
                    EnDekubaba* dekubaba = (EnDekubaba*)actor;
                    if (EnDekubaba_GetStateIndex(dekubaba) == 0) {
                        SPDLOG_INFO("[EnemyDefeated] Dekubaba netId={} dormant — Actor_Kill direct (avoids stuck-alive bug from scene-respawn replay, #187)", netId);
                        EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        isKillingNetworkActor = true;
                        Actor_Kill(actor);
                        isKillingNetworkActor = false;
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] Dekubaba netId={} — triggering natural death cycle", netId);
                    EnDekubaba_SetupDyingNet(dekubaba, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // Boss_Goma: route through SetupDyingNet so peer plays
                // the death animation, drops the heart container, and
                // spawns the blue warp via BossGoma_Defeated's natural
                // sequence. Without this routing, peer's defeat shows
                // Goma blinking out instantly with none of the post-fight
                // room state changes.
                if (actor->id == ACTOR_BOSS_GOMA) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.BossGoma.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] BossGoma netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] BossGoma netId={} — triggering natural death cycle", netId);
                    BossGoma_SetupDyingNet((BossGoma*)actor, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // En_Goma (Boss_Goma's Larva): hatched larvae play a real
                // Hurt → Die animation; route through SetupDyingNet so the
                // natural cycle plays on peer instead of an instant
                // Actor_Kill blink-out. Egg-state larvae (gomaType !=
                // ENGOMA_NORMAL) have no death anim — fall through to the
                // generic Actor_Kill path below.
                if (actor->id == ACTOR_EN_GOMA &&
                    ((EnGoma*)actor)->gomaType == ENGOMA_NORMAL) {
                    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.EnGoma.dupDetect");
                    if (EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                        SPDLOG_INFO("[EnemyDefeated] EnGoma netId={} already dying — duplicate, dedup only", netId);
                        EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                        return;
                    }
                    SPDLOG_INFO("[EnemyDefeated] EnGoma netId={} — triggering natural death cycle", netId);
                    EnGoma_SetupDyingNet((EnGoma*)actor, gPlayState);
                    EnemyStateSync::TransitionTo(*ext, EnemyStateSync::LifecyclePhase::DyingByNetwork);
                    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                    return;
                }

                // En_Hintnuts: when peer's local Leave kill block fires
                // first (peer's projectedPos.z<0 trigger arrives before
                // peer's animFlagAndTimer reaches 0, beating host's local
                // Leave kill), peer broadcasts ENEMY_DEFEATED. The
                // generic Actor_Kill below removes host's #3 hintnut
                // before host's local Leave actionFunc reaches its kill
                // block — so host never runs `if (params==3) sPuzzleCounter
                // = 3` (z_en_hintnuts.c:469-472). Without sPuzzleCounter
                // hitting 3 on host, hintnuts #1 and #2 stay frozen
                // forever in vanilla Freeze actionFunc (z_en_hintnuts.c:
                // 487-495), and host's hintnutsPuzzleCounter broadcasts
                // overwrite peer's local 3 back to 2 — both clients leave
                // #1/#2 stunned in place (logs 217).
                //
                // Apply Leave's puzzle-completion side effects on host
                // before the kill: sPuzzleCounter = 3 advances host's
                // local Freeze actionFunc to its sink+Actor_Kill path;
                // Flags_SetClear mirrors the room-clear flag (also
                // replicates via SET_FLAG sync from peer's Leave, but
                // we set it locally for ordering parity).
                if (actor->id == ACTOR_EN_HINTNUTS && actor->params == 3) {
                    EnHintnuts_SetPuzzleCounter(3);
                    Flags_SetClear(gPlayState, actor->room);
                    SPDLOG_INFO("[EnemyDefeated] Hintnut #3 netId={} — puzzle complete (sPuzzleCounter=3, room clear flag set)",
                                netId);
                }

                // KB-44 — egg-state EnGoma kill bypasses the natural
                // death cycle (vanilla writes parent->childrenGohmaState
                // = -1 inside EnGoma_UpdateDamage's egg-die block at
                // z_en_goma.c:691-697 OR EnGoma_Dead at z_en_goma.c:444).
                // When a peer kills an egg locally, peer broadcasts
                // ENEMY_DEFEATED; host falls through to the generic
                // Actor_Kill below without running either vanilla write,
                // leaving host's BossGoma::childrenGohmaState slot at
                // 1 forever. BossGoma_CeilingIdle then loops because
                // the all-dead exit needs ALL three slots = -1.
                // Apply the slot write here as host-side compensation.
                // (Hatched larvae are routed through SetupDyingNet
                // above and reach EnGoma_Dead's natural write; this
                // branch only catches the egg-state fall-through.)
                if (actor->id == ACTOR_EN_GOMA && actor->params < 3 &&
                    actor->parent != nullptr) {
                    BossGoma* parent = (BossGoma*)actor->parent;
                    parent->childrenGohmaState[actor->params] = -1;
                    SPDLOG_INFO("[EnemyDefeated] EnGoma egg netId={} — set parent "
                                "childrenGohmaState[{}] = -1 (compensation for "
                                "Actor_Kill bypassing natural cycle)",
                                netId, (int)actor->params);
                }

                SPDLOG_INFO("[EnemyDefeated] Killing actor id={} netId={}", actor->id, netId);
                isKillingNetworkActor = true;
                Actor_Kill(actor);
                isKillingNetworkActor = false;
                return;
            }
            actor = next;
        }
    }

    // Also check ACTORCAT_MISC: a Karebaba moves there during DeadItemDrop/Dead.
    {
        Actor* misc = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
        while (misc != nullptr) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(misc));
            if (ext != nullptr && ext->netId == netId) {
                EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyDefeated.Karebaba.MISC.dupDetect");
            }
            if (ext != nullptr && ext->netId == netId &&
                EnemyStateSync::PhaseImpliesHasLocalDeath(ext->phase)) {
                SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} in ACTORCAT_MISC natural cycle — duplicate, dedup only", netId);
                EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);
                return;
            }
            misc = misc->next;
        }
    }

    SPDLOG_WARN("[EnemyDefeated] No actor found for netId={} — buffering as pendingKill (scene not loaded yet?)",
                netId);
    EnemyStateSync::HostBookkeeping::Instance().RecordPendingKill(netId);

    // Also record the kill in deadEnemiesByScene so the scene host's
    // join-time replay (HandlePacket_UpdateClientState) covers this enemy.
    // Phase 2: extract both sceneNum and timeline from netId (Pillar B
    // packed both: bits 30-16 = scene & 0x7FFF, bit 31 = linkAge & 0x1)
    // so the recording happens on whichever client is currently scene
    // host of (sceneNum, timeline) — not the global host. This handles
    // the case where peer alone in a room kills an enemy and is the
    // authoritative recorder for that scene's dead set.
    {
        int16_t sceneFromNetId    = (int16_t)((netId >> 16) & 0x7FFF);
        uint8_t timelineFromNetId = (uint8_t)((netId >> 31) & 0x1);
        // netId doesn't encode roomNum, so use my local current room.
        // If I'm in the actor's room I'm a candidate for scene host;
        // otherwise IsSceneHost returns false and the record fires on
        // the client that IS in that room.
        if (gPlayState != nullptr &&
            ::SceneAuthority::IsRoomHost(sceneFromNetId,
                                          (s8)gPlayState->roomCtx.curRoom.num,
                                          timelineFromNetId)) {
            EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(sceneFromNetId, netId);
        }
    }
}

// Phase=Regrowing, phaseChanged=true — Karebaba respawn-skip signal.
// Receiver still in DeadItemDrop/Dead skips ahead to Regrow. No-op if
// the actor is already past the death cycle.
void Anchor::HandlePacket_EnemyRespawn(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;

    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    uint32_t netId = payload.value("netId",    (uint32_t)0);
    s16      scene = payload.value("sceneNum", (s16)0);

    if (VALIDATE(::ReceiveValidator::ValidateSameScene(scene)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    // Search ACTORCAT_MISC first: Karebaba lives there during
    // DeadItemDrop / Dead states.
    Actor* actor = nullptr;
    {
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
        while (it) {
            const EnemyNetId* e = ObjectExtension::GetInstance().Get<EnemyNetId>(it);
            if (e && e->netId == netId) { actor = it; break; }
            it = it->next;
        }
    }
    // Fallback: already back in ACTORCAT_ENEMY.
    if (!actor) {
        Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
        while (it) {
            const EnemyNetId* e = ObjectExtension::GetInstance().Get<EnemyNetId>(it);
            if (e && e->netId == netId) { actor = it; break; }
            it = it->next;
        }
    }

    if (!actor) {
        // Race fix (logs 31, netId 2153105155): defeat for not-yet-loaded
        // actor was buffered as pendingKill, then respawn arrived before
        // load. Clear pendingKill so the actor will spawn alive.
        if (EnemyStateSync::HostBookkeeping::Instance().IsPendingKill(netId)) {
            SPDLOG_INFO("[EnemyRespawn] netId={} actor not loaded; cancelling pendingKill so it spawns alive",
                        netId);
            EnemyStateSync::HostBookkeeping::Instance().ClearPendingKill(netId);
        } else {
            SPDLOG_DEBUG("[EnemyRespawn] netId={} not found — ignoring (actor already respawned?)", netId);
        }
        return;
    }

    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext != nullptr) {
        EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyRespawn.pendingCheck");
    }
    if (!ext || !EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase)) {
        return;
    }

    if (actor->id != ACTOR_EN_KAREBABA) {
        return;
    }

    s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
    if (curState == 6 || curState == 8) {
        SPDLOG_INFO("[EnemyRespawn] netId={} skipping to Regrow (was state={})", netId, curState);
        EnKarebaba_SetupRegrowNet((EnKarebaba*)actor);
    }
}

// ===========================================================================
// DISPATCH — sole entry point for ENEMY_STATE wire packets.
// ===========================================================================

void Anchor::HandlePacket_EnemyState(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;

    // Generic NPC State Sync Phase 0: team-scope filter.
    //
    // If the sender declared this actor's state as Team-scoped, only
    // apply on this client when our TeamId matches the sender's. Cross-
    // team packets are dropped (defence in depth — relay's targetTeamId
    // routing should already prevent these from arriving, but we don't
    // trust the relay).
    //
    // Default scope on receive is Global (back-compat with legacy
    // schema-1 senders that don't include a scope field). Defeated and
    // Regrowing phases never carry a scope field today and rely on this
    // default — death/respawn events propagate globally regardless of
    // the actor's normal scope.
    const std::string scopeStr = payload.value("scope", std::string("global"));
    if (scopeStr == "team") {
        const uint32_t senderId = payload.value("clientId", (uint32_t)0);
        auto senderIt = clients.find(senderId);
        if (senderIt == clients.end()) {
            // Unknown sender — drop (could be a stale packet from a
            // recently-disconnected peer; we have no way to verify
            // their team).
            return;
        }
        const std::string& senderTeam = senderIt->second.teamId;
        const std::string ownTeam = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        if (senderTeam != ownTeam) {
            return;
        }
    }

    const std::string phase        = payload.value("phase", std::string("Alive"));
    const bool        phaseChanged = payload.value("phaseChanged", false);
    const uint32_t    netId        = payload.value("netId", (uint32_t)0);

    SPDLOG_DEBUG("[EnemyState] Recv netId={} phase={} phaseChanged={}",
                 netId, phase, phaseChanged);

    if (phase == "Alive" && !phaseChanged) {
        HandlePacket_EnemyUpdate(payload);
    } else if (phase == "Alive" && phaseChanged) {
        HandlePacket_EnemySpawn(payload);
    } else if (phase == "DyingByLocal" && phaseChanged) {
        HandlePacket_EnemyDefeated(payload);
    } else if (phase == "Regrowing" && phaseChanged) {
        HandlePacket_EnemyRespawn(payload);
    } else {
        SPDLOG_WARN("[EnemyState] Unhandled phase combination netId={} phase={} phaseChanged={}",
                    netId, phase, phaseChanged);
    }
}
