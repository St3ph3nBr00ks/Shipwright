/*
 * NpcCompanionInit.cpp — registers the unconditional OnGameFrameUpdate
 * hook that drives the NPC Follower (Flotilla NPC Companion).
 *
 * Plan: Plans/npc_follower_plan.md.
 *
 * Why this lives outside the Anchor's COND_HOOK:
 *   - The player-rigged AI Follower (Anchor::SetFollowerActive +
 *     friends in HookHandlers.cpp) is non-host-only by design and
 *     gates all its ticks on isConnected. The NPC Companion is a
 *     general enhancement that should work in single-player too.
 *
 * Why the hook is unconditional (NOT a COND_HOOK on the CVar value):
 *   - COND_HOOK gates registration on the CVar — when the CVar goes
 *     1→0, COND_HOOK unregisters the callback and the despawn-
 *     detection logic never runs. Spawned NPC would linger.
 *   - Unconditional registration keeps the tick firing regardless;
 *     TickFollowerNpcCVar is cheap when steady-state (one
 *     CVarGetInteger + int compare in the no-op case) and handles
 *     both edges + the auto-respawn after scene transitions.
 *
 * The actual spawn/despawn helpers live in
 * soh/soh/Network/Anchor/AIFollowerNPC/FollowerNPC.cpp; this file is
 * just the registration shim.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

namespace {

void OnGameFrameUpdateNpcCompanion() {
    if (Anchor::Instance == nullptr) return;
    // CVar transition polling — handles the OFF→ON spawn and the
    // ON→OFF despawn. Cheap when steady-state (no edge → early-return
    // after one CVarGetInteger).
    Anchor::Instance->TickFollowerNpcCVar();
}

// Scene transitions destroy the actor list. Our cached
// mFollowerNpcLocalActor pointer becomes dangling — checking
// pointer->update is undefined behaviour (the memory may be freed,
// reused by another actor with non-null update, or contain stale
// bytes). The earlier "stale-pointer cleanup" inside
// TickFollowerNpcCVar relied on the dangling-read returning NULL,
// which doesn't reliably happen — so the auto-respawn never fired
// in field test.
//
// Fix: explicit clear on OnSceneSpawnActors (fires after the new
// scene's actor list is initialised but before the first actor
// tick). After the clear, the next TickFollowerNpcCVar sees CVar=on
// AND pointer=null AND auto-respawns at the player's new-scene pos.
//
// Same clear empties mPeerFollowerNpcs — peers' next FOLLOWER_NPC_SPAWN
// (auto-broadcast when their TickFollowerNpcCVar auto-respawns)
// will repopulate the map.
void OnSceneSpawnActorsNpcCompanion() {
    if (Anchor::Instance == nullptr) return;
    Anchor::Instance->ClearFollowerNpcSceneCache();
}

void RegisterNpcCompanion() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameUpdateNpcCompanion);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        OnSceneSpawnActorsNpcCompanion);
}

}  // namespace

static RegisterShipInitFunc initFunc(RegisterNpcCompanion, {});
