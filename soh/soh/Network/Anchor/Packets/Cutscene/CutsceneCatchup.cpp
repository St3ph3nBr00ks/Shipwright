#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/CutsceneCatchup.h"
#include "soh/Enhancements/audio/AnchorAudioSeek.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

#include <chrono>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;

// Per-kind setup entry points. These invoke the SAME actor-side
// cutscene setup path that the vanilla trigger would have hit —
// establishes csCtx.segment, cs-side D_8015FCC* skip flags, actor
// action func, and Player cutscene action mode. Without this, my
// catchup-apply only sets csCtx.state (letterbox appears) but the
// cutscene engine has nothing to execute (script segment unset)
// and Player retains gameplay control.
int BgTreemouth_ForceIntroCutscene(PlayState* play);
}
// func_800645A0 is the vanilla per-tick cutscene body (z_demo.c:170).
// Normally called once per real frame from Play_Update (z_play.c:1239).
// For Option B silent fast-forward, we call it up to N extra times
// per real frame from TickCutsceneCatchup to accelerate playback
// from frame 0 to the leader's current frame. Declared in functions.h
// but re-declared here inside an extern "C" block for clarity.
extern "C" void func_800645A0(PlayState* play, CutsceneContext* csCtx);

// CUTSCENE_FRAME_SYNC / CUTSCENE_CATCHUP_REQUEST / CUTSCENE_CATCHUP_RESPONSE
//
// Plan: Claude/Plans/cutscene_late_join_plan.md §3
// Analysis: Claude/Analysis/cutscene_entry_gate_design_2026-07-07.md
//
// FRAME_SYNC: 1Hz leader broadcast of csCtx.frames + state. Peers use
// this for ongoing drift correction after initial catch-up. Session-
// monotonic seq (Pitfall 43 pattern) rejects reordered packets.
//
// CATCHUP_REQUEST: late-joiner → leader, one-shot targeted packet
// (via targetClientId). Late-joiner detects a same-team peer mid-
// cutscene on scene-entry and requests the ledger snapshot.
//
// CATCHUP_RESPONSE: leader → late-joiner, carries the full ledger
// delta (spawns / flags / items / music / camera / actors) plus the
// leader's current csCtx.frames + csCtx.state. Late-joiner applies
// atomically, then vanilla playback resumes from the leader's frame.
//
// Team-id gating: FRAME_SYNC stamps targetTeamId so the relay routes
// only to same-team clients. REQUEST/RESPONSE use targetClientId
// (point-to-point) but also stamp targetTeamId defensively — belt-
// and-suspenders since a leader-of-team-A shouldn't answer a
// late-joiner-of-team-B even if their client-IDs happen to align.

