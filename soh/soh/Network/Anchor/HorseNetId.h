#pragma once

// Attached to ACTOR_EN_HORSE instances that participate in cross-client
// sync. Mirror of the EnemyNetId pattern; horse sync is a separate
// pipeline because horse is Skin-rendered (not SkelAnime) and its
// animation model is enum-dispatched rather than joint-table-driven.
//
// Per Plans/horse_sync_plan.md:
//   - Owner-authoritative: rider IS the owner of the horse they're
//     mounted on; ownerClientId identifies the player.
//   - Receivers' local horse instances run vanilla state-machine code
//     but skip AI / boost / bridge-jump triggers when isPeerOwned is
//     true; animation is driven by the synced actionState enum.
//   - Vanilla rider position is derived per-receiver from
//     horse.world.pos + horse.riderPos (Skin saddle limb offset); never
//     shipped on the wire.

#include <cstdint>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

struct HorseNetId {
    // Stable identifier across receivers. Constructed as
    // (ownerClientId << 24) | ((uint16_t)sceneNum << 8) | params so two
    // peers' Eponas in the same scene with the same params produce
    // distinct ids. Owner-side: assigned at OnActorSpawn for in-scope
    // horse instances. Receiver-side: assigned by Anchor_SpawnPeerHorse
    // from the HORSE_SPAWN packet payload.
    uint32_t netId = 0;

    // Owning client's id. 0 means "not assigned yet" (the local actor
    // hasn't been admitted to the sync pipeline). For owner == local
    // client, isPeerOwned == false and the actor runs full vanilla AI.
    uint32_t ownerClientId = 0;

    // True on every receiver except the owner. Gates local AI skip
    // (state-machine decisions, bridge-jump auto-trigger, boost decrement,
    // mount-side eligibility for other players, etc.) — peer horses
    // animate from synced actionState and don't make their own choices.
    bool isPeerOwned = false;

    // Last ENHORSE_ACT_* received from the owner. -1 means "no state
    // received yet"; mirrors the EnemyNetId::netStateIndex convention.
    // Receivers map this to the action-function index via the actor's
    // own state machine.
    int16_t lastAppliedActionState = -1;
};
