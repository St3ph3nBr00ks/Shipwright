#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * UPDATE_ROOM_STATE
 */

nlohmann::json Anchor::PrepRoomState() {
    nlohmann::json payload;
    payload["ownerClientId"] = ownClientId;
    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (isGlobalRoom) {
        // Global room uses hardcoded settings
        payload["pvpMode"] = 0;
        payload["showLocationsMode"] = 0;
        payload["teleportMode"] = 0;
        payload["syncItemsAndFlags"] = 0;

        // Settings-sync v1 (UPDATE_ROOM_STATE schema 2) — global-room
        // path: hardcoded defaults. Sending the block explicitly (rather
        // than omitting it) ensures peers transitioning from a non-global
        // room overwrite any stale cvars values; relying on the
        // receiver's `.contains()` guard would leave the prior owner's
        // settings in place.
        nlohmann::json& cvars = payload["cvars"] = nlohmann::json::object();
        cvars["timeTravel"]           = 1;     // TIME_TRAVEL_OOT
        cvars["damageMult"]           = 0;
        cvars["fallDamageMult"]       = 0;
        cvars["voidDamageMult"]       = 0;
        cvars["freezeTime"]           = 0;
        cvars["randomizedEnemies"]    = 0;
        cvars["newDrops"]             = 0;
        cvars["hyperEnemies"]         = 0;
        cvars["noRestrictItems"]      = 0;
        cvars["bonkDamageMult"]       = 0;
        cvars["infiniteAmmo"]         = 0;
        cvars["climbEverything"]      = 0;
        cvars["hookshotEverything"]   = 0;
        cvars["speedModifierValue"]   = 1.0f;
        cvars["superTunic"]           = 0;
        cvars["timelessEquipment"]    = 0;
        cvars["bombTimerMultiplier"]  = 1.0f;
        cvars["shieldTwoHanded"]      = 0;
        cvars["removeExplosiveLimit"] = 0;
        cvars["fireproofDekuShield"]  = 0;
    } else {
        payload["pvpMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.PvpMode"), 1);
        payload["showLocationsMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.ShowLocationsMode"), 1);
        payload["teleportMode"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.TeleportMode"), 1);
        payload["syncItemsAndFlags"] = CVarGetInteger(CVAR_REMOTE_ANCHOR("RoomSettings.SyncItemsAndFlags"), 1);

        // Settings-sync v1 (UPDATE_ROOM_STATE schema 2) — snapshot local
        // gameplay CVars into the wire payload. Senders other than the
        // original owner have their payload ignored by the relay (see
        // findOrCreateRoom at server.go:287); the owner's snapshot is
        // authoritative.
        nlohmann::json& cvars = payload["cvars"] = nlohmann::json::object();
        cvars["timeTravel"]           = CVarGetInteger(CVAR_ENHANCEMENT("TimeTravel"), 1 /* TIME_TRAVEL_OOT */);
        cvars["damageMult"]           = CVarGetInteger(CVAR_ENHANCEMENT("DamageMult"), 0);
        cvars["fallDamageMult"]       = CVarGetInteger(CVAR_ENHANCEMENT("FallDamageMult"), 0);
        cvars["voidDamageMult"]       = CVarGetInteger(CVAR_ENHANCEMENT("VoidDamageMult"), 0);
        cvars["freezeTime"]           = CVarGetInteger(CVAR_CHEAT("FreezeTime"), 0);
        cvars["randomizedEnemies"]    = CVarGetInteger(CVAR_ENHANCEMENT("RandomizedEnemies"), 0);
        cvars["newDrops"]             = CVarGetInteger(CVAR_ENHANCEMENT("NewDrops"), 0);
        cvars["hyperEnemies"]         = CVarGetInteger(CVAR_ENHANCEMENT("HyperEnemies"), 0);
        cvars["noRestrictItems"]      = CVarGetInteger(CVAR_CHEAT("NoRestrictItems"), 0);
        cvars["bonkDamageMult"]       = CVarGetInteger(CVAR_ENHANCEMENT("BonkDamageMult"), 0);
        cvars["infiniteAmmo"]         = CVarGetInteger(CVAR_CHEAT("InfiniteAmmo"), 0);
        cvars["climbEverything"]      = CVarGetInteger(CVAR_CHEAT("ClimbEverything"), 0);
        cvars["hookshotEverything"]   = CVarGetInteger(CVAR_CHEAT("HookshotEverything"), 0);
        cvars["speedModifierValue"]   = CVarGetFloat(CVAR_CHEAT("SpeedModifier.Value"), 1.0f);
        cvars["superTunic"]           = CVarGetInteger(CVAR_CHEAT("SuperTunic"), 0);
        cvars["timelessEquipment"]    = CVarGetInteger(CVAR_CHEAT("TimelessEquipment"), 0);
        cvars["bombTimerMultiplier"]  = CVarGetFloat(CVAR_CHEAT("BombTimerMultiplier"), 1.0f);
        cvars["shieldTwoHanded"]      = CVarGetInteger(CVAR_CHEAT("ShieldTwoHanded"), 0);
        cvars["removeExplosiveLimit"] = CVarGetInteger(CVAR_ENHANCEMENT("RemoveExplosiveLimit"), 0);
        cvars["fireproofDekuShield"]  = CVarGetInteger(CVAR_CHEAT("FireproofDekuShield"), 0);
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

    // Settings-sync v1 (schema 2). Pre-Pillar-F senders + legacy v1
    // senders won't carry the "cvars" object — fall back to existing
    // values (zero-initialised at construction; subsequent updates
    // from a v2 sender refresh them). Each field is also individually
    // .contains()-guarded so a partially-populated payload from a
    // future schema bump still applies what it can.
    if (payload["state"].contains("cvars") && payload["state"]["cvars"].is_object()) {
        const auto& cvars = payload["state"]["cvars"];
        if (cvars.contains("timeTravel"))           roomState.cvars.timeTravel           = cvars["timeTravel"].get<s32>();
        if (cvars.contains("damageMult"))           roomState.cvars.damageMult           = cvars["damageMult"].get<s32>();
        if (cvars.contains("fallDamageMult"))       roomState.cvars.fallDamageMult       = cvars["fallDamageMult"].get<s32>();
        if (cvars.contains("voidDamageMult"))       roomState.cvars.voidDamageMult       = cvars["voidDamageMult"].get<s32>();
        if (cvars.contains("freezeTime"))           roomState.cvars.freezeTime           = cvars["freezeTime"].get<u8>();
        if (cvars.contains("randomizedEnemies"))    roomState.cvars.randomizedEnemies    = cvars["randomizedEnemies"].get<u8>();
        if (cvars.contains("newDrops"))             roomState.cvars.newDrops             = cvars["newDrops"].get<u8>();
        if (cvars.contains("hyperEnemies"))         roomState.cvars.hyperEnemies         = cvars["hyperEnemies"].get<u8>();
        if (cvars.contains("noRestrictItems"))      roomState.cvars.noRestrictItems      = cvars["noRestrictItems"].get<u8>();
        if (cvars.contains("bonkDamageMult"))       roomState.cvars.bonkDamageMult       = cvars["bonkDamageMult"].get<s32>();
        if (cvars.contains("infiniteAmmo"))         roomState.cvars.infiniteAmmo         = cvars["infiniteAmmo"].get<u8>();
        if (cvars.contains("climbEverything"))      roomState.cvars.climbEverything      = cvars["climbEverything"].get<u8>();
        if (cvars.contains("hookshotEverything"))   roomState.cvars.hookshotEverything   = cvars["hookshotEverything"].get<u8>();
        if (cvars.contains("speedModifierValue"))   roomState.cvars.speedModifierValue   = cvars["speedModifierValue"].get<f32>();
        if (cvars.contains("superTunic"))           roomState.cvars.superTunic           = cvars["superTunic"].get<u8>();
        if (cvars.contains("timelessEquipment"))    roomState.cvars.timelessEquipment    = cvars["timelessEquipment"].get<u8>();
        if (cvars.contains("bombTimerMultiplier"))  roomState.cvars.bombTimerMultiplier  = cvars["bombTimerMultiplier"].get<f32>();
        if (cvars.contains("shieldTwoHanded"))      roomState.cvars.shieldTwoHanded      = cvars["shieldTwoHanded"].get<u8>();
        if (cvars.contains("removeExplosiveLimit")) roomState.cvars.removeExplosiveLimit = cvars["removeExplosiveLimit"].get<u8>();
        if (cvars.contains("fireproofDekuShield"))  roomState.cvars.fireproofDekuShield  = cvars["fireproofDekuShield"].get<u8>();

        // Diagnostic: confirms cvars block round-tripped successfully. Useful
        // for verifying owner-side admin-pane edits propagate to peers, and
        // for catching wire-format drift after future schema bumps. Logged
        // only when the cvars sub-object is present (v1 senders skipped).
        SPDLOG_INFO("[SettingsSync] Received cvars from owner={} infiniteAmmo={} damageMult={} timeTravel={} speedModifier={:.2f}",
                    roomState.ownerClientId, (int)roomState.cvars.infiniteAmmo, roomState.cvars.damageMult,
                    roomState.cvars.timeTravel, roomState.cvars.speedModifierValue);
    }
}