namespace {

// Constants
constexpr int64_t kCatchupTimeoutMs      = 2000;    // §10 Q3 resolved
constexpr int32_t kFrameSyncIntervalMs   = 1000;    // 1Hz per §3.1

// Static timer state — file-scope OK since these are singletons per
// process. TickCutsceneCatchup polls at 60 Hz; we only actually SEND
// when the elapsed interval crosses the threshold.
std::chrono::steady_clock::time_point sLastFrameSyncBroadcast;

// Build the "<csKind>:<csKey>" key used across ledger + pendingCatchups
// (mirror of MakeDedupKey in CutsceneStart.cpp — kept local to avoid
// including a private header from that TU).
std::string MakeKindKey(const std::string& csKind, uint32_t csKey) {
    return csKind + ":" + std::to_string(csKey);
}

// Serialize a ledger entry into a JSON payload. Used for
// CATCHUP_RESPONSE. Called only on the leader.
nlohmann::json SerializeLedgerEntry(
    const ::CutsceneCatchup::CutsceneCatchupEntry& e) {

    nlohmann::json out;
    out["csKind"]              = e.csKind;
    out["csKey"]               = e.csKey;
    out["sceneNum"]            = (int)e.sceneNum;
    out["roomNum"]             = (int)e.roomNum;
    out["linkAge"]             = (int)e.linkAge;
    out["startFrame"]          = e.startFrame;
    out["currentLeaderFrame"]  = e.currentLeaderFrame;

    // Spawned actors
    nlohmann::json spawns = nlohmann::json::array();
    for (const auto& r : e.spawnedActors) {
        nlohmann::json rec;
        rec["netId"]  = r.netId;
        rec["actorId"]= (int)r.actorId;
        rec["params"] = (int)r.params;
        rec["room"]   = (int)r.room;
        rec["pos"]    = nlohmann::json::array({r.pos.x, r.pos.y, r.pos.z});
        rec["rot"]    = nlohmann::json::array({(int)r.rot.x, (int)r.rot.y,
                                                (int)r.rot.z});
        spawns.push_back(rec);
    }
    out["spawnedActors"] = spawns;

    // Flags set
    nlohmann::json flags = nlohmann::json::array();
    for (const auto& r : e.flagsSet) {
        nlohmann::json rec;
        rec["flagType"]      = (int)r.flagType;
        rec["flag"]          = (int)r.flag;
        rec["sceneOrRoomNum"]= (int)r.sceneOrRoomNum;
        flags.push_back(rec);
    }
    out["flagsSet"] = flags;

    // Items granted
    nlohmann::json items = nlohmann::json::array();
    for (const auto& r : e.itemsGranted) {
        nlohmann::json rec;
        rec["itemId"] = (int)r.itemId;
        rec["amount"] = r.amount;
        items.push_back(rec);
    }
    out["itemsGranted"] = items;

    // Music snapshot — compute offsetMs from steady_clock delta so the
    // receiver can call Anchor_SeekBGMToMs directly.
    nlohmann::json music;
    music["seqId"] = e.music.seqId;
    if (e.music.seqId >= 0) {
        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - e.music.startedAt).count();
        music["offsetMs"] = (int64_t)delta + e.music.offsetFromStartMs;
    } else {
        music["offsetMs"] = 0;
    }
    out["music"] = music;

    // Camera
    nlohmann::json cam;
    cam["eye"]     = nlohmann::json::array({e.camera.eye.x, e.camera.eye.y,
                                             e.camera.eye.z});
    cam["at"]      = nlohmann::json::array({e.camera.at.x, e.camera.at.y,
                                             e.camera.at.z});
    cam["fov"]     = e.camera.fov;
    cam["validAt"] = e.camera.validAt;
    out["camera"] = cam;

    // Actors snapshot
    nlohmann::json actors = nlohmann::json::array();
    for (const auto& s : e.actors) {
        nlohmann::json rec;
        rec["netId"]        = s.netId;
        rec["actorId"]      = (int)s.actorId;
        rec["params"]       = (int)s.params;
        rec["room"]         = (int)s.room;
        rec["home"]         = nlohmann::json::array({s.home.x, s.home.y, s.home.z});
        rec["pos"]          = nlohmann::json::array({s.pos.x, s.pos.y, s.pos.z});
        rec["rot"]          = nlohmann::json::array({(int)s.rot.x, (int)s.rot.y,
                                                      (int)s.rot.z});
        rec["shapeRot"]     = nlohmann::json::array({(int)s.shapeRot.x,
                                                      (int)s.shapeRot.y,
                                                      (int)s.shapeRot.z});
        rec["animFrame"]    = s.animFrame;
        rec["actionFuncKey"]= s.actionFuncKey;
        actors.push_back(rec);
    }
    out["actors"] = actors;

    return out;
}

