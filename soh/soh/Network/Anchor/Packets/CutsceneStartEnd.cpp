#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/JsonConversions.hpp"  // Vec3f to_json / from_json

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
// Pending-cutscene buffer (Bug 3 — log 184 GDT meets Link CS)
// ---------------------------------------------------------------------

// TTL for buffered CUTSCENE_START packets. 100 frames at 20 Hz ≈ 5 s.
// Long enough to cover normal loading-zone traversal (the user-observed
// case was P2 ~3 s behind P1 entering Inside Great Deku Tree); short
// enough that a buffered START whose target scene is never visited
// ages out instead of firing on a future unrelated scene visit.
static constexpr uint16_t kPendingCutsceneTtlFrames = 100;

void Anchor::BufferPendingCutsceneStart(const nlohmann::json& payload,
                                        int16_t sceneNum, uint8_t timeline,
                                        const std::string& csKind, int32_t csKey) {
    // Replace any existing entry with the same key — host re-sending START
    // for the same CS (different csState) overwrites the buffered copy.
    for (auto& p : pendingCutsceneStarts) {
        if (p.sceneNum == sceneNum && p.timeline == timeline &&
            p.csKind == csKind && p.csKey == csKey) {
            p.payload   = payload;
            p.ttlFrames = kPendingCutsceneTtlFrames;
            return;
        }
    }
    pendingCutsceneStarts.push_back(
        { payload, sceneNum, timeline, csKind, csKey, kPendingCutsceneTtlFrames });
}

void Anchor::RemovePendingCutsceneStart(int16_t sceneNum, uint8_t timeline,
                                        const std::string& csKind, int32_t csKey) {
    for (auto it = pendingCutsceneStarts.begin(); it != pendingCutsceneStarts.end(); ++it) {
        if (it->sceneNum == sceneNum && it->timeline == timeline &&
            it->csKind == csKind && it->csKey == csKey) {
            pendingCutsceneStarts.erase(it);
            return;
        }
    }
}

