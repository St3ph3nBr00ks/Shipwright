// Refactor A.8 — NPC-AI extern "C" shim bridge.
//
// Single C-linkage facade for the Hintnut local-AI suppression gate.
// Moved from HookHandlers.cpp on 2026-06-01 per Plans/A.8_design_review.md
// (DR-5) Stage 3.8.
//
// Domain: NPC AI suppression — actor-side gates that disable local AI
// when the actor's state machine is host-authoritative.
// No file-statics. No cross-domain coupling.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

// Hintnut state machine is host-authoritative (room host runs the AI;
// peers receive ENEMY_STATE and apply via ApplyNetState). Peers must
// not run HitByScrubProjectile1+2 locally on nutball impact — would
// cause the local actor to transition out of sync with host's
// authoritative state. Returns true on peers (non-room-hosts) so the
// local hit gets routed through PROJECTILE_HIT_ENEMY instead.
//
// Single-player and offline branches: returns false (no Anchor, no
// connection, or no scope info) so vanilla behavior is preserved.
extern "C" bool Anchor_ShouldSuppressHintnutsLocalAI(Actor* actor) {
    if (actor == nullptr) return false;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    if (gPlayState == nullptr) return false;
    // Peer = "I'm not the room host of MY current (sceneNum, roomNum,
    // timeline)". The actor is in my actor list so its scope is mine.
    return !::SceneAuthority::IsMyCurrentRoomHost();
}
