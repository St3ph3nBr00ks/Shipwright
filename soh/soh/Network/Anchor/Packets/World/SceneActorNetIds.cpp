#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

// KB-18 (#177) Option 4 — host-authoritative netId snapshot.
//
// Some actors (confirmed: En_Sw / Skullwalltula; suspected: any actor
// whose Init runs a raycast or other collision-dependent computation
// against a partially-initialised scene) mutate `actor->home.pos`
// non-deterministically. The two clients then compute different
// netIds for the same scene-table entry. The host's netId is the
// canonical truth; this packet broadcasts the host's mapping so non-
// hosts can override their local compute.
//
// Wire shape:
//   {
//     "type": "SCENE_ACTOR_NETIDS",
//     "schema": 1,
//     "sceneNum": 0x55,
//     "timeline": 1,
//     "entries": [
//       { "actorId": 149, "room": 0, "params": 0,
//         "homePos": [283, 478, 357], "netId": 2147522014 },
//       ...
//     ]
//   }
//
// Triggered by: OnSceneSpawnActors host-path. The sender defers to the
// next OnGameFrameUpdate via Anchor::pendingSceneActorNetIdsBroadcast
// so the broadcast happens AFTER all static actors have completed
// their Init + EnemyNetId assignment (OnSceneSpawnActors itself fires
// while the static-actor batch is still loading).
//
// Receive: cache the snapshot keyed by (sceneNum, timeline). If the
// receiving client's current scene matches, retroactively rewrite the
// netId on every matching already-spawned actor. Subsequent
// OnActorSpawn calls in the same scene check the cache first.
//
// Bandwidth: ~80 bytes JSON per entry; a busy scene with 30 static
// syncable actors costs ~2.4 KB once per scene-spawn. Negligible.
//
// Plan: Claude/Plans/kb18_investigation.md (Option 4).

namespace {

// Tolerance (squared) on homePos match. Per KB-18 evidence, En_Sw's
// raycast can produce ~18 unit Y deltas. ±50 per axis gives plenty of
// headroom while still distinguishing distinct actors of the same
// type in the same room (typically separated by hundreds of units).
constexpr float kHomePosTolerance     = 50.0f;
constexpr float kHomePosToleranceSq   = kHomePosTolerance * kHomePosTolerance;

inline uint32_t MakeKey(int16_t sceneNum, uint8_t timeline) {
    return ((uint32_t)(uint16_t)sceneNum << 8) | (uint32_t)timeline;
}

}  // namespace

void Anchor::SendPacket_SceneActorNetIds() {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!::SceneAuthority::IsEffectiveHost()) return;

    int16_t sceneNum = (int16_t)gPlayState->sceneNum;
    uint8_t timeline = (uint8_t)(gSaveContext.linkAge & 1);

    nlohmann::json entries = nlohmann::json::array();

    for (size_t catIdx = 0; catIdx < kSyncableActorCategoriesCount; catIdx++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[catIdx]].head;
        while (actor != nullptr) {
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext != nullptr && ext->netId != 0) {
                nlohmann::json entry;
                entry["actorId"] = (int)actor->id;
                entry["room"]    = (int)actor->room;
                entry["params"]  = (int)actor->params;
                entry["homePos"] = { actor->home.pos.x, actor->home.pos.y, actor->home.pos.z };
                entry["netId"]   = ext->netId;
                entries.push_back(std::move(entry));
            }
            actor = actor->next;
        }
    }

    if (entries.empty()) return;

    nlohmann::json payload;
    payload["type"]         = SCENE_ACTOR_NETIDS;
    payload["sceneNum"]     = sceneNum;
    payload["timeline"]     = timeline;
    payload["entries"]      = std::move(entries);
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");

    SPDLOG_INFO("[SceneActorNetIds] Host broadcast sceneNum={} timeline={} entryCount={}",
                sceneNum, (int)timeline, payload["entries"].size());

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SceneActorNetIds(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    // Host already has the canonical mapping (it's the sender's mapping).
    // Skip applying our own broadcast.
    if (::SceneAuthority::IsEffectiveHost()) return;

    int16_t sceneNum = (int16_t)payload.value("sceneNum", -1);
    uint8_t timeline = (uint8_t)payload.value("timeline", 0);
    if (sceneNum < 0) return;
    if (!payload.contains("entries") || !payload["entries"].is_array()) return;

    uint32_t key = MakeKey(sceneNum, timeline);
    auto& cache = sceneActorNetIdSnapshots[key];
    cache.clear();
    cache.reserve(payload["entries"].size());

    for (const auto& entry : payload["entries"]) {
        SceneActorNetIdEntry e;
        e.actorId = (int16_t)entry.value("actorId", 0);
        e.room    = (int8_t)entry.value("room", 0);
        e.params  = (int16_t)entry.value("params", 0);
        e.netId   = (uint32_t)entry.value("netId", (uint32_t)0);
        if (entry.contains("homePos") && entry["homePos"].is_array() && entry["homePos"].size() == 3) {
            e.homePos.x = entry["homePos"][0].get<float>();
            e.homePos.y = entry["homePos"][1].get<float>();
            e.homePos.z = entry["homePos"][2].get<float>();
        } else {
            e.homePos = { 0.0f, 0.0f, 0.0f };
        }
        cache.push_back(e);
    }

    SPDLOG_INFO("[SceneActorNetIds] Cached sceneNum={} timeline={} entryCount={}",
                sceneNum, (int)timeline, cache.size());

    // Retroactively reassign netIds for already-spawned actors. Handles
    // the race where non-host loads the scene faster than the snapshot
    // arrives over the wire.
    if (gPlayState != nullptr &&
        (int16_t)gPlayState->sceneNum == sceneNum &&
        (uint8_t)(gSaveContext.linkAge & 1) == timeline) {
        ApplyHostNetIdsToCurrentScene(sceneNum, timeline);
    }
}

