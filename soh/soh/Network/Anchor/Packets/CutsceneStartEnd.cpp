#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"

#include <libultraship/log/luslog.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
extern PlayState* gPlayState;
}

// #164 / cutscene_start_end_detector_spec.md — packet family that bridges
// host's cutscene state to peers in the same scene + timeline. Two
// independent control planes are watched and OR'd together:
//
//   savecontext path: gSaveContext.cutsceneIndex rises 0 → non-zero
//                     (Master Sword pull, Sage chambers, Owl scenes,
//                     scripted scene-layer cutscenes).
//   actor-internal:   gPlayState->csCtx.state rises IDLE → non-IDLE
//                     (Boss_Goma intro/defeat and the other 13 actor-
//                     internal CS users, which call func_80064520 /
//                     func_80064534 directly without writing
//                     cutsceneIndex).
//
// Receiver synchronously writes the matching field. The save-context
// branch lets the engine's func_80068ECC auto-promote csCtx.state next
// frame; the actor-internal branch writes csCtx.state directly so
// Player_Update at z_player.c:12101-12113 auto-locks the player on the
// next tick (no explicit input-lock packet field needed). Per-(sceneNum,
// csKind, csKey) idempotency keeps spurious double-START / double-END
// out of the receiver state.
//
// Migration handoff is automatic: activeCutscenes is tracked on host AND
// non-host both, so a mid-cutscene host migration leaves the new
// effective host with the right active record. END detection on the
// new host fires correctly when csCtx.state returns to IDLE.

// ---------------------------------------------------------------------
// Active-record helpers (Anchor::activeCutscenes)
// ---------------------------------------------------------------------

bool Anchor::IsCutsceneActive(int16_t sceneNum, uint8_t timeline,
                              const std::string& csKind, int32_t csKey) const {
    for (const auto& rec : activeCutscenes) {
        if (rec.sceneNum == sceneNum && rec.timeline == timeline &&
            rec.csKind == csKind && rec.csKey == csKey) {
            return true;
        }
    }
    return false;
}

void Anchor::MarkCutsceneActive(int16_t sceneNum, uint8_t timeline,
                                const std::string& csKind, int32_t csKey) {
    if (IsCutsceneActive(sceneNum, timeline, csKind, csKey)) return;
    activeCutscenes.push_back({ sceneNum, timeline, csKind, csKey });
}

void Anchor::MarkCutsceneInactive(int16_t sceneNum, uint8_t timeline,
                                  const std::string& csKind, int32_t csKey) {
    for (auto it = activeCutscenes.begin(); it != activeCutscenes.end(); ++it) {
        if (it->sceneNum == sceneNum && it->timeline == timeline &&
            it->csKind == csKind && it->csKey == csKey) {
            activeCutscenes.erase(it);
            return;
        }
    }
}

