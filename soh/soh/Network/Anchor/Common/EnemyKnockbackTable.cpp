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

    // ─── Gerudo (En_GeldB) #138 ──────────────────────────────────
    // z_en_geldB.c:924 — func_8002F71C(play, this, 6.0f, yawTowardsPlayer, 6.0f)
    // Existing pointer-equality gate at z_en_geldB.c:923
    // (`&player->actor == swordCollider.base.at`) handles Bug 1 for
    // the typical case (same shape as En_Ik / En_Ba). No per-actor
    // source edit needed.
    { ACTOR_EN_GELDB, { 6.0f, 6.0f, 2 /* LARGE */, 0 } },

    // ─── Flare Dancer (En_Fd flame attack) #98 ───────────────────
    // z_en_fd.c:315 — func_8002F71C(play, this, speedXZ+2.0f, yawTowardsPlayer, 6.0f)
    // Speed is variable (this->actor.speedXZ + 2.0f). Flare Dancer's
    // speedXZ at attack impact is typically 2 (slow dance approach),
    // giving knockback speed ≈ 4. Use 4.0 as baseline; the variable
    // value isn't reproducible cross-machine without per-frame ship.
    // Source-side linkInRange gate added at z_en_fd.c:304 (the AT_HIT
    // branch was unconditional re: GET_PLAYER, Bug 1 territory).
    { ACTOR_EN_FD, { 4.0f, 6.0f, 2 /* LARGE */, 0 } },

    // ─── Fire Trap (En_Honotrap flame drop) #143 ─────────────────
    // z_en_honotrap.c:324 — func_8002F71C(play, this, 5.0f, yawTowardsPlayer, 0.0f)
    // Flame projectile drops vertically; single-impact (FlameDrop
    // action transitions to vanish after AT_HIT). Source-side
    // linkInRange gate added at z_en_honotrap.c:322-326 (the AT_HIT
    // branch was unconditional re: GET_PLAYER, Bug 1 territory).
    { ACTOR_EN_HONOTRAP, { 5.0f, 0.0f, 2 /* LARGE */, 0 } },

    // ─── En_Bx (water spike enemy) #134 ──────────────────────────
    // z_en_bx.c:158 — func_8002F71C(play, this, 6.0f, tmp32, 6.0f)
    // Yaw is variable per param bit 0x80 (world.rot.y vs yawTowardsPlayer).
    // Existing complex multi-gate at z_en_bx.c:140-143 combines
    // `xzDistToPlayer <= 70.0f` with three pointer-equality checks —
    // mostly fails-closed when DummyPlayer is the .at-target. No
    // per-actor source edit for this commit; flag risk in audit doc
    // if field-test surfaces false-knockback on host.
    { ACTOR_EN_BX, { 6.0f, 6.0f, 2 /* LARGE */, 0 } },

    // ─── Door_Killer (patrol door enemy) — no per-actor tracker ──
    // z_door_killer.c:381 — func_8002F6D4(play, this, 6.0f, yawTowardsPlayer, 6.0f, 16)
    // Uses func_8002F6D4 (not _71C) — last arg 16 is kbDmg (additive
    // damage applied during knockback animation). Existing coordinate-
    // based gate at z_door_killer.c:374-380 (`|playerPosRelToDoor.y|<20
    // && |x|<20 && 0<z<100`) is a precise box check around the door
    // that naturally prevents Bug 1 (only fires when local Link is
    // physically passing through the door's impact zone).
    { ACTOR_DOOR_KILLER, { 6.0f, 6.0f, 2 /* LARGE */, 16 /* kbDmg */ } },

    // ─── Bg_Haka_Tubo (Shadow Temple skull jar flames) ────────────
    // z_bg_haka_tubo.c:125 — func_8002F71C(play, this, 5.0f, yawTowardsPlayer, 5.0f)
    // No per-actor tracker (BG-class environmental hazard). Source-side
    // linkInRange gate added at z_bg_haka_tubo.c:123 (the AT_HIT branch
    // was unconditional re: GET_PLAYER, Bug 1 territory).
    { ACTOR_BG_HAKA_TUBO, { 5.0f, 5.0f, 2 /* LARGE */, 0 } },

    // ─── Bg_Hidan_Curtain (Fire Temple flame curtain) ────────────
    // z_bg_hidan_curtain.c:219 — func_8002F71C(play, this, 5.0f, yawTowardsPlayer, 1.0f)
    // No per-actor tracker (BG-class hazard). Source-side linkInRange
    // gate added at z_bg_hidan_curtain.c:217 (unconditional func_8002F71C
    // was Bug 1 territory).
    { ACTOR_BG_HIDAN_CURTAIN, { 5.0f, 1.0f, 2 /* LARGE */, 0 } },

    // ─── Bg_Hidan_Firewall (Fire Temple fire wall) ───────────────
    // z_bg_hidan_firewall.c:137 (inside BgHidanFirewall_Collide) —
    //   func_8002F71C(play, this, 5.0f, phi_a3, 1.0f)
    // Yaw is `phi_a3` (computed locally as shape.rot.y or +0x8000
    // based on Actor_IsFacingPlayer). Not affected by #153 overlay.
    // No per-actor tracker (BG-class hazard). Source-side linkInRange
    // gate added at z_bg_hidan_firewall.c:183 (the gate-site that
    // calls Collide).
    { ACTOR_BG_HIDAN_FIREWALL, { 5.0f, 1.0f, 2 /* LARGE */, 0 } },

    // ─── En_Mm (Running Man / Marathon Runner — body shove) ──────
    // z_en_mm.c:471 — func_8002F71C(play, this, 3.0f, yawTowardsPlayer, 4.0f)
    // Gate is OC2_HIT_PLAYER (body overlap), not AT_HIT — fires when the
    // sprinting Running Man bumps into the player after the marathon prize
    // has been claimed (ITEMGETINF_3B). Source-side linkInRange gate
    // added at z_en_mm.c:470 (OC2 fires for both local Link and any
    // DummyPlayer overlap on host, Bug 1 territory).
    { ACTOR_EN_MM, { 3.0f, 4.0f, 2 /* LARGE */, 0 } },

    // ─── Bg_Ydan_Maruta (Deku Tree spike trap log) ───────────────
    // z_bg_ydan_maruta.c:141 — func_8002F71C(play, this, 7.0f, shape.rot.y, 6.0f)
    // Yaw is `shape.rot.y` (own facing — not affected by #153 overlay).
    // No per-actor tracker (BG-class hazard). Source-side linkInRange
    // gate added at z_bg_ydan_maruta.c:140 (the AT_HIT branch was
    // unconditional re: GET_PLAYER, Bug 1 territory).
    { ACTOR_BG_YDAN_MARUTA, { 7.0f, 6.0f, 2 /* LARGE */, 0 } },

    // ─── Bg_Hidan_Fwbig (Fire Temple big fire wall) ──────────────
    // z_bg_hidan_fwbig.c:228 — func_8002F71C(play, this, 5.0f, world.rot.y, 1.0f)
    // Yaw is `world.rot.y` (computed from player projection at line 219;
    // own facing — not affected by #153 overlay yaw cache). Same params
    // as Bg_Hidan_Firewall (small variant). State-machine reaction
    // (`actionFunc = BgHidanFwbig_Lower` at line 230) MUST fire regardless
    // of Bug 1 gate so cross-machine state parity holds. Source-side
    // linkInRange gate added at z_bg_hidan_fwbig.c:227 (wraps the
    // func_8002F71C call only; state transition runs unconditionally).
    // No per-actor tracker (BG-class hazard).
    { ACTOR_BG_HIDAN_FWBIG, { 5.0f, 1.0f, 2 /* LARGE */, 0 } },

    // ─── Bg_Hidan_Sekizou (Fire Temple fire-breathing statue) ────
    // z_bg_hidan_sekizou.c:262 (inside func_8088D750) —
    //   func_8002F71C(play, this, 5.0f, phi_a3, 1.0f)
    // Yaw `phi_a3` is computed from `yawTowardsPlayer` at line 243
    // then cardinal-snapped via a switch table — partially affected
    // by #153 overlay (cache read at line 243). Receiver uses host's
    // shipped yaw verbatim via Player_Update; for a statue facing a
    // fixed cardinal direction this is functionally correct (host's
    // statue snaps to the closest cardinal, peer's local statue
    // independently snaps). Source-side linkInRange gate added at
    // z_bg_hidan_sekizou.c:276 (AT_HIT branch).
    // No per-actor tracker (BG-class hazard).
    { ACTOR_BG_HIDAN_SEKIZOU, { 5.0f, 1.0f, 2 /* LARGE */, 0 } },

    // ─── En_Niw (regular cucco — swarm attack body slam) ─────────
    // z_en_niw.c:1080 — func_8002F6D4(play, this, 2.0f, world.rot.y, 0.0f, 0x10)
    // Yaw is `world.rot.y` (own facing — not affected by #153 overlay).
    // kbDmg=0x10 (1 heart additive). Source-side linkInRange gate
    // added at z_en_niw.c:1079 (the gate used xyzDistToPlayerSq which
    // is overwritten by #153's nearest-player overlay).
    { ACTOR_EN_NIW, { 2.0f, 0.0f, 2 /* LARGE */, 0x10 /* kbDmg */ } },

    // ─── En_Attack_Niw (angry cucco — kamikaze dive) ─────────────
    // z_en_attack_niw.c:369 — func_8002F6D4(play, this, 2.0f, world.rot.y, 0.0f, 0x10)
    // Yaw is `world.rot.y` (own facing — not affected by #153 overlay).
    // kbDmg=0x10. Same params as En_Niw (the angry cucco IS a swarm
    // member spawned from En_Niw). Source-side linkInRange gate
    // added at z_en_attack_niw.c:365 (xyzDistToPlayerSq gate, Bug 1).
    { ACTOR_EN_ATTACK_NIW, { 2.0f, 0.0f, 2 /* LARGE */, 0x10 /* kbDmg */ } },

    // Future entries (Pitfall 28 audit remaining queue):
    //  ACTOR_EN_BROB   — Coiled spike (DynaPolyActor_IsPlayerOnTop
    //    based — needs different cross-machine treatment, not Path A)
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
