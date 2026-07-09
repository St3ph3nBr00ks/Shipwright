#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Bridge/AnchorMessageBridge.h"
#include "soh/Network/Anchor/Bridge/AnchorSceneBridge.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/CutsceneCatchup.h"
#include "soh/Network/Anchor/Common/TeleportEffect.h"
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

// R4 (2026-07-09) — Tick sub-helpers extracted from TickCutsceneCatchup
// for readability. Each handles one concern of the ~350-line tick body.
// Behavior-preserving refactor; each helper's semantics documented at
// definition site in TickCutsceneCatchup.

// Broadcast the sparkle burst effect at (x,y,z) using the local
// player's own Anchor color. Fires both remotely (via SendPacket_
// TeleportEffect) and locally (SendPacket wraps the local spawn too).
// Called at BOTH departure and arrival positions of the teleport so
// observers see sparkles on both sides of the pop-in. See
// Common/TeleportEffect.h for the rendering primitive.
void BroadcastSparklesForOwnColor(float x, float y, float z) {
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return;
    auto it = anchor->clients.find(anchor->ownClientId);
    if (it == anchor->clients.end()) return;
    const auto& color = it->second.color;
    // env color = 60% intensity of prim for a subtle outer glow.
    const uint8_t envR = (uint8_t)((int)color.r * 3 / 5);
    const uint8_t envG = (uint8_t)((int)color.g * 3 / 5);
    const uint8_t envB = (uint8_t)((int)color.b * 3 / 5);
    anchor->SendPacket_TeleportEffect(x, y, z,
                                       color.r, color.g, color.b,
                                       envR, envG, envB);
}

// Bug 13 fix — deferred teleport apply. When Fix P.2 detected a room
// mismatch and initiated a room load, the teleport target was persisted
// on the Anchor instance. Each frame we check whether the load has
// completed (curRoom matches pending target) and apply once ready.
// Safety net: after 2s, apply regardless (better to place Link at
// leader coords with slightly wrong geometry than never place him).
void ApplyDeferredTeleportIfReady(std::chrono::steady_clock::time_point now) {
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return;
    if (!anchor->catchupDeferredTeleportValid) return;
    if (gPlayState == nullptr) return;

    static std::chrono::steady_clock::time_point sDeferredArmedAt =
        std::chrono::steady_clock::time_point::min();
    if (sDeferredArmedAt == std::chrono::steady_clock::time_point::min()) {
        sDeferredArmedAt = now;
    }
    const int8_t curRoom = (int8_t)gPlayState->roomCtx.curRoom.num;
    const bool roomMatches = (anchor->catchupPendingRoomSwitchTarget < 0) ||
                             (curRoom == anchor->catchupPendingRoomSwitchTarget);
    // Variant C (2026-07-09) — require the room to be BOTH matched AND
    // fully loaded before applying the teleport. `curRoom.num`
    // transitions synchronously at func_8009728C call time (z_room.c:589)
    // — well before the DMA completes and `curRoom.segment` is set by
    // func_800973FC. Reading roomMatches alone would fire the teleport
    // on the very next frame, potentially placing Link inside a room
    // whose mesh pointer is still null. Downstream Player_Update /
    // BgCheck reads then hit either a stale prior-room floor (wrong Y)
    // or no floor (void-out fall state). See
    // Analysis/peer_room_load_vs_cutscene_catchup_2026-07-09.md §4 R1/R2.
    //
    // The safetyExpired branch is retained unchanged so pathological
    // load-hang cases still resolve — better a mis-teleport than an
    // infinite hang.
    const bool roomReady = AnchorSceneBridge::IsCurrentRoomFullyLoaded();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - sDeferredArmedAt).count();
    const bool safetyExpired = ms > 2000;
    if (!((roomMatches && roomReady) || safetyExpired)) return;

    Player* peerLink = GET_PLAYER(gPlayState);
    if (peerLink != nullptr) {
        // Broadcast DEPARTURE sparkles at OLD position before we write
        // the new pos. Observers see the peer's DummyPlayer still at
        // OLD position when the packet arrives; sparkles fire there and
        // linger (30-frame particle life) after the DummyPlayer teleports
        // away — matches the "left behind" visual language of Farore's
        // Wind departures.
        BroadcastSparklesForOwnColor(peerLink->actor.world.pos.x,
                                     peerLink->actor.world.pos.y,
                                     peerLink->actor.world.pos.z);
        peerLink->actor.world.pos.x = anchor->catchupDeferredTeleportPos.x;
        peerLink->actor.world.pos.y = anchor->catchupDeferredTeleportPos.y;
        peerLink->actor.world.pos.z = anchor->catchupDeferredTeleportPos.z;
        peerLink->actor.shape.rot.y = anchor->catchupDeferredTeleportYaw;
        // Broadcast ARRIVAL sparkles at NEW position after write. Peer's
        // DummyPlayer follows via PLAYER_UPDATE (~1 tick later) so
        // observers see sparkles first, then the character resolves into
        // them — sage-arrival visual sequence.
        BroadcastSparklesForOwnColor(peerLink->actor.world.pos.x,
                                     peerLink->actor.world.pos.y,
                                     peerLink->actor.world.pos.z);
        SPDLOG_INFO("[CutsceneCatchup] Bug 13 fix — applied deferred "
                    "teleport to ({:.0f},{:.0f},{:.0f}) yaw={} after "
                    "room load (curRoom={} target={} matches={} "
                    "roomReady={} safetyExpired={} elapsedMs={})",
                    anchor->catchupDeferredTeleportPos.x,
                    anchor->catchupDeferredTeleportPos.y,
                    anchor->catchupDeferredTeleportPos.z,
                    (int)anchor->catchupDeferredTeleportYaw,
                    (int)curRoom,
                    (int)anchor->catchupPendingRoomSwitchTarget,
                    (int)roomMatches, (int)roomReady,
                    (int)safetyExpired,
                    (long long)ms);
    }
    anchor->catchupDeferredTeleportValid = false;
    anchor->catchupPendingRoomSwitchTarget = -1;
    sDeferredArmedAt = std::chrono::steady_clock::time_point::min();
}

