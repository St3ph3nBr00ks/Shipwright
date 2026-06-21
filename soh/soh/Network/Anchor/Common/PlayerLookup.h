#pragma once

// Player-lookup helper shared by enemy sync, AI follower, and any
// future feature that needs "who's the nearest player to this enemy".
// Extracted from HookHandlers.cpp in #173 Phase 1.
//
// See Claude/Plans/anchor_code_decoupling.md.

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

// Returns the Actor* of the nearest player-type actor to `enemy`.
// Considers the local player and all live DummyPlayer actors.
// DummyPlayers that are out-of-scene are already at (-9999,-9999,-9999) by
// DummyPlayer_Update, so they are naturally excluded by the distance comparison.
//
// Per-candidate liveness gates (Bug 1 fix, 2026-06-17 — mirrors
// PickHostileTargetForInvader):
//   - Local Player skipped when gSaveContext.health <= 0 or
//     play->csCtx.state != CS_STATE_IDLE.
//   - DummyPlayer skipped when peer's client.stateFlags1 has
//     PLAYER_STATE1_DEAD set (peer is in death cycle).
//
// May return nullptr when no valid candidate exists (all players
// dead / in cutscene). Direct C++ consumers MUST defend against
// nullptr; the C wrapper `Anchor_GetNearestPlayerActor` falls back
// to the local Link so vanilla actor `.c` consumers retain the
// pre-existing "never-NULL" contract they assume when immediately
// dereferencing the returned pointer.
Actor* FindNearestPlayerActor(Actor* enemy, PlayState* play);

// Fills `outActors[0..min(maxCount, returned)-1]` with the local Link's
// Actor* followed by each in-timeline DummyPlayer Actor* present in
// the actor list. Cross-timeline DummyPlayers are filtered out (Pillar
// B Phase 3) so callers spawning enemies / picking targets don't see
// invisible peers from the other timeline. Returns the total count.
//
// Out-of-scene DummyPlayers are excluded by their (-9999,-9999,-9999)
// position — callers that want to gate by scene presence should
// distance-filter on the returned actors.
int GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount);

// Multi-player hostile-target picker for NPC Invader.
//
// Walks the FULL session — local Player + every in-timeline DummyPlayer
// in the actor list + the local NPC Follower (when targetable) — and
// returns the closest valid candidate by XZ distance from the invader.
//
// Per-candidate validity gates (any failure → skip):
//   - For the local Player: gPlayState->sceneNum equals the invader's
//     scene (always true since the local Player IS in our scene), save
//     loaded (Anchor::Instance->IsSaveLoaded() / gSaveContext fallback),
//     not in cutscene (gPlayState->csCtx.state == CS_STATE_IDLE),
//     and gPlayState->sceneNum is NOT in IsSceneFlaggedNoInvaders.
//   - For DummyPlayers: same-timeline filter (Pillar B Phase 3),
//     plus the owning peer's AnchorClient must be online + isSaveLoaded
//     + csCtxState == CS_STATE_IDLE + sceneNum == local scene + scene
//     not blacklisted. Out-of-scene DummyPlayers (at -9999) are
//     naturally excluded by sceneNum mismatch since DummyPlayer_Update
//     parks them outside the live actor coordinate range.
//   - For the NPC Follower: IsFollowerNpcTargetable() must be true,
//     update pointer non-null, same-scene as invader, and the invader
//     is in a non-blacklisted scene.
//
// Scoring: closest by 2D (XZ) distance squared. Future phases can
// extend with LOS / aggro / sticky-target on top of this — keep the
// signature stable.
//
// Returns nullptr when no valid candidate exists. Callers should
// treat nullptr as "no target — hold IDLE".
//
// In a single-player session the function collapses to "local Player
// is the only candidate" with the same gating; in that case it returns
// the local Player when it passes the gates, otherwise nullptr.
//
// This is the multi-player replacement for Anchor_GetNearestPlayerActor
// when the consumer is the NPC Invader — that legacy helper is used by
// vanilla-enemy targeting and does NOT apply session-level validity
// gates (cutscene / save-loaded / scene blacklist). Other consumers
// of Anchor_GetNearestPlayerActor remain unchanged; this helper is
// the new entry point for the Invader actor's target-acquisition path.
Actor* PickHostileTargetForInvader(Actor* invader, PlayState* play);

// Returns true if any peer (non-self, online, save-loaded) is currently
// in `sceneNum`. Use this when a hook or handler needs to gate work on
// "is anyone besides me in this scene". Mirrors the inline loop pattern
// duplicated across HookHandlers.cpp pre-extraction (~2 verified sites).
bool AnyPeerInScene(int16_t sceneNum);

// Live-Link distance / yaw / height helpers — see Pitfall 28.
//
// `actor->xzDistToPlayer` and friends are computed once per frame in
// z_actor.c:2686-2690 against `player` cached at the top of
// Actor_UpdateAll, then OVERWRITTEN by HookHandlers.cpp:2496-2499's
// nearest-player overlay (#153) so AI pursues the closer DummyPlayer
// when one is in scene. That overlay is correct for AI targeting and
// MUST stay — it's what makes enemies target peers in multiplayer.
//
// But vanilla also uses the same cached fields to gate effect
// application against the LOCAL Link ("did I just hit the player?",
// "should I knock back the player?"). With the overlay active, those
// gates read distance-to-DummyPlayer instead of distance-to-local-Link
// and fire effects against `GET_PLAYER(play)` (= real Link) anyway —
// see log 417/418 Goroiwa bug.
//
// These helpers return the TRUTHFUL local-Link relation regardless of
// what the overlay wrote into the cache. Use them at any effect-
// application gate; never at AI-decision gates (those are correctly
// served by the cached fields with overlay).
extern "C" f32 Anchor_DistXZToLocalLink(Actor* actor, PlayState* play);
extern "C" s16 Anchor_YawTowardLocalLink(Actor* actor, PlayState* play);
extern "C" f32 Anchor_HeightDiffToLocalLink(Actor* actor, PlayState* play);

// VMP Phase B — direct-damage broadcast helper for OC2-style touch
// attackers (En_St, En_Ssh, En_Go, En_Go2, etc.) that call
// `play->damagePlayer(play, -N)` directly INSTEAD OF going through the
// AT collider damage path. DummyPlayer.cpp's existing AC_HIT detector
// only catches AT-collider hits, so OC2 touch attacks don't broadcast
// via the standard Path A wire path — they need this explicit helper.
//
// Call after the actor's existing gate passes ("did I just touch a
// player?"). The helper walks DummyPlayers in the local scene and
// sends DAMAGE_PLAYER to each peer whose DummyPlayer is within `range`
// XZ of the attacker. Knockback params come from EnemyKnockbackTable
// lookup on `attacker->id`; if the attacker isn't registered the
// helper still sends `directDamage` but with no knockback block.
//
// `directDamage` is the HP units the actor would have passed to
// `play->damagePlayer(play, -directDamage)`. Receiver mirrors that
// call locally on the peer's machine. See
// Plans/pitfall_28_actor_audit.md "Phase 2 — VMP".
//
// Returns the number of peers notified.
extern "C" int Anchor_BroadcastDirectDamageInRange(Actor* attacker, PlayState* play,
                                                   f32 range, u8 directDamage);
