#include "EnemyKnockbackTable.h"

#include <unordered_map>

extern "C" {
#include "macros.h"  // ACTOR_EN_* ids
}

namespace AnchorKnockback {

// Per-actor knockback parameter table.
//
// Each entry mirrors the exact arguments the actor's AT_HIT branch
// passes to func_8002F6D4 / _71C / _758 / _7A0. Cite the source
// file:line and the wrapper used so future maintainers can verify
// against vanilla code.
//
// Add new entries here; no other code changes are needed for the
// cross-machine knockback wire path to use them.
static const std::unordered_map<int16_t, KnockbackParams> sTable = {
    // ─── Goroiwa (rolling boulder) ────────────────────────────────
    // z_en_goroiwa.c:655 (vanilla branch — was inside `if (linkInRange)`):
    //   func_8002F6D4(play, &this->actor, 2.0f, yawTowardsPlayer, 0.0f, 0);
    // func_8002F6D4 → func_8002F698 with arg5=2 (= PLAYER_KNOCKBACK_LARGE)
    // and arg6 = caller's last arg = 0 (knockbackDamage).
    { ACTOR_EN_GOROIWA, { 2.0f, 0.0f, 2 /* LARGE */, 0 /* knockbackDamage */ } },

    // Future entries (TODO — extend per Pitfall 28 actor audit):
    //  ACTOR_EN_IK     — Iron Knuckle axe (z_en_ik.c:793 func_8002F71C)
    //  ACTOR_EN_GELDB  — Gerudo (z_en_geldB.c:924 func_8002F71C)
    //  ACTOR_EN_MB     — Moblin spear / club (z_en_mb.c multi-site)
    //  ACTOR_EN_BIGOKUTA — Big Octorok body slam (z_en_bigokuta.c:742)
    //  ACTOR_BG_JYA_GOROIWA — Spirit boulder (same family as En_Goroiwa)
    //  ... (37 total intersection per Pitfall 28 audit)
};

bool LookupKnockback(int16_t actorId, KnockbackParams* out) {
    auto it = sTable.find(actorId);
    if (it == sTable.end()) return false;
    if (out != nullptr) *out = it->second;
    return true;
}

}  // namespace AnchorKnockback