// Variant C.2.3 (2026-07-09) — fade-to-white overlay driver.
//
// State machine documented on Anchor::catchupFadeState. Uses the vanilla
// envCtx.fillScreen + screenFillColor mechanism (same as
// Cutscene_Command_TransitionFX in z_demo.c:1349). We drive our own
// alpha ramping over time rather than piggy-backing on cutscene-command
// timing because the fade duration must correlate with catchup pipeline
// state, not a fixed frame range.
//
// Called at the END of TickCutsceneCatchup so vanilla cutscene commands
// during fast-forward don't clobber our writes (they run earlier via
// func_800645A0 loop).
void UpdateCatchupFadeOverlay(std::chrono::steady_clock::time_point now) {
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return;
    if (gPlayState == nullptr) return;
    if (anchor->catchupFadeState == Anchor::CatchupFadeState::INACTIVE) return;

    // Vanilla parity — default scene-transition fade is 60 units ≈ 1 s
    // (z_play.c:873; TRANS_TYPE_FADE_WHITE default). FAST=0.33s,
    // SLOW=2.5s. Matching the vanilla default keeps the fade cadence
    // familiar and non-jarring.
    constexpr int kFadeToWhiteMs   = 1000;
    constexpr int kFadeFromWhiteMs = 1000;
    constexpr int kIdleDebounceMs  = 200;
    constexpr int kSafetyTimeoutMs = 8000;

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - anchor->catchupFadeStateChangedAt).count();

    // Safety timeout — force fade-in if stuck for too long in any state
    // other than FADING_FROM_WHITE. Prevents a pathological stuck fade
    // from freezing the screen forever. FADING_FROM_WHITE has its own
    // bounded duration.
    if (anchor->catchupFadeState != Anchor::CatchupFadeState::FADING_FROM_WHITE &&
        elapsedMs > kSafetyTimeoutMs) {
        SPDLOG_WARN("[CutsceneCatchup] Variant C.2.3 — fade overlay safety "
                    "timeout after {} ms in state={}, forcing FADING_FROM_WHITE",
                    (long long)elapsedMs,
                    (int)anchor->catchupFadeState);
        anchor->catchupFadeState = Anchor::CatchupFadeState::FADING_FROM_WHITE;
        anchor->catchupFadeStateChangedAt = now;
        anchor->catchupFadeHoldIdleSince =
            std::chrono::steady_clock::time_point::min();
        return;
    }

    uint8_t alpha = 0;
    switch (anchor->catchupFadeState) {
        case Anchor::CatchupFadeState::FADING_TO_WHITE: {
            if (elapsedMs >= kFadeToWhiteMs) {
                anchor->catchupFadeState = Anchor::CatchupFadeState::HOLDING_WHITE;
                anchor->catchupFadeStateChangedAt = now;
                SPDLOG_INFO("[CutsceneCatchup] Variant C.2.3 — fade reached "
                            "HOLDING_WHITE");
                alpha = 255;
            } else {
                alpha = (uint8_t)((elapsedMs * 255) / kFadeToWhiteMs);
            }
            break;
        }
        case Anchor::CatchupFadeState::HOLDING_WHITE: {
            alpha = 255;
            const bool pipelineIdle =
                (anchor->catchupFastForwardTarget == 0) &&
                !anchor->catchupDeferredTeleportValid &&
                !anchor->catchupDeltaDeferred &&
                (anchor->catchupRequestGateArmedAt ==
                    std::chrono::steady_clock::time_point::min()) &&
                anchor->pendingCatchups.empty();
            if (pipelineIdle) {
                if (anchor->catchupFadeHoldIdleSince ==
                    std::chrono::steady_clock::time_point::min()) {
                    anchor->catchupFadeHoldIdleSince = now;
                }
                const auto idleMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - anchor->catchupFadeHoldIdleSince).count();
                if (idleMs >= kIdleDebounceMs) {
                    anchor->catchupFadeState =
                        Anchor::CatchupFadeState::FADING_FROM_WHITE;
                    anchor->catchupFadeStateChangedAt = now;
                    anchor->catchupFadeHoldIdleSince =
                        std::chrono::steady_clock::time_point::min();
                    SPDLOG_INFO("[CutsceneCatchup] Variant C.2.3 — pipeline "
                                "idle for {} ms; entering FADING_FROM_WHITE",
                                (long long)idleMs);
                }
            } else {
                // Reset idle debounce on any pipeline activity.
                anchor->catchupFadeHoldIdleSince =
                    std::chrono::steady_clock::time_point::min();
            }
            break;
        }
        case Anchor::CatchupFadeState::FADING_FROM_WHITE: {
            if (elapsedMs >= kFadeFromWhiteMs) {
                anchor->catchupFadeState = Anchor::CatchupFadeState::INACTIVE;
                anchor->catchupFadeStateChangedAt =
                    std::chrono::steady_clock::time_point::min();
                anchor->catchupFadeHoldIdleSince =
                    std::chrono::steady_clock::time_point::min();
                // Clear the fill so vanilla can reclaim the render.
                gPlayState->envCtx.fillScreen = false;
                gPlayState->envCtx.screenFillColor[3] = 0;
                SPDLOG_INFO("[CutsceneCatchup] Variant C.2.3 — fade complete, "
                            "INACTIVE");
                return;
            }
            alpha = (uint8_t)(
                ((kFadeFromWhiteMs - elapsedMs) * 255) / kFadeFromWhiteMs);
            break;
        }
        default:
            return;
    }

    // Write the vanilla fill-screen state. Rendering runs each frame in
    // Play_Draw's post-envCtx path.
    gPlayState->envCtx.fillScreen = true;
    gPlayState->envCtx.screenFillColor[0] = 255;
    gPlayState->envCtx.screenFillColor[1] = 255;
    gPlayState->envCtx.screenFillColor[2] = 255;
    gPlayState->envCtx.screenFillColor[3] = alpha;
}

