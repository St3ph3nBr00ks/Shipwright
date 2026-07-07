/*
 * TeamMarker.cpp — Team Marker spawn director + owner-lookup sidecar.
 *
 * Plan: Plans/team_marker_plan.md.
 * Tracker: St3ph3nBr00ks/Shipwright#219.
 *
 * v1 scope (Phase 2 + Phase 5 landed here):
 *   - Spawn director reconciles the set of per-peer markers against the
 *     same-team, same-scene, online, save-loaded subset of
 *     Anchor::clients each tick from OnGameFrameUpdate.
 *   - Sidecar maps: peerClientId → marker Actor* + Actor* → peerClientId.
 *     Params can't hold clientId (s16 vs uint32_t) so the sidecar map is
 *     the source of truth for owner identity.
 *   - Per-tick pos + room field update so the marker follows the peer's
 *     position + is drawn in the local player's current room (so it
 *     renders even when peer is in a different room — Phase 3 through-
 *     walls draw pairs with this).
 *   - Draw-context flag pair (unused today; kept for any future hook
 *     that needs to identify the marker's draw call — mirrors NPC
 *     Follower's Anchor_FollowerNpcDrawBegin/End pattern).
 *
 * State lives file-static — Anchor.h stays untouched.
 */

#include "soh/Network/Anchor/TeamMarker/TeamMarker.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "z64.h"
#include "src/overlays/actors/ovl_En_TeamMarker/z_en_team_marker.h"  // EnTeamMarker struct
extern PlayState* gPlayState;
extern s16        gEnTeamMarkerId;
}

// ─── Configuration ───────────────────────────────────────────────────
// Height above the peer's PosRot origin (Link's world.pos = feet) where
// the fairy hovers. Sized to the vanilla Link body height for each age so
// the marker sits just above the peer's head — mirrors where Navi
// perches when following the local player. Adult Link is ~70u tall
// (torso top ~60u, head crown ~70u); child Link is ~50u.
static constexpr float kMarkerYOffsetAdult = 70.0f;
static constexpr float kMarkerYOffsetChild = 50.0f;

// LOS raycast — camera eye → marker pos. Ray hits geometry → peer
// obscured → show fairy. Ray clear → peer visible → hide fairy. We
// probe from `GET_ACTIVE_CAM(play)->eye` (handles Z-target subcams etc.)
// to the marker pos + a small down-offset so the ray targets the peer's
// upper chest / head, not the fairy floating above (which would clear
// walls the peer is behind).
static constexpr float kLosProbeDropY = 40.0f;  // aim at peer head, not fairy above

// Marker Y offset for a given linkAge (LINK_AGE_ADULT == 0,
// LINK_AGE_CHILD == 1). Falls back to adult offset for out-of-range
// values.
static inline float MarkerYOffsetForAge(s32 linkAge) {
    return (linkAge == LINK_AGE_CHILD) ? kMarkerYOffsetChild : kMarkerYOffsetAdult;
}

// Master enable CVar.
#define CVAR_TEAM_MARKER_ENABLED CVAR_ENHANCEMENT("Anchor.TeamMarker.Enabled")

// ─── State ───────────────────────────────────────────────────────────
// peerClientId → marker Actor* replica we spawned.
static std::unordered_map<uint32_t, Actor*> sMarkerByPeer;
// marker Actor* → peerClientId (inverse — used at draw time to resolve
// owner without consulting params).
static std::unordered_map<Actor*, uint32_t> sPeerByMarker;
// Current draw's marker actor. Set by Anchor_TeamMarkerDrawBegin;
// cleared by Anchor_TeamMarkerDrawEnd.
static Actor* sCurrentlyDrawingMarker = nullptr;

// ─── C API (extern "C") ──────────────────────────────────────────────
extern "C" void Anchor_TeamMarkerDrawBegin(Actor* marker) {
    sCurrentlyDrawingMarker = marker;
}

extern "C" void Anchor_TeamMarkerDrawEnd(void) {
    sCurrentlyDrawingMarker = nullptr;
}

extern "C" Actor* Anchor_GetCurrentlyDrawingTeamMarker(void) {
    return sCurrentlyDrawingMarker;
}

