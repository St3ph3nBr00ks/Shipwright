#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/EnforcedCVarRegistry.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * UPDATE_ROOM_STATE — schema 3 (settings-sync v2)
 *
 * Wire format break from v1: the `state.cvars` object now uses the full
 * CVar macro expansion as keys (e.g. "gEnhancements.DamageMult") rather
 * than the v1 short-name convention ("damageMult"). Old v1 receivers
 * looking for short keys will see none and gracefully fall back to local
 * CVars (degradation, not crash). The set of enforced CVars is iterated
 * from Common/EnforcedCVarRegistry.cpp's `kEnforcedInts` / `kEnforcedFloats`.
 */

nlohmann::json Anchor::PrepRoomState() {
    nlohmann::json payload;
    payload["ownerClientId"] = ownClientId;
    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (isGlobalRoom) {
        // Global room uses hardcoded session-topology settings.
        payload["pvpMode"] = 0;
        payload["showLocationsMode"] = 0;
        payload["teleportMode"] = 0;
        payload["syncItemsAndFlags"] = 0;
    } else {
        payload["pvpMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.PvpMode"), 1);
        payload["showLocationsMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.ShowLocationsMode"), 1);
        payload["teleportMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.TeleportMode"), 1);
        payload["syncItemsAndFlags"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.SyncItemsAndFlags"), 1);
    }

    // Enforced CVars block. Iterate the registry; emit each CVar's
    // local value (or hardcoded default for the global-room path).
    // Senders other than the original owner have their payload
    // ignored by the relay (see findOrCreateRoom at server.go:287);
    // the owner's snapshot is authoritative. The cvars block is
    // emitted on BOTH branches (global + non-global) so peers
    // transitioning from a non-global room overwrite any stale values
    // rather than relying on the receiver's `.contains()` guard which
    // would leave the prior owner's settings in place.
    nlohmann::json& cvars = payload["cvars"] = nlohmann::json::object();
    for (const auto& e : AnchorCVarSync::kEnforcedInts) {
        cvars[e.cvarName] = isGlobalRoom
            ? e.defaultValue
            : CVarGetInteger(e.cvarName, e.defaultValue);
    }
    for (const auto& e : AnchorCVarSync::kEnforcedFloats) {
        cvars[e.cvarName] = isGlobalRoom
            ? e.defaultValue
            : CVarGetFloat(e.cvarName, e.defaultValue);
    }

    return payload;
}

void Anchor::SendPacket_UpdateRoomState() {
    nlohmann::json payload;
    payload["type"] = UPDATE_ROOM_STATE;
    payload["state"] = PrepRoomState();

    Network::SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UpdateRoomState(nlohmann::json payload) {
    if (!payload.contains("state")) {
        return;
    }

    roomState.ownerClientId = payload["state"]["ownerClientId"].get<uint32_t>();
    roomState.pvpMode = payload["state"]["pvpMode"].get<u8>();
    roomState.showLocationsMode = payload["state"]["showLocationsMode"].get<u8>();
    roomState.teleportMode = payload["state"]["teleportMode"].get<u8>();
    roomState.syncItemsAndFlags = payload["state"]["syncItemsAndFlags"].get<u8>();

    // Settings-sync v2 (UPDATE_ROOM_STATE schema 3). Iterate registry;
    // for each enforced CVar name look up the JSON value and write
    // into the map. Missing keys are dropped — the wrapper will fall
    // back to local for those reads, which is correct behaviour for
    // mid-rollout schema mismatches.
    if (payload["state"].contains("cvars") && payload["state"]["cvars"].is_object()) {
        const auto& cvars = payload["state"]["cvars"];
        roomState.enforcedInts.clear();
        roomState.enforcedFloats.clear();
        for (const auto& e : AnchorCVarSync::kEnforcedInts) {
            if (cvars.contains(e.cvarName)) {
                roomState.enforcedInts[e.cvarName] = cvars[e.cvarName].get<int32_t>();
            }
        }
        for (const auto& e : AnchorCVarSync::kEnforcedFloats) {
            if (cvars.contains(e.cvarName)) {
                roomState.enforcedFloats[e.cvarName] = cvars[e.cvarName].get<float>();
            }
        }

        // Diagnostic: confirms cvars block round-tripped successfully.
        // Logs a handful of representative values for quick sanity
        // checking. Useful for verifying owner-side admin-pane / Host
        // Settings widget edits propagate to peers, and for catching
        // wire-format drift after future schema bumps. Logged only
        // when the cvars sub-object is present.
        auto intOrZero = [&](const char* name) -> int32_t {
            auto it = roomState.enforcedInts.find(name);
            return it != roomState.enforcedInts.end() ? it->second : 0;
        };
        auto floatOrZero = [&](const char* name) -> float {
            auto it = roomState.enforcedFloats.find(name);
            return it != roomState.enforcedFloats.end() ? it->second : 0.0f;
        };
        SPDLOG_INFO("[SettingsSync] Received cvars from owner={} count={} (ints={} floats={}) infiniteAmmo={} damageMult={} speedModifier={:.2f}",
                    roomState.ownerClientId,
                    roomState.enforcedInts.size() + roomState.enforcedFloats.size(),
                    roomState.enforcedInts.size(),
                    roomState.enforcedFloats.size(),
                    intOrZero(CVAR_CHEAT("InfiniteAmmo")),
                    intOrZero(CVAR_ENHANCEMENT("DamageMult")),
                    floatOrZero(CVAR_CHEAT("SpeedModifier.Value")));

        // Re-fire any RegisterShipInitFunc listeners gated on enforced
        // CVar names. This propagates host-side changes to peer-local
        // hook registrations (COND_HOOK / COND_VB_SHOULD blocks in
        // Cheats/* and Difficulty/* and elsewhere). Without it, peers'
        // per-frame side effects — InfiniteAmmo refill, FreezeTime
        // clock-lock, HyperEnemies double-tick, BonkDamage bonk
        // handler — never engage on wire updates even though the
        // wrapper read returns host's value.
        AnchorCVarSync::OnCvarsReceivedDispatch();
    }
}