void Anchor::ReplayPendingCutsceneStartsForScene(int16_t sceneNum, uint8_t timeline) {
    if (pendingCutsceneStarts.empty()) return;
    // Snapshot matching entries before replay so the receive-handler's
    // own bookkeeping mutations (MarkCutsceneActive etc.) don't invalidate
    // the iteration. Entries are removed from the buffer as we go.
    std::vector<nlohmann::json> toReplay;
    for (auto it = pendingCutsceneStarts.begin(); it != pendingCutsceneStarts.end(); ) {
        if (it->sceneNum == sceneNum && it->timeline == timeline) {
            toReplay.push_back(it->payload);
            it = pendingCutsceneStarts.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& payload : toReplay) {
        SPDLOG_INFO("[CutsceneStart] Replaying buffered START for sceneNum=0x{:02X} (peer transitioned in)",
                    (int)sceneNum);
        HandlePacket_CutsceneStart(payload);
    }
}

void Anchor::TickPendingCutsceneStarts() {
    for (auto it = pendingCutsceneStarts.begin(); it != pendingCutsceneStarts.end(); ) {
        if (it->ttlFrames == 0) {
            SPDLOG_INFO("[CutsceneStart] Pending START expired — sceneNum=0x{:02X} csKind={} csKey={}",
                        (int)it->sceneNum, it->csKind, it->csKey);
            it = pendingCutsceneStarts.erase(it);
        } else {
            it->ttlFrames--;
            ++it;
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

    // Schema 2 — roomNum field added for same-room gate (log 174).
    // Cutscenes are scoped to the room they fire in; peers in a
    // different room of the same scene must NOT have csCtx.state
    // forced — that input-locks Link mid-room-transition with
    // unstable floor collision and triggers a void-fall.
    const int8_t roomNum = (int8_t)gPlayState->roomCtx.curRoom.num;

    // Schema 3 — triggerPos / triggerRotY for cutscene-trigger position
    // (log 178/179 fix). Captured from the local Player's position at
    // the moment the cutscene edge fires. On the receive side, peer's
    // Link is teleported to this position before csKey is applied to
    // gSaveContext.cutsceneIndex. Without this, savecontext cutscenes
    // play with peer's Link at its current location while the cutscene
    // script's camera flies to scene-coordinate locations the Link is
    // not standing at — visible as "Link's model floats in the void
    // while camera focuses on something else." Boss_Goma actor-
    // internal cutscenes also benefit (peer who entered the boss room
    // from a corner gets snapped near the boss for the intro).
    Vec3f  triggerPos  = { 0.0f, 0.0f, 0.0f };
    int16_t triggerRotY = 0;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer != nullptr) {
        triggerPos  = localPlayer->actor.world.pos;
        triggerRotY = localPlayer->actor.shape.rot.y;
    }

    nlohmann::json payload;
    payload["type"]          = CUTSCENE_START;
    payload["sceneNum"]      = (int16_t)gPlayState->sceneNum;
    payload["roomNum"]       = roomNum;
    payload["csKind"]        = csKind;
    payload["csKey"]         = csKey;
    payload["csState"]       = csState;
    payload["triggerPos"]    = triggerPos;
    payload["triggerRotY"]   = triggerRotY;
    payload["ownerClientId"] = ownClientId;
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[CutsceneStart] Sending kind={} key={} state={} scene=0x{:02X} room={} pos=({:.0f},{:.0f},{:.0f})",
                csKind, csKey, (int)csState, (int)gPlayState->sceneNum, (int)roomNum,
                triggerPos.x, triggerPos.y, triggerPos.z);

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
    // Bug 3 (log 184) — when peer is in a different scene than the
    // cutscene's, BUFFER instead of dropping. Common race: host enters
    // a loading zone (e.g. Inside Great Deku Tree) and immediately
    // triggers the entry cutscene; peer is still 2-3 s behind in the
    // outgoing scene. Without buffering, peer's ValidateSameScene drops
    // the START, peer enters the new scene with no synced cutscene
    // state, and host plays the CS alone. With buffering, peer
    // transitions in and replays the buffered START via
    // ReplayPendingCutsceneStartsForScene (called from
    // OnSceneSpawnActors). TTL-bounded so a buffered entry whose target
    // scene is never visited ages out instead of firing on a future
    // unrelated scene match.
    if (gPlayState == nullptr || (int16_t)gPlayState->sceneNum != sceneNum) {
        const std::string csKindForBuffer  = payload.value("csKind", std::string("actor_unknown"));
        const int32_t     csKeyForBuffer   = payload.value("csKey",  (int32_t)-1);
        const uint8_t     timelineForBuffer = (uint8_t)(gSaveContext.linkAge & 1);
        BufferPendingCutsceneStart(payload, sceneNum, timelineForBuffer,
                                   csKindForBuffer, csKeyForBuffer);
        SPDLOG_INFO("[CutsceneStart] Buffered — peer in scene 0x{:02X} but CS is for scene 0x{:02X} (csKind={} csKey={})",
                    gPlayState != nullptr ? (int)gPlayState->sceneNum : -1,
                    (int)sceneNum, csKindForBuffer, csKeyForBuffer);
        return;
    }

    // Same-room gate (log 174 fix). Cutscenes are scoped to the room
    // they fire in. Applying csCtx.state to a peer in a different room
    // of the same scene input-locks Link via Player_SetCsActionWithHaltedActors
    // — which can trigger a void-fall when Link is mid-room-transition
    // (floor collision unstable during door-cross). Drop the apply if
    // local room doesn't match the cutscene's room. Schema-2 field; if
    // a pre-schema-2 sender omits roomNum we get the default sentinel
    // -1 which will mismatch any real room — defensive but means
    // legacy peers' cutscenes won't propagate. Acceptable since both
    // ends of a session are running the same build.
    const int8_t senderRoomNum = payload.value("roomNum", (int8_t)-1);
    const int8_t localRoomNum  = (gPlayState != nullptr)
                                     ? (int8_t)gPlayState->roomCtx.curRoom.num
                                     : (int8_t)-1;
    if (senderRoomNum >= 0 && localRoomNum != senderRoomNum) {
        SPDLOG_INFO("[CutsceneStart] Dropped — room mismatch (sender room={} local room={}); peer not in cutscene's room",
                    (int)senderRoomNum, (int)localRoomNum);
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

    // Schema 3 — teleport peer's Link to the cutscene-trigger position
    // BEFORE applying csKey (logs 178/179 fix). Without this, savecontext
    // cutscenes play with peer's Link at an unrelated position while the
    // cutscene script's camera flies to scene-coordinate locations
    // unrelated to peer's actual location — visible as "Link's model
    // floating in the void." For Boss_Goma actor-internal cutscenes,
    // this also pulls peer in from a wall corner of the boss arena to a
    // sensible viewing position next to the host.
    //
    // Pre-cutscene Link state is replaced; the cutscene's own
    // PLAYER_CUEID frames take over Link's position immediately after
    // (typically frame 1) so the teleport-then-script handoff is clean.
    // Defensive: only teleport if both pos values look sane (non-zero
    // sentinel). A pre-schema-3 sender omits the field; we get the
    // default zero-initialized Vec3f and skip the teleport rather than
    // moving Link to (0,0,0).
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer != nullptr && payload.contains("triggerPos")) {
        Vec3f triggerPos = payload.value("triggerPos", Vec3f{ 0.0f, 0.0f, 0.0f });
        int16_t triggerRotY = payload.value("triggerRotY", (int16_t)0);
        const bool posIsSane = (triggerPos.x != 0.0f || triggerPos.y != 0.0f ||
                                triggerPos.z != 0.0f);
        if (posIsSane) {
            localPlayer->actor.world.pos = triggerPos;
            localPlayer->actor.shape.rot.y = triggerRotY;
            localPlayer->actor.world.rot.y = triggerRotY;
            // Zero velocity so post-teleport gravity doesn't make Link
            // sliding-fall through any momentary geometry inconsistency.
            localPlayer->actor.velocity.x = 0.0f;
            localPlayer->actor.velocity.y = 0.0f;
            localPlayer->actor.velocity.z = 0.0f;
            localPlayer->actor.speedXZ = 0.0f;
            // Log 183 P2 crash root cause — refresh bgCheck after teleport.
            // Without this, Link's `floorPoly` stays whatever it was at the
            // pre-cutscene location (or NULL if Link was airborne / void).
            // Cutscenes freeze Link in place, so floorPoly never updates
            // through Player_Update's normal path. When the cutscene later
            // ends and a code path walks collision data via floorPoly
            // (Boss_Goma's substate 150 → Player_SetCsActionWithHaltedActors,
            // post-cutscene Player csAction handlers, etc.), it dereferences
            // NULL and crashes (RAX=0, RBX=0x10 = sizeof(CollisionPoly),
            // R14=0x78 = offset of Actor.floorPoly).
            //
            // Flags 7 = FLAG_0 | FLAG_1 | FLAG_2 — standard floor + wall +
            // ceiling check. Heights (18 wall, 6 radius, 80 ceiling) match
            // values z_player.c uses elsewhere; conservative bounds.
            Actor_UpdateBgCheckInfo(gPlayState, &localPlayer->actor,
                                    18.0f, 6.0f, 80.0f, 7);
            SPDLOG_INFO("[CutsceneStart] Teleport peer Link to triggerPos=({:.0f},{:.0f},{:.0f}) rotY=0x{:04X} bgCheckFlags=0x{:04X}",
                        triggerPos.x, triggerPos.y, triggerPos.z, (int)(uint16_t)triggerRotY,
                        (unsigned int)localPlayer->actor.bgCheckFlags);
        }
    }

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
    const std::string csKind   = payload.value("csKind", std::string("actor_unknown"));
    const int32_t     csKey    = payload.value("csKey",  (int32_t)-1);
    const uint8_t     timeline = (uint8_t)(gSaveContext.linkAge & 1);

    // Bug 3 — drop any pending START with the same key. If host's
    // CS ended before peer transitioned into the matching scene,
    // we don't want the buffered START firing on a future scene
    // entry (which would re-play an already-finished cutscene from
    // frame 0 with the host already gone).
    RemovePendingCutsceneStart(sceneNum, timeline, csKind, csKey);

    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

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

    // Bug 1 (log 184 Saria conversation, camera stuck on peer) — when peer's
    // local cutscene script ends naturally, the engine releases sub-cameras
    // and returns the main camera to ACTIVE so it tracks Link again. If our
    // forced csCtx.state=IDLE write above interrupts the script before its
    // camera-cleanup frame runs (or if script timing diverged from host's
    // and host's CUTSCENE_END arrived first), any sub-camera the script set
    // up persists — peer's view is locked on whatever the cutscene was
    // looking at, even after Link can move again. User confirmed scene
    // change reset the camera, which matches: scene init reinits the
    // camera array.
    //
    // Defensive cleanup: release all sub-cameras and reactivate cam id 0.
    // No-op when cleanup already happened naturally; match the pattern at
    // z_boss_dodongo.c:455-458 (Play_ClearAllSubCameras + ChangeCameraStatus
    // back to ACTIVE on the main slot).
    Play_ClearAllSubCameras(gPlayState);
    Play_ChangeCameraStatus(gPlayState, 0, CAM_STAT_ACTIVE);

    MarkCutsceneInactive(sceneNum, timeline, csKind, csKey);

    SPDLOG_INFO("[CutsceneEnd] Received kind={} key={} scene=0x{:02X}",
                csKind, csKey, (int)sceneNum);
}
