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

// FLAG_SCENE_SWITCH bit space split (per Flags_SetSwitch at z_actor.c:675):
//   bits  0..0x1F → actorCtx.flags.swch     (persistent — saved to
//                                            gSaveContext.sceneFlags,
//                                            survives scene reload)
//   bits 0x20..0x3F → actorCtx.flags.tempSwch (per-scene-visit, RESET
//                                              on every scene reload)
//
// Temp flags must NOT be replicated / replayed by WorldStateSync.
// Replaying them via ApplyKnownFlagsForScene on scene-spawn re-asserts
// state OoT meant to clear, e.g. a pressure plate that resets when
// you step off shows up as still-pressed when you re-enter the room
// (Test 3 finding — flag 0x3C / 0x3D incorrectly persisting across
// session restart, observed as "switch already active before P1
// touched it").
//
// v1 scope: silently skip temp flags at every entry point. They fall
// back to vanilla local-only OoT behaviour. Cross-client sync of
// temp flags is a future enhancement that needs scene-visit-scoped
// storage and explicit reset-on-scene-load semantics.
inline bool IsTemporaryFlag(int16_t flagType, int16_t flag) {
    return flagType == FLAG_SCENE_SWITCH && flag >= 0x20;
}

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
    if (IsTemporaryFlag(flagType, flag)) {
        return;  // tempSwch flags reset on scene reload — don't replicate
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
    if (IsTemporaryFlag(flagType, flag)) {
        // Defensive — a non-upgraded peer may still send a temp-flag
        // packet. Drop it rather than store-and-replay (which would
        // re-introduce the persistence bug on the receiver).
        return;
    }
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

void OnLocalFlagUnset(int16_t sceneNum, int16_t flagType, int16_t flag) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) {
        return;
    }
    if (sApplyingNetworkFlag) {
        return;  // echo from a network-driven Flags_UnsetSwitch
    }
    if (IsTemporaryFlag(flagType, flag)) {
        return;  // not replicated on set, so no broadcast needed on unset
    }
    if (Anchor::Instance->roomState.syncItemsAndFlags == 0) {
        return;
    }

    uint8_t timeline = (uint8_t)gSaveContext.linkAge;
    WorldStateKey key{sceneNum, timeline, flagType, flag};
    size_t erased = sSetFlags.erase(key);
    if (erased == 0) {
        return;  // wasn't set; don't broadcast (idempotency)
    }

    nlohmann::json payload;
    payload["type"]     = Anchor::WORLD_FLAG_UNSET;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"]     = flag;
    PacketTimeline::SetTimelineField(payload);
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    Anchor::Instance->SendJsonToRemote(payload);

    SPDLOG_INFO("[WorldStateSync] Local flag unset sceneNum={} timeline={} flagType={} flag=0x{:02X} — broadcast",
                sceneNum, (int)timeline, flagType, flag);
}

void ReceiveFlagUnset(int16_t sceneNum, uint8_t timeline,
                      int16_t flagType, int16_t flag) {
    if (IsTemporaryFlag(flagType, flag)) {
        return;
    }
    WorldStateKey key{sceneNum, timeline, flagType, flag};
    sSetFlags.erase(key);

    if (gPlayState != nullptr &&
        (int16_t)gPlayState->sceneNum == sceneNum &&
        (uint8_t)gSaveContext.linkAge == timeline &&
        flagType == FLAG_SCENE_SWITCH) {
        sApplyingNetworkFlag = true;
        Flags_UnsetSwitch(gPlayState, flag);
        sApplyingNetworkFlag = false;
        SPDLOG_INFO("[WorldStateSync] Applied received unset locally sceneNum={} timeline={} flag=0x{:02X}",
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

        // Defensive — a peer running pre-fix code may include temp flags
        // in its snapshot. Drop those before they enter sSetFlags so the
        // joiner doesn't pick up the persistence bug from a stale peer.
        if (IsTemporaryFlag(flagType, flag)) continue;

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