// Apply a serialized delta to the local state on the late-joiner.
// Called from HandlePacket_CutsceneCatchupResponse. Runs on the
// game thread.
void ApplyCatchupDelta(const nlohmann::json& payload) {
    if (gPlayState == nullptr) return;

    // 1. Spawns — re-spawn each recorded actor via Actor_Spawn. Guard
    //    with isSpawningNetworkActor so the OnActorSpawn hook doesn't
    //    re-broadcast them.
    if (payload.contains("spawnedActors") &&
        payload["spawnedActors"].is_array()) {
        for (const auto& r : payload["spawnedActors"]) {
            // v1: log-only for spawn replay. Actor_Spawn requires an
            // objectId which we don't ship; getting the correct one
            // per actor is Phase 3.5 work (would need a per-actor-id
            // lookup, similar to what ActorDB provides on host).
            // Cannot verify: whether all cutscene-spawned actors need
            // a specific objectId or if 0 works for gameplay_keep-
            // resident ones. Defer.
            SPDLOG_INFO("[CutsceneCatchup] Spawn replay TODO actorId={:#x} "
                        "netId={} — v1 skip",
                        (int)r.value("actorId", 0), r.value("netId", 0u));
            (void)r;
        }
    }

    // 2. Flags — replay via the existing SET_FLAG apply path. Bracket
    //    with the same isApplyingNetworkFlag guard that packet uses.
    //    Cannot-verify note: the SetFlag.cpp receive path expects a
    //    specific packet shape; calling into it from here would need
    //    a helper extraction. v1 uses direct Flags_* calls.
    if (payload.contains("flagsSet") && payload["flagsSet"].is_array()) {
        for (const auto& r : payload["flagsSet"]) {
            const int16_t ft = (int16_t)r.value("flagType", 0);
            const int16_t fv = (int16_t)r.value("flag", 0);
            SPDLOG_INFO("[CutsceneCatchup] Flag replay ft={} f={} — v1 skip",
                        (int)ft, (int)fv);
            (void)ft; (void)fv;
        }
    }

    // 3. Items — replay via GIVE_ITEM path. Same caveat as flags.
    if (payload.contains("itemsGranted") &&
        payload["itemsGranted"].is_array()) {
        for (const auto& r : payload["itemsGranted"]) {
            const int16_t itemId = (int16_t)r.value("itemId", 0);
            SPDLOG_INFO("[CutsceneCatchup] Item replay itemId={} — v1 skip",
                        (int)itemId);
            (void)itemId;
        }
    }

    // 4. Music — seek to the leader's current offset. This is the
    //    load-bearing piece Q4 investigation validated.
    if (payload.contains("music")) {
        const auto& m = payload["music"];
        const int seqId = m.value("seqId", -1);
        const int64_t offsetMs = m.value("offsetMs", (int64_t)0);
        if (seqId >= 0 && offsetMs > 0) {
            const int rc = Anchor_SeekBGMToMs(
                /*playerIdx*/ 0,     // BGM main channel
                seqId,
                (int)offsetMs);
            SPDLOG_INFO("[CutsceneCatchup] Music seek seqId={:#x} "
                        "offsetMs={} rc={}",
                        seqId, offsetMs, rc);
        }
    }

    // 5. Camera — apply at Phase 4 time. v1 lets vanilla next-frame
    //    tick regenerate camera from the mid-cutscene state.

    // 6. Actor snapshots — v1 defers (see R12 in plan §7). Vanilla
    //    dispatcher re-drives cutscene-controlled actors when we
    //    resume playback at leader.frame.

    // 7. Set up cutscene state per-kind. This invokes the same actor-
    //    side cutscene entry the vanilla trigger would have hit
    //    (BgTreemouth_ForceIntroCutscene for deku_tree_intro) — sets
    //    csCtx.segment, cutsceneTrigger, actor action func.
    const std::string csKind = payload.value("csKind", std::string(""));
    if (csKind == "deku_tree_intro") {
        const int rc = BgTreemouth_ForceIntroCutscene(gPlayState);
        SPDLOG_INFO("[CutsceneCatchup] Setup deku_tree_intro rc={}", rc);
    } else {
        SPDLOG_WARN("[CutsceneCatchup] No per-kind setup for csKind='{}' — "
                    "cutscene engine may lack script data",
                    csKind);
    }

    // 8. Silent fast-forward (Option B) — NOT frame-jump.
    //
    // Field-test 622 confirmed that overwriting csCtx.frames/state
    // directly skips the vanilla state-transition (func_80068ECC)
    // AND every frame-0..N-1 command's side effects (camera cache,
    // Player teleport, dialogue open/close, Player action lock,
    // audio flag). Result: black bars but nothing functional.
    //
    // Fix: leave csCtx.frames = 0 and csCtx.state = IDLE. Vanilla's
    // per-tick func_800645A0 will drive the state machine cleanly
    // from IDLE → UNSKIPPABLE_INIT → SKIPPABLE_INIT → SKIPPABLE_EXEC
    // → per-frame command dispatch. TickCutsceneCatchup then invokes
    // func_800645A0 extra times per real frame until csCtx.frames
    // reaches leaderFrame. Every intermediate frame's setup commands
    // execute in-order — matching vanilla exactly.
    //
    // See Analysis/cutscene_catchup_p2_engagement_2026-07-07.md §3
    // for the full option matrix (A=reset, B=silent-ff, C=partial,
    // D=hybrid). Option B chosen 2026-07-07 for natural coop UX.
    const int32_t leaderFrame = (int32_t)payload.value("leaderFrame",
                                                       (int32_t)0);
    if (leaderFrame > 0 && ::Anchor::Instance != nullptr) {
        // Reset stale cutscene state before setting the fast-forward
        // target. csCtx.frames is a persistent PlayState field that
        // vanilla only rewrites in func_80068ECC's SKIPPABLE_INIT
        // branch (z_demo.c:2162). Without this reset, TickCutscene
        // Catchup's exit condition (frames >= target) fires
        // immediately on values left over from prior activity — false
        // catchup completion, vanilla then plays at 1× from frame 0
        // and P2 falls behind. Observed in log 623 (frames=159 stale).
        //
        // With state = IDLE + cutsceneTrigger=1 (set by
        // BgTreemouth_ForceIntroCutscene), vanilla's next tick drives
        // the transition IDLE → INIT → EXEC cleanly; fast-forward
        // then accelerates frame-0 command dispatch as intended.
        gPlayState->csCtx.frames = 0;
        gPlayState->csCtx.state  = CS_STATE_IDLE;
        ::Anchor::Instance->catchupFastForwardTarget = leaderFrame;
        SPDLOG_INFO("[CutsceneCatchup] Applied delta — silent fast-forward "
                    "target=frame {} (vanilla will drive state machine + "
                    "command dispatch; TickCutsceneCatchup accelerates)",
                    leaderFrame);
    } else {
        SPDLOG_INFO("[CutsceneCatchup] Applied delta — leaderFrame=0 "
                    "(no fast-forward needed)");
    }
}

}  // namespace

