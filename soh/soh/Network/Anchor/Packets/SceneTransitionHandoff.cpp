#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * SCENE_TRANSITION_HANDOFF
 *
 * Leader→follower packet that lets the follower follow the leader through any
 * scene transition (regular door, grotto, crawlspace entry, boss-room door,
 * cave entry, etc.). See #169 Phase C for design rationale and why this
 * supersedes the G13 boss-scene deactivate path.
 *
 * Leader-side send (HookHandlers.cpp OnGameFrameUpdate):
 *   Edge-detect `gPlayState->transitionTrigger` rising from TRANS_TRIGGER_OFF
 *   to TRANS_TRIGGER_START. At that moment the leader has already walked into
 *   the transition's trigger volume and OoT has set
 *   `gPlayState->nextEntranceIndex` to the destination. Player position is
 *   still in the source scene (unload hasn't happened yet), so we capture
 *   world.pos + shape.rot.y right there. One packet per transition, sent to
 *   every other client in the room.
 *
 * Receive-side (below):
 *   Only followers in an active follower session react. Non-followers ignore.
 *   The packet is stashed on the Anchor instance as `pendingTransitionXxx`
 *   and re-evaluated each frame by the follower state machine:
 *     - if our current sceneNum matches `pendingTransitionFromScene` AND we
 *       are within kHandoffProximity (60 units XZ) of `pendingTransitionPos`,
 *       we set `gPlayState->nextEntranceIndex` + `transitionTrigger` and
 *       clear the pending state;
 *     - if our sceneNum changes to something other than fromScene, the
 *       handoff is stale (we made the transition ourselves or skipped it) —
 *       clear;
 *     - timeout after kHandoffTimeoutFrames (~12 s). On timeout the follower
 *       falls through to the G11 manual-walkthrough behaviour.
 *
 * NOTE: This packet bypasses the door logic (key check, animation) — we
 * trigger the scene load directly. That means the follower crosses locked
 * doors without holding a key. Considered acceptable because the leader has
 * already demonstrated access; the follower is a semi-automated companion,
 * not a separate save-state. If randomizer or PvP cases surface a concern,
 * gate on `roomState.syncItemsAndFlags` or similar.
 */

void Anchor::SendPacket_SceneTransitionHandoff(s16 fromSceneNum, s16 toEntranceIndex,
                                                Vec3f triggerPos, s16 triggerRotY) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"]            = SCENE_TRANSITION_HANDOFF;
    payload["fromSceneNum"]    = fromSceneNum;
    payload["toEntranceIndex"] = toEntranceIndex;
    payload["triggerPos"]      = triggerPos;
    payload["triggerRotY"]     = triggerRotY;

    SPDLOG_INFO("[SceneTransitionHandoff] Sending from scene 0x{:02X} to entrance 0x{:04X} "
                "triggerPos=({:.0f},{:.0f},{:.0f})",
                (int)fromSceneNum, (int)(u16)toEntranceIndex,
                triggerPos.x, triggerPos.y, triggerPos.z);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_SceneTransitionHandoff(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    // Only followers react. Non-follower clients don't auto-traverse scenes.
    if (!IsFollowerActive()) {
        SPDLOG_DEBUG("[SceneTransitionHandoff] Ignored — follower mode not active on this client");
        return;
    }

    s16   fromSceneNum = payload.value("fromSceneNum", (s16)0);
    s16   toEntrance   = payload.value("toEntranceIndex", (s16)0);
    Vec3f triggerPos   = payload.value("triggerPos", Vec3f{ 0.0f, 0.0f, 0.0f });
    s16   triggerRotY  = payload.value("triggerRotY", (s16)0);

    // Store as pending. If a previous handoff was still outstanding we
    // overwrite — the newer packet is what the leader actually did last.
    pendingTransitionFromScene     = fromSceneNum;
    pendingTransitionEntrance      = toEntrance;
    pendingTransitionPos           = triggerPos;
    pendingTransitionRotY          = triggerRotY;
    pendingTransitionTimeoutFrames = 720; // ~12s at 60fps; VirtualBox 20fps ≈ 36s
    hasPendingTransition           = true;

    SPDLOG_INFO("[SceneTransitionHandoff] Received from scene 0x{:02X} to entrance 0x{:04X} "
                "triggerPos=({:.0f},{:.0f},{:.0f}) — pending until follower reaches trigger",
                (int)fromSceneNum, (int)(u16)toEntrance,
                triggerPos.x, triggerPos.y, triggerPos.z);
}