void Anchor::ApplyHostNetIdsToCurrentScene(int16_t sceneNum, uint8_t timeline) {
    if (gPlayState == nullptr) return;
    auto cacheIt = sceneActorNetIdSnapshots.find(MakeKey(sceneNum, timeline));
    if (cacheIt == sceneActorNetIdSnapshots.end()) return;
    const auto& cache = cacheIt->second;

    int reassigned = 0;
    for (size_t catIdx = 0; catIdx < kSyncableActorCategoriesCount; catIdx++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[catIdx]].head;
        while (actor != nullptr) {
            EnemyNetId* ext = const_cast<EnemyNetId*>(
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext != nullptr) {
                uint32_t hostNetId = LookupHostNetIdForActor(actor, cache);
                if (hostNetId != 0 && hostNetId != ext->netId) {
                    SPDLOG_INFO("[SceneActorNetIds] Reassign actorId={} room={} params={} home=({:.0f},{:.0f},{:.0f}) netId={} → {}",
                                actor->id, actor->room, actor->params,
                                actor->home.pos.x, actor->home.pos.y, actor->home.pos.z,
                                ext->netId, hostNetId);
                    ext->netId = hostNetId;
                    reassigned++;
                }
            }
            actor = actor->next;
        }
    }

    if (reassigned > 0) {
        SPDLOG_INFO("[SceneActorNetIds] Reassigned {} actor netIds in sceneNum={} timeline={}",
                    reassigned, sceneNum, (int)timeline);
    }
}

uint32_t Anchor::LookupHostNetIdForActor(Actor* actor,
                                         const std::vector<SceneActorNetIdEntry>& cache) {
    int matches = 0;
    uint32_t matchedNetId = 0;
    for (const auto& e : cache) {
        if (e.actorId != actor->id)     continue;
        if (e.room    != actor->room)   continue;
        if (e.params  != actor->params) continue;
        float dx = e.homePos.x - actor->home.pos.x;
        float dy = e.homePos.y - actor->home.pos.y;
        float dz = e.homePos.z - actor->home.pos.z;
        if ((dx * dx + dy * dy + dz * dz) > kHomePosToleranceSq) continue;
        matches++;
        matchedNetId = e.netId;
    }
    if (matches > 1) {
        SPDLOG_WARN("[SceneActorNetIds] Ambiguous match — actorId={} room={} params={} home=({:.0f},{:.0f},{:.0f}); "
                    "{} entries within tolerance. Falling back to local compute.",
                    actor->id, actor->room, actor->params,
                    actor->home.pos.x, actor->home.pos.y, actor->home.pos.z,
                    matches);
        return 0;
    }
    return matchedNetId;
}

uint32_t Anchor::LookupHostNetIdForCurrentScene(Actor* actor) {
    if (gPlayState == nullptr) return 0;
    int16_t sceneNum = (int16_t)gPlayState->sceneNum;
    uint8_t timeline = (uint8_t)(gSaveContext.linkAge & 1);
    auto cacheIt = sceneActorNetIdSnapshots.find(MakeKey(sceneNum, timeline));
    if (cacheIt == sceneActorNetIdSnapshots.end()) return 0;
    return LookupHostNetIdForActor(actor, cacheIt->second);
}