// ---- SendPacket_CutsceneFrameSync -----------------------------------

void Anchor::SendPacket_CutsceneFrameSync() {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!isConnected) return;
    // ONLY the leader broadcasts. Definitive signal: ledger ownership.
    // The ledger is populated only by CutsceneCatchup::Record* functions
    // which are gated by IsLeader() && IsInCutscene() on the game thread;
    // peers who hydrated `cutsceneStartActive` from received FRAME_SYNC
    // do NOT populate the ledger. Using ledger presence keeps the send
    // path immune to the previous "hydrated peer becomes phantom leader"
    // bug that caused restart loops in field-test 621.
    if (cutsceneCatchupLedger.empty()) return;
    if (gPlayState->csCtx.state == CS_STATE_IDLE) return;
    if (!::SceneAuthority::IsRoomHost(
            (int16_t)gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num,
            (uint8_t)(gSaveContext.linkAge & 0x1))) {
        return;  // belt-and-suspenders — room-host + ledger-owner
    }

    // Broadcast the leader's frame for the one active cutscene.
    // v1 assumes single-cutscene sessions — pick first.
    const std::string& kindKey = *cutsceneStartActive.begin();
    auto sep = kindKey.find(':');
    if (sep == std::string::npos) return;
    const std::string csKind = kindKey.substr(0, sep);
    uint32_t csKey = 0;
    try {
        csKey = (uint32_t)std::stoul(kindKey.substr(sep + 1));
    } catch (...) { csKey = 0; }

    const uint64_t seq = ++cutsceneFrameSyncSequence;

    nlohmann::json payload;
    payload["type"]          = CUTSCENE_FRAME_SYNC;
    payload["seq"]           = seq;
    payload["csKind"]        = csKind;
    payload["csKey"]         = csKey;
    payload["sceneNum"]      = (int)gPlayState->sceneNum;
    // Room scope — receiver gates on same-room match. Cutscene actors,
    // camera, warp coordinates are all room-scoped; catchup in a
    // different room within the same scene is not a valid scope.
    payload["roomNum"]       = (int)gPlayState->roomCtx.curRoom.num;
    payload["leaderFrame"]   = (int)gPlayState->csCtx.frames;
    payload["leaderState"]   = (int)gPlayState->csCtx.state;
    payload["targetTeamId"]  = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"),
                                              "default");
    payload["quiet"]         = true;   // 1Hz — no need to spam relay logs
    PacketTimeline::SetTimelineField(payload);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneFrameSync(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    // Session-monotonic seq (Pitfall 43).
    if (payload.contains("seq") && payload["seq"].is_number_unsigned()) {
        const uint64_t seq = payload["seq"].get<uint64_t>();
        if (seq <= peerLastAppliedCutsceneFrameSeq) return;
        peerLastAppliedCutsceneFrameSeq = seq;
    }

    // Same scene AND same room. Cutscene actors, camera cues, warp
    // coordinates are all room-scoped; catchup in a different room
    // within the same scene is not a valid scope. When P2 enters the
    // leader's room later, OnSceneSpawnActors triggers detection.
    // Field-test 621 exposed this: P1 in scene 85 room 1 broadcast to
    // P2 in scene 85 room 0, causing phantom-broadcast and eventual
    // circular catchup on P1's own cutscene.
    const int16_t sceneNum = (int16_t)payload.value("sceneNum", -1);
    if (sceneNum != (int16_t)gPlayState->sceneNum) return;
    const int8_t roomNum = (int8_t)payload.value("roomNum", -1);
    if (roomNum != -1 && roomNum != (int8_t)gPlayState->roomCtx.curRoom.num) {
        return;
    }

    // Extract csKind + csKey so we can hydrate cutsceneStartActive if
    // we're a late-joiner (connected AFTER the leader's CUTSCENE_START
    // broadcast). Without this, DetectAndRequestCutsceneCatchup skips
    // because it can't build a kindKey — resulting in no catchup for
    // late-connect scenarios.
    const std::string csKind = payload.value("csKind", std::string(""));
    const uint32_t    csKey  = payload.value("csKey", (uint32_t)0);
    const int32_t leaderFrame = (int32_t)payload.value("leaderFrame", 0);
    const int leaderState = payload.value("leaderState", 0);
    (void)leaderFrame; (void)leaderState;

    if (!csKind.empty()) {
        const std::string kindKey = csKind + ":" + std::to_string(csKey);

        // IMPORTANT: if we own this cutscene's ledger entry, WE are the
        // leader. Ignore incoming FRAME_SYNC for this kindKey — accepting
        // it would drive us to request catchup from a phantom broadcaster
        // (typically a late-joiner peer whose own SendPacket_CutsceneFrameSync
        // slipped through their gates), which would then loop-restart our
        // own cutscene when the phantom RESPONSE arrives. Ledger ownership
        // is the definitive "I am the leader" signal.
        const bool iAmLeader = cutsceneCatchupLedger.count(kindKey) > 0;
        if (iAmLeader) {
            return;
        }

        if (cutsceneStartActive.count(kindKey) == 0) {
            cutsceneStartActive.insert(kindKey);
            SPDLOG_INFO("[CutsceneCatchup] FRAME_SYNC observed new kindKey='{}' — "
                        "populating cutsceneStartActive for late-joiner mode",
                        kindKey);
        }

        // Direct-request path for late-joiners. FRAME_SYNC receipt is
        // itself proof the sender is mid-cutscene. Skip the clients-map
        // scan and go straight to REQUEST if we don't already have one
        // pending for this kindKey. Sender identity: relay stamps
        // `clientId` on incoming packets.
        const uint32_t senderClientId = payload.value("clientId", (uint32_t)0);
        const bool haveEnabled     = CutsceneCatchupEnabled();
        const bool alreadyPending  = pendingCatchups.count(kindKey) > 0;
        const bool selfBroadcast   = (senderClientId == ownClientId);
        const bool validSender     = (senderClientId != 0);
        const bool alreadyInCs     = (gPlayState->csCtx.state != CS_STATE_IDLE);
        SPDLOG_INFO("[CutsceneCatchup] FRAME_SYNC gates — sender={} own={} "
                    "kindKey='{}' enabled={} alreadyPending={} selfBroadcast={} "
                    "validSender={} alreadyInCs={} — will{} fire",
                    senderClientId, ownClientId, kindKey,
                    haveEnabled, alreadyPending, selfBroadcast, validSender,
                    alreadyInCs,
                    (validSender && !selfBroadcast && !alreadyPending &&
                     haveEnabled && !alreadyInCs)
                        ? "" : " NOT");
        if (validSender && !selfBroadcast && !alreadyPending && haveEnabled &&
            !alreadyInCs) {
            PendingCatchup p;
            p.deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(kCatchupTimeoutMs);
            p.leaderClientId = senderClientId;
            pendingCatchups[kindKey] = p;

            SPDLOG_INFO("[CutsceneCatchup] FRAME_SYNC direct-request from "
                        "sender={} — requesting catchup for '{}'",
                        senderClientId, kindKey);
            SendPacket_CutsceneCatchupRequest(senderClientId, csKind, csKey);
        }
    }
}

