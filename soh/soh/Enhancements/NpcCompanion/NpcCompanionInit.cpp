/*
 * NpcCompanionInit.cpp — registers the unconditional OnGameFrameUpdate
 * hook that drives the NPC Follower (Flotilla NPC Companion).
 *
 * Plan: Plans/npc_follower_plan.md.
 *
 * Why this lives outside the Anchor's COND_HOOK:
 *   - The AI Player Follower (Anchor::SetFollowerActive +
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
 * soh/soh/Network/Anchor/NPCFollower/FollowerNPC.cpp; this file is
 * just the registration shim.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

namespace {

void OnGameFrameUpdateNpcCompanion() {
    if (Anchor::Instance == nullptr) return;
    // CVar transition polling — handles the OFF→ON spawn and the
    // ON→OFF despawn. Cheap when steady-state (no edge → early-return
    // after one CVarGetInteger).
    Anchor::Instance->TickFollowerNpcCVar();
}

// SCENE transitions destroy the actor list (NPC dies); ROOM
// transitions within the same scene do NOT — actors persist
// across rooms. But OnSceneSpawnActors fires for BOTH (z_actor.c:2601
// triggers it whenever play->numSetupActors != 0, which happens on
// every room load too, not just scene loads).
//
// Naively clearing the cache on every fire caused duplicate NPCs
// during door-open transitions: old NPC actor still alive in the
// previous room, but our pointer was cleared so a NEW NPC spawned
// in the current room → both visible.
//
// Fix: cache last-seen sceneNum. Only clear the pointer when the
// scene actually changes (the actor is genuinely dead). For room
// transitions within a scene, the actor persists; we instead
// update its `actor->room` field each tick (in
// Anchor_TickFollowerNpcActor) so the engine renders it in the
// leader's current room. Single instance, follows leader through
// doors, no duplication.
//
// This pattern (persistent actor + per-tick room sync) is the
// reusable mechanism for NPC Invader cross-room pursuit. Engine-
// level scene transitions still kill all actors; system-level
// state (CVar / Invader-active flag) drives respawn in the new
// scene. Cross-room within a scene is handled entirely by the
// actor.room field update — no respawn needed.
void OnSceneSpawnActorsNpcCompanion() {
    if (Anchor::Instance == nullptr) return;
    if (gPlayState == nullptr) return;
    static int16_t sLastSceneNum = -1;
    const int16_t curScene = gPlayState->sceneNum;
    if (curScene != sLastSceneNum) {
        // True scene transition — old actor list is dead, our pointer
        // is dangling. Clear the cache; TickFollowerNpcCVar will
        // auto-respawn next tick.
        Anchor::Instance->ClearFollowerNpcSceneCache();
        sLastSceneNum = curScene;
    }
    // Else: room transition within the same scene. Actor persists;
    // don't clear the pointer (would duplicate-spawn).
}

void RegisterNpcCompanion() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(
        OnGameFrameUpdateNpcCompanion);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneSpawnActors>(
        OnSceneSpawnActorsNpcCompanion);
}

}  // namespace

static RegisterShipInitFunc initFunc(RegisterNpcCompanion, {});
