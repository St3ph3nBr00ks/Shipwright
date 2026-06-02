/*
 * NAV_TEST_DIRECTIVE — Navigation Test Harness coordination packet.
 *
 * Two directives:
 *   "RUN"      P1 → P2. P2 teleports its Link to the spawn point and
 *              enables AI Player Follower mode targeting P1. P2 also enters
 *              combat-disabled mode (via local NavTest.CombatDisabled
 *              CVar) so AI Player Follower's combat tier doesn't fire.
 *   "REACHED"  P2 → P1. Sent when P2's AI Player Follower mode reaches the
 *              test target (P1's position) within 3D 60u. Carries the
 *              elapsed ms so P1 records it in the run results.
 *
 * Plan: Claude/Plans/ai_nav_test_harness_plan.md §4.2.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/AINavTest.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

void Anchor::SendPacket_NavTestDirective(const std::string& directive,
                                          const Vec3f& spawnPos,
                                          int16_t spawnSceneNum,
                                          int8_t  spawnRoomNum,
                                          int     runIndex,
                                          int     reachedMs) {
    if (!isConnected) {
        SPDLOG_WARN("[NavTestDirective] send skipped: not connected — P2 "
                    "will NOT receive the RUN directive (single-client "
                    "test). If running multi-client, check Anchor connection.");
        return;
    }

    nlohmann::json payload;
    payload["type"]            = NAV_TEST_DIRECTIVE;
    payload["directive"]       = directive;
    payload["senderClientId"]  = ownClientId;
    payload["spawnPos"]        = { spawnPos.x, spawnPos.y, spawnPos.z };
    payload["spawnSceneNum"]   = (int)spawnSceneNum;
    payload["spawnRoomNum"]    = (int)spawnRoomNum;
    payload["runIndex"]        = runIndex;
    payload["reachedMs"]       = reachedMs;

    // NOTE: deliberately NOT setting "targetClientId" — the Anchor relay
    // treats that field as a unicast destination, so any value (including
    // ownClientId) would route ONLY to that client, not broadcast. RUN
    // needs to reach all peers (P2 + any future N-th client) so they
    // can teleport + enable AI Player Follower mode. REACHED needs to reach
    // P1 (the test conductor), which works fine because P1 is just a
    // peer in the room and gets the broadcast.
    // Log 246 bug: previously set targetClientId = ownClientId thinking
    // it identified the sender. Result: P1 unicast the packet to itself,
    // P2 never received it. User had to manually toggle the AI Player Follower
    // menu checkbox.

    SPDLOG_INFO("[NavTestDirective] Sending directive='{}' runIndex={} "
                "spawnPos=({:.0f},{:.0f},{:.0f}) reachedMs={}",
                directive, runIndex,
                spawnPos.x, spawnPos.y, spawnPos.z, reachedMs);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_NavTestDirective(nlohmann::json payload) {
    const uint32_t senderClientId = payload.value("senderClientId", (uint32_t)0);

    // Diagnostic — every packet received logs entry so we can confirm
    // P2 received the RUN directive (log 245 user report: P2's AI
    // Follower mode didn't engage; without this log it was impossible
    // to tell whether the packet arrived).
    SPDLOG_INFO("[NavTestDirective] RECEIVED packet from senderClientId={} "
                "(my ownClientId={}); directive={}",
                senderClientId, ownClientId,
                payload.value("directive", std::string("(missing)")));

    // Ignore self-echoes.
    if (senderClientId == ownClientId) {
        SPDLOG_INFO("[NavTestDirective] self-echo ignored");
        return;
    }

    const std::string directive = payload.value("directive", std::string("RUN"));

    if (directive == "RUN") {
        // P2 (or any peer) receives: teleport our Link to the spawn
        // point + enable AI Player Follower mode targeting the sender + flip
        // combat-disabled CVar.
        if (gPlayState == nullptr) {
            SPDLOG_WARN("[NavTestDirective] RUN ignored: gPlayState null");
            return;
        }
        if (!payload.contains("spawnPos") || !payload["spawnPos"].is_array() ||
            payload["spawnPos"].size() != 3) {
            SPDLOG_WARN("[NavTestDirective] RUN ignored: malformed spawnPos");
            return;
        }
        const Vec3f spawnPos = {
            payload["spawnPos"][0].get<float>(),
            payload["spawnPos"][1].get<float>(),
            payload["spawnPos"][2].get<float>(),
        };
        const int16_t spawnSceneNum =
            (int16_t)payload.value("spawnSceneNum", (int)-1);

        // Scene mismatch: ignore. The test conductor should ensure
        // both clients are in the same scene before running.
        if (gPlayState->sceneNum != spawnSceneNum) {
            SPDLOG_WARN("[NavTestDirective] RUN ignored: scene mismatch "
                        "(our={} sent={})",
                        (int)gPlayState->sceneNum, (int)spawnSceneNum);
            return;
        }

        Player* player = GET_PLAYER(gPlayState);
        if (player == nullptr) {
            SPDLOG_WARN("[NavTestDirective] RUN ignored: GET_PLAYER null");
            return;
        }

        // Teleport our Link to the spawn point.
        player->actor.world.pos = spawnPos;
        player->actor.velocity.x = 0.0f;
        player->actor.velocity.y = 0.0f;
        player->actor.velocity.z = 0.0f;

        // Enable harness flags + combat-disabled. AI Player Follower mode is
        // NOT a CVar — it's runtime state on the Anchor instance
        // (Menu.cpp toggles it via SetFollowerActive). Call directly
        // (log 244 fix 2026-05-18: previously this packet set a CVar
        // that didn't exist and AI Player Follower mode never engaged).
        CVarSetInteger(CVAR_ENHANCEMENT("AI.NavTest.Enabled"), 1);
        CVarSetInteger(CVAR_ENHANCEMENT("AI.NavTest.CombatDisabled"), 1);

        // Enable every "Nav Data Usage" feature locally so the Player
        // AI Player Follower's nav substrate consumer runs. Without this,
        // default-off CVars on P2 mean the AI Player Follower falls back to
        // legacy direct-yaw pursuit even after SetFollowerActive(true).
        AINavTest::EnableAllNavDataUsageFeatures();

        Anchor::Instance->SetFollowerActive(true);

        // Latch the P2 test-mode flag so the AI Player Follower's reach
        // detector fires when this client closes within 60u of the
        // sender (P1). Auto-clears after 120s.
        AINavTest::NotifyP2TestStarted();

        SPDLOG_INFO("[NavTestDirective] RUN received from client {} — "
                    "teleported to spawn point ({:.0f},{:.0f},{:.0f}) + "
                    "enabled AI Player Follower mode + combat-disabled",
                    senderClientId, spawnPos.x, spawnPos.y, spawnPos.z);
        return;
    }

    if (directive == "REACHED") {
        // P1 receives this from P2 when P2's AI Player Follower mode reaches
        // the test target. P2's packet payload's reachedMs is a
        // sentinel (0) — P1 computes its own elapsed time relative to
        // the current active run's start frame. Network latency adds
        // a small constant (~tens of ms) which is fine for a
        // diagnostic. P1 records via ReportAIFollowerReach which
        // updates the active run's aiFollowerMs in sRunHistory.
        if (!AINavTest::IsRunActive()) {
            SPDLOG_WARN("[NavTestDirective] REACHED ignored: no run active "
                        "on this client (sender expected us to be P1)");
            return;
        }
        // ReportAIFollowerReach takes the ms value to store. Pass a
        // sentinel of 0 to signal "use P1's own clock"; the function
        // checks for 0 and computes elapsed ms locally.
        AINavTest::ReportAIFollowerReach(/*reportedMsFromP2=*/0);
        return;
    }

    SPDLOG_WARN("[NavTestDirective] Unknown directive='{}'", directive);
}