// ---- CUTSCENE_CATCHUP_REQUEST ---------------------------------------

void Anchor::SendPacket_CutsceneCatchupRequest(uint32_t leaderClientId,
                                                const std::string& csKind,
                                                uint32_t csKey) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!isConnected) return;

    nlohmann::json payload;
    payload["type"]           = CUTSCENE_CATCHUP_REQUEST;
    payload["csKind"]         = csKind;
    payload["csKey"]          = csKey;
    payload["sceneNum"]       = (int)gPlayState->sceneNum;
    payload["roomNum"]        = (int)gPlayState->roomCtx.curRoom.num;
    payload["linkAge"]        = (int)(gSaveContext.linkAge & 0x1);
    payload["targetClientId"] = leaderClientId;
    payload["targetTeamId"]   = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"),
                                               "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneCatchup] REQUEST → leader={} csKind={} csKey={:#x}",
                leaderClientId, csKind, csKey);
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneCatchupRequest(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const std::string csKind = payload.value("csKind", std::string(""));
    const uint32_t    csKey  = payload.value("csKey", (uint32_t)0);
    if (csKind.empty()) return;

    // Only respond if we're the leader (room host) AND have a ledger
    // entry for this cutscene.
    if (!::SceneAuthority::IsRoomHost(
            (int16_t)gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num,
            (uint8_t)(gSaveContext.linkAge & 0x1))) {
        SPDLOG_INFO("[CutsceneCatchup] REQUEST for '{}' but I'm not room host; ignoring",
                    csKind);
        return;
    }

    const std::string kindKey = MakeKindKey(csKind, csKey);
    auto it = cutsceneCatchupLedger.find(kindKey);
    if (it == cutsceneCatchupLedger.end()) {
        SPDLOG_INFO("[CutsceneCatchup] REQUEST for '{}' but no ledger entry; "
                    "ignoring", kindKey);
        return;
    }

    // Update the frame counter one last time before serialising.
    ::CutsceneCatchup::UpdateFrameCounter();

    // The request MUST carry the requester's clientId (relay stamps
    // it as sender). Peel that out for the response's targetClientId.
    const uint32_t requesterClientId = payload.value("clientId",
                                                     (uint32_t)0);
    if (requesterClientId == 0) {
        SPDLOG_WARN("[CutsceneCatchup] REQUEST missing clientId; drop");
        return;
    }

    SendPacket_CutsceneCatchupResponse(requesterClientId, csKind, csKey);
}

