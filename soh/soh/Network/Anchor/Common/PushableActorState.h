#ifndef SOH_ANCHOR_COMMON_PUSHABLE_ACTOR_STATE_H
#define SOH_ANCHOR_COMMON_PUSHABLE_ACTOR_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;

// Returns 1 when `actor` is currently mid-push on the local client (active
// player-driven slide OR post-edge fall), 0 otherwise. Used by ENEMY_STATE
// receive-side to skip the world.pos write so a peer's stale broadcast
// doesn't fight the local push animation.
//
// Add new pushable actors by extending the switch in PushableActorState.cpp;
// every consumer site reads this helper rather than the actor-specific
// state field, so admitting a new pushable is a single-file change.
int Anchor_IsActorMidPush(struct Actor* actor);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ANCHOR_COMMON_PUSHABLE_ACTOR_STATE_H