// ---------------------------------------------------------------------
// Detector-time classifier
// ---------------------------------------------------------------------
//
// Returns the kind string written to the wire. v1 only classifies
// Boss_Goma in detail; the other 13 actor-internal CS users fall through
// to "actor_unknown" which still gets the synchronous csCtx.state write
// (input-lock benefit) on receivers.
namespace {

// First-Boss_Goma-only classifier. Sufficient for the demo scope —
// vanilla never spawns multiple Boss_Goma actors in one room, and the
// First Dungeon Demo (#167) doesn't introduce mods that would. If a
// future scenario does spawn two simultaneously, the second's intro/
// defeat cutscenes would mis-classify; classifier should then walk all
// matches and prefer the one currently in cutscene state.
const char* ClassifyBossGomaKind() {
    if (gPlayState == nullptr) return nullptr;
    Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head;
    while (a != nullptr) {
        if (a->id == ACTOR_BOSS_GOMA) {
            const s16 idx = BossGoma_GetStateIndex((BossGoma*)a);
            if (idx == 0x20) return "gohma_death";
            if (idx == 0x00) return "gohma_intro";
            return "gohma_other";
        }
        a = a->next;
    }
    return nullptr;
}

std::string ClassifyCutsceneKind(uint16_t currCsIndex, uint8_t /*currCsState*/) {
    // Save-context drives the index. If non-zero it's a save-context CS.
    if (currCsIndex != 0) return "savecontext";

    const char* boss = ClassifyBossGomaKind();
    if (boss != nullptr) return boss;

    return "actor_unknown";
}

int32_t DeriveCsKey(const std::string& csKind, uint16_t currCsIndex) {
    if (csKind == "savecontext") return (int32_t)currCsIndex;

    // Actor-internal: key on the originating actor's netId where we have
    // one. v1 only resolves Boss_Goma since it's the only kind we
    // classify positively.
    if (gPlayState != nullptr &&
        (csKind == "gohma_intro" || csKind == "gohma_death")) {
        Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head;
        while (a != nullptr) {
            if (a->id == ACTOR_BOSS_GOMA) {
                const EnemyNetId* ext =
                    ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                if (ext != nullptr) return (int32_t)ext->netId;
                break;
            }
            a = a->next;
        }
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------
// Edge detector (host-only emit; non-host run-through tracks
// activeCutscenes for migration robustness)
// ---------------------------------------------------------------------

void Anchor::ResetCutsceneDetectorState() {
    cutscenePrevCsIndex    = 0;
    cutscenePrevCsState    = CS_STATE_IDLE;
    cutscenePrevSaveLoaded = false;
}

void Anchor::DetectAndSendCutsceneEdges(uint16_t currCsIndex, uint8_t currCsState) {
    const bool nowLoaded = IsSaveLoaded();

    // Re-initialise on the rising edge of save-load so the first frame
    // after entering a save doesn't fire a phantom START from a stale
    // title-screen value.
    if (nowLoaded && !cutscenePrevSaveLoaded) {
        cutscenePrevCsIndex    = currCsIndex;
        cutscenePrevCsState    = currCsState;
        cutscenePrevSaveLoaded = true;
        return;
    }
    cutscenePrevSaveLoaded = nowLoaded;
    if (!nowLoaded || gPlayState == nullptr) {
        cutscenePrevCsIndex = currCsIndex;
        cutscenePrevCsState = currCsState;
        return;
    }

    const bool indexEdgeStart = (cutscenePrevCsIndex == 0) && (currCsIndex != 0);
    const bool stateEdgeStart =
        (cutscenePrevCsState == CS_STATE_IDLE) && (currCsState != CS_STATE_IDLE);
    const bool startEdge = indexEdgeStart || stateEdgeStart;

    const bool indexEdgeEnd = (cutscenePrevCsIndex != 0) && (currCsIndex == 0);
    const bool stateEdgeEnd =
        (cutscenePrevCsState != CS_STATE_IDLE) && (currCsState == CS_STATE_IDLE);
    const bool endEdge = indexEdgeEnd || stateEdgeEnd;

    // Both host and non-host need to keep prev* up to date so migration
    // handoff doesn't see a phantom edge on the first frame after the
    // local client becomes effective host. But only the effective host
    // emits packets and updates the dedup bookkeeping.
    if (!::SceneAuthority::IsEffectiveHost()) {
        cutscenePrevCsIndex = currCsIndex;
        cutscenePrevCsState = currCsState;
        return;
    }

    const int16_t sceneNum = (int16_t)gPlayState->sceneNum;
    const uint8_t timeline = (uint8_t)(gSaveContext.linkAge & 1);

    if (startEdge) {
        const std::string csKind = ClassifyCutsceneKind(currCsIndex, currCsState);
        const int32_t     csKey  = DeriveCsKey(csKind, currCsIndex);
        if (!IsCutsceneActive(sceneNum, timeline, csKind, csKey)) {
            // Carry literal csState byte so receivers don't have to
            // discriminate SKIPPABLE_INIT vs UNSKIPPABLE_INIT themselves.
            const uint8_t outState =
                (currCsState != CS_STATE_IDLE) ? currCsState
                                               : (uint8_t)CS_STATE_SKIPPABLE_INIT;
            SendPacket_CutsceneStart(csKind, csKey, outState);
            MarkCutsceneActive(sceneNum, timeline, csKind, csKey);
        }
    }

    if (endEdge) {
        // Find any matching active record from the host side and clear it.
        // The actor that fired the start may have been despawned by now
        // (Gohma defeat sequence runs through actionFunc until well past
        // the END edge), so we don't reclassify — instead we walk our
        // own activeCutscenes for entries in this scene+timeline and END
        // each. In practice there is at most one such entry per scene.
        std::vector<CutsceneActiveRecord> toEnd;
        for (const auto& rec : activeCutscenes) {
            if (rec.sceneNum == sceneNum && rec.timeline == timeline) {
                toEnd.push_back(rec);
            }
        }
        for (const auto& rec : toEnd) {
            SendPacket_CutsceneEnd(rec.csKind, rec.csKey, "natural");
            MarkCutsceneInactive(rec.sceneNum, rec.timeline, rec.csKind, rec.csKey);
        }
    }

    cutscenePrevCsIndex = currCsIndex;
    cutscenePrevCsState = currCsState;
}

// ---------------------------------------------------------------------
// Send-side
// ---------------------------------------------------------------------

void Anchor::SendPacket_CutsceneStart(const std::string& csKind, int32_t csKey,
                                      uint8_t csState) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    nlohmann::json payload;
    payload["type"]          = CUTSCENE_START;
    payload["sceneNum"]      = (int16_t)gPlayState->sceneNum;
    payload["csKind"]        = csKind;
    payload["csKey"]         = csKey;
    payload["csState"]       = csState;
    payload["ownerClientId"] = ownClientId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneStart] Sending kind={} key={} state={} scene=0x{:02X}",
                csKind, csKey, (int)csState, (int)gPlayState->sceneNum);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::SendPacket_CutsceneEnd(const std::string& csKind, int32_t csKey,
                                    const std::string& endReason) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    nlohmann::json payload;
    payload["type"]          = CUTSCENE_END;
    payload["sceneNum"]      = (int16_t)gPlayState->sceneNum;
    payload["csKind"]        = csKind;
    payload["csKey"]         = csKey;
    payload["endReason"]     = endReason;
    payload["ownerClientId"] = ownClientId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneEnd] Sending kind={} key={} reason={} scene=0x{:02X}",
                csKind, csKey, endReason, (int)gPlayState->sceneNum);

    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

