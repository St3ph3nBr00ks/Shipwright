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

    // Optional per-csKey opt-in predicate. When set + returns true, the
    // cutscene is treated as "player must intentionally opt in":
    //   1. ApplyCutsceneStartByKind on receive records the state
    //      (cutsceneStartActive gets the entry) but does NOT invoke
    //      applyForce. Receiver stays in gameplay mode.
    //   2. FRAME_SYNC-driven auto-catchup on receive is suppressed —
    //      receiver does not force-join the peer's cutscene.
    //   3. If the receiver's own actor code LATER fires the local
    //      trigger, it can call Anchor_TryEngageOptInCatchup to
    //      converge with the ongoing peer cutscene instead of starting
    //      fresh (see z_bg_treemouth.c come-back branch for the
    //      canonical use).
    //
    // Design intent: come-back-style cutscenes where vanilla requires
    // an explicit player action to enter (Z-target). Auto-triggered
    // cutscenes (proximity/story-beat) should leave this null so they
    // force-sync as before.
    std::function<bool(uint32_t csKey)> optInPredicate;
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

// -----------------------------------------------------------------------
// Opt-in catchup engagement — companion to Handler::optInPredicate.
//
// Called from the actor's local trigger site. Checks whether a peer
// currently owns the specified (csKind, csKey) cutscene (present in
// cutsceneStartActive but not local-origin). If yes, sends a catchup
// REQUEST to the peer originator and returns 1 — the actor should skip
// its normal fresh-start path (segment write, cutsceneTrigger, notify)
// because the incoming CUTSCENE_CATCHUP_RESPONSE will apply the state.
// Returns 0 if no peer is running the cutscene — actor should proceed
// with a normal fresh start (which will also broadcast its own
// CUTSCENE_START).
extern "C" int Anchor_TryEngageOptInCatchup(const char* csKind, uint32_t csKey);
