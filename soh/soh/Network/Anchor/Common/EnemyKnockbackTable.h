#pragma once

#include <libultraship/libultraship.h>  // pre-load C++ template bridge headers
                                        // before z64.h pulls them in via extern "C"

extern "C" {
#include "z64.h"
}

// Per-actor vanilla knockback parameter lookup — Path A receive-side
// payload, paired with the Pitfall 28 sender-side gate. See the
// "Path A workflow" entry in session_state.md.
//
// ─── Background ───────────────────────────────────────────────────
//
// When a vanilla enemy hits the local Link, its AT_HIT branch calls
// one of func_8002F6D4 / _8002F71C / _8002F758 / _8002F7A0 with
// specific arguments (knockback speed, vertical velocity, type,
// damage). The function writes those onto GET_PLAYER's knockback*
// fields; Player_Update reads them next tick and applies the full
// vanilla damage response (HP loss, animation, iframes via
// Player_SetIntangibility).
//
// For cross-machine PvE damage routing, the receiver (peer) needs the
// SAME arguments so its Player_Update produces the same effect. We
// mirror the constants here so the sender (host's DummyPlayer.cpp
// AC_HIT detection) can ship them in the DAMAGE_PLAYER packet without
// touching enemy code.
//
// ─── Adding a new attacker (full Path A workflow) ─────────────────
//
// Triggered by the `[Path A bypass]` WARN log fired in DummyPlayer.cpp
// when an unregistered attacker hits a DummyPlayer. The full fix is
// TWO parts — each addresses ONE of the two bug classes:
//
// (1) Bug 2 (cross-machine damage parity)
//     Add an entry to sTable in EnemyKnockbackTable.cpp:
//     - Read the actor's AT_HIT branch in its .c file
//     - Find the func_8002F* call → extract {speed, yaw, yVel, kbDmg}
//       from the args; pick `type` per the wrapper cheat sheet at the
//       top of the .cpp file (which wrapper → which knockbackType)
//     - Cite file:line in the entry comment for verifiability
//     This alone makes Bug 2 close — peer takes correct damage +
//     vanilla knockback animation + iframes via Player_Update.
//
// (2) Bug 1 (host's local Link must not be falsely knocked back)
//     The same vanilla actor calls func_8002F* UNCONDITIONALLY in its
//     AT_HIT branch, setting GET_PLAYER's knockback fields → host's
//     local Link gets knocked back even when only a DummyPlayer was
//     hit. Pitfall 28 fix in the actor's .c file:
//
//       if (this->collider.base.atFlags & AT_HIT) {
//           this->collider.base.atFlags &= ~AT_HIT;
//           s32 linkInRange = (Anchor_DistXZToLocalLink(&this->actor, play)
//                              < HIT_RADIUS);                       // ← add
//           // Part A: enemy state-machine reaction — always fires.
//           //   (yaw flip, state transitions, collision cooldowns, etc.)
//           if (linkInRange) {                                       // ← add
//               // Part B: local-Link side effects — only when local
//               //   Link is the actual target. Existing vanilla:
//               func_8002F6D4(play, &this->actor, ...);
//               Player_PlaySfx(...);
//           }
//       }
//
//     HIT_RADIUS = collider radius + Link body 30u + small margin.
//     Existing precedents: En_Goroiwa = 100u (sphere 58), Bg_Jya_
//     Goroiwa = 100u, En_Bigokuta = 90u (cylinder 50).
//
//     Some actors already have an equivalent gate (e.g., En_Ik's
//     `if (&player->actor == base.at)` pointer-equality at
//     z_en_ik.c:782) — in that case skip part (2) and just ship (1).
//
// Without (2), Bug 1 stays present for that attacker even after (1)
// fixes Bug 2. Both halves are required for full vanilla parity.
//
// Reference:
//   Plans/pitfall_28_actor_audit.md       — 42-actor candidate list
//   Plans/dummy_player_damage_table_audit.md — why the wire path looks
//                                           the way it does
//   session_state.md Pitfall 28 + "Path A workflow"

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
