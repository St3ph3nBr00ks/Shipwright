#pragma once

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

// Per-actor vanilla knockback parameter lookup.
//
// Background — when a vanilla enemy hits the local Link, the enemy's
// AT_HIT branch calls one of func_8002F6D4 / _8002F71C / _8002F758 /
// _8002F7A0 with specific arguments (knockback speed, vertical
// velocity, type, damage). The function writes those onto GET_PLAYER's
// knockback* fields; Player_Update reads them next tick and applies
// the full vanilla damage response (HP loss, animation, iframes via
// Player_SetIntangibility).
//
// For cross-machine PvE damage routing, the receiver (peer) needs the
// SAME arguments so its Player_Update produces the same effect. The
// values are constants baked into each enemy's AT_HIT branch — we
// mirror them here so the sender (host's DummyPlayer.cpp AC_HIT
// detection) can ship them in the DAMAGE_PLAYER packet without
// touching enemy code.
//
// Single responsibility — this header / module is the ONE place that
// catalogues per-actor knockback constants. Adding a new attacker =
// one new entry in EnemyKnockbackTable.cpp's sTable. No edits in any
// packet handler or hook are needed.
//
// See Plans/dummy_player_damage_table_audit.md and the Pitfall 28
// actor audit for the candidate list (the 42 actors that invoke a
// direct-player-effect function).

namespace AnchorKnockback {

// Mirrors vanilla's PLAYER_KNOCKBACK_* values (z64player.h:591-594):
//   0 = NONE, 1 = SMALL, 2 = LARGE, 3 = LARGE_SHOCK.
// Derived from which wrapper the enemy uses:
//   func_8002F6D4 / _71C  → LARGE   (func_8002F698 hardcodes arg5=2)
//   func_8002F758 / _7A0  → SMALL   (func_8002F698 hardcodes arg5=1)
//   Direct func_8002F698  → caller picks (used by shock-effect actors)
struct KnockbackParams {
    float    speed;       // → Player.knockbackSpeed
    float    yVelocity;   // → Player.knockbackYVelocity
    uint32_t type;        // → Player.knockbackType (1/2/3)
    uint32_t damage;      // → Player.knockbackDamage (additive to colChkInfo.damage)
};

// Look up vanilla knockback params for `actorId`.
//
// Returns true and writes the params into `*out` if registered.
// Returns false if the actor has no registered entry — caller should
// then either omit the knockback block from the wire payload (legacy
// receiver fallback) or pass a generic default.
bool LookupKnockback(int16_t actorId, KnockbackParams* out);

}  // namespace AnchorKnockback