// ---- CUTSCENE_CATCHUP_RESPONSE --------------------------------------

void Anchor::SendPacket_CutsceneCatchupResponse(uint32_t requesterClientId,
                                                 const std::string& csKind,
                                                 uint32_t csKey) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (!isConnected) return;

    const std::string kindKey = MakeKindKey(csKind, csKey);
    auto it = cutsceneCatchupLedger.find(kindKey);
    if (it == cutsceneCatchupLedger.end()) return;

    nlohmann::json payload = SerializeLedgerEntry(*(it->second));
    payload["type"]            = CUTSCENE_CATCHUP_RESPONSE;
    payload["targetClientId"]  = requesterClientId;
    payload["targetTeamId"]    = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"),
                                                "default");
    // Field aliases the receiver reads directly (avoids nested dig).
    payload["leaderFrame"]     = it->second->currentLeaderFrame;
    payload["leaderState"]     = (int)gPlayState->csCtx.state;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneCatchup] RESPONSE → requester={} kindKey='{}' "
                "leaderFrame={} spawns={} flags={} items={} actors={}",
                requesterClientId, kindKey,
                it->second->currentLeaderFrame,
                it->second->spawnedActors.size(),
                it->second->flagsSet.size(),
                it->second->itemsGranted.size(),
                it->second->actors.size());
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CutsceneCatchupResponse(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const std::string csKind = payload.value("csKind", std::string(""));
    const uint32_t    csKey  = payload.value("csKey", (uint32_t)0);
    if (csKind.empty()) return;

    const std::string kindKey = MakeKindKey(csKind, csKey);
    auto it = pendingCatchups.find(kindKey);
    if (it == pendingCatchups.end()) {
        SPDLOG_INFO("[CutsceneCatchup] RESPONSE for '{}' but not pending; drop",
                    kindKey);
        return;
    }

    SPDLOG_INFO("[CutsceneCatchup] RESPONSE received for '{}' — applying delta",
                kindKey);
    ApplyCatchupDelta(payload);
    pendingCatchups.erase(it);
}

