#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/ObjectExtension/ObjectExtension.h"
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
#include "z64.h"
#include "overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
// Issue #153 — En_Goroiwa carries extra path-state fields in ENEMY_UPDATE.
#include "overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_UPDATE
 *
 * Sent by the host every frame for each enemy actor in the current scene.
 * Non-host clients apply the received state to the matching local actor.
 * Enemy AI is suppressed on non-hosts via ShouldActorUpdate so enemies
 * are position/pose puppets driven entirely by the host.
 *
 * Phase 5 #60 — the per-netId last-sent cache lets us skip packets whose
 * content matches the last actual send within a keepalive window.
 * Design + constraints in Claude/Plans/phase5_enemy_update_threshold.md.
 *
 * Packet fields:
 *   netId     - deterministic id assigned at spawn (see HookHandlers.cpp)
 *   sceneNum  - guards against cross-scene application
 *   pos       - actor->world.pos
 *   rot       - actor->world.rot
 *   shapeRot  - actor->shape.rot (may differ from world.rot for animated enemies)
 *   health    - actor->colChkInfo.health (enables death detection)
 *   jointTable - (optional) SkelAnime joint rotations; only present when the enemy
 *                has a supported skeleton. Drives the rendered pose on non-host.
 */

namespace {

// Per-actor extras that the payload carries conditionally. Computed once per
// send invocation (GatherExtras) and consumed by BOTH the skip predicate and
// the payload build. Forcing both sites through one struct prevents future
// drift when new per-actor fields are added — if you extend ENEMY_UPDATE
// with another state field, add it here and both the skip predicate and the
// payload builder pick it up.
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
};

// Snapshot of the last ENEMY_UPDATE that actually went out (not skipped)
// for a given netId. Delta comparisons are always against this "last sent"
// snapshot, never against "last sampled" — otherwise sub-threshold drift
// accumulates without ever flushing.
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

// Compile-time spatial thresholds. See the plan doc for rationale. Kept as
// plain constants on the first landing — promote to CVars only if field
// measurement shows actor-specific tuning is required.
constexpr float kThresholdXZSq    = 4.0f;   // 2.0 OoT units² in XZ
constexpr float kThresholdY       = 2.0f;   // 2.0 OoT units in Y
constexpr s16   kRotThresholdS16  = 256;    // ≈ 1.4°
constexpr float kScaleEpsilon     = 0.01f;  // per-axis scale near-equality

