#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

#include <string>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "z64cutscene.h"
#include "macros.h"
#include "overlays/actors/ovl_Bg_Treemouth/z_bg_treemouth.h"
extern PlayState* gPlayState;

// Bg_Treemouth exports the peer-side trigger entry (defined in
// z_bg_treemouth.c). Bypasses the local-Link proximity check and drives
// the intro-cutscene state directly. csKey selects the variant:
//   csKey == 0 → first-encounter (D_808BCE20).
//   csKey == 1 → come-back (D_808BD2A0).
// Returns 1 on success, 0 if no local instance was found in the current scene.
int BgTreemouth_ForceIntroCutscene(PlayState* play, uint32_t csKey);
}

/**
 * CUTSCENE_START / CUTSCENE_END — generic cutscene bracket primitive.
 *
 * Plan reference:
 *   Claude/Plans/packet_family_cutscene_start_end.md
 *   Claude/Plans/cutscene_start_end_detector_spec.md
 *
 * ## Wire format
 *
 * ```json
 * // CUTSCENE_START
 * {
 *   "type": "CUTSCENE_START",
 *   "schema": 1,
 *   "sceneNum": 85,
 *   "csKind": "deku_tree_intro",  // savecontext | deku_tree_intro | ...
 *   "csKey": 0,                    // savecontext: cutsceneIndex value;
 *                                  // actor-internal: 0 today (per-actor
 *                                  // singleton), room for actor netId later.
 *   "timeline": 0,                 // via PacketTimeline
 *   "targetTeamId": "default"
 * }
 *
 * // CUTSCENE_END
 * {
 *   ... same fields ...
 *   "endReason": "natural"         // natural | skipped | aborted
 * }
 * ```
 *
 * ## Send-side
 *
 * Two dispatch classes:
 *
 * 1. **savecontext** — auto-detected. TickCutsceneStartDetector fires
 *    every OnGameFrameUpdate and watches gSaveContext.cutsceneIndex
 *    edges (0 → non-zero fires START; non-zero → 0 fires END). csKey =
 *    the cutsceneIndex value. Covers scripted engine cutscenes driven
 *    through the standard cutsceneIndex mechanism (~150 sites in the
 *    decomp; most are internal sub-CS chains that never trip the edge
 *    detector because they go non-zero → non-zero).
 *
 * 2. **Actor-driven kinds** — explicit Anchor_NotifyCutsceneStart /
 *    _End calls from the actor's C-side trigger site. Covers cutscenes
 *    where cutsceneIndex stays 0 but csCtx.state / .segment are written
 *    directly. Pilot consumer: Bg_Treemouth's "Deku Tree summons Link"
 *    intro. Each new consumer adds one Anchor_Notify call at its
 *    trigger site and one case in the receive-side switch here.
 *
 * ## Receive-side
 *
 * HandlePacket dispatches by csKind to a per-kind handler. Idempotency
 * dedup via cutsceneStartActive set — repeated STARTs for the same
 * (kind, key) tuple are no-ops. Same-scene + same-timeline gates drop
 * packets that don't apply to the local client's state.
 *
 * ## Late-join
 *
 * Not covered by v1. A late-joiner who enters after a cutscene starts
 * misses the cutscene entirely; they'll rejoin the world at the
 * post-cutscene state whenever the current cutscene ends (or naturally,
 * per their own scene-entry logic). Acceptable — most narrative
 * cutscenes are <30 seconds.
 */

