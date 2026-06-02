#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyHostBookkeeping.h"

#include <libultraship/log/luslog.h>
#include <nlohmann/json.hpp>

// Test 1.5 exit-gated fix — broadcast on host when a scene becomes
// unoccupied (last player + host all transitioned out). Receivers
// clear scene-scoped buffered state for that scene so the next entrant
// sees a fresh actor pool (vanilla respawn parity).
//
// Architecture rationale: the previous entry-gated vacancy check only
// cleared host-side state on a returning client's UpdateClientState.
// It missed the per-client `pendingKillNetIds` set, so kills that
// arrived during the brief empty window stayed buffered and fired
// when that client re-entered. Test 1.5 regression on log 165:
// P1 killed Dekubaba, both clients went to Kokiri Forest, P2 returned
// to Inside Deku Tree first → P2's pendingKillNetIds[2147505654] still
// held the kill → OnActorSpawn killed it locally even though host's
// mSceneDeaths had been cleared.
//
// Exit-gated semantic: the moment of becoming-empty is broadcast
// exactly once. Every receiver clears in lockstep. No race between
// concurrent re-entrants.

void Anchor::SendPacket_SceneDeathsCleared(int16_t sceneNum, uint8_t timeline) {
    if (!::SceneAuthority::IsEffectiveHost()) {
        // Only host detects vacancy + broadcasts. Belt-and-suspenders —
        // any non-host call is a no-op rather than crash-on-bad-call.
        return;
    }

    nlohmann::json payload;
    payload["type"]     = SCENE_DEATHS_CLEARED;
    payload["sceneNum"] = (int)sceneNum;
    payload["timeline"] = (int)timeline;

    SPDLOG_INFO("[SceneDeathsCleared] Broadcasting sceneNum={} timeline={}",
                (int)sceneNum, (int)timeline);

    // Apply locally first — host clears its own mSceneDeaths /
    // mDefeatBroadcasts / mPendingKills, then sends to peers.
    auto& bk = EnemyStateSync::HostBookkeeping::Instance();
    bk.ClearScene(sceneNum);
    bk.ClearAllDefeatBroadcasts();
    bk.ClearPendingKillsForScene(sceneNum, timeline);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SceneDeathsCleared(nlohmann::json payload) {
    int16_t sceneNum = (int16_t)payload.value("sceneNum", (int)-1);
    uint8_t timeline = (uint8_t)payload.value("timeline", (int)0xFF);

    if (sceneNum < 0) {
        SPDLOG_WARN("[SceneDeathsCleared] Invalid sceneNum in payload — dropping");
        return;
    }

    SPDLOG_INFO("[SceneDeathsCleared] Received sceneNum={} timeline={} — clearing local pendingKills",
                (int)sceneNum, (int)timeline);

    auto& bk = EnemyStateSync::HostBookkeeping::Instance();
    // mSceneDeaths and mDefeatBroadcasts are host-only; non-host's calls
    // operate on its own (empty for client) maps, harmless.
    bk.ClearScene(sceneNum);
    bk.ClearAllDefeatBroadcasts();
    // The actually-meaningful action on non-host: drop locally-buffered
    // pending kills for the now-empty scene. Without this, a kill that
    // arrived between host's clear and the client's re-entry still fires
    // on the freshly-spawned actor (Test 1.5 regression).
    bk.ClearPendingKillsForScene(sceneNum, timeline);
}