extern "C" unsigned int Anchor_GetTeamMarkerOwnerClientId(Actor* marker) {
    if (marker == nullptr) return 0;
    auto it = sPeerByMarker.find(marker);
    if (it == sPeerByMarker.end()) return 0;
    return (unsigned int)it->second;
}

extern "C" int Anchor_TeamMarkerShouldSuppress(PlayState* play) {
    if (play == nullptr) return 1;
    // Cutscene running — Player_InCsMode wraps csCtx.state != CS_STATE_IDLE
    // plus PLAYER_STATE1_IN_CUTSCENE etc.
    if (Player_InCsMode(play)) return 1;
    return 0;
}

extern "C" int Anchor_GetTeamMarkerColor(Actor* marker, unsigned char* r, unsigned char* g, unsigned char* b) {
    if (marker == nullptr || r == nullptr || g == nullptr || b == nullptr) return 0;
    if (Anchor::Instance == nullptr) return 0;

    auto peerIt = sPeerByMarker.find(marker);
    if (peerIt == sPeerByMarker.end()) return 0;

    auto clientIt = Anchor::Instance->clients.find(peerIt->second);
    if (clientIt == Anchor::Instance->clients.end()) return 0;

    *r = clientIt->second.color.r;
    *g = clientIt->second.color.g;
    *b = clientIt->second.color.b;
    return 1;
}

// ─── Internal helpers ────────────────────────────────────────────────
namespace {

// Return the local player's teamId string, or "default" if we can't
// resolve it. Mirrors DummyPlayer's same-team check convention.
std::string LocalTeamId() {
    if (Anchor::Instance == nullptr) return "default";
    auto it = Anchor::Instance->clients.find(Anchor::Instance->ownClientId);
    if (it == Anchor::Instance->clients.end()) return "default";
    if (it->second.teamId.empty()) return "default";
    return it->second.teamId;
}

void KillMarkerActor(Actor* actor) {
    if (actor == nullptr || gPlayState == nullptr) return;
    if (actor->update == nullptr) return;  // already dead
    Actor_Kill(actor);
}

// Reconcile marker set against desired peer set. Called each frame.
void TickTeamMarkers() {
    const bool masterEnabled = CVarGetInteger(CVAR_TEAM_MARKER_ENABLED, 1) != 0;

    if (!masterEnabled || Anchor::Instance == nullptr || !Anchor::Instance->isConnected) {
        // Master toggle off, or not in a session — drop everything. If
        // pointers survived a disable/enable cycle they'd be dangling.
        if (!sMarkerByPeer.empty() || !sPeerByMarker.empty()) {
            for (auto& kv : sMarkerByPeer) KillMarkerActor(kv.second);
            sMarkerByPeer.clear();
            sPeerByMarker.clear();
        }
        return;
    }
    if (gPlayState == nullptr) return;
    if (gEnTeamMarkerId == 0) return;

    const s16      sceneNum   = gPlayState->sceneNum;
    const s8       curRoomNum = gPlayState->roomCtx.curRoom.num;
    const uint32_t ownId      = Anchor::Instance->ownClientId;
    const std::string myTeam  = LocalTeamId();

    // Pass 1 — despawn markers whose peer no longer qualifies.
    for (auto it = sMarkerByPeer.begin(); it != sMarkerByPeer.end();) {
        const uint32_t peerId = it->first;
        Actor*         actor  = it->second;
        bool keep = false;

        // Actor still alive?
        if (actor == nullptr || actor->update == nullptr) {
            keep = false;
        } else {
            auto clientIt = Anchor::Instance->clients.find(peerId);
            if (clientIt != Anchor::Instance->clients.end()) {
                const AnchorClient& c = clientIt->second;
                const std::string cTeam = c.teamId.empty() ? std::string("default") : c.teamId;
                keep = c.online && c.isSaveLoaded && (cTeam == myTeam) && (c.sceneNum == sceneNum);
            }
        }

        if (!keep) {
            KillMarkerActor(actor);
            sPeerByMarker.erase(actor);
            it = sMarkerByPeer.erase(it);
        } else {
            ++it;
        }
    }

    // Pass 2 — spawn markers for qualifying peers not yet tracked.
    for (auto& kv : Anchor::Instance->clients) {
        const uint32_t     peerId = kv.first;
        const AnchorClient& c     = kv.second;
        if (peerId == ownId) continue;
        if (!c.online || !c.isSaveLoaded) continue;
        const std::string cTeam = c.teamId.empty() ? std::string("default") : c.teamId;
        if (cTeam != myTeam) continue;
        if (c.sceneNum != sceneNum) continue;
        if (sMarkerByPeer.count(peerId) != 0) continue;

        const float yOffset = MarkerYOffsetForAge(c.linkAge);
        const Vec3f pos = { c.posRot.pos.x, c.posRot.pos.y + yOffset, c.posRot.pos.z };
        Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, gEnTeamMarkerId,
                                     pos.x, pos.y, pos.z,
                                     0 /* rotX */, 0 /* rotY */, 0 /* rotZ */,
                                     0 /* params — sidecar map is source of truth */);
        if (spawned == nullptr) {
            SPDLOG_WARN("[TeamMarker] Actor_Spawn failed for peer {}", (unsigned)peerId);
            continue;
        }
        sMarkerByPeer[peerId] = spawned;
        sPeerByMarker[spawned] = peerId;
        // Nametag intentionally NOT registered on the fairy — the peer's
        // colored name on their DummyPlayer already serves that purpose.
    }