// Monotonic clock — matches the profiler's clock so the two can be compared
// on the same timeline and is immune to wall-clock jumps (NTP, sleep/resume).
uint64_t NowMonotonicMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// FNV-1a over the skeleton's joint rotations plus (when present) morph
// rotations. A distinct separator mixed in when morphTable is present keeps
// an absent morphTable from colliding with a zero-filled morphTable.
// Returns 0 when no skeleton is present — serves double duty as "no pose"
// sentinel; a real skeleton nearly never hashes exactly to 0.
uint64_t HashLimbs(const SkelAnime* anime, uint8_t limbCount) {
    if (anime == nullptr || limbCount == 0) return 0;
    uint64_t h = 14695981039346656037ULL; // FNV-1a offset basis
    auto mix16 = [&h](uint16_t v) { h ^= (uint64_t)v; h *= 1099511628211ULL; };
    // limbCount + 1 entries — index 0 is the root translation, 1..limbCount
    // are limb rotations. Same range SendPacket_EnemyUpdate writes.
    for (uint8_t i = 0; i <= limbCount; i++) {
        mix16((uint16_t)anime->jointTable[i].x);
        mix16((uint16_t)anime->jointTable[i].y);
        mix16((uint16_t)anime->jointTable[i].z);
    }
    if (anime->morphTable != nullptr) {
        mix16(0xFFFFu); // presence separator
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

// Returns true when the candidate packet's content matches the last actual
// send for this netId AND we're inside the keepalive window. First send
// always goes through (no cache entry). All four C1–C4 constraints from the
// plan doc are enforced here.
bool ShouldSkipEnemyUpdate(uint32_t netId,
                           const Actor* actor,
                           const EnemyNetId* ext,
                           const EnemyUpdateExtras& extras,
                           uint64_t nowMs,
                           uint64_t keepaliveMs) {
    auto it = sLastSentByNetId.find(netId);
    if (it == sLastSentByNetId.end()) return false;
    const EnemyUpdateLastSent& p = it->second;

    // C4 — keepalive floor. Covers late joiners and dropped packets.
    if ((nowMs - p.lastSentMs) >= keepaliveMs) return false;

    // C2 — health.
    if (actor->colChkInfo.health != p.health) return false;

    // C1 — per-actor state-machine fields.
    if (ExtrasDiffer(extras, p.extras)) return false;

    // C3 — animation pose.
    uint64_t curHash = (ext != nullptr) ? HashLimbs(ext->skelAnime, ext->limbCount) : 0;
    if (curHash != p.jointHash) return false;

    // Spatial thresholds last — cheap rejects above are more likely to hit.
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

} // namespace

// Public — called on scene-load (HookHandlers.cpp OnSceneSpawnActors) and on
// reconnect (Anchor.cpp OnConnected) so a fresh peer sees a send on the
// next frame instead of waiting for the keepalive timer to elapse.
void Anchor_ClearEnemyUpdateCache() {
    sLastSentByNetId.clear();
}

void Anchor::SendPacket_EnemyUpdate(uint32_t netId, Actor* actor) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only send if at least one other client is in the same scene AND room.
    // Room filtering prevents "No actor found" spam when one client loads a room
    // slightly ahead of another — the other client doesn't have the actors yet.
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

    // Phase 5 #60 — skip no-op packets.
    //
    // CVar=0 bypasses every part of the skip logic (no cache compute, no
    // cache update, no predicate). That makes the CVar=0 code path
    // byte-identical to the pre-patch behavior, a clean safety hatch for
    // field triage.
    //
    // Raw CVar strings (not the CVAR_ENHANCEMENT macro) for consistency
    // with Anchor.cpp's existing profiler CVar read.
    const bool     skipEnabled  = CVarGetInteger("gEnhancements.EnemyUpdateSkipEnabled", 1) != 0;
    // Keepalive is tightened from the plan's 1000ms to 250ms to shrink the
    // late-join / reconnect stale-position window — see the plan's rec #1.
    // Clamped ≥ 1 so a mistyped 0 doesn't disable keepalive entirely.
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
        // CVar off path — still need extras for the payload build below, but
        // no cache interaction.
        extras = GatherExtras(actor);
    }

    nlohmann::json payload;
    payload["type"]     = ENEMY_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = netId;
    payload["pos"]      = actor->world.pos;
    payload["rot"]      = actor->world.rot;
    payload["shapeRot"] = actor->shape.rot;
    payload["health"]   = actor->colChkInfo.health;
    payload["scale"]    = actor->scale;
    payload["quiet"]    = true;
    PacketTimeline::SetTimelineField(payload);

    // Karebaba: sync action state and params timer so non-host state machine matches host.
    // Non-host uses these to call ApplyNetState when the host's state differs from its own.
    if (extras.hasKarebaba) {
        payload["actionState"] = extras.karebabaActionState;
        payload["actorParams"] = extras.karebabaActorParams;
    }

    // Goroiwa (#153): rolling-boulder path state.
    // Local non-host action func will run for collision but its own waypoint
    // advance can drift across frame-rate boundaries. Path index lives in
    // params and is identical on both clients via scene setupPathList; we just
    // need to keep the position+waypoint authoritative.
    if (extras.hasGoroiwa) {
        payload["actionState"]    = extras.goroiwaActionState;
        payload["goroiwaCurWp"]   = (int)extras.goroiwaCurWp;
        payload["goroiwaNextWp"]  = (int)extras.goroiwaNextWp;
        payload["goroiwaPathDir"] = (int)extras.goroiwaPathDir;
        payload["goroiwaFlags"]   = (int)extras.goroiwaFlags;
    }

    // Include the joint/morph tables if this enemy has a supported skeleton.
    if (ext != nullptr && ext->skelAnime != nullptr && ext->limbCount > 0) {
        nlohmann::json joints = nlohmann::json::array();
        nlohmann::json morphs = nlohmann::json::array();
        // limbCount + 1 entries: index 0 is the root translation, 1..limbCount are limb rotations.
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

    // Rec #2 — only write the cache when we actually had a connection to
    // enqueue into. If the Anchor was disconnected, SendJsonToRemote silently
    // drops the packet; caching it would cause the predicate to skip for up
    // to keepaliveMs after reconnection. OnConnected separately clears the
    // whole cache (rec #1), so this guard is belt-and-braces.
    if (skipEnabled && isConnected) {
        UpdateLastSentCache(netId, actor, ext, extras, nowMs);
    }
}

void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Pillar B Phase 1 — drop cross-timeline scene-scoped traffic.
    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    // Only non-hosts apply incoming enemy state.
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
    Vec3s shapeRot = payload.value("shapeRot", rot); // fall back to rot if field absent
    s8 health      = (s8)payload.value("health", 1);
    Vec3f scale    = payload.value("scale", Vec3f{ 1.0f, 1.0f, 1.0f });

    // Walk every syncable actor category looking for the netId match.
    // Shared list lives in Anchor.h — see kSyncableActorCategories.
    // Most synced actors are ACTORCAT_ENEMY; world-actors added via
    // IsSyncedWorldActor (issue #153) plus BOSS/ITEMACTION transitions
    // live elsewhere.
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

    // Cache state unconditionally so OnActorUpdate can re-apply it after the
    // enemy's own update() runs (required for collision registration — Fix 4),
    // and so that Karebaba respawn re-sync has fresh values on revival.
    // netHealth is NOT cached here — it is only updated when we actually apply
    // the health value to the actor (see multi-hit guard below).
    ext->hasNetState = true;
    ext->netPos      = pos;
    ext->netRot      = rot;
    ext->netShapeRot = shapeRot;
    ext->netScale    = scale;

    // Only write to the live actor when not in a local death animation.
    // After a local kill, hasLocalDeath=true; the actor's own death code
    // drives world.pos/rot/scale/joints each frame (e.g. BounceAround
    // modifies world.rot every frame for Gold Skulltula). Overwriting with
    // stale host values corrupts the death animation visually.
    EnemyStateSync::AuditBooleansVsPhase(*ext, "HandlePacket_EnemyUpdate.applyGuard");
    if (!ext->hasLocalDeath) {
        // En_Karebaba and En_Dekubaba: world.pos is computed analytically
        // each frame from home.pos + animated angles; shape.rot is driven
        // entirely by the local state machine. Overriding either causes
        // wobble or misalignment. Skip both; jointTable sync handles visuals.
        if (actor->id != ACTOR_EN_DEKUBABA && actor->id != ACTOR_EN_KAREBABA) {
            actor->world.pos = pos;
            actor->shape.rot = shapeRot;
        }
        actor->world.rot = rot;

        // Health sync: only apply if host's health is <= our current value
        // (host has taken as much or more damage). Prevents the host's stale
        // higher value from resetting locally-dealt damage on multi-hit enemies.
        if (health <= actor->colChkInfo.health) {
            actor->colChkInfo.health = health;
            ext->netHealth           = health;
        }
        actor->scale = scale;

        // Apply joint/morph tables if present in this packet and the actor has a skeleton.
        // Skip for En_Karebaba in active attack states (Awaken=2/Upright=3/Spin=4/Retract=7):
        // during these states the local update() drives the joint table each frame from its
        // own timers. P1's table reflects P1's animation phase, which differs from P2's
        // (both cycle Upright↔Spin but out of phase). Applying P1's table every packet
        // (~50ms) causes the head to flicker between P1's pose and P2's locally-computed
        // pose — visible as heads growing/shrinking on P2. During dormant states (Grow/Idle/
        // Regrow) joint table sync IS needed to show the correct growth/shrink animation.
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

        // Karebaba: cache received state index so OnActorUpdate can drive
        // the local state machine to match the host's current state.
        if (actor->id == ACTOR_EN_KAREBABA && payload.contains("actionState")) {
            ext->netStateIndex  = (s16)payload["actionState"].get<int>();
            ext->netActorParams = (s16)payload.value("actorParams", (int)0);
        }

        // Goroiwa (#153): cache path-state + apply current waypoint immediately so
        // the local action func reads consistent state on its next tick.
        // ApplyNetState (resetting actionFunc) is handled in OnActorUpdate so it
        // happens after the local update() pass, mirroring Karebaba's pattern.
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