// ---------------------------------------------------------------------
// Receive-side
// ---------------------------------------------------------------------

void Anchor::HandlePacket_CutsceneStart(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const int16_t sceneNum = payload.value("sceneNum", (int16_t)0);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    const std::string csKind   = payload.value("csKind",  std::string("actor_unknown"));
    const int32_t     csKey    = payload.value("csKey",   (int32_t)-1);
    uint8_t           csState  = payload.value("csState", (uint8_t)CS_STATE_SKIPPABLE_INIT);
    const uint8_t     timeline = (uint8_t)(gSaveContext.linkAge & 1);

    // Bounds-clamp incoming csState to the valid CS_STATE_* enum range
    // (0..4 per z64cutscene.h:97-103). Defends against a malicious or
    // corrupted peer injecting an out-of-range value into csCtx.state,
    // which the engine's state machine doesn't validate.
    if (csState > CS_STATE_UNSKIPPABLE_EXEC) {
        SPDLOG_WARN("[CutsceneStart] Out-of-range csState={} from peer; clamping to SKIPPABLE_INIT",
                    (int)csState);
        csState = CS_STATE_SKIPPABLE_INIT;
    }
    // SKIPPABLE_INIT is the safe default for "actor-internal cutscene
    // begins" — it's also what every save-context CS rises to after one
    // frame, so receivers never enter the post-START flow with state==0.
    if (csState == CS_STATE_IDLE) {
        csState = CS_STATE_SKIPPABLE_INIT;
    }

    if (IsCutsceneActive(sceneNum, timeline, csKind, csKey)) {
        // Adjacent-frame index/state re-fire from the host arriving as a
        // separate packet. Idempotent no-op.
        return;
    }

    if (gPlayState == nullptr) return;

    if (csKind == "savecontext") {
        // Engine's func_80068ECC will auto-advance csCtx.state next
        // frame; we only set the index here so timing matches a local
        // save-context cutscene start.
        gSaveContext.cutsceneIndex = (u16)csKey;
    } else {
        // Actor-internal: write csCtx.state directly so Player_Update's
        // auto-promote at z_player.c:12101-12113 fires next frame.
        gPlayState->csCtx.state = csState;
    }

    MarkCutsceneActive(sceneNum, timeline, csKind, csKey);

    SPDLOG_INFO("[CutsceneStart] Received kind={} key={} state={} scene=0x{:02X}",
                csKind, csKey, (int)csState, (int)sceneNum);
}

void Anchor::HandlePacket_CutsceneEnd(nlohmann::json payload) {
    if (!IsSaveLoaded()) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const int16_t sceneNum = payload.value("sceneNum", (int16_t)0);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    const std::string csKind   = payload.value("csKind", std::string("actor_unknown"));
    const int32_t     csKey    = payload.value("csKey",  (int32_t)-1);
    const uint8_t     timeline = (uint8_t)(gSaveContext.linkAge & 1);

    // No-op if we never saw the matching START (late-join replay path).
    if (!IsCutsceneActive(sceneNum, timeline, csKind, csKey)) {
        return;
    }

    if (gPlayState == nullptr) return;

    if (csKind == "savecontext") {
        gSaveContext.cutsceneIndex = 0;
        gSaveContext.gameMode      = GAMEMODE_NORMAL;
        Audio_SetCutsceneFlag(0);
        gPlayState->csCtx.state    = CS_STATE_IDLE;
    } else {
        gPlayState->csCtx.state = CS_STATE_IDLE;
        Audio_SetCutsceneFlag(0);
    }

    MarkCutsceneInactive(sceneNum, timeline, csKind, csKey);

    SPDLOG_INFO("[CutsceneEnd] Received kind={} key={} scene=0x{:02X}",
                csKind, csKey, (int)sceneNum);
}
