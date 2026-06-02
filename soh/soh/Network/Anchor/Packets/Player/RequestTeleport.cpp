#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneMultiplayerConfig.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

/**
 * REQUEST_TELEPORT
 *
 * Because we don't have all the necessary information to directly teleport to a player, we emit a request,
 * in which they will respond with a TELEPORT_TO packet, with the necessary information.
 */

void Anchor::SendPacket_RequestTeleport(uint32_t clientId) {
    if (!CanTeleportTo(clientId)) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = REQUEST_TELEPORT;
    payload["targetClientId"] = clientId;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RequestTeleport(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    SendPacket_TeleportTo(clientId);
}

// Reusable function to check if teleporting to a client is allowed
bool Anchor::CanTeleportTo(uint32_t clientId) {
    // Teleporting is disabled
    if (roomState.teleportMode == 0) {
        return false;
    }

    // You're not loaded into a save
    if (!IsSaveLoaded()) {
        return false;
    }

    // The client doesn't exist
    if (clients.find(clientId) == clients.end()) {
        return false;
    }

    AnchorClient& client = clients[clientId];

    // The client is yourself
    if (client.self) {
        return false;
    }

    // The client isn't online or loaded into a save
    if (!client.online || !client.isSaveLoaded) {
        return false;
    }

    // Teleporting to team only, but the client is not on your team
    std::string ownTeamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    if (roomState.teleportMode == 1 && client.teamId != ownTeamId) {
        return false;
    }

    // No scene loaded — sentinel guard, not part of the §5 override table.
    if (client.sceneNum == SCENE_ID_MAX) {
        return false;
    }

    // §5 SceneMultiplayerOverrides — refuse teleport into scenes flagged
    // disableTeleportTo. Retires the previous hard-coded "problematic
    // scenes" list (Castle Town markets, ToT exterior, back alleys,
    // grottos). See Common/SceneMultiplayerConfig.cpp seed map.
    if (SceneMultiplayerConfig::GetSceneOverrides(client.sceneNum, /*roomNum=*/-1).disableTeleportTo) {
        return false;
    }

    // Pillar B Phase 5 — the previous "child can't teleport to Outside
    // Ganon's Castle / adult can't teleport to Hyrule Castle" hard-blocks
    // are now superseded by Anchor_SwitchAgeAndTeleport. Cross-timeline
    // teleport into those scenes is allowed; the §5 forceCastleExitPath
    // override (seeded for both castle scenes in SceneMultiplayerConfig)
    // reroutes the entrance to ENTR_CASTLE_GROUNDS_SOUTH_EXIT to avoid
    // the age-mismatched fall-through-floor risk that motivated the
    // original guards. Refusal still happens at the AnchorSwitchAge
    // TimeTravel-disabled gate when the user has opted out.

    return true;
}
