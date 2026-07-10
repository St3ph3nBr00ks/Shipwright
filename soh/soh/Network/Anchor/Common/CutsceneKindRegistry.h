#pragma once

#include <functional>
#include <string>

// Registry of actor-driven cutscene "kinds" for the Anchor MP sync
// framework. Each kind maps a csKind string (wire identifier) to a set
// of handler callbacks that (a) locate the actor + drive the local
// cutscene entry on a peer, and (b) optionally hook end/teardown.
//
// Rationale (see Analysis/generic_cutscene_dialog_sync_helpers_2026-07-09.md):
// with 100+ potential customers, replace the growing if/else chains in
// ApplyCutsceneStartByKind (CutsceneStart.cpp) and CutsceneCatchup.cpp
// Setup dispatch with a table lookup. Each new customer adds one
// registration line + a small handler pair instead of touching multiple
// dispatch call sites.
//
// The registry does NOT own the actor-side trigger logic (that stays in
// the actor's .c file per SRP + SoC). It owns wire dispatch only.
//
// Companion generic helper: Anchor_ForceCutsceneOnActor — see below.

namespace CutsceneKindRegistry {

struct Handler {
    // Peer-side apply. Called both when CUTSCENE_START is received AND
    // when CUTSCENE_CATCHUP_RESPONSE is applied. Returns non-zero on
    // success (actor found + cutscene entered locally), 0 on failure
    // (e.g. actor not present in current room). The two call sites are
    // identical today; if they diverge in the future, split into two
    // slots.
    std::function<int(uint32_t csKey)> applyForce;

    // Optional end handler. Called from ApplyCutsceneEndByKind. Default
    // (null) is a no-op (vanilla drives its own teardown). Rarely
    // needed — reserved for actors that need explicit end-hook cleanup.
    std::function<bool(uint32_t csKey, const std::string& endReason)> applyEnd;
};

// Look up a handler by csKind. Returns nullptr for unregistered kinds.
// Lazy-initialises the built-in table on first call.
const Handler* Find(const std::string& csKind);

// Register a new kind at runtime. Rare — most customers should live in
// the built-in table (see CutsceneKindRegistry.cpp). Provided for
// dynamic registration by tests / future dev-tool overrides.
void Register(const std::string& csKind, Handler handler);

}  // namespace CutsceneKindRegistry

// -----------------------------------------------------------------------
// Generic actor-cutscene setup helper — Helper D.
//
// Locates the specified actor in the given category, writes
// play->csCtx.segment, sets gSaveContext.cutsceneTrigger, and invokes
// the actor's per-type setup adapter. Returns 1 on success, 0 if the
// actor is not present locally.
//
// Actor code exposes a `<Actor>_SetupActionAdapter(Actor*, void*)` thin
// wrapper that casts to the actor's typed action-func setter. This
// avoids Helper D needing to know about per-actor typed function
// pointers.
//
// Callable from both C++ (Anchor) and C (actor .c files); declared with
// C linkage.
extern "C" int Anchor_ForceCutsceneOnActor(
    struct PlayState* play,
    int16_t actorId,
    uint8_t actorCategory,
    void* segment,
    void (*setupAdapter)(struct Actor* actor, void* actionFunc),
    void* actionFunc);
