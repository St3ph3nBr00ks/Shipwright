// Refactor A.8 — Anchor-core extern "C" shim bridge.
//
// Five C-linkage facades over Anchor-core queries that allow vanilla
// decomp actor code (`soh/src/overlays/actors/...`) to consult Anchor
// state without pulling in C++ headers. Moved from HookHandlers.cpp on
// 2026-06-01 per Plans/A.8_design_review.md (DR-5) Stage 3.8.
//
// Domain: Anchor core — generic player-actor / host-authority queries.
// No file-statics. No cross-domain coupling. Pure facades.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"

extern "C" {
#include "z64.h"
#include "macros.h"  // GET_PLAYER (Pitfall 15)
extern PlayState* gPlayState;
}

// C-callable wrapper so enemy C-code files (e.g. z_en_dekubaba.c) can query the
// nearest player actor without pulling in C++ headers. Returns the nearest
// player-type Actor* (local player or closest DummyPlayer). Safe to call any time
// gPlayState is valid; falls back to local player when Anchor is not active.
//
// Bug 1 fix (2026-06-17): FindNearestPlayerActor now gates the local-Player seed
// on liveness + cutscene state and may return nullptr when no valid candidate
// exists (all players dead / in cutscene). The vast majority of vanilla actor
// `.c` consumers immediately dereference the returned pointer
// (e.g. `(Player*)Anchor_GetNearestPlayerActor(...)->meleeWeaponState`), so the
// wrapper preserves the pre-existing never-NULL contract by falling back to the
// local Link's actor. The fallback to host's dead Link is acceptable for these
// consumers because:
//   - They read position / animation / equipment data, not combat-engagement
//     decisions. The corpse's world.pos is valid; reading from it is harmless.
//   - The host-side #153 enemy-AI overlay (HookHandlers.cpp) does its own
//     nullptr handling and skips the cached-field patch when nullptr surfaces.
// See Claude/Analysis/dead_player_targeting_and_final_blow_2026-06-17.md §9.
extern "C" Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play) {
    Actor* nearest = FindNearestPlayerActor(enemy, play);
    if (nearest == nullptr && play != nullptr) {
        Player* localPlayer = GET_PLAYER(play);
        if (localPlayer != nullptr) {
            nearest = &localPlayer->actor;
        }
    }
    return nearest;
}

// Hyrule Field Stalchild spawner (En_Encount1, ACTORCAT_PROP) needs to
// round-robin spawn positions across local Link + in-timeline DummyPlayers
// so peers see Stalchildren clustered around their own Link, not just
// host's. Returns the same player list FindNearestPlayerActor walks; the
// per-player budget (2 each) is enforced by the caller.
extern "C" int Anchor_GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount) {
    return GetSyncedPlayerActors(play, outActors, maxCount);
}

// C-callable: returns true if this client is the effective host. Used by
// per-actor C-code files that need to gate AI decisions to host-authoritative
// behaviour (e.g. EnHintnuts's "burrow when any player too close" logic must
// only run on host so peers follow via state-sync rather than independently
// burrowing on local-distance checks).
//
// Phase 1 semantics (global host). For actor-context C code, prefer
// Anchor_IsCurrentRoomHost() below — it picks up Pillar A Phase 2 per-
// room authority so a peer alone in a room becomes that room's host.
extern "C" bool Anchor_IsEffectiveHost(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return true;
    return ::SceneAuthority::IsEffectiveHost();
}

// C-callable: Pillar A Phase 2 — true when this client is the room host
// for its current (sceneNum, roomNum, timeline). Used by actor C code
// that should be running its host-authoritative logic when this client
// is alone in a room (e.g. EnHintnuts's burrow-when-too-close gate),
// independent of the global effective host. Falls back to global host
// when Anchor isn't active or gPlayState is null.
extern "C" bool Anchor_IsCurrentRoomHost(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return true;
    return ::SceneAuthority::IsMyCurrentRoomHost();
}

// Flotilla custom voice (#83/#84) — returns local player's clientId for the
// voice-emission emitter capture in Player_PlaySfx (z_actor.c:2242). The
// caller writes the return value into gAnchorCurrentEmitterClientId (a
// game-thread thread-local declared in code_800F7260.c) right before
// Audio_PlaySoundGeneral; the SoundRequest captures it; later phases thread
// it through the audio cmd queue to drive the per-emitter sample
// substitution lookup at the audio-thread Audio_GetSfx call site.
//
// Returns 0 when Anchor is not connected — preserves the vanilla code path
// (no substitution, no overhead).
extern "C" uint32_t Anchor_GetLocalEmitterClientId(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return 0;
    return Anchor::Instance->ownClientId;
}

// C-callable: returns true if any DummyPlayer (remote player) is currently
// standing on top of the given DynaPolyActor's footprint. Used by
// `DynaPolyActor_IsPlayerOnTop` callers (Obj_Lift, etc.) to make
// step-on-top triggers multiplayer-aware. Local player's IsPlayerOnTop
// flag continues to come from the engine's own dyna physics; this helper
// is a strict OR-fallthrough that fires only when the local check missed.
//
// Geometry: XZ proximity ≤ 120 units (matches Obj_Lift's 3×3 fragment
// grid at 120-unit spacing) AND Y delta in [-10, +80] (player is at or
// just above the platform top).
extern "C" bool Anchor_IsAnyPeerOnDyna(Actor* dynaActor) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr || dynaActor == nullptr) return false;

    const f32 kXZRangeSq = 120.0f * 120.0f;
    const f32 kYMin      = -10.0f;
    const f32 kYMax      =  80.0f;

    Actor* npc = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            f32 dx = npc->world.pos.x - dynaActor->world.pos.x;
            f32 dz = npc->world.pos.z - dynaActor->world.pos.z;
            f32 dy = npc->world.pos.y - dynaActor->world.pos.y;
            if (dx * dx + dz * dz < kXZRangeSq && dy > kYMin && dy < kYMax) {
                return true;
            }
        }
        npc = npc->next;
    }
    return false;
}
