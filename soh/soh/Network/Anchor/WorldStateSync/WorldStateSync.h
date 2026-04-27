#pragma once

// Pillar C v1 — globally-replicated WorldStateSync.
//
// Architecture commitment (master design doc §4.C): every client maintains
// a full local copy of persistent world flags for every scene regardless
// of which scene is currently loaded. Updates flow as additive events
// (WORLD_FLAG_SET) and are commutative + idempotent — order doesn't
// matter, late arrivals are safe.
//
// First customer in v1: FLAG_SCENE_SWITCH events fired by Flags_SetSwitch
// (z_actor.c:675). Covers Mad Scrub eye target, generic switch puzzles,
// and any actor that calls into the global switch-flag setter — all in
// one hook registration.
//
// Cross-timeline scope (Pillar B): the WorldState key includes the
// sender's linkAge. A child-Kokiri eye target and an adult-Kokiri eye
// target are distinct entries.
//
// Late-join snapshot: WORLD_STATE_REQUEST (sent from OnConnected) →
// peer replies WORLD_STATE_SNAPSHOT carrying its full mSetFlags set.
// Joiner merges; idempotent.
//
// Plan: Claude/Plans/pillar_c_worldstatesync.md.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <cstdint>
#include <cstddef>

extern "C" {
#include "z64.h"
}

namespace WorldStateSync {

// Identity of a single replicated flag in the world.
//
// (sceneNum, timeline) keys match the design doc Pillar C "WorldState
// key". flagType + flag together identify which bit within that scene.
// v1 only populates flagType = FLAG_SCENE_SWITCH; the field is reserved
// for future flag families (FLAG_SCENE_TREASURE, FLAG_SCENE_CLEAR, ...).
struct WorldStateKey {
    int16_t sceneNum;
    uint8_t timeline;
    int16_t flagType;
    int16_t flag;

    bool operator==(const WorldStateKey& o) const noexcept {
        return sceneNum == o.sceneNum && timeline == o.timeline &&
               flagType == o.flagType && flag == o.flag;
    }
};

struct WorldStateKeyHash {
    size_t operator()(const WorldStateKey& k) const noexcept {
        // Mix all four fields. sceneNum is u8 in OoT today but the type is
        // s16 so we don't lose room for future expansion; timeline is one
        // bit; flagType has only ~12 enum values; flag is bounded by the
        // scene's actor-flag-bit count (typically < 64). A simple multiply-
        // and-XOR over the fields gives near-uniform distribution under
        // realistic loads.
        size_t h = (size_t)(uint16_t)k.sceneNum;
        h = h * 131u + k.timeline;
        h = h * 131u + (size_t)(uint16_t)k.flagType;
        h = h * 131u + (size_t)(uint16_t)k.flag;
        return h;
    }
};

// Records a local flag-set event. Broadcasts WORLD_FLAG_SET to the team
// (gated on syncItemsAndFlags + team scope by the sender side) and adds
// the entry to the local set so subsequent snapshot requests carry it.
//
// Idempotent — calling twice for the same (sceneNum, linkAge, flagType,
// flag) emits one packet and stays at one set entry.
//
// Echo guard: when a remote WORLD_FLAG_SET arrives and we apply it
// locally via Flags_SetSwitch, the resulting OnSceneFlagSet hook re-
// enters this function. The IsApplyingNetworkFlag() check below
// suppresses re-broadcast.
void OnLocalFlagSet(int16_t sceneNum, int16_t flagType, int16_t flag);

// True while a network-driven flag set is being applied to local game
// state — the hook handler in HookHandlers.cpp consults this to skip
// the broadcast that would otherwise echo back to the network.
bool IsApplyingNetworkFlag();

// Walk the local mSetFlags and apply every entry whose (sceneNum,
// timeline) matches the supplied scene. Called from
// OnSceneSpawnActors so a freshly-loaded scene picks up state that
// peers have already established.
void ApplyKnownFlagsForScene(int16_t sceneNum, uint8_t timeline);

// Asks every team-mate for their current WorldState snapshot. Called
// from Anchor::OnConnected after the initial handshake. Each team-mate
// replies with WORLD_STATE_SNAPSHOT.
void SendRequestWorldState();

// Snapshot payload assembly — used by the peer responding to a
// WORLD_STATE_REQUEST.
nlohmann::json BuildSnapshotPayload();

// Snapshot apply — merge entries from a received WORLD_STATE_SNAPSHOT
// into the local mSetFlags. If the current scene matches any newly-
// arrived entry, apply it to game state immediately.
void ApplySnapshotPayload(const nlohmann::json& payload);

// Direct mSetFlags mutation used by the WORLD_FLAG_SET receive handler.
// Adds the entry (idempotent) and applies to local game state if the
// supplied (sceneNum, timeline) matches the currently-loaded scene.
void ReceiveFlagSet(int16_t sceneNum, uint8_t timeline,
                    int16_t flagType, int16_t flag);

// Lifecycle — clear all local state. Called from Anchor::Disable.
void Reset();

}  // namespace WorldStateSync
