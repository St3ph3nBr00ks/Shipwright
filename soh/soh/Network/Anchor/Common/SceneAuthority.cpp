#include "SceneAuthority.h"
#include "soh/Network/Anchor/Anchor.h"

#include <climits>

extern "C" {
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace SceneAuthority {

uint32_t GetEffectiveHostClientId() {
    // Pillar A Phase 1 — read the cached value maintained by
    // Anchor::RecomputeEffectiveHost(). The real election rule lives there.
    if (!::Anchor::Instance) return 0;
    return ::Anchor::Instance->effectiveHostClientId;
}

bool IsEffectiveHost() {
    if (!::Anchor::Instance) return false;
    return ::Anchor::Instance->effectiveHostClientId == ::Anchor::Instance->ownClientId;
}

uint32_t GetSceneHostClientId(int16_t sceneNum, int8_t roomNum, uint8_t timeline) {
    // Pillar A Phase 2 — per-(sceneNum, roomNum, timeline) authority
    // election.
    //
    // Walk the clients map and pick the lowest clientId among those
    // currently in (sceneNum, roomNum, timeline). The clients map is
    // kept in sync via UPDATE_CLIENT_STATE / PLAYER_UPDATE broadcasts
    // that every client receives, so all clients converge on the same
    // election result (modulo a brief room-transition window).
    //
    // Why roomNum is part of the key: OoT multi-room scenes (Inside
    // Deku Tree, dungeons) load actors per-room. A client in room A
    // doesn't have actor-list visibility into room B even though both
    // rooms share sceneNum. Without roomNum in the election, a client
    // alone in room B would lose authority to a same-scene client in
    // room A whose actor list doesn't contain room B's enemies — the
    // state machines for room B's actors then never run.
    //
    // Self-state quirk: SendPacket_UpdateClientState broadcasts our new
    // sceneNum/roomNum but does NOT locally write to clients[ownClientId]
    // until the broadcast echoes back. So immediately after a transition
    // our self-entry in clients lags ~50ms. To avoid mis-electing during
    // that window, consult gPlayState/gSaveContext directly for self
    // instead of the stale clients[self] entry.
    //
    // linkAge can be -1 (uninitialised) / 0 (adult) / 1 (child) — mask
    // to the low bit for timeline.
    if (!::Anchor::Instance) return 0;
    const uint32_t ownId = ::Anchor::Instance->ownClientId;

    uint32_t lowestId = UINT32_MAX;
    bool     found    = false;

    // Check self via live local state (gPlayState is the live scene
    // and current room; gSaveContext.linkAge is the live timeline).
    // gPlayState may be null pre-scene-load — in that case we're not
    // "in" any scene.
    if (gPlayState != nullptr &&
        (s16)gPlayState->sceneNum == sceneNum &&
        (s8)gPlayState->roomCtx.curRoom.num == roomNum &&
        ((uint8_t)(gSaveContext.linkAge & 0x1)) == timeline) {
        if (ownId < lowestId) {
            lowestId = ownId;
            found    = true;
        }
    }

    // Check other clients via clients map (peer state arrives via
    // UPDATE_CLIENT_STATE / PLAYER_UPDATE broadcasts).
    for (auto& [cid, client] : ::Anchor::Instance->clients) {
        if (cid == ownId) continue; // self handled above with live state
        if (!client.online) continue;
        if (client.sceneNum != sceneNum) continue;
        if (client.curRoomNum != roomNum) continue;
        if ((uint8_t)(client.linkAge & 0x1) != timeline) continue;
        if (cid < lowestId) {
            lowestId = cid;
            found    = true;
        }
    }
    if (!found) {
        // Empty (sceneNum, roomNum, timeline) — fall back to the global
        // effective host so the call site gets a sensible answer instead
        // of 0. Legitimate case: ENEMY_STATE arrives for an actor whose
        // room no online client is currently in (e.g. the room was
        // unloaded between send and receive).
        return GetEffectiveHostClientId();
    }
    return lowestId;
}

bool IsSceneHost(int16_t sceneNum, int8_t roomNum, uint8_t timeline) {
    if (!::Anchor::Instance) return false;
    return GetSceneHostClientId(sceneNum, roomNum, timeline) ==
           ::Anchor::Instance->ownClientId;
}

bool IsMyCurrentSceneHost() {
    if (!gPlayState) return IsEffectiveHost();
    return IsSceneHost((int16_t)gPlayState->sceneNum,
                       (int8_t)gPlayState->roomCtx.curRoom.num,
                       (uint8_t)(gSaveContext.linkAge & 0x1));
}

}  // namespace SceneAuthority
