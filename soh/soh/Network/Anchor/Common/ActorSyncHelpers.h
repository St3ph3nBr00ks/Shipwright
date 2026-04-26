#pragma once

// Cross-cutting actor-sync helpers shared across all sync layers.
// Extracted from HookHandlers.cpp + Anchor.h in #173 Phase 1.
//
// See Claude/Plans/anchor_code_decoupling.md.

#include <cstddef>
#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

// Receive-side actor-list scans walk these 8 categories. Covers every
// runtime category transition surveyed in session_state.md
// (Karebaba→MISC, Armos→BG, Po Sisters→PROP, Heishi2→ITEMACTION, etc.)
// plus BOSS for full-boss sync. Adding an ID to IsSyncedWorldActor
// admits both default-category and transition-category instances.
// `inline constexpr` (C++17) deduplicates across translation units.
inline constexpr u8 kSyncableActorCategories[] = {
    ACTORCAT_ENEMY,
    ACTORCAT_BOSS,
    ACTORCAT_PROP,
    ACTORCAT_BG,
    ACTORCAT_NPC,
    ACTORCAT_SWITCH,
    ACTORCAT_ITEMACTION,
    ACTORCAT_MISC,
};
inline constexpr size_t kSyncableActorCategoriesCount =
    sizeof(kSyncableActorCategories) / sizeof(kSyncableActorCategories[0]);

/**
 * Returns the SkelAnime* for an enemy actor, or nullptr if unsupported.
 *
 * Most enemies in OoT place SkelAnime immediately after the base Actor struct.
 * A minority have other fields in between. Known exceptions are handled via
 * explicit struct casts; everything else uses the generic layout with validation.
 *
 * Generic layout (used by ~117 of ~156 animated enemies):
 *   struct GenericEnemy { Actor actor; SkelAnime skelAnime; };
 *
 * Exception pattern (e.g. Redead, Wolfos, Stalfos):
 *   struct { Actor actor; Vec3s bodyPartsPos[N]; SkelAnime skelAnime; };
 *   — handled by casting to the specific enemy struct.
 */
SkelAnime* GetEnemySkelAnime(Actor* actor);

// Issue #153 — admission predicate for non-ACTORCAT_ENEMY actors that should
// participate in the sync pipeline (ENEMY_UPDATE, ENEMY_DEFEATED, etc.).
//
// The original three gate sites (OnActorSpawn / OnActorUpdate / ShouldActorUpdate)
// hard-checked `category == ACTORCAT_ENEMY`. That excludes hostile/world actors
// in PROP / BG / NPC / SWITCH / MISC, even when they affect cross-client gameplay
// (rolling boulders, scripted-path NPCs, eye-switch traps, etc.).
//
// Adding actor IDs here joins them to the sync pipeline without disturbing the
// existing ACTORCAT_ENEMY-only flow. Per-actor sync logic (payload extension,
// re-apply behaviour) still has to be implemented case by case in the relevant
// hook bodies and packet handlers.
//
// Pending future allowlist entries surfaced in research:
//   ACTOR_EN_PO_DESERT     — Desert Poe / Guide Poe   (#124, ACTORCAT_BG)
//   ACTOR_EN_PO_RELAY      — Dampé's Ghost            (#125, ACTORCAT_NPC)
//   ACTOR_EN_ANUBICE_TAG   — Anubis spawn marker      (#116, ACTORCAT_SWITCH)
bool IsSyncedWorldActor(int16_t actorId);

// True when the actor should be considered for sync. Called from each filter
// site to keep the gate logic identical everywhere.
inline bool IsSyncableActor(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || IsSyncedWorldActor(actor->id);
}