namespace {

std::string MakeDedupKey(const std::string& csKind, uint32_t csKey) {
    return csKind + ":" + std::to_string(csKey);
}

// Per-kind receive handler — locates the local trigger and fires it.
// Returns true when the local mirror was fired; false when the peer
// couldn't apply (missing actor, unknown kind, etc.).
bool ApplyCutsceneStartByKind(const std::string& csKind, uint32_t csKey) {
    if (csKind == "savecontext") {
        // Save-context cutscene: write cutsceneIndex + let the engine
        // pick it up on the next frame. Plan §Input-lock approach
        // confirms Player_Update auto-halts when csCtx.state != IDLE,
        // which the engine will drive from the cutsceneIndex trigger.
        if (gPlayState == nullptr || gSaveContext.cutsceneIndex != 0) {
            return false;  // already active locally, or no game state
        }
        // Fix I.2 — receiver-side defense-in-depth. Refuse the
        // savecontext apply when a non-savecontext cutscene is
        // already active (either originated locally or received
        // from a peer). Applying savecontext by writing
        // cutsceneIndex=csKey (typically 0xFFFD — the vanilla
        // "trigger cutscene from cutsceneTrigger flag" magic
        // sentinel per z_play.c:467-470 + z_demo.c:2145-2157)
        // while an actor-driven cutscene is in flight overwrites
        // the actor's own state and may trigger vanilla void /
        // scene-reload on the peer. Sender-side Fix I.1 blocks
        // most cases at the source; this guard defends against a
        // peer whose sender is unpatched OR any race path we
        // haven't traced. Log 630 root cause. See
        // Analysis/cutscene_savecontext_double_broadcast_2026-07-08.md.
        if (Anchor::Instance != nullptr) {
            for (const auto& k : Anchor::Instance->cutsceneStartActive) {
                if (k.compare(0, 12, "savecontext:") != 0) {
                    SPDLOG_INFO("[CutsceneStart] Refused savecontext "
                                "apply csKey=0x{:04X} — non-savecontext "
                                "cutscene '{}' already active",
                                (unsigned)csKey, k);
                    return false;
                }
            }
        }
        gSaveContext.cutsceneIndex = (u16)csKey;
        SPDLOG_INFO("[CutsceneStart] Applied savecontext csKey=0x{:04X}", (unsigned)csKey);
        return true;
    }

    if (csKind == "deku_tree_intro") {
        // Bg_Treemouth pilot. Peer locates its local Bg_Treemouth and
        // fires the intro trigger path bypassing the local-Link
        // Actor_IsFacingAndNearPlayer distance check that gates the
        // vanilla trigger (z_bg_treemouth.c:155). The C-side helper
        // also sets EVENTCHKINF_MET_DEKU_TREE — safe because that
        // flag would have synced via SET_FLAG anyway.
        if (gPlayState == nullptr) return false;
        int fired = BgTreemouth_ForceIntroCutscene(gPlayState, csKey);
        if (fired) {
            SPDLOG_INFO("[CutsceneStart] Applied deku_tree_intro csKey={} on peer",
                        csKey);
            return true;
        }
        SPDLOG_INFO("[CutsceneStart] deku_tree_intro peer had no local Bg_Treemouth");
        return false;
    }

    SPDLOG_WARN("[CutsceneStart] Unknown csKind='{}' — packet ignored", csKind);
    return false;
}

// Per-kind end handler. For savecontext this is a no-op — the local
// engine will naturally clear cutsceneIndex when the CS state machine
// completes. For actor-driven kinds each actor also drives its own
// end path locally; the wire END is only a bookkeeping signal for
// dedup + the plan §5.3 UI banner shutdown. Kept in place so the
// send-side edge detector's END edges are echoed to peers.
bool ApplyCutsceneEndByKind(const std::string& csKind, uint32_t csKey,
                            const std::string& endReason) {
    (void)csKey;
    (void)endReason;
    if (csKind == "savecontext" || csKind == "deku_tree_intro") {
        // No teardown work required. Vanilla local state-machine
        // cleanup fires naturally. Idempotency dedup key is cleared
        // by the caller after this returns.
        return true;
    }
    SPDLOG_WARN("[CutsceneStart] Unknown csKind='{}' on END — packet ignored", csKind);
    return false;
}

}  // namespace

