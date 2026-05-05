#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * BOSS_EXIT_TEAM_WARP
 *
 * Team-routed scene transition for synced boss-room exits. Fires from the
 * existing transitionTrigger OFF→START edge in OnGameFrameUpdate when the
 * (sourceSceneNum, destEntranceIndex) pair is in the IsSyncedBossExit
 * allowlist (today: SCENE_DEKU_TREE_BOSS → Kokiri Forest blue-warp paths).
 *
 * Receivers in the same team + same boss room apply the same transition
 * silently (no fade VFX, no Song-of-Time effect) so the post-fight exit
 * stays grouped: P1 walks into the warp → P2 instantly transitions to the
 * same destination, sees the same dying-tree cutscene if armed, and lands
 * in Kokiri Forest at the same beat.
 *
 * Idempotency: if the receiver's local transitionTrigger is already firing,
 * the receive site is a no-op — covers the race when both teammates touch
 * the warp the same frame.
 *
 * NOT a generic "follow leader through any door" — that's
 * SCENE_TRANSITION_HANDOFF (used by the AI Follower). This packet pulls
 * teammates through ONE specific transition class (synced boss exits)
 * because keeping the team grouped at the dungeon-clear moment is part of
 * the demo's emotional payoff. Other transitions stay per-player.
 */

void Anchor::SendPacket_BossExitTeamWarp(s16 sourceSceneNum, s16 entranceIndex,
                                          u16 nextCutsceneIndex, s8 transitionType,
                                          s8 nextTransitionType) {
    nlohmann::json payload;
    payload["type"]               = BOSS_EXIT_TEAM_WARP;
    payload["targetTeamId"]       = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["sourceSceneNum"]     = (int)sourceSceneNum;
    payload["entranceIndex"]      = (int)entranceIndex;
    payload["nextCutsceneIndex"]  = (int)nextCutsceneIndex;
    payload["transitionType"]     = (int)transitionType;
    payload["nextTransitionType"] = (int)nextTransitionType;

    SPDLOG_INFO("[BossExitTeamWarp] Sending sourceSceneNum={} entrance={} nextCs=0x{:04X} "
                "transType={} nextTransType={}",
                (int)sourceSceneNum, (int)entranceIndex,
                (unsigned)nextCutsceneIndex, (int)transitionType, (int)nextTransitionType);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_BossExitTeamWarp(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    s16 sourceSceneNum    = (s16)payload.value("sourceSceneNum", -1);
    s16 entranceIndex     = (s16)payload.value("entranceIndex", -1);
    u16 nextCutsceneIndex = (u16)payload.value("nextCutsceneIndex", 0);
    s8  transitionType    = (s8)payload.value("transitionType", 0);
    s8  nextTransitionType = (s8)payload.value("nextTransitionType", 0);

    // Receiver must be in the same boss scene as the trigger. A teammate
    // who's already left the boss room (e.g. fell in a void to reset the
    // fight before the kill) shouldn't be yanked back through the warp.
    if (gPlayState->sceneNum != sourceSceneNum) {
        SPDLOG_INFO("[BossExitTeamWarp] Drop — local scene {} != source scene {}",
                    (int)gPlayState->sceneNum, (int)sourceSceneNum);
        return;
    }

    // Idempotency: if our local transitionTrigger already fired this frame
    // (we touched the warp at the same time), don't double-arm. Also covers
    // re-entry of an in-flight transition for any reason.
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        SPDLOG_INFO("[BossExitTeamWarp] Drop — local transitionTrigger already firing");
        return;
    }

    SPDLOG_INFO("[BossExitTeamWarp] Apply: entrance={} nextCs=0x{:04X} transType={} nextTransType={}",
                (int)entranceIndex, (unsigned)nextCutsceneIndex,
                (int)transitionType, (int)nextTransitionType);

    // Mirror the local-trigger writes from z_door_warp1.c:565-567 + the
    // OoT engine's expected respawn-down pattern from TeleportTo.cpp's
    // same-timeline path. Setting nextEntranceIndex + transitionTrigger is
    // the canonical "transition immediately" sequence; nextCutsceneIndex
    // carries the post-warp cutscene (e.g. 0xFFF1 for Great Deku Tree
    // dying CS) so receivers see the same intro as the trigger.
    gPlayState->nextEntranceIndex   = entranceIndex;
    gPlayState->transitionTrigger    = TRANS_TRIGGER_START;
    gPlayState->transitionType       = transitionType;
    gSaveContext.nextCutsceneIndex   = nextCutsceneIndex;
    gSaveContext.nextTransitionType  = nextTransitionType;
}