// ---- TickCutsceneCatchup --------------------------------------------
// Called from OnGameFrameUpdate (registered in Phase 4 wiring). Handles:
//   - 1Hz FRAME_SYNC emit (leader-only).
//   - Snapshot updates on the ledger (camera/actors at 1Hz cadence).
//   - Pending-catchup deadline enforcement (2s timeout → drop entry;
//     safety-net will handle the local cutscene state).

// ---- CVar gate ------------------------------------------------------

bool Anchor::CutsceneCatchupEnabled() const {
    // Default 1 per Q2 (2026-07-07) — pure coop UX improvement, does
    // not alter vanilla single-player feel.
    return CVarGetInteger(
        CVAR_ENHANCEMENT("Anchor.CutsceneLateJoin"), 1) != 0;
}

// ---- Late-joiner scene-entry detection ------------------------------

void Anchor::DetectAndRequestCutsceneCatchup() {
    if (!isConnected) return;
    if (gPlayState == nullptr) return;
    if (!IsSaveLoaded()) return;

    const int16_t   myScene    = (int16_t)gPlayState->sceneNum;
    const uint8_t   myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    const std::string myTeamId =
        CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");

    for (auto& [cid, client] : clients) {
        if (cid == ownClientId) continue;
        if (client.self) continue;
        if (!client.online) continue;
        if (!client.isSaveLoaded) continue;
        if (client.sceneNum != myScene) continue;
        if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
        if (client.teamId != myTeamId) continue;
        if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;

        // Peer is mid-cutscene in our scene. We don't know THEIR
        // csKind/csKey — but our own cutsceneStartActive set has
        // been populated by the CUTSCENE_START broadcast from the
        // leader (relay routes by targetTeamId; we've been receiving).
        // v1 assumes single-cutscene sessions: request catchup for
        // whatever the first active kind key is.
        if (cutsceneStartActive.empty()) {
            SPDLOG_INFO("[CutsceneCatchup] Peer clientId={} mid-cs (csState={}) "
                        "but cutsceneStartActive empty — no key to request",
                        cid, (int)client.csCtxState);
            continue;
        }
        const std::string& kindKey = *cutsceneStartActive.begin();
        auto sep = kindKey.find(':');
        if (sep == std::string::npos) continue;
        const std::string csKind = kindKey.substr(0, sep);
        uint32_t csKey = 0;
        try {
            csKey = (uint32_t)std::stoul(kindKey.substr(sep + 1));
        } catch (...) { csKey = 0; }

        // Skip if we already have a pending catchup for this kindKey —
        // prevents REQUEST-spam when DetectAndRequestCutsceneCatchup
        // is re-triggered by FRAME_SYNC arrival on late-joiner path.
        if (pendingCatchups.count(kindKey) > 0) {
            continue;
        }

        // Register pending catchup + fire request.
        PendingCatchup p;
        p.deadline = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(kCatchupTimeoutMs);
        p.leaderClientId = cid;
        pendingCatchups[kindKey] = p;

        SPDLOG_INFO("[CutsceneCatchup] Detected mid-cs peer clientId={} — "
                    "requesting catchup for '{}'", cid, kindKey);
        SendPacket_CutsceneCatchupRequest(cid, csKind, csKey);
        break;  // v1: one catchup per scene entry (single-cs invariant)
    }
}