void Anchor::SendPacket_CutsceneStart(const std::string& csKind, uint32_t csKey) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    // Send-side dedup: skip if we've already broadcast a START for this
    // key without a matching END. Prevents repeated fires when both
    // detector paths (savecontext edge + explicit Notify) trigger for
    // the same cutscene.
    std::string dedupKey = MakeDedupKey(csKind, csKey);
    if (cutsceneStartActive.count(dedupKey) > 0) {
        return;
    }
    cutsceneStartActive.insert(dedupKey);
    // Fix G — mark this cutscene as locally-originated. Only entries
    // in this sibling set can promote the local client to leader for
    // the catchup ledger. Peer-received starts insert into
    // cutsceneStartActive but NOT this set. See Analysis/cutscene_
    // late_join_bugs_deep_analysis_2026-07-08.md Bug 2.
    cutsceneStartActiveLocalOrigin.insert(dedupKey);
    // Fix O — record local client as the cutscene originator for this
    // key. Peer-received starts populate this map with the sender's
    // clientId in HandlePacket_CutsceneStart. Vote-skip authority
    // routes through this instead of room-host during active cutscenes.
    // See Analysis/cutscene_room_desync_and_vote_scope_2026-07-08.md.
    cutsceneOriginatorByKindKey[dedupKey] = ownClientId;

    nlohmann::json payload;
    payload["type"]         = CUTSCENE_START;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["csKind"]       = csKind;
    payload["csKey"]        = csKey;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneStart] Sending csKind={} csKey=0x{:X} sceneNum={}",
                csKind, csKey, (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneStart(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[CutsceneStart] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[CutsceneStart] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    std::string csKind = payload.value("csKind", std::string(""));
    uint32_t    csKey  = payload.value("csKey", (uint32_t)0);
    if (csKind.empty()) {
        SPDLOG_WARN("[CutsceneStart] Packet missing csKind");
        return;
    }

    // Receive-side idempotency — if we already saw a START for this key,
    // drop. Prevents replay when a sender fires the same edge repeatedly
    // (send-side dedup already prevents this, defense-in-depth here).
    std::string dedupKey = MakeDedupKey(csKind, csKey);
    if (cutsceneStartActive.count(dedupKey) > 0) {
        return;
    }
    cutsceneStartActive.insert(dedupKey);
    // Fix O — record sender clientId as the cutscene originator. The
    // relay stamps `clientId` on incoming packets, so payload.value()
    // returns the sender's client id. Falls back to 0 if absent
    // (shouldn't happen but defensive).
    cutsceneOriginatorByKindKey[dedupKey] =
        payload.value("clientId", (uint32_t)0);

    ApplyCutsceneStartByKind(csKind, csKey);
}

void Anchor::SendPacket_CutsceneEnd(const std::string& csKind, uint32_t csKey,
                                     const std::string& endReason) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    std::string dedupKey = MakeDedupKey(csKind, csKey);
    if (cutsceneStartActive.count(dedupKey) == 0) {
        // No active START recorded — nothing to end. Prevents spurious
        // ENDs from firing on cutsceneIndex edges we never broadcast a
        // START for (e.g. cold-boot with a pre-existing non-zero index).
        return;
    }
    cutsceneStartActive.erase(dedupKey);
    // Fix G — mirror erase of the local-origin sibling set.
    cutsceneStartActiveLocalOrigin.erase(dedupKey);
    // Fix O — mirror erase of the originator map.
    cutsceneOriginatorByKindKey.erase(dedupKey);

    nlohmann::json payload;
    payload["type"]         = CUTSCENE_END;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["csKind"]       = csKind;
    payload["csKey"]        = csKey;
    payload["endReason"]    = endReason;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneEnd] Sending csKind={} csKey=0x{:X} reason={}",
                csKind, csKey, endReason);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneEnd(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) return;

    std::string csKind    = payload.value("csKind", std::string(""));
    uint32_t    csKey     = payload.value("csKey", (uint32_t)0);
    std::string endReason = payload.value("endReason", std::string("natural"));
    if (csKind.empty()) return;

    std::string dedupKey = MakeDedupKey(csKind, csKey);
    if (cutsceneStartActive.count(dedupKey) == 0) {
        return;  // never saw the START — nothing to clean up
    }
    cutsceneStartActive.erase(dedupKey);
    // Fix G — mirror erase (idempotent — peer receives never inserted
    // into the local-origin set, so erase is a no-op for those).
    cutsceneStartActiveLocalOrigin.erase(dedupKey);
    // Fix O — mirror erase of the originator map.
    cutsceneOriginatorByKindKey.erase(dedupKey);

    ApplyCutsceneEndByKind(csKind, csKey, endReason);
}

