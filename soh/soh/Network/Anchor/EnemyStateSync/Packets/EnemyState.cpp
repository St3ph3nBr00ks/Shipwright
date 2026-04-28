#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
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
    uint64_t h = 14695981039346656037ULL;
    auto mix16 = [&h](uint16_t v) { h ^= (uint64_t)v; h *= 1099511628211ULL; };
    for (uint8_t i = 0; i <= limbCount; i++) {
        mix16((uint16_t)anime->jointTable[i].x);
        mix16((uint16_t)anime->jointTable[i].y);
        mix16((uint16_t)anime->jointTable[i].z);
    }
    if (anime->morphTable != nullptr) {
        mix16(0xFFFFu);
        for (uint8_t i = 0; i <= limbCount; i++) {
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
        e.hasDekubaba         = true;
        e.dekubabaActionState = EnDekubaba_GetStateIndex((EnDekubaba*)actor);
    }
    return e;
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
    }
    return false;
}

int RotDeltaAbs(s16 a, s16 b) {
    int d = (int)(int16_t)(a - b);
    return std::abs(d);
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
    }

    if (ext != nullptr && ext->skelAnime != nullptr && ext->limbCount > 0) {
        nlohmann::json joints = nlohmann::json::array();
        nlohmann::json morphs = nlohmann::json::array();
        for (uint8_t i = 0; i <= ext->limbCount; i++) {
            joints.push_back(ext->skelAnime->jointTable[i]);
            if (ext->skelAnime->morphTable != nullptr) {
                morphs.push_back(ext->skelAnime->morphTable[i]);
            }
        }
        payload["jointTable"] = joints;
        if (!morphs.empty()) {
            payload["morphTable"] = morphs;
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

    nlohmann::json payload;
    payload["type"]         = ENEMY_STATE;
    payload["phase"]        = "Alive";
    payload["phaseChanged"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorId"]  = actor->id;
    payload["pos"]      = actor->home.pos;
    payload["rot"]      = actor->home.rot;
    payload["params"]   = actor->params;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[EnemySpawn] Sending spawn actorId={} pos=({:.1f},{:.1f},{:.1f}) params={}",
                actor->id, actor->home.pos.x, actor->home.pos.y, actor->home.pos.z, actor->params);

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

    if (::SceneAuthority::IsEffectiveHost()) {
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
// state; host short-circuits (no self-apply).
void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    if (::SceneAuthority::IsEffectiveHost()) {
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
        SPDLOG_WARN("[EnemyUpdate] No actor found for netId={} sceneNum={} — possible netId mismatch",
                    netId, sceneNum);
        return;
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
        // En_Karebaba and En_Dekubaba: world.pos and shape.rot are
        // animation-driven each frame. Skip both; jointTable sync covers visuals.
        if (actor->id != ACTOR_EN_DEKUBABA && actor->id != ACTOR_EN_KAREBABA) {
            actor->world.pos = pos;
            actor->shape.rot = shapeRot;
        }
        actor->world.rot = rot;

        if (health <= actor->colChkInfo.health) {
            actor->colChkInfo.health = health;
            ext->netHealth           = health;
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
            if (payload.contains("jointTable")) {
                const auto& joints = payload["jointTable"];
                uint8_t count = static_cast<uint8_t>(
                    std::min((size_t)(ext->limbCount + 1), joints.size()));
                for (uint8_t i = 0; i < count; i++) {
                    ext->skelAnime->jointTable[i] = joints[i].get<Vec3s>();
                }
            }
            if (payload.contains("morphTable") && ext->skelAnime->morphTable != nullptr) {
                const auto& morphs = payload["morphTable"];
                uint8_t count = static_cast<uint8_t>(
                    std::min((size_t)(ext->limbCount + 1), morphs.size()));
                for (uint8_t i = 0; i < count; i++) {
                    ext->skelAnime->morphTable[i] = morphs[i].get<Vec3s>();
                }
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

    isSpawningNetworkActor = true;
    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId,
                                 pos.x, pos.y, pos.z,
                                 rot.x, rot.y, rot.z, params);
    isSpawningNetworkActor = false;

    if (spawned == nullptr) {
        SPDLOG_WARN("[EnemySpawn] Actor_Spawn failed for actorId={} — actor limit reached or invalid id",
                    actorId);
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
                if (::SceneAuthority::IsEffectiveHost()) {
                    EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(gPlayState->sceneNum, netId);
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

    // Also record the kill in deadEnemiesByScene so the host's join-time
    // replay (HandlePacket_UpdateClientState) covers this enemy.
    if (::SceneAuthority::IsEffectiveHost()) {
        int16_t sceneFromNetId = (int16_t)((netId >> 16) & 0x7FFF);
        EnemyStateSync::HostBookkeeping::Instance().RecordSceneDeath(sceneFromNetId, netId);
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