    // Pass 3 — per-frame position + room + LOS raycast.
    Camera* activeCam = (gPlayState != nullptr) ? GET_ACTIVE_CAM(gPlayState) : nullptr;
    for (auto& kv : sMarkerByPeer) {
        const uint32_t peerId = kv.first;
        Actor*         actor  = kv.second;
        auto clientIt = Anchor::Instance->clients.find(peerId);
        if (clientIt == Anchor::Instance->clients.end()) continue;

        const float yOffset = MarkerYOffsetForAge(clientIt->second.linkAge);
        const Vec3f pos = { clientIt->second.posRot.pos.x,
                            clientIt->second.posRot.pos.y + yOffset,
                            clientIt->second.posRot.pos.z };
        actor->world.pos = pos;
        actor->prevPos   = pos;  // suppress interp from prior frame
        // Marker lives in whatever room the LOCAL player is in — the
        // fairy is a HUD-style overlay on the local's screen. This is
        // what makes the marker render regardless of which room the
        // peer's actual world position resolves to.
        actor->room = curRoomNum;

        // LOS raycast from active camera to peer's chest/head. Cache
        // result on the actor for its own Draw to consume.
        int obscured = 0;
        if (activeCam != nullptr && gPlayState != nullptr) {
            Vec3f startPos = activeCam->eye;
            Vec3f endPos   = { pos.x, pos.y - kLosProbeDropY, pos.z };
            Vec3f hitPos;
            CollisionPoly* hitPoly = nullptr;
            // Returns 1 when the segment hits geometry. Hit == peer
            // obscured from view.
            obscured = BgCheck_AnyLineTest1(&gPlayState->colCtx, &startPos, &endPos, &hitPos, &hitPoly, 0);
        }
        ((EnTeamMarker*)actor)->obscured = obscured;
    }
}

void ClearSceneCache() {
    // Called after scene transition destroys the actor list. Don't
    // Actor_Kill — the engine already did. Just drop pointers.
    sMarkerByPeer.clear();
    sPeerByMarker.clear();
    sCurrentlyDrawingMarker = nullptr;
}

void OnGameFrameUpdateTeamMarker() {
    TickTeamMarkers();
}

void OnSceneSpawnActorsTeamMarker() {
    // OnSceneSpawnActors fires on both scene transitions and same-scene
    // room reloads. Same NpcCompanionInit pattern: cache last sceneNum,
    // only clear on true scene change. Room transitions preserve actor
    // list; a per-room clear would double-spawn during door crossings.
    if (gPlayState == nullptr) return;
    static int16_t sLastSceneNum = -1;
    const int16_t curScene = gPlayState->sceneNum;
    if (curScene != sLastSceneNum) {
        ClearSceneCache();
        sLastSceneNum = curScene;
    }
}

void RegisterTeamMarker() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameUpdateTeamMarker);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        OnSceneSpawnActorsTeamMarker);
}

}  // namespace

static RegisterShipInitFunc initFuncTeamMarker(RegisterTeamMarker, {});
