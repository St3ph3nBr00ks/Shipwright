#include "WorldStateSync.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "functions.h"
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace {

// Backing store for replicated flags. Once an entry lands here, it stays
// for the remainder of the session — v1 doesn't model unset (flags go
// out / torches extinguish). Phase 2+ customers will introduce a
// WORLD_FLAG_UNSET counterpart and move this to a value-bearing map.
std::unordered_set<WorldStateSync::WorldStateKey,
                   WorldStateSync::WorldStateKeyHash> sSetFlags;

// Echo guard. True while ReceiveFlagSet is in the middle of calling
// Flags_SetSwitch on the local PlayState — the resulting OnSceneFlagSet
// hook would otherwise re-broadcast the packet we just received.
bool sApplyingNetworkFlag = false;

}  // anonymous namespace

namespace WorldStateSync {

bool IsApplyingNetworkFlag() {
    return sApplyingNetworkFlag;
}

void Reset() {
    sSetFlags.clear();
    sApplyingNetworkFlag = false;
}

void OnLocalFlagSet(int16_t sceneNum, int16_t flagType, int16_t flag) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) {
        return;
    }
    if (sApplyingNetworkFlag) {
        return;  // echo from a network-driven Flags_SetSwitch
    }
    // Existing global gate — same one used by SET_FLAG / GIVE_ITEM / etc.
    if (Anchor::Instance->roomState.syncItemsAndFlags == 0) {
        return;
    }

    uint8_t timeline = (uint8_t)gSaveContext.linkAge;
    WorldStateKey key{sceneNum, timeline, flagType, flag};
    auto [_, inserted] = sSetFlags.insert(key);
    if (!inserted) {
        return;  // already in set; don't re-broadcast (idempotency)
    }

    nlohmann::json payload;
    payload["type"]     = Anchor::WORLD_FLAG_SET;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"]     = flag;
    PacketTimeline::SetTimelineField(payload);

    // Team-scoped broadcast — match the existing SET_FLAG / GIVE_ITEM
    // pattern (same gate, same routing).
    std::string ownTeamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["targetTeamId"] = ownTeamId;
    Anchor::Instance->SendJsonToRemote(payload);

    SPDLOG_INFO("[WorldStateSync] Local flag set sceneNum={} timeline={} flagType={} flag=0x{:02X} — broadcast",
                sceneNum, (int)timeline, flagType, flag);
}

void ReceiveFlagSet(int16_t sceneNum, uint8_t timeline,
                    int16_t flagType, int16_t flag) {
    WorldStateKey key{sceneNum, timeline, flagType, flag};
    sSetFlags.insert(key);

    // Apply to the live game state if the receiving client happens to be
    // in the same scene + timeline. Otherwise the entry sits in sSetFlags
    // and ApplyKnownFlagsForScene picks it up at next scene-spawn.
    if (gPlayState != nullptr &&
        (int16_t)gPlayState->sceneNum == sceneNum &&
        (uint8_t)gSaveContext.linkAge == timeline &&
        flagType == FLAG_SCENE_SWITCH) {
        sApplyingNetworkFlag = true;
        Flags_SetSwitch(gPlayState, flag);
        sApplyingNetworkFlag = false;
        SPDLOG_INFO("[WorldStateSync] Applied received flag locally sceneNum={} timeline={} flag=0x{:02X}",
                    sceneNum, (int)timeline, flag);
    }
}

void ApplyKnownFlagsForScene(int16_t sceneNum, uint8_t timeline) {
    if (gPlayState == nullptr) return;

    int applied = 0;
    sApplyingNetworkFlag = true;
    for (const auto& key : sSetFlags) {
        if (key.sceneNum != sceneNum || key.timeline != timeline) continue;
        if (key.flagType == FLAG_SCENE_SWITCH) {
            Flags_SetSwitch(gPlayState, key.flag);
            applied++;
        }
    }
    sApplyingNetworkFlag = false;

    if (applied > 0) {
        SPDLOG_INFO("[WorldStateSync] Applied {} known flag(s) on scene-spawn sceneNum={} timeline={}",
                    applied, sceneNum, (int)timeline);
    }
}

void SendRequestWorldState() {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return;
    if (Anchor::Instance->roomState.syncItemsAndFlags == 0) return;

    nlohmann::json payload;
    payload["type"]         = Anchor::WORLD_STATE_REQUEST;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    Anchor::Instance->SendJsonToRemote(payload);
    SPDLOG_INFO("[WorldStateSync] Sent WORLD_STATE_REQUEST to team");
}

nlohmann::json BuildSnapshotPayload() {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& key : sSetFlags) {
        entries.push_back({
            {"sceneNum", key.sceneNum},
            {"timeline", key.timeline},
            {"flagType", key.flagType},
            {"flag",     key.flag},
        });
    }
    return entries;
}

void ApplySnapshotPayload(const nlohmann::json& payload) {
    if (!payload.is_array()) return;

    int newEntries = 0;
    for (const auto& entry : payload) {
        int16_t sceneNum = entry.value("sceneNum", (int16_t)-1);
        uint8_t timeline = entry.value("timeline", (uint8_t)0);
        int16_t flagType = entry.value("flagType", (int16_t)0);
        int16_t flag     = entry.value("flag",     (int16_t)0);
        if (sceneNum < 0) continue;

        WorldStateKey key{sceneNum, timeline, flagType, flag};
        auto [_, inserted] = sSetFlags.insert(key);
        if (inserted) newEntries++;
    }

    SPDLOG_INFO("[WorldStateSync] Applied snapshot — {} new entries (total now {})",
                newEntries, sSetFlags.size());

    // Apply newly-arrived entries to current scene immediately.
    if (gPlayState != nullptr) {
        ApplyKnownFlagsForScene((int16_t)gPlayState->sceneNum,
                                (uint8_t)gSaveContext.linkAge);
    }
}

}  // namespace WorldStateSync
