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
    // Wrapper → knockback type cheat sheet (per z_actor.c:2216-2240):
    //   func_8002F6D4(play, actor, speed, yaw, yVel, kbDmg) → LARGE (2) with kbDmg
    //   func_8002F71C(play, actor, speed, yaw, yVel)        → LARGE (2) with kbDmg=0
    //   func_8002F758(play, actor, speed, yaw, yVel, kbDmg) → SMALL (1) with kbDmg
    //   func_8002F7A0(play, actor, speed, yaw, yVel)        → SMALL (1) with kbDmg=0

    // ─── Goroiwa (rolling boulder) ────────────────────────────────
    // z_en_goroiwa.c:637 — func_8002F6D4(play, this, 2.0f, yawTowardsPlayer, 0.0f, 0)
    { ACTOR_EN_GOROIWA, { 2.0f, 0.0f, 2 /* LARGE */, 0 /* kbDmg */ } },

    // ─── Bg_Jya_Goroiwa (Spirit Temple boulder) ───────────────────
    // z_bg_jya_goroiwa.c:156 — func_8002F6D4(play, thisx, 2.0f, yawTowardsPlayer, 0.0f, 0)
    // Same family as En_Goroiwa, same vanilla params.
    { ACTOR_BG_JYA_GOROIWA, { 2.0f, 0.0f, 2 /* LARGE */, 0 } },

    // ─── Iron Knuckle (axe swing) ────────────────────────────────
    // z_en_ik.c:793 — func_8002F71C(play, &this->actor, 8.0f, yawTowardsPlayer, 8.0f)
    // AT damage = 0x40 (4 hearts vanilla) — shipped via raw AT damage path.
    // Existing pointer-equality gate at z_en_ik.c:782 handles Bug 1
    // class for the typical case; no per-actor source edit needed.
    { ACTOR_EN_IK, { 8.0f, 8.0f, 2 /* LARGE */, 0 } },

    // ─── Bigokuta (Big Octorok body slam) ─────────────────────────
    // z_en_bigokuta.c:742 — func_8002F71C(play, this, 10.0f, world.rot.y+effectRot, 5.0f)
    // Note: knockback direction is computed from actor's own rotation +
    // a relative-yaw-derived rotation offset, NOT yawTowardsPlayer
    // directly. This means the receiver's knockbackYaw (computed from
    // attackerPos→self) will differ from the host's exact value, but
    // the magnitude (LARGE knockback, 10 speed, 5 yVel) is preserved.
    // Cosmetic difference; functional parity holds.
    { ACTOR_EN_BIGOKUTA, { 10.0f, 5.0f, 2 /* LARGE */, 0 } },

    // ─── Bari (En_Ba — jellyfish blob split from Vali) #128 ──────
    // z_en_ba.c:310 — func_8002F71C(play, this, 8.0f, yawTowardsPlayer, 8.0f)
    // Existing pointer-equality gate at z_en_ba.c:309
    // (`collider.base.at == &player->actor`) handles Bug 1 for the
    // typical case (same shape as En_Ik / En_GeldB — fails-closed
    // when DummyPlayer overwrites .at, no false knockback on local
    // Link). No per-actor source edit needed.
    { ACTOR_EN_BA, { 8.0f, 8.0f, 2 /* LARGE */, 0 } },

    // Future entries (Pitfall 28 audit remaining queue):
    //  ACTOR_EN_GELDB  — Gerudo (z_en_geldB.c:924 func_8002F71C 6.0/yaw/6.0)
    //  ACTOR_EN_BROB   — Coiled spike (z_en_brob.c func_8002F71C 5.0/yaw/1.0)
    //  ACTOR_EN_FD     — Flare Dancer (z_en_fd.c:315 func_8002F71C speed+2/yaw/6.0)
    //  ACTOR_EN_HONOTRAP — Fire trap (z_en_honotrap.c:324 func_8002F71C 5.0/yaw/0.0)
    //  ACTOR_EN_MB     — Moblin (7 sites — needs per-site analysis)
    //  ACTOR_EN_IK shield AC handling (separate from axe AT)
    //  ... (full list in Plans/pitfall_28_actor_audit.md Tier 2/4)
};

bool LookupKnockback(int16_t actorId, KnockbackParams* out) {
    auto it = sTable.find(actorId);
    if (it == sTable.end()) return false;
    if (out != nullptr) *out = it->second;
    return true;
}

}  // namespace AnchorKnockback