void Anchor::TickCutsceneStartDetector() {
    if (!isConnected) return;
    if (gPlayState == nullptr) return;

    const uint16_t currIdx   = (uint16_t)gSaveContext.cutsceneIndex;
    const uint8_t  currState = (uint8_t)gPlayState->csCtx.state;
    const uint16_t prevIdx   = cutsceneStartDetectorPrevIndex;
    (void)currState;  // reserved for actor-driven edge detection later

    // savecontext START edge: cutsceneIndex went 0 → non-zero.
    // Fix I.1 — suppress the savecontext broadcast when an actor-
    // driven cutscene is already active locally. The cutsceneIndex
    // change to non-zero is frequently a downstream side effect of
    // an actor-driven cutscene's own state machine (e.g., Bg_Treemouth
    // deku_tree_intro sets cutsceneTrigger=1 in Init; vanilla
    // func_80068ECC translates that on the next frame into
    // cutsceneIndex=0xFFFD). Broadcasting an independent
    // "savecontext, csKey=0xFFFD" packet causes peers to write
    // cutsceneIndex=0xFFFD on THEIR game state, tripping vanilla's
    // special-cutscene dispatch and triggering void/reload — the
    // log 630 P2 crash chain. See Analysis/cutscene_savecontext_
    // double_broadcast_2026-07-08.md for the full chain.
    if (prevIdx == 0 && currIdx != 0) {
        bool actorDrivenActive = false;
        for (const auto& k : cutsceneStartActiveLocalOrigin) {
            if (k.compare(0, 12, "savecontext:") != 0) {
                actorDrivenActive = true;
                break;
            }
        }
        if (!actorDrivenActive) {
            SendPacket_CutsceneStart("savecontext", (uint32_t)currIdx);
        } else {
            SPDLOG_INFO("[CutsceneStart] Suppressed savecontext broadcast "
                        "(cutsceneIndex 0x{:X}) — actor-driven cutscene "
                        "already active", (unsigned)currIdx);
        }
    }

    // savecontext END edge: cutsceneIndex went non-zero → 0.
    // Fix I.1 (mirror) — when the START was suppressed above, no
    // matching END was inserted into cutsceneStartActive, so the
    // erase inside SendPacket_CutsceneEnd is a no-op. The
    // downstream `cutsceneStartActive.count(dedupKey) == 0` early-
    // return at CutsceneStart.cpp handles this cleanly. Kept
    // unconditional for symmetry with legitimate savecontext ends.
    if (prevIdx != 0 && currIdx == 0) {
        SendPacket_CutsceneEnd("savecontext", (uint32_t)prevIdx, "natural");
    }

    cutsceneStartDetectorPrevIndex = currIdx;
    cutsceneStartDetectorPrevState = currState;
}

// ---------------------------------------------------------------------
// C bridge — actor-driven kinds fire via these entry points. Called
// from actor .c files at the vanilla trigger site (see z_bg_treemouth.c
// for the pilot). NO-OP in single-player or when Anchor is off.
// ---------------------------------------------------------------------

extern "C" void Anchor_NotifyCutsceneStart(const char* csKind, unsigned int csKey) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isEnabled) return;
    if (csKind == nullptr) return;
    Anchor::Instance->SendPacket_CutsceneStart(std::string(csKind),
                                                (uint32_t)csKey);
}

extern "C" void Anchor_NotifyCutsceneEnd(const char* csKind, unsigned int csKey,
                                          const char* endReason) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isEnabled) return;
    if (csKind == nullptr) return;
    Anchor::Instance->SendPacket_CutsceneEnd(
        std::string(csKind), (uint32_t)csKey,
        std::string(endReason ? endReason : "natural"));
}
