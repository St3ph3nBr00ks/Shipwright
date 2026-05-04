#ifndef ANCHOR_ACTOR_SYNC_SCOPE_H
#define ANCHOR_ACTOR_SYNC_SCOPE_H

// Generic Actor State Sync — scope axis (Phase 0 framework).
//
// Each Actor instance declares one of three sync scopes that determine
// which clients apply state updates received over the wire:
//
//   - None   — no sync. Per-client behaviour. ENEMY_STATE packets are
//              not emitted for actors with this scope.
//   - Team   — only clients with the same TeamId as the sender apply
//              the state. Used for quest-affected actors whose state
//              should diverge across teams (e.g. Mido — once one team
//              has unlocked his side-step, that progress shouldn't
//              auto-apply to the other team).
//   - Global — all clients apply the state regardless of team. Used
//              for world-canon physical state (water levels, push
//              blocks, rotating logs, enemies). This is the existing
//              behaviour for all currently-synced actors.
//
// Scope is stored per-actor-instance via ObjectExtension (sibling to
// EnemyNetId) — a single ACTOR_EN_MD instance in one scene can have
// scope = Team while a different instance in another scene has scope
// = Global. Most actors get one consistent scope across instances;
// the per-instance plumbing supports future per-instance overrides
// without restructuring.
//
// Phase 0 (this framework): default = Global, preserves all existing
// synced-actor behaviour. Send-side reads scope and adds it to the
// outgoing ENEMY_STATE payload. Receive-side filters Team-scoped
// packets by sender's TeamId.
//
// Phase 1 (Mido v1 customer, #184): Mido's Init hook sets scope =
// Team. Other NPCs added incrementally per surfaced bug.
//
// Phase 3 (migration): default flips to None. Currently-synced actors
// (Boss_Goma, En_Goroiwa, En_Sw, En_Dekunuts, ENEMY-category enemies)
// gain explicit Init-time scope = Global declarations.
//
// See `Plans/generic_npc_state_sync.md` for full design.

#ifdef __cplusplus

struct Actor;

namespace AnchorSync {

enum class ActorSyncScope {
    None,
    Team,
    Global,
};

// Returns the sync scope currently set on an actor instance. Returns
// Global if no scope has been explicitly set (Phase 0 default —
// preserves existing synced-actor behaviour). Returns None if actor
// is null.
ActorSyncScope GetActorSyncScope(const Actor* actor);

// Sets the sync scope on an actor instance. Typically called from the
// actor's Init hook. Persists for the actor's lifetime via
// ObjectExtension.
void SetActorSyncScope(const Actor* actor, ActorSyncScope scope);

// Returns the string name used in JSON payloads. "global" / "team".
// None scope returns "none" (caller should typically not emit packets
// for None-scope actors).
const char* ActorSyncScopeToString(ActorSyncScope scope);

// Parses the JSON string back to enum. Unknown strings return Global
// (forward-compat / legacy default).
ActorSyncScope ActorSyncScopeFromString(const char* s);

}  // namespace AnchorSync

#endif  // __cplusplus

#endif  // ANCHOR_ACTOR_SYNC_SCOPE_H
