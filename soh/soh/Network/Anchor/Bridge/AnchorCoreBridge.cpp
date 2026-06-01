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
extern PlayState* gPlayState;
}

// C-callable wrapper so enemy C-code files (e.g. z_en_dekubaba.c) can query the
// nearest player actor without pulling in C++ headers. Returns the nearest
// player-type Actor* (local player or closest DummyPlayer). Safe to call any time
// gPlayState is valid; falls back to local player when Anchor is not active.
extern "C" Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play) {
    return FindNearestPlayerActor(enemy, play);
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