void Anchor::TickCutsceneCatchup() {
    if (!isConnected) return;
    if (gPlayState == nullptr) return;

    const auto now = std::chrono::steady_clock::now();

    // Leader side — 1Hz FRAME_SYNC emit + snapshot refresh.
    if (!cutsceneStartActive.empty()) {
        const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - sLastFrameSyncBroadcast).count();
        if (sinceLast >= kFrameSyncIntervalMs) {
            sLastFrameSyncBroadcast = now;
            ::CutsceneCatchup::UpdateFrameCounter();
            ::CutsceneCatchup::SnapshotCamera();
            ::CutsceneCatchup::SnapshotActors();
            ::CutsceneCatchup::ReconcileLifecycle();
            SendPacket_CutsceneFrameSync();
        }
    }

    // Peer side — deadline enforcement on pending catchups.
    for (auto it = pendingCatchups.begin(); it != pendingCatchups.end();) {
        if (now >= it->second.deadline) {
            SPDLOG_WARN("[CutsceneCatchup] Pending catchup for '{}' timed out "
                        "after {}ms; dropping (safety net will hold local cs "
                        "in IDLE if it fires)",
                        it->first, kCatchupTimeoutMs);
            it = pendingCatchups.erase(it);
        } else {
            ++it;
        }
    }

    // Peer side — Option B silent fast-forward. When ApplyCatchupDelta
    // set catchupFastForwardTarget to the leader's current frame, we
    // accelerate vanilla's cutscene tick to reach that target quickly
    // (visible as a ~1-second blur on the late-joiner's screen). Every
    // frame's setup commands (camera cache, Player teleport, dialogue
    // open/close, action locks) fire in-order via vanilla dispatch —
    // matching the leader's mid-cutscene state exactly.
    //
    // kMaxTicksPerRealFrame = 10 → 500 frames catchup ≈ 50 real frames
    // ≈ 0.8s at 60fps. Rate is empirically balanced: high enough to
    // finish quickly, low enough to avoid audio-command reordering
    // artifacts within a single real-frame audio window.
    if (catchupFastForwardTarget > 0) {
        constexpr int kMaxTicksPerRealFrame = 10;
        int ticksFired = 0;
        for (int i = 0; i < kMaxTicksPerRealFrame; i++) {
            if (gPlayState->csCtx.frames >= catchupFastForwardTarget) break;
            func_800645A0(gPlayState, &gPlayState->csCtx);
            ticksFired++;
        }
        if (gPlayState->csCtx.frames >= catchupFastForwardTarget) {
            SPDLOG_INFO("[CutsceneCatchup] Fast-forward complete — "
                        "csCtx.frames={} caught up to target={}",
                        (int)gPlayState->csCtx.frames,
                        catchupFastForwardTarget);
            catchupFastForwardTarget = 0;
        } else if (ticksFired > 0) {
            SPDLOG_INFO("[CutsceneCatchup] Fast-forward progress: frames={} / "
                        "target={} (ticked {}x this frame)",
                        (int)gPlayState->csCtx.frames,
                        catchupFastForwardTarget, ticksFired);
        }
    }
}