// Per-frame leader-side chain-depth tracker. Detects msgCtx.msgLength
// changes while textId stays the same (indicates vanilla loaded next
// sub-textbox). Also detects textId changes → resets shadow via bridge.
// The chain-depth counter is retained for backward-compat with the
// (retired) Bug 14 mechanism; not currently consumed.
void UpdateLeaderChainDepthTracker() {
    auto* anchor = Anchor::Instance;
    if (anchor == nullptr) return;
    if (gPlayState == nullptr) return;

    const uint16_t curTextId = (uint16_t)gPlayState->msgCtx.textId;
    const uint16_t curMsgLen = (uint16_t)gPlayState->msgCtx.msgLength;
    if (curTextId != anchor->leaderChainTrackerLastTextId) {
        anchor->leaderChainTrackerLastTextId = curTextId;
        anchor->leaderChainTrackerLastMsgLen = curMsgLen;
        anchor->leaderMsgChainDepthCurrent   = 0;
        // Shadow reset — Message_StartTextbox has reset msgBufPos=0.
        AnchorMessageBridge::ResetShadowForNewTextbox();
    } else if (curTextId != 0 && curMsgLen != 0 &&
               curMsgLen != anchor->leaderChainTrackerLastMsgLen) {
        anchor->leaderChainTrackerLastMsgLen = curMsgLen;
        anchor->leaderMsgChainDepthCurrent++;
    }
}

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

    // Fix N + N.2 — leader's current message textId + Link world state.
    // Peer applies these AFTER fast-forward completes to (a) open the
    // matching textbox and (b) teleport Link to leader's pos. Absence
    // of the fields (older-build leader) is handled by receiver-side
    // defaults. See Analysis/cutscene_overshoot_and_teleport_2026-07-08.md.
    out["leaderMsgTextId"] = (int)e.leaderMsgTextId;
    if (e.hasLeaderPlayerSnapshot) {
        out["leaderPlayerPos"] = nlohmann::json::array({
            e.leaderPlayerPos.x, e.leaderPlayerPos.y, e.leaderPlayerPos.z });
        out["leaderPlayerYaw"] = (int)e.leaderPlayerYaw;
    }
    // Fix P.1 — leader's roomNum. Peer gates Fix N.2 teleport on
    // curRoom match to avoid placing Link in unloaded geometry.
    out["leaderRoomNum"] = (int)e.leaderRoomNum;
    // Design E — leader's msgBufPos + msgMode for direct-jump to
    // sub-textbox position on peer (retires Bug 14 chain-depth loop).
    out["leaderMsgBufPos"] = (int)e.leaderMsgBufPos;
    out["leaderMsgMode"]   = (int)e.leaderMsgMode;
    // Backward-compat field: kept in wire format at zero during
    // Design E rollout so older peer builds don't misapply. Retired
    // by Design E — see Common/CutsceneCatchup.h comment.
    out["leaderMsgChainDepth"] = 0;

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
    int setupRc = 1;   // default success (no-per-kind branch below sets 0)
    if (csKind == "deku_tree_intro") {
        setupRc = BgTreemouth_ForceIntroCutscene(gPlayState);
        SPDLOG_INFO("[CutsceneCatchup] Setup deku_tree_intro rc={}", setupRc);
    } else {
        setupRc = 0;
        SPDLOG_WARN("[CutsceneCatchup] No per-kind setup for csKind='{}' — "
                    "cutscene engine may lack script data",
                    csKind);
    }

    // If per-kind setup failed (e.g., BgTreemouth actor not loaded on this
    // client because we're in the wrong room), abort BEFORE arming fast-
    // forward. Without a successful setup, cutsceneTrigger is not set →
    // func_800645A0's inner block (cutsceneIndex >= 0xFFF0 gate) never
    // fires → csCtx.frames never advances → fast-forward loops infinitely
    // with frames=0 (observed in log 624). See Analysis/cutscene_ff_
    // reset_and_mp_dialogue_deadline_2026-07-08.md Finding 1.
    if (setupRc == 0) {
        SPDLOG_WARN("[CutsceneCatchup] Aborting fast-forward — per-kind "
                    "setup failed. Client may be in wrong room / missing "
                    "actor. Catchup will not run; local cutscene state "
                    "left as-is.");
        return;
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
    // Fix N + N.2 — extract leader's textbox + Link position from payload
    // and stash on the Anchor instance so TickCutsceneCatchup can apply
    // them the moment fast-forward completes. See Analysis/cutscene_
    // overshoot_and_teleport_2026-07-08.md.
    // Fix P.1 — also extract leader's roomNum so the teleport gates
    // on room match. See Analysis/cutscene_room_desync_and_vote_scope_
    // 2026-07-08.md.
    if (::Anchor::Instance != nullptr) {
        ::Anchor::Instance->catchupPendingMsgTextId =
            (uint16_t)payload.value("leaderMsgTextId", 0);
        ::Anchor::Instance->catchupPendingPlayerPosValid = false;
        if (payload.contains("leaderPlayerPos") &&
            payload["leaderPlayerPos"].is_array() &&
            payload["leaderPlayerPos"].size() == 3) {
            const auto& p = payload["leaderPlayerPos"];
            ::Anchor::Instance->catchupPendingPlayerPos.x =
                (float)p[0].get<float>();
            ::Anchor::Instance->catchupPendingPlayerPos.y =
                (float)p[1].get<float>();
            ::Anchor::Instance->catchupPendingPlayerPos.z =
                (float)p[2].get<float>();
            ::Anchor::Instance->catchupPendingPlayerYaw =
                (int16_t)payload.value("leaderPlayerYaw", 0);
            ::Anchor::Instance->catchupPendingPlayerPosValid = true;
        }
        ::Anchor::Instance->catchupPendingRoomNum =
            (int8_t)payload.value("leaderRoomNum", -1);
        // Design E — read leader's msgBufPos + msgMode so peer can
        // jump directly to leader's sub-textbox position after
        // Message_StartTextbox (retires Bug 14's broken chain-depth
        // loop). See Analysis/cutscene_catchup_dialogue_chain_design_
        // gap_2026-07-08.md §7 Design E.
        ::Anchor::Instance->catchupPendingMsgBufPos =
            (uint16_t)payload.value("leaderMsgBufPos", 0);
        ::Anchor::Instance->catchupPendingMsgMode =
            (uint8_t)payload.value("leaderMsgMode", 0);
        // Retired Bug 14 field — read as zero for back-compat.
        ::Anchor::Instance->catchupPendingMsgChainDepth = 0;
        SPDLOG_INFO("[CutsceneCatchup] Applied delta — leader textbox=0x{:04X} "
                    "playerPos={} yaw={} roomNum={} msgBufPos={} msgMode={} "
                    "(queued for post-fast-forward apply)",
                    (unsigned)::Anchor::Instance->catchupPendingMsgTextId,
                    ::Anchor::Instance->catchupPendingPlayerPosValid
                        ? "yes" : "absent",
                    (int)::Anchor::Instance->catchupPendingPlayerYaw,
                    (int)::Anchor::Instance->catchupPendingRoomNum,
                    (int)::Anchor::Instance->catchupPendingMsgBufPos,
                    (int)::Anchor::Instance->catchupPendingMsgMode);
    }
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
            // Fix O — record the FRAME_SYNC sender as the cutscene
            // originator on late-join. The sender is the leader by
            // definition (they broadcast the FRAME_SYNC while running
            // the cutscene). Enables vote-skip authority routing when
            // the peer late-joined via FRAME_SYNC-driven request.
            cutsceneOriginatorByKindKey[kindKey] =
                payload.value("clientId", (uint32_t)0);
            SPDLOG_INFO("[CutsceneCatchup] FRAME_SYNC observed new kindKey='{}' — "
                        "populating cutsceneStartActive for late-joiner mode "
                        "(originator=clientId {})",
                        kindKey, cutsceneOriginatorByKindKey[kindKey]);
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
        // Fix M — respect active fast-forward. If we're already
        // catching up (RESPONSE received + fast-forward armed), a
        // second FRAME_SYNC-driven REQUEST would trigger a duplicate
        // RESPONSE from the leader and re-populate pendingCatchups,
        // which caused the safety-net race in log 631 (Bug K).
        //
        // Variant C.2.1 (2026-07-09) extension — also treat deferred-
        // delta and deferred-teleport states as "catching up". Log 652
        // showed spurious REQUESTs firing during these windows even
        // though a catchup was clearly in progress.
        //
        // Variant C.2.3 (2026-07-09) extension — also treat the OnScene-
        // SpawnActors delay window as "catching up". Log 654 showed
        // FRAME_SYNC arriving ~400 ms after peer entered scene, bypassing
        // the 1 s Variant C.2.2 delay entirely and running the entire
        // fast-forward + teleport during the intended "peer sees room"
        // window. Result: fade + catchup engagement felt jarring because
        // there was no quiet moment before the catchup started. With
        // this gate, FRAME_SYNC waits for the delay window to elapse
        // before firing REQUEST — same UX as the OnSceneSpawnActors path.
        const bool alreadyCatchingUp =
            (catchupFastForwardTarget > 0) ||
            catchupDeltaDeferred ||
            catchupDeferredTeleportValid ||
            (catchupRequestGateArmedAt !=
                std::chrono::steady_clock::time_point::min());
        SPDLOG_INFO("[CutsceneCatchup] FRAME_SYNC gates — sender={} own={} "
                    "kindKey='{}' enabled={} alreadyPending={} selfBroadcast={} "
                    "validSender={} alreadyInCs={} alreadyCatchingUp={} — will{} fire",
                    senderClientId, ownClientId, kindKey,
                    haveEnabled, alreadyPending, selfBroadcast, validSender,
                    alreadyInCs, alreadyCatchingUp,
                    (validSender && !selfBroadcast && !alreadyPending &&
                     haveEnabled && !alreadyInCs && !alreadyCatchingUp)
                        ? "" : " NOT");
        if (validSender && !selfBroadcast && !alreadyPending && haveEnabled &&
            !alreadyInCs && !alreadyCatchingUp) {
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

    // Option 3 part 1 (2026-07-09) — live-read leader's message state at
    // RESPONSE send time. SerializeLedgerEntry copies the snapshot values
    // (captured at 1 Hz by SnapshotCamera / SnapshotActors). If the
    // leader advanced dialogue between the snapshot tick and the
    // REQUEST arriving, the peer will land at a stale sub-textbox
    // position and perpetually trail by one sub.
    //
    // Log 648 pattern: peer landed at msgBufPos=127 while leader was at
    // msgBufPos=331 (11 subs ahead). Overriding with the live shadow +
    // live msgCtx values here converges the peer to the leader's actual
    // position in the response. Companion fix in
    // SendPacket_CutsceneTextAdvanced covers ongoing drift; see
    // Analysis/log_648_snapshot_staleness_2026-07-09.md.
    payload["leaderMsgTextId"] = (int)gPlayState->msgCtx.textId;
    payload["leaderMsgBufPos"] =
        (int)AnchorMessageBridge::GetLeaderMsgBufPosLastSubStart();
    payload["leaderMsgMode"]   = (int)gPlayState->msgCtx.msgMode;

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

    // Variant C.2 (2026-07-09) — defer the entire delta apply pipeline
    // (music seek, per-kind setup, fast-forward arm, teleport queueing)
    // when the peer's room isn't fully loaded yet.
    //
    // Rationale (log 651 review): OnSceneSpawnActors nominally fires
    // AFTER func_800973FC completes room DMA, so IsCurrentRoomFullyLoaded
    // is typically true at RESPONSE-arrival time. But belt-and-suspenders
    // is warranted because:
    //   (a) FRAME_SYNC-driven late-join REQUEST path can fire without
    //       a preceding OnSceneSpawnActors edge (peer already in scene,
    //       receives FRAME_SYNC from mid-cutscene leader).
    //   (b) Cutscene_Command_TransitionFX case 24 (z_demo.c:380) nulls
    //       curRoom.segment mid-cutscene; a re-target RESPONSE (Fix R
    //       or safety net path) arriving in that window would apply
    //       against a null-segment room.
    //   (c) Peer's own room state may transiently be `status=1` if a
    //       concurrent Play_ChangeSceneRoom is in flight.
    //
    // In the common case (log 651 pattern), this gate lifts within one
    // game tick (elapsedMs=0 semantics). It only introduces a visible
    // wait in the pathological cases the belt-and-suspenders targets.
    //
    // Latest-RESPONSE-wins: if a second RESPONSE arrives while we're
    // still deferred, overwrite the payload (matches idempotent apply
    // semantics — Fix R's re-target flow relies on this).
    if (!AnchorSceneBridge::IsCurrentRoomFullyLoaded()) {
        const auto now = std::chrono::steady_clock::now();
        catchupDeltaDeferred            = true;
        catchupDeltaDeferredPayload     = payload;
        catchupDeltaDeferredKindKey     = kindKey;
        catchupDeltaDeferredArmedAt     = now;
        SPDLOG_INFO("[CutsceneCatchup] Variant C.2 — RESPONSE for '{}' "
                    "deferred until peer's room is fully loaded "
                    "(status={} segment={})",
                    kindKey,
                    (int)gPlayState->roomCtx.status,
                    (const void*)gPlayState->roomCtx.curRoom.segment);
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

bool Anchor::HasSameSceneMidCsPeer() const {
    // Variant C.2.3 helper — extracted from DetectAndRequestCutsceneCatchup's
    // peer scan loop. Returns true if there is at least one same-scene
    // same-timeline same-team peer currently mid-cutscene. Called from
    // OnSceneSpawnActors to decide whether to arm catchupRequestGateArmedAt
    // + catchupFadeState. Without this pre-check, entering ANY scene while
    // catchup is enabled would fire the 1 s delay + fade-to-white overlay
    // even when no peer is actually mid-cutscene — visible glitch.
    if (!isConnected) return false;
    if (gPlayState == nullptr) return false;
    const int16_t myScene = (int16_t)gPlayState->sceneNum;
    const uint8_t myTimeline = (uint8_t)(gSaveContext.linkAge & 0x1);
    const std::string myTeamId =
        CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    for (const auto& [cid, client] : clients) {
        if (cid == ownClientId) continue;
        if (client.self) continue;
        if (!client.online) continue;
        if (!client.isSaveLoaded) continue;
        if (client.sceneNum != myScene) continue;
        if ((uint8_t)(client.linkAge & 0x1) != myTimeline) continue;
        if (client.teamId != myTeamId) continue;
        if (client.csCtxState == 0 /* CS_STATE_IDLE */) continue;
        return true;
    }
    return false;
}

void Anchor::DetectAndRequestCutsceneCatchup() {
    if (!isConnected) return;
    if (gPlayState == nullptr) return;
    if (!IsSaveLoaded()) return;

    // Variant C.2.1 (2026-07-09) — "already engaged" gate.
    //
    // Mirror of the FRAME_SYNC direct-request path's alreadyCatchingUp +
    // alreadyInCs gates (HandlePacket_CutsceneFrameSync above). Without
    // these here, cutscene-driven Cutscene_Command_ChangeRoom fired
    // during peer's fast-forward triggers a fresh OnSceneSpawnActors
    // edge → DetectAndRequestCutsceneCatchup fires → fresh REQUEST →
    // fresh RESPONSE → fresh fast-forward → more cutscene commands.
    //
    // Log 652 pattern: peer walked into room 1 at 17.358 (REQUEST #1);
    // fast-forward #1's ChangeRoom re-triggered SceneSpawnActors at
    // 17.768 (REQUEST #2); Bug 13 room-load's ChangeRoom re-triggered
    // again at 18.578 (REQUEST #3). Each RESPONSE armed a new fast-
    // forward whose cutscene commands could null curRoom.segment
    // (Cutscene_Command_TransitionFX case 24 in z_demo.c:380) AFTER
    // Fix N.2 teleported Link into room 1 — user-visible "missing
    // geometry after teleport".
    //
    // Gate covers all in-flight catchup states:
    //   - catchupFastForwardTarget > 0: fast-forward loop is driving
    //     the local cutscene from RESPONSE-armed target.
    //   - catchupDeltaDeferred: RESPONSE arrived but is waiting for
    //     roomReady before applying (Variant C.2).
    //   - catchupDeferredTeleportValid: Bug 13 fix has queued a
    //     post-fast-forward teleport pending room load (Variant C).
    //   - pendingCatchups non-empty: REQUEST already sent, RESPONSE
    //     not yet received (covered by the per-kindKey check below
    //     too; belt-and-suspenders here).
    //   - Play_InCsMode: peer is already locally in a cutscene state
    //     (catchup would replay commands the local cutscene is
    //     already running).
    if (catchupFastForwardTarget > 0 ||
        catchupDeltaDeferred ||
        catchupDeferredTeleportValid ||
        !pendingCatchups.empty() ||
        Play_InCsMode(gPlayState)) {
        return;
    }

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

    // R4-extracted helpers. See anonymous namespace above for
    // implementation. Each corresponds to one concern of this tick.
    ApplyDeferredTeleportIfReady(now);
    UpdateLeaderChainDepthTracker();

    // Variant C.2.2 (2026-07-09) — scene-entry REQUEST delay.
    //
    // OnSceneSpawnActors arms catchupRequestGateArmedAt. This poll
    // fires DetectAndRequestCutsceneCatchup once the configured delay
    // has elapsed, then disarms so it fires exactly once per scene
    // entry. Default 1000 ms; user-tuneable via
    // gEnhancements.Anchor.CutsceneLateJoinRequestDelayMs
    // (0 = fire immediately, matches pre-C.2.2 behavior).
    //
    // See Anchor.h catchupRequestGateArmedAt for full rationale.
    if (catchupRequestGateArmedAt !=
        std::chrono::steady_clock::time_point::min()) {
        const int64_t delayMs = (int64_t)CVarGetInteger(
            CVAR_ENHANCEMENT("Anchor.CutsceneLateJoinRequestDelayMs"), 1000);
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - catchupRequestGateArmedAt).count();
        if (elapsedMs >= delayMs) {
            SPDLOG_INFO("[CutsceneCatchup] Variant C.2.2 — request delay "
                        "elapsed ({} ms >= {} ms); firing "
                        "DetectAndRequestCutsceneCatchup",
                        (long long)elapsedMs, (long long)delayMs);
            catchupRequestGateArmedAt =
                std::chrono::steady_clock::time_point::min();
            DetectAndRequestCutsceneCatchup();
        }
    }

    // Variant C.2 (2026-07-09) — apply a deferred RESPONSE delta once
    // the peer's room is fully loaded. Ordering intent:
    //
    //   1. This block runs BEFORE the fast-forward loop below, so a
    //      RESPONSE that arrives on the same tick as the room becoming
    //      ready applies + arms fast-forward all in this tick — no
    //      wasted frame.
    //   2. After apply, pendingCatchups is erased for this kindKey so
    //      the deadline-enforcement loop below doesn't drop it.
    //   3. 2 s safety timeout mirrors ApplyDeferredTeleportIfReady:
    //      better a stale-context apply than an infinite hang if the
    //      room load pathologically never completes.
    //
    // See Analysis/peer_room_load_vs_cutscene_catchup_2026-07-09.md.
    if (catchupDeltaDeferred) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - catchupDeltaDeferredArmedAt).count();
        const bool roomReady = AnchorSceneBridge::IsCurrentRoomFullyLoaded();
        const bool safetyExpired = ms > 2000;
        if (roomReady || safetyExpired) {
            const std::string kindKey = catchupDeltaDeferredKindKey;
            nlohmann::json payload = catchupDeltaDeferredPayload;
            // Clear state BEFORE apply so any nested writes don't
            // re-trigger this branch on the same tick.
            catchupDeltaDeferred = false;
            catchupDeltaDeferredPayload = nlohmann::json{};
            catchupDeltaDeferredKindKey.clear();
            catchupDeltaDeferredArmedAt =
                std::chrono::steady_clock::time_point::min();
            SPDLOG_INFO("[CutsceneCatchup] Variant C.2 — applying deferred "
                        "RESPONSE for '{}' (roomReady={} safetyExpired={} "
                        "elapsedMs={})",
                        kindKey, (int)roomReady, (int)safetyExpired,
                        (long long)ms);
            ApplyCatchupDelta(payload);
            pendingCatchups.erase(kindKey);
        }
    }

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
        const int32_t framesBefore = (int32_t)gPlayState->csCtx.frames;
        for (int i = 0; i < kMaxTicksPerRealFrame; i++) {
            if (gPlayState->csCtx.frames >= catchupFastForwardTarget) break;
            // Fix L.2 — clear message state via the bridge so
            // vanilla's Cutscene_Command_Textbox rewind mechanism
            // doesn't pin csFrames. See Analysis/cutscene_fix_l_-
            // insufficient_2026-07-08.md and AnchorMessageBridge.h.
            AnchorMessageBridge::ClearMessageStateForFastForwardTick();
            func_800645A0(gPlayState, &gPlayState->csCtx);
            ticksFired++;
        }
        const int32_t framesAfter = (int32_t)gPlayState->csCtx.frames;

        // Fix D — fast-forward timeout defense-in-depth. If csCtx.frames
        // stops advancing while catchupFastForwardTarget > 0, the cutscene
        // engine is either not running (cutsceneTrigger not set, e.g. per-
        // kind setup failed) or is stuck on an internal WAIT state we can't
        // drive forward. Without this timeout, the loop pings func_800645A0
        // forever with no progress, spamming per-frame log lines. Fix A
        // (setup rc=0 bail-out) catches the main case at entry; this catches
        // any residual runtime stall.
        //
        // Counter increments when a real-frame block of ticks made zero
        // progress; resets on any progress. Threshold ~60 real frames
        // (~1 second at 60fps) is generous — real fast-forward typically
        // completes in <60 real frames total. See analysis Finding 4.
        static int sConsecutiveNoProgressFrames = 0;
        constexpr int kNoProgressThresholdFrames = 60;
        if (framesAfter == framesBefore) {
            sConsecutiveNoProgressFrames++;
            if (sConsecutiveNoProgressFrames >= kNoProgressThresholdFrames) {
                SPDLOG_WARN("[CutsceneCatchup] Fast-forward stalled — "
                            "csCtx.frames={} target={} unchanged for {} "
                            "real frames. Aborting fast-forward; local "
                            "cutscene will proceed at normal rate if the "
                            "engine is running, or remain frozen otherwise.",
                            (int)gPlayState->csCtx.frames,
                            catchupFastForwardTarget,
                            sConsecutiveNoProgressFrames);
                catchupFastForwardTarget = 0;
                sConsecutiveNoProgressFrames = 0;
            }
        } else {
            sConsecutiveNoProgressFrames = 0;
        }

        if (gPlayState->csCtx.frames >= catchupFastForwardTarget) {
            SPDLOG_INFO("[CutsceneCatchup] Fast-forward complete — "
                        "csCtx.frames={} caught up to target={}",
                        (int)gPlayState->csCtx.frames,
                        catchupFastForwardTarget);
            catchupFastForwardTarget = 0;
            sConsecutiveNoProgressFrames = 0;
            // Fix N — restore leader's textbox on peer. Fast-forward's
            // Fix L.2 msgLength=0 write suppresses vanilla's textbox
            // rewind mechanism; without restoration, vanilla's normal
            // tick after loop exit advances csCtx.frames past leader's
            // target and opens the NEXT textbox naturally (log 633
            // overshoot to textbox 0x1017). Message_StartTextbox
            // reloads the text data, sets msgLength to non-zero, and
            // primes vanilla's rewind check at csFrames >= endFrame.
            // Peer now holds at leader's textbox until vote-skip
            // advance fires.
            if (catchupPendingMsgTextId != 0) {
                Message_StartTextbox(gPlayState,
                                     (u16)catchupPendingMsgTextId,
                                     nullptr);
                SPDLOG_INFO("[CutsceneCatchup] Fix N — opened leader's "
                            "textbox 0x{:04X} on peer post-fast-forward",
                            (unsigned)catchupPendingMsgTextId);
                // Design E v2 — jump peer's message system directly
                // to leader's sub-textbox position by force-writing
                // ONLY msgCtx.msgBufPos. Do NOT override msgMode.
                // Message_StartTextbox above set msgMode=TEXT_START.
                // Vanilla's next ~10 game frames flow through
                // TEXT_START → BOX_GROWING (×8) → STARTING → NEXT_MSG
                // naturally, setting up the visible textbox state
                // (R_TEXTBOX_Y from R_TEXTBOX_Y_TARGET via
                // Message_GrowTextbox; textboxColorAlphaCurrent grown
                // to target so background fades in properly). When
                // NEXT_MSG fires, Message_Decode reads from
                // msgBufPos = leader's forced value and produces the
                // correct sub-textbox content. See
                // Analysis/design_e_rendering_side_effects_2026-07-08.md.
                //
                // Log 641 v1 bugs (fixed here):
                //   Bug 1: no dark background — bypassed Message_
                //          GrowTextbox which grows textboxColorAlpha.
                //   Bug 2: text on top half — bypassed TEXT_START
                //          which sets R_TEXTBOX_Y_TARGET based on
                //          textBoxType via sTextboxLowerYPositions[].
                //
                // Replaces the broken Bug 14 loop (Message_ContinueTextbox
                // — that vanilla function restarts the message with
                // msgBufPos=0, so calling it N times just re-opened
                // sub 1 N times). See Analysis/cutscene_catchup_
                // dialogue_chain_design_gap_2026-07-08.md §4.
                //
                // Only apply if msgBufPos > 0 — msgBufPos=0 is the
                // fresh Message_StartTextbox state and needs no
                // adjustment.
                //
                // Deferred (potential Bug 3): if leader's captured
                // msgBufPos points AT a BOX_BREAK (the case when
                // leader is in AWAIT_NEXT / DISPLAYING at snapshot),
                // peer's Message_Decode from that position produces
                // empty content. Peer would display correct visual
                // chrome but no text. If field test confirms this,
                // capture "start of current sub" via a shadow field
                // updated at Fix S/T's msgBufPos++ transitions.
                if (catchupPendingMsgBufPos > 0) {
                    const uint16_t priorPos =
                        (uint16_t)gPlayState->msgCtx.msgBufPos;
                    AnchorMessageBridge::JumpToLeaderSubTextboxPosition(
                        catchupPendingMsgBufPos);
                    SPDLOG_INFO("[CutsceneCatchup] Design E v2 — jumped "
                                "peer msgBufPos from {} to leader's {} "
                                "(leaderMsgMode was {}) for textId 0x"
                                "{:04X}",
                                (int)priorPos,
                                (int)catchupPendingMsgBufPos,
                                (int)catchupPendingMsgMode,
                                (unsigned)catchupPendingMsgTextId);
                }
                catchupPendingMsgTextId = 0;
                catchupPendingMsgBufPos = 0;
                catchupPendingMsgMode   = 0;
                catchupPendingMsgChainDepth = 0;
            }
            // Fix N.2 — teleport peer's Link to leader's world position.
            // Fast-forward's csCtx.frames advances 10x per real frame
            // while Player_Update runs 1x per real frame, so Link's
            // cutscene walk animation is truncated. Explicit teleport
            // to leader's pos gets Link visually into the correct
            // final position. Silent-fast-forward's design tradeoff:
            // the walk animation itself is a brief blur.
            //
            // Fix P.2 — gate the teleport on room match. Log 634 Bug 9
            // showed that vanilla's mid-fast-forward Cutscene_Command_
            // ChangeRoom put peer in room 0 while leader was in room 1
            // at snapshot time. Teleporting Link to leader's coords
            // placed him inside room 1's geometry (unloaded on peer)
            // = void / pink fog. Safer to leave Link where the vanilla
            // dispatch put him when rooms mismatch — the visual
            // outcome will be "Link stops mid-walk" rather than
            // "Link in unloaded geometry". Cannot verify: whether
            // calling Play_ChangeSceneRoom to switch peer to the
            // leader's room mid-cutscene is safe; deferred pending
            // field-test signal.
            if (catchupPendingPlayerPosValid) {
                const int8_t curRoom =
                    (int8_t)gPlayState->roomCtx.curRoom.num;
                if (catchupPendingRoomNum >= 0 &&
                    curRoom != catchupPendingRoomNum) {
                    // Bug 13 fix — load leader's room BEFORE the
                    // teleport instead of skipping. Peer's fast-forward
                    // drove vanilla's cutscene-layer dispatch which
                    // reset curRoom to 0. Leader's snapshot roomNum
                    // is authoritative for the current cutscene phase.
                    // func_8009728C initiates the room DMA/OTR load
                    // via the vanilla room-load pipeline. The teleport
                    // is deferred to a later frame once curRoom
                    // matches; see per-frame check below. See
                    // Analysis/cutscene_room_reload_and_sub_textbox_
                    // 2026-07-08.md Bug 13.
                    const int32_t rc = func_8009728C(gPlayState,
                                                     &gPlayState->roomCtx,
                                                     (int32_t)catchupPendingRoomNum);
                    SPDLOG_INFO("[CutsceneCatchup] Bug 13 fix — initiated "
                                "room load: curRoom={} → leaderRoomNum={} "
                                "(func_8009728C rc={}); teleport to "
                                "({:.0f},{:.0f},{:.0f}) yaw={} deferred "
                                "until load completes",
                                (int)curRoom,
                                (int)catchupPendingRoomNum,
                                rc,
                                catchupPendingPlayerPos.x,
                                catchupPendingPlayerPos.y,
                                catchupPendingPlayerPos.z,
                                (int)catchupPendingPlayerYaw);
                    // Persist the teleport target on Anchor state; the
                    // per-frame poll further down runs each tick and
                    // applies once curRoom == pendingSwitchTarget.
                    catchupPendingRoomSwitchTarget =
                        catchupPendingRoomNum;
                    catchupDeferredTeleportPos.x =
                        catchupPendingPlayerPos.x;
                    catchupDeferredTeleportPos.y =
                        catchupPendingPlayerPos.y;
                    catchupDeferredTeleportPos.z =
                        catchupPendingPlayerPos.z;
                    catchupDeferredTeleportYaw =
                        catchupPendingPlayerYaw;
                    catchupDeferredTeleportValid = true;
                } else if (!AnchorSceneBridge::IsCurrentRoomFullyLoaded()) {
                    // Variant C (2026-07-09) — room number matches but
                    // the mesh isn't live yet. Two ways this happens:
                    //   (a) mid-DMA state (rare — post-fast-forward the
                    //       load should be complete, but defense-in-depth).
                    //   (b) Cutscene_Command_TransitionFX case 24 nulled
                    //       curRoom.segment during fast-forward
                    //       (z_demo.c:380). Segment stays null until a
                    //       later cutscene script command restores it.
                    // Route through the deferred-teleport path so
                    // ApplyDeferredTeleportIfReady's per-tick gate can
                    // apply once the segment is live (or the 2 s safety
                    // fires). See Analysis §4 R4.
                    catchupPendingRoomSwitchTarget = -1;  // no room-load
                                                          // needed — num
                                                          // already matches
                    catchupDeferredTeleportPos.x =
                        catchupPendingPlayerPos.x;
                    catchupDeferredTeleportPos.y =
                        catchupPendingPlayerPos.y;
                    catchupDeferredTeleportPos.z =
                        catchupPendingPlayerPos.z;
                    catchupDeferredTeleportYaw =
                        catchupPendingPlayerYaw;
                    catchupDeferredTeleportValid = true;
                    SPDLOG_INFO("[CutsceneCatchup] Variant C — curRoom={} "
                                "matches leaderRoomNum={} but mesh not live "
                                "(status={} segment={}); deferring teleport "
                                "to ({:.0f},{:.0f},{:.0f}) yaw={}",
                                (int)curRoom,
                                (int)catchupPendingRoomNum,
                                (int)gPlayState->roomCtx.status,
                                (const void*)gPlayState->roomCtx.curRoom.segment,
                                catchupPendingPlayerPos.x,
                                catchupPendingPlayerPos.y,
                                catchupPendingPlayerPos.z,
                                (int)catchupPendingPlayerYaw);
                } else {
                    Player* peerLink = GET_PLAYER(gPlayState);
                    if (peerLink != nullptr) {
                        // Departure sparkles at OLD pos before overwrite.
                        // See BroadcastSparklesForOwnColor comment above
                        // for full rationale.
                        BroadcastSparklesForOwnColor(
                            peerLink->actor.world.pos.x,
                            peerLink->actor.world.pos.y,
                            peerLink->actor.world.pos.z);
                        peerLink->actor.world.pos.x = catchupPendingPlayerPos.x;
                        peerLink->actor.world.pos.y = catchupPendingPlayerPos.y;
                        peerLink->actor.world.pos.z = catchupPendingPlayerPos.z;
                        peerLink->actor.shape.rot.y = catchupPendingPlayerYaw;
                        // Arrival sparkles at NEW pos after write.
                        BroadcastSparklesForOwnColor(
                            peerLink->actor.world.pos.x,
                            peerLink->actor.world.pos.y,
                            peerLink->actor.world.pos.z);
                        SPDLOG_INFO("[CutsceneCatchup] Fix N.2 — teleported "
                                    "peer Link to leader pos=({:.0f},{:.0f},"
                                    "{:.0f}) yaw={} (curRoom={} matches "
                                    "leaderRoomNum={}, mesh live)",
                                    catchupPendingPlayerPos.x,
                                    catchupPendingPlayerPos.y,
                                    catchupPendingPlayerPos.z,
                                    (int)catchupPendingPlayerYaw,
                                    (int)curRoom,
                                    (int)catchupPendingRoomNum);
                    }
                }
                catchupPendingPlayerPosValid = false;
                catchupPendingRoomNum = -1;
            }
        } else if (ticksFired > 0 && framesAfter != framesBefore) {
            SPDLOG_INFO("[CutsceneCatchup] Fast-forward progress: frames={} / "
                        "target={} (ticked {}x this frame)",
                        (int)gPlayState->csCtx.frames,
                        catchupFastForwardTarget, ticksFired);
        }
    }

    // Fix R — continuous re-target on textId mismatch.
    //
    // After fast-forward completes, the peer's csCtx.frames matches the
    // snapshot target, but by then the leader may have advanced through
    // a camera pan into the next textbox. The peer resumes 1x playback
    // and (from the user's view) "auto-advance stopped" because the next
    // textbox arrives seconds later. Log 637 pattern: P2 loaded the
    // correct room but stayed on the wrong dialogue after a mid-cutscene
    // camera movement.
    //
    // Mechanism: leader's ongoing CUTSCENE_TEXT_VOTE_STATE broadcasts
    // carry the leader's current msgTextId. When peer is in-cutscene,
    // fast-forward is idle, no catchup is pending, and the leader's
    // textId ≠ peer's local textId, issue a fresh CUTSCENE_CATCHUP_-
    // REQUEST. The leader re-snapshots and re-sends the ledger; peer's
    // ApplyCatchupDelta re-arms fast-forward to the new target.
    //
    // Self-limiting: stops firing once textIds match (or cutscene ends).
    // Rate-limited: at most one re-target per kReTargetIntervalMs to
    // avoid storming the leader when it legitimately holds on a long
    // sub-textbox chain.
    if (catchupFastForwardTarget == 0 &&
        pendingCatchups.empty() &&
        Play_InCsMode(gPlayState) &&
        cutsceneTextAdvanceState.active &&
        cutsceneTextAdvanceState.textId != 0 &&
        cutsceneTextAdvanceState.textId !=
            (uint16_t)gPlayState->msgCtx.textId) {

        constexpr int kReTargetIntervalMs = 2000;
        const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - catchupLastReTargetRequestAt).count();
        if (catchupLastReTargetRequestAt ==
                std::chrono::steady_clock::time_point::min() ||
            sinceLast >= kReTargetIntervalMs) {

            // Find the active cutscene we're catching up on. v1 assumes
            // single-cutscene sessions — pick the first entry of
            // cutsceneStartActive whose originator we know.
            std::string csKindKey;
            uint32_t leaderClientId = 0;
            for (const auto& key : cutsceneStartActive) {
                const auto it = cutsceneOriginatorByKindKey.find(key);
                if (it != cutsceneOriginatorByKindKey.end() &&
                    it->second != 0 && it->second != ownClientId) {
                    csKindKey       = key;
                    leaderClientId  = it->second;
                    break;
                }
            }

            if (!csKindKey.empty() && leaderClientId != 0) {
                // Split "<csKind>:<csKey>" back into components. csKey
                // is a uint32_t formatted as decimal in the key. Guard
                // against malformed keys just in case.
                const auto colonPos = csKindKey.rfind(':');
                if (colonPos != std::string::npos) {
                    const std::string csKind = csKindKey.substr(0, colonPos);
                    uint32_t csKey = 0;
                    try {
                        csKey = (uint32_t)std::stoul(csKindKey.substr(colonPos + 1));
                    } catch (...) {
                        csKey = 0;
                    }

                    SPDLOG_INFO("[CutsceneCatchup] Fix R — re-target: peer "
                                "textId=0x{:04X} lags leader textId=0x{:04X} "
                                "(csFrames={}); requesting fresh catchup "
                                "delta for '{}' from leader clientId={}",
                                (unsigned)gPlayState->msgCtx.textId,
                                (unsigned)cutsceneTextAdvanceState.textId,
                                (int)gPlayState->csCtx.frames,
                                csKindKey, leaderClientId);

                    catchupLastReTargetRequestAt = now;
                    SendPacket_CutsceneCatchupRequest(leaderClientId,
                                                     csKind, csKey);
                }
            }
        }
    }

    // Reset the re-target rate limiter whenever textIds match. Prevents
    // the 2s cooldown from carrying across a legitimate advance (peer
    // catches up naturally → user advances the dialogue → next mismatch
    // should trigger immediately, not wait for the cooldown).
    if (catchupFastForwardTarget == 0 &&
        cutsceneTextAdvanceState.active &&
        cutsceneTextAdvanceState.textId ==
            (uint16_t)gPlayState->msgCtx.textId &&
        catchupLastReTargetRequestAt !=
            std::chrono::steady_clock::time_point::min()) {
        catchupLastReTargetRequestAt =
            std::chrono::steady_clock::time_point::min();
    }

    // Variant C.2.3 (2026-07-09) — drive the fade-to-white overlay
    // AFTER all pipeline work + fast-forward is complete for this tick.
    // Placement ensures our envCtx.fillScreen + screenFillColor writes
    // are the last for the frame; vanilla cutscene commands that ran
    // during fast-forward (Cutscene_Command_TransitionFX) cannot
    // subsequently clobber our overlay. See UpdateCatchupFadeOverlay
    // in the anonymous namespace above.
    UpdateCatchupFadeOverlay(now);
}
