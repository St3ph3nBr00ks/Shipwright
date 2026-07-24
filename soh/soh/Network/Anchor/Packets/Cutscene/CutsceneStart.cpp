#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/CutsceneKindRegistry.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

#include <string>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "z64cutscene.h"
#include "macros.h"
extern PlayState* gPlayState;
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
    // Bugs 8/10/15 (playtest 2026-07-15) — refuse to force-apply any
    // cutscene we've already watched to completion in this session.
    // Fires when P1 has finished a one-shot cutscene, then P2 enters
    // the scene, P2's local trigger fires (fresh save context), and
    // P2's CUTSCENE_START would otherwise force P1 to replay the
    // same cutscene. Applies uniformly to savecontext and actor-driven
    // kinds — for both, "completed" means the terminal END was
    // observed (locally or via peer's CUTSCENE_END). Vanilla EventChkInf
    // handles cross-session persistence.
    const std::string dedupKey = csKind + ":" + std::to_string(csKey);
    if (Anchor::Instance != nullptr &&
        Anchor::Instance->cutsceneStartCompleted.count(dedupKey) > 0) {
        SPDLOG_INFO("[CutsceneStart] Refused apply — dedupKey='{}' already "
                    "completed in this session (playtest bugs 8/10/15)",
                    dedupKey);
        return false;
    }

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

    // Actor-driven kinds route through the CutsceneKindRegistry table.
    // Adding a new customer = 1 registration line in
    // Common/CutsceneKindRegistry.cpp + 1 Force helper in the actor's .c.
    // Prior if/else-chain dispatch was replaced 2026-07-09 (see
    // Analysis/generic_cutscene_dialog_sync_helpers_2026-07-09.md
    // Helpers B + D).
    if (const auto* handler = CutsceneKindRegistry::Find(csKind)) {
        // Opt-in kinds (e.g., come-back cutscenes gated on Z-target):
        // record the state but do NOT force-apply. The receiver stays
        // in gameplay mode; if their own local trigger fires later,
        // they can engage catchup via Anchor_TryEngageOptInCatchup.
        // See Analysis/deku_tree_come_back_sync_design_reversal_2026-07-09.md.
        if (handler->optInPredicate && handler->optInPredicate(csKey)) {
            SPDLOG_INFO("[CutsceneStart] csKind={} csKey={} is opt-in — "
                        "recorded but NOT forcing local entry",
                        csKind, csKey);
            return true;
        }
        if (!handler->applyForce) {
            SPDLOG_WARN("[CutsceneStart] csKind='{}' registered without applyForce; "
                        "packet ignored", csKind);
            return false;
        }
        const int fired = handler->applyForce(csKey);
        if (fired) {
            SPDLOG_INFO("[CutsceneStart] Applied csKind={} csKey={} on peer",
                        csKind, csKey);
            return true;
        }
        SPDLOG_INFO("[CutsceneStart] csKind={} apply failed (actor not local?)",
                    csKind);
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
    if (csKind == "savecontext") {
        // Vanilla self-teardown; no explicit end hook needed.
        return true;
    }

    // Actor-driven kinds route through the registry. Handler is optional
    // (applyEnd may be nullptr) — most customers rely on vanilla self-
    // teardown, so registered-but-null is the common shape and treated
    // as a success no-op.
    if (const auto* handler = CutsceneKindRegistry::Find(csKind)) {
        if (handler->applyEnd) {
            return handler->applyEnd(csKey, endReason);
        }
        return true;  // registered but no end hook — vanilla self-teardown
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

    // Fix F (log 662) — one-actor-one-variant invariant. For actor-driven
    // kinds, a fresh START with a different csKey supersedes any prior
    // variant of the same csKind (BgTreemouth can only be in one cutscene
    // at a time). Purge stale same-csKind entries by firing synthetic
    // CUTSCENE_END packets before adding the new key. Prevents the log-662
    // chain where ActiveKindKey() sees size=2 and returns empty, blocking
    // ledger creation for the new variant. Skip for savecontext — its
    // csKey is cutsceneIndex, which can legitimately have concurrent values
    // in v1 (not enforced but harmless).
    // See Analysis/deku_tree_stale_ledger_wins_race_2026-07-09.md Fix F.
    if (csKind != "savecontext") {
        const std::string prefix = csKind + ":";
        std::vector<uint32_t> staleCsKeys;
        for (const auto& activeKey : cutsceneStartActive) {
            if (activeKey.size() > prefix.size() &&
                activeKey.compare(0, prefix.size(), prefix) == 0 &&
                activeKey != dedupKey) {
                try {
                    staleCsKeys.push_back((uint32_t)std::stoul(
                        activeKey.substr(prefix.size())));
                } catch (...) {}
            }
        }
        for (uint32_t staleCsKey : staleCsKeys) {
            SendPacket_CutsceneEnd(csKind, staleCsKey, "superseded");
        }
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
    // Split scene check into state-tracking vs Apply. State
    // (cutsceneStartActive, cutsceneOriginatorByKindKey) is recorded
    // regardless of scene match — otherwise a peer entering the sender's
    // scene later would fire its own local SendPacket_CutsceneStart and
    // become a phantom leader (log 700 P2 Lost Woods Saria desync). Apply
    // still only runs when in the correct scene, preserving per-kind
    // side-effect safety. See Claude/Analysis/lost_woods_phantom_leader_2026-07-13.md.
    const bool sceneMatches = (sceneNum == (s16)gPlayState->sceneNum);

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
        // Simultaneous-broadcast tiebreak (playtest 2026-07-16 log 733).
        // When two clients each fire SendPacket_CutsceneStart for the
        // same (csKind, csKey) at ~the same time (both entered the
        // scene, both triggered a fresh-start locally), each sets
        // cutsceneOriginatorByKindKey[dedupKey] = ownClientId. Without
        // reconciliation, each client treats itself as the vote-skip
        // authority and votes never sync between them.
        //
        // Deterministic tiebreak: LOWEST clientId wins as originator.
        // Both clients converge on the same authority.
        //
        // If we WERE the originator and lose the tiebreak, remove
        // ourselves from cutsceneStartActiveLocalOrigin — we defer
        // FRAME_SYNC leadership to the peer too, avoiding parallel
        // leader broadcasts.
        //
        // See Claude/Analysis/simultaneous_cutscene_start_vote_desync_log733_2026-07-16.md.
        const uint32_t senderId = payload.value("clientId", (uint32_t)0);
        const uint32_t currentOriginator =
            (cutsceneOriginatorByKindKey.count(dedupKey) > 0)
                ? cutsceneOriginatorByKindKey[dedupKey]
                : 0;
        if (senderId != 0 && senderId != currentOriginator &&
            (currentOriginator == 0 || senderId < currentOriginator)) {
            const uint32_t prevOriginator = currentOriginator;
            cutsceneOriginatorByKindKey[dedupKey] = senderId;
            const bool weWereOriginator = (prevOriginator == ownClientId);
            if (weWereOriginator) {
                cutsceneStartActiveLocalOrigin.erase(dedupKey);
            }
            SPDLOG_INFO("[CutsceneStart] Simultaneous-broadcast tiebreak: "
                        "key='{}' sender={} < previous originator={} (self-was-originator={}); "
                        "originator updated to sender.",
                        dedupKey, senderId, prevOriginator,
                        weWereOriginator ? "true" : "false");
        }
        return;
    }

    // Fix F (log 662) — same-csKind purge on the receiver side. Defends
    // against Pitfall 43 goroutine reorder where the sender's synthetic
    // CUTSCENE_END(superseded) may arrive AFTER this CUTSCENE_START. Erase
    // stale same-csKind entries locally so the "one-actor-one-variant"
    // invariant holds regardless of arrival order. No broadcast — the
    // sender's synthetic END handles cross-client notification.
    // See Analysis/deku_tree_stale_ledger_wins_race_2026-07-09.md Fix F.
    if (csKind != "savecontext") {
        const std::string prefix = csKind + ":";
        std::vector<std::string> staleKeys;
        for (const auto& activeKey : cutsceneStartActive) {
            if (activeKey.size() > prefix.size() &&
                activeKey.compare(0, prefix.size(), prefix) == 0 &&
                activeKey != dedupKey) {
                staleKeys.push_back(activeKey);
            }
        }
        for (const auto& staleKey : staleKeys) {
            cutsceneStartActive.erase(staleKey);
            cutsceneStartActiveLocalOrigin.erase(staleKey);
            cutsceneOriginatorByKindKey.erase(staleKey);
            SPDLOG_INFO("[CutsceneStart] Fix F — purged stale same-csKind "
                        "key='{}' on receive of fresh '{}'",
                        staleKey, dedupKey);
        }
    }

    cutsceneStartActive.insert(dedupKey);
    // Fix O — record sender clientId as the cutscene originator. The
    // relay stamps `clientId` on incoming packets, so payload.value()
    // returns the sender's client id. Falls back to 0 if absent
    // (shouldn't happen but defensive).
    cutsceneOriginatorByKindKey[dedupKey] =
        payload.value("clientId", (uint32_t)0);

    if (sceneMatches) {
        ApplyCutsceneStartByKind(csKind, csKey);
    } else {
        SPDLOG_INFO("[CutsceneStart] Cross-scene state recorded key='{}' — "
                    "local scene {} != sender scene {}; Apply skipped "
                    "(will be picked up by dedup + Fix R when we enter "
                    "the sender's scene)",
                    dedupKey, (int)gPlayState->sceneNum, (int)sceneNum);
    }
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

    // Phase 4a — arm the coordination-point barrier BEFORE state
    // cleanup so the expected-peer snapshot reflects current
    // participation. Only for natural ends; superseded / aborted
    // ends are not coordination points.
    //
    // ArmCoordinationBarrier is itself a no-op when the CVar is off
    // or no peers qualify — safe to call unconditionally.
    //
    // Plans/phase_4a_wire_first_consumer_design_2026-07-16.md Change 3.
    if (endReason == "natural") {
        ArmCoordinationBarrier(dedupKey);
    }

    cutsceneStartActive.erase(dedupKey);
    // Fix G — mirror erase of the local-origin sibling set.
    cutsceneStartActiveLocalOrigin.erase(dedupKey);
    // Fix O — mirror erase of the originator map.
    cutsceneOriginatorByKindKey.erase(dedupKey);
    // Bugs 8/10/15 — record local-side completion so future peer STARTs
    // for the same (kind, key) don't force us to replay this cutscene.
    cutsceneStartCompleted.insert(dedupKey);

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
    // Split scene check — mirror of HandlePacket_CutsceneStart's approach.
    // State must be cleared regardless of scene match (otherwise
    // cutsceneStartActive holds a stale peer cutscene forever and blocks
    // future legitimate STARTs via the dedup gate). Apply still only runs
    // when in the correct scene. See Claude/Analysis/lost_woods_phantom_leader_2026-07-13.md.
    const bool sceneMatches = (sceneNum == (s16)gPlayState->sceneNum);

    std::string csKind    = payload.value("csKind", std::string(""));
    uint32_t    csKey     = payload.value("csKey", (uint32_t)0);
    std::string endReason = payload.value("endReason", std::string("natural"));
    if (csKind.empty()) return;

    std::string dedupKey = MakeDedupKey(csKind, csKey);

    // Phase 4a — coordination-point ack. Always fire (no-op if no
    // barrier armed). The sender's clientId identifies the peer.
    // Runs BEFORE the cutsceneStartActive check so peer ends that
    // arrive after we've already cleaned up locally still release
    // any pending barrier.
    //
    // Plans/phase_4a_wire_first_consumer_design_2026-07-16.md Change 4.
    const uint32_t senderClientId = payload.value("clientId", (uint32_t)0);
    MarkCoordinationReceived(dedupKey, senderClientId);

    if (cutsceneStartActive.count(dedupKey) == 0) {
        return;  // never saw the START — nothing to clean up
    }

    // Phase 4a — deferred erase. When the coordination CVar is ON AND
    // our local cutscene is still running (csCtx.state != IDLE), the
    // peer's END means THEY finished, not us. Keeping the entry in
    // cutsceneStartActive lets our own Fix G iteration find it when
    // our local cutscene reaches IDLE, so we broadcast our own END
    // and satisfy peers' coordination barriers. Cleanup happens then.
    //
    // With CVar OFF: fall through to eager erase (original behavior).
    const bool coordEnabled =
        CVarGetInteger(CVAR_ENHANCEMENT("Anchor.CutsceneWaitForPeer"), 0) != 0;
    const bool localStillInCutscene =
        coordEnabled && gPlayState->csCtx.state != CS_STATE_IDLE;
    if (localStillInCutscene) {
        SPDLOG_INFO("[CutsceneEnd] Peer clientId={} sent END for '{}' but "
                    "local still in cutscene — deferring state erase to "
                    "local Fix G", senderClientId, dedupKey);
        return;
    }

    cutsceneStartActive.erase(dedupKey);
    // Fix G — mirror erase (idempotent — peer receives never inserted
    // into the local-origin set, so erase is a no-op for those).
    cutsceneStartActiveLocalOrigin.erase(dedupKey);
    // Fix O — mirror erase of the originator map.
    cutsceneOriginatorByKindKey.erase(dedupKey);
    // Bugs 8/10/15 — record completion whether the END was ours or a
    // peer's. Either way, this cutscene reached its terminal state in
    // our view of the session; refuse future replays.
    cutsceneStartCompleted.insert(dedupKey);

    if (sceneMatches) {
        ApplyCutsceneEndByKind(csKind, csKey, endReason);
    } else {
        SPDLOG_INFO("[CutsceneEnd] Cross-scene state cleared key='{}' — "
                    "local scene {} != sender scene {}; Apply skipped",
                    dedupKey, (int)gPlayState->sceneNum, (int)sceneNum);
    }
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
    //
    // Scope fix (log 734, 2026-07-16): iterate cutsceneStartActive
    // (all known active cutscenes, self + received) instead of
    // cutsceneStartActiveLocalOrigin (only cutscenes THIS client
    // broadcast). Peers receiving deku_tree_intro from another
    // client have an empty local-origin set for that key, so the
    // pre-fix check failed → peer spuriously broadcast
    // savecontext:0xFFFD when vanilla wrote cutsceneIndex=0xFFFD for
    // the "opens mouth" chain → downstream FRAME_SYNC + Fix R
    // divergence → cutscene exit on originator. See
    // Analysis/cutscene_dialogue_architectural_review_log734_2026-07-16.md
    // and Analysis/coordination_point_sync_reassessment_2026-07-16.md
    // for the full failure chain + rejected wider architectural rewrite
    // (rewrite deferred; scope fix is behavior-preserving superset).
    if (prevIdx == 0 && currIdx != 0) {
        bool actorDrivenActive = false;
        for (const auto& k : cutsceneStartActive) {
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

        // Fix G (log 664/665) — actor-driven cutscene end via the same
        // falling edge. Vanilla drives cutsceneIndex back to 0 when
        // ANY cutscene (savecontext or actor-driven via cutsceneTrigger
        // + func_80068ECC) ends. There is no separate actor-driven end
        // signal in vanilla. Piggyback on this edge to fire natural
        // ENDs for any local-origin actor-driven kinds still in
        // cutsceneStartActive — otherwise their dedup entries persist
        // forever and block re-triggering (log 664 P2 second come-back
        // was silently dropped by SendPacket_CutsceneStart's dedup
        // check because 'deku_tree_intro:1' from the earlier applied
        // start was never cleared).
        //
        // Only iterate cutsceneStartActiveLocalOrigin — we only own
        // the end signal for cutscenes WE originated. Peer-originated
        // starts will be ended by the peer's own edge detector, then
        // propagated via CUTSCENE_END packet.
        //
        // See Analysis/deku_tree_bidirectional_come_back_2026-07-09.md
        // Fix G.
        // Phase 4a — when the coordination-point CVar is ON, iterate
        // cutsceneStartActive (self-originated + peer-received). This
        // makes peers also fire END for received cutscenes so the
        // originator's coordination barrier receives a matching ack.
        // With CVar OFF (default), keep the original local-origin
        // iteration for backward compatibility.
        //
        // See Plans/phase_4a_wire_first_consumer_design_2026-07-16.md
        // Change 1.
        const bool coordEnabled =
            CVarGetInteger(CVAR_ENHANCEMENT("Anchor.CutsceneWaitForPeer"), 0) != 0;
        const std::set<std::string>& source =
            coordEnabled ? cutsceneStartActive : cutsceneStartActiveLocalOrigin;

        std::vector<std::pair<std::string, uint32_t>> actorEnds;
        for (const auto& key : source) {
            auto sep = key.find(':');
            if (sep == std::string::npos) continue;
            std::string csKind = key.substr(0, sep);
            if (csKind == "savecontext") continue;
            uint32_t csKey = 0;
            try {
                csKey = (uint32_t)std::stoul(key.substr(sep + 1));
            } catch (...) { continue; }
            actorEnds.emplace_back(std::move(csKind), csKey);
        }
        for (const auto& [csKind, csKey] : actorEnds) {
            SendPacket_CutsceneEnd(csKind, csKey, "natural");
        }
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
