#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemyNetId.h"  // #243.7.2 — explicit (was transitive via Anchor.h)
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
// Per-actor AC_HIT setter — Phase 3 of #174/#175. Each enemy's collider lives
// at a different field path on its own struct, so we cast and set per actor id.
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
#include "src/overlays/actors/ovl_En_Firefly/z_en_firefly.h"
#include "src/overlays/actors/ovl_En_Crow/z_en_crow.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_En_St/z_en_st.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
// 2026-06-07 wave — actors landing the same AC_HIT routing as En_St
// (log 431 follow-up audit). Each enemy below either has no acHitInfo
// deref in its damage code (safe-easy add) or its deref is null-
// guarded in the same commit as the case addition. See bug findings
// doc Plans/Testing/BugFindings_EnSt_Log431_2026-06-07.md.
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Reeba/z_en_reeba.h"
#include "src/overlays/actors/ovl_En_Ik/z_en_ik.h"
#include "src/overlays/actors/ovl_En_Hintnuts/z_en_hintnuts.h"
#include "src/overlays/actors/ovl_En_Peehat/z_en_peehat.h"
#include "src/overlays/actors/ovl_En_Poh/z_en_poh.h"
#include "src/overlays/actors/ovl_En_Bili/z_en_bili.h"
// #126 — Bari big jellyfish (En_Vali) AC_HIT routing.
#include "src/overlays/actors/ovl_En_Vali/z_en_vali.h"
// En_Zf — Lizalfos + Dinolfos AC_HIT routing.
#include "src/overlays/actors/ovl_En_Zf/z_en_zf.h"
// En_Mb — Moblin (Club / SpearGuard / SpearPatrol) AC_HIT routing.
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
// En_Bigokuta — Big Octo miniboss AC_HIT routing (#130).
#include "src/overlays/actors/ovl_En_Bigokuta/z_en_bigokuta.h"
// En_Fd — Flare Dancer enflamed shell AC_HIT routing (#100).
#include "src/overlays/actors/ovl_En_Fd/z_en_fd.h"
// En_GeldB — Gerudo Thief AC_HIT routing.
#include "src/overlays/actors/ovl_En_GeldB/z_en_geldb.h"
// En_Po_Field — Field Poe (Hyrule Field night) AC_HIT routing.
#include "src/overlays/actors/ovl_En_Po_Field/z_en_po_field.h"
// #129 / en_bb_sync_plan.md — Bubble (En_Bb) AC_HIT routing.
#include "src/overlays/actors/ovl_En_Bb/z_en_bb.h"
// En_Ssh — Skulltula sibling, same multi-collider front-shield pattern
// as En_St. Added 2026-06-07 after audit triggered by En_St fixes.
#include "src/overlays/actors/ovl_En_Ssh/z_en_ssh.h"
// Bug B — En_Goma (Boss_Goma's larvae) needs AC_HIT for its damage block to
// fire. Larva is null-guarded in z_en_goma.c so synthetic AC_HIT without
// acHitInfo no longer crashes (treated as dmgFlags=0 → falls through to the
// sword-damage path, decrements health by 1, plays SetupHurt).
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
// En_Skb (Stalchild) — Hyrule Field at night spawner output. Damage gate
// at z_en_skb.c:456 reads collider.base.acFlags & 2 (AC_HIT).
#include "src/overlays/actors/ovl_En_Skb/z_en_skb.h"
// En_Tite (Tektite) — feature/sync-en-tite. Single ColliderJntSph at
// `collider`. Damage gate at z_en_tite.c:850 (EnTite_CheckDamage) reads
// `collider.base.acFlags & AC_HIT`. No `base.ac->X` derefs in damage
// path (verified per Plans/ApplySyncAcHitToActor_BaseAcAudit_2026-06-07.md
// pattern). Safe direct AC_HIT bit-set.
#include "src/overlays/actors/ovl_En_Tite/z_en_tite.h"
// Boss_Goma — non-host damage requires synthesised acHitInfo + BUMP_HIT
// because BossGoma_UpdateHit (z_boss_goma.c:1823) gates on bumperFlags &
// BUMP_HIT (not AC_HIT) and dereferences acHitInfo->toucher.dmgFlags.
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
// NPC Invader — runtime-allocated actor id (gEnInvaderId), so the AC_HIT
// dispatch below uses a runtime if() rather than a case label. EnInvader_Update
// reads `collider.base.acFlags & AC_HIT` then decrements health when
// `colChkInfo.damage > 0` (z_en_invader.c). Without setting AC_HIT here, host's
// queued DAMAGE_ENEMY damage gets added to colChkInfo.damage but never
// consumed — host's Invader is unkillable from peer hits. (Race B fix:
// peer's local damage on a synced enemy is force-routed through host so
// the kill-attribution and drop pipeline are driven by the same code
// path as host-local hits.)
#include "src/overlays/actors/ovl_En_Invader/z_en_invader.h"
extern PlayState* gPlayState;
}

/**
 * DAMAGE_ENEMY
 *
 * Sent by a non-host client when its local player hits an enemy this frame.
 * The host is the only receiver — it applies the damage to its authoritative
 * copy of the enemy via colChkInfo.damage, which the enemy's own update()
 * will process on the next frame (same path as a local sword hit).
 *
 * Why colChkInfo.damage and not a direct health write:
 *   Setting colChkInfo.damage lets the enemy's own update() process the hit
 *   naturally — playing a hit reaction animation, transitioning to a death
 *   state if health reaches 0, and calling GameInteractor_ExecuteOnEnemyDefeat
 *   which broadcasts ENEMY_DEFEATED. A direct health write would bypass all
 *   of that, leaving the enemy alive on the host with 0 health.
 *
 * Timing:
 *   CollisionCheck_Damage runs BEFORE Actor_UpdateAll each frame. By the time
 *   OnActorUpdate fires (inside Actor_UpdateAll), colChkInfo.damage holds the
 *   total damage applied this frame and has NOT yet been cleared by
 *   CollisionCheck_ResetDamage (which runs after OnActorUpdate). The non-host
 *   reads this value and sends it here. The host sets colChkInfo.damage on its
 *   actor during the subsequent OnGameFrameUpdate (after Actor_UpdateAll), so
 *   it persists into the next frame's actor->update() without being double-cleared.
 *
 * Guards:
 *   - Only sent when !hasLocalDeath: on the killing blow, actor->update() fires
 *     OnEnemyDefeat (which sets hasLocalDeath) before OnActorUpdate runs, so
 *     ENEMY_DEFEATED already handles the kill — no DAMAGE_ENEMY needed.
 *   - Host only applies if actor is alive (update != NULL, health > 0).
 *   - Uses += so multiple packets arriving in the same frame accumulate correctly.
 *
 * Packet fields:
 *   netId         - identifies the enemy that was hit
 *   sceneNum      - guards against cross-scene application
 *   damage        - colChkInfo.damage value (total damage from all hits this frame)
 *   damageEffect  - schema 2 (#174/#175): enemy.colChkInfo.damageEffect set by the
 *                   damage-table lookup in CollisionCheck_SetATvsACDamage
 *   atHitEffect   - schema 2 (#174/#175): player.colChkInfo.atHitEffect set by
 *                   CollisionCheck_SetATvsAC. Many enemies branch on these.
 *
 * Schema-versioning note (Pillar F):
 *   The new fields are sent unconditionally rather than gated through
 *   PeerSupportsField. They are two scalar ints; legacy pre-Pillar-F peers
 *   tolerate missing fields by deserialising-to-default, and conversely a
 *   pre-bump receiver simply sees `payload.contains("damageEffect") == false`
 *   and falls back to the schema-1 behaviour. Cost of always sending is
 *   negligible; gating saves nothing.
 */

void Anchor::SendPacket_DamageEnemy(uint32_t netId, u8 damage, u8 damageEffect, u8 atHitEffect) {
    SendPacket_DamageEnemy(netId, damage, damageEffect, atHitEffect, false /* isArmoredHit */);
}

void Anchor::SendPacket_DamageEnemy(uint32_t netId, u8 damage, u8 damageEffect, u8 atHitEffect, bool isArmoredHit) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = DAMAGE_ENEMY;
    payload["sceneNum"]     = gPlayState->sceneNum;
    payload["netId"]        = netId;
    payload["damage"]       = damage;
    // Schema 2 (#174/#175) — see comment block above for rationale.
    payload["damageEffect"] = (int)damageEffect;
    payload["atHitEffect"]  = (int)atHitEffect;
    // Schema 3 (#90 / log 434) — armored-hit flag for En_St front-shield
    // sync. Only emitted when true (false omits the field, preserving
    // legacy behavior + smaller payload for common case).
    if (isArmoredHit) {
        payload["isArmoredHit"] = true;
    }
    payload["quiet"]        = true;
    PacketTimeline::SetTimelineField(payload);

    // Send only to the room host — it is the authority on enemy health
    // for actors in our current room. Per-room targeting (Pillar A Phase 2)
    // ensures the packet reaches the actual authoritative client even when
    // the original room owner is offline or in a different scene.
    uint32_t targetId = ::SceneAuthority::GetRoomHostClientId(
        (int16_t)gPlayState->sceneNum,
        (int8_t)gPlayState->roomCtx.curRoom.num,
        (uint8_t)(gSaveContext.linkAge & 0x1));
    if (targetId != 0 && targetId != ownClientId) {
        auto it = clients.find(targetId);
        if (it != clients.end() && it->second.online && it->second.isSaveLoaded) {
            payload["targetClientId"] = targetId;
            SendJsonToRemote(payload);
            SPDLOG_INFO("[DamageEnemy] Sent netId={} damage={} damageEffect={} atHitEffect={} target={}",
                        netId, (int)damage, (int)damageEffect, (int)atHitEffect, targetId);
        }
    }
}

void Anchor::HandlePacket_DamageEnemy(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Pillar B Phase 1 — drop cross-timeline scene-scoped traffic.
    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        return;
    }

    // Only the host applies incoming damage events.
    if (!::SceneAuthority::IsEffectiveHost()) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (VALIDATE(::ReceiveValidator::ValidateSameScene(sceneNum)) !=
        ::ReceiveValidator::ValidationVerdict::Valid) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);
    u8       damage = (u8)payload.value("damage", 0);
    // Schema 3 (#90 / log 434) — En_St front-shield (armored) hit.
    // Peer sends this when they hit the Skulltula's invulnerable face;
    // host plays the sway animation locally without applying damage.
    bool isArmoredHit = payload.value("isArmoredHit", false);
    if (damage == 0 && !isArmoredHit) {
        return;
    }

    // Find the target across all syncable categories. Widened beyond
    // ACTORCAT_ENEMY so non-host damage to non-ENEMY synced actors
    // (allowlisted via IsSyncedWorldActor or mid-transition BOSS /
    // ITEMACTION / MISC instances) doesn't silently drop.
    Actor* actor = FindActorByNetId(gPlayState, netId);
    if (actor == nullptr) {
        return;
    }
    // Skip dead actors — actor->update == NULL after Actor_Kill.
    if (actor->update == nullptr || actor->colChkInfo.health == 0) {
        return;
    }

    // En_St / En_Ssh armored-hit special path: peer hit the invulnerable
    // front shield. Set AC_HIT on cyl [2] only so host's CheckHitFront(side)
    // fires the sway anim WITHOUT calling Actor_ApplyDamage. The
    // resulting swayTimer / playSwayFlag changes are captured in
    // host's joint table on the next ENEMY_STATE broadcast — sway
    // animation now propagates cross-machine to all peers in scene.
    if (isArmoredHit && actor->id == ACTOR_EN_ST) {
        EnSt* st = (EnSt*)actor;
        st->colCylinder[2].base.acFlags |= AC_HIT;
        SPDLOG_INFO("[DamageEnemy] En_St armored-hit RECV netId={} — set cyl[2] AC_HIT "
                    "(sway anim will propagate via ENEMY_STATE)", netId);
        return;
    }
    if (isArmoredHit && actor->id == ACTOR_EN_SSH) {
        EnSsh* ssh = (EnSsh*)actor;
        ssh->colCylinder[2].base.acFlags |= AC_HIT;
        SPDLOG_INFO("[DamageEnemy] En_Ssh armored-hit RECV netId={} — set cyl[2] AC_HIT "
                    "(sway anim will propagate via ENEMY_STATE)", netId);
        return;
    }

    // #190 — queue damage onto the actor's EnemyNetId extension instead of
    // poking colChkInfo + AC_HIT directly. A new DrainPendingSyncDamage
    // call from the host-side ShouldActorUpdate hook applies the queued
    // values on the first frame the actor's update is about to run.
    // This survives Item Get / cutscene / text-box / ocarina freezes
    // (where actor updates are paused but CollisionCheck reset passes
    // would clear the synthetic AC_HIT bit and colChkInfo.damage).
    //
    // pendingSyncDamage accumulates (multi-hit same frame OR multi-hit
    // during a freeze sums up); damageEffect / atHitEffect are
    // last-write-wins (the most-recent hit's flavour wins, which is what
    // the actor's UpdateDamage expects anyway since vanilla overwrites
    // those fields per-hit).
    EnemyNetId* mut = const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
    if (mut == nullptr) {
        // No extension — actor wasn't admitted to sync. Defensive
        // fallback: apply directly so no behaviour regression for
        // actors that don't yet have an EnemyNetId.
        actor->colChkInfo.damage += damage;
        if (payload.contains("damageEffect")) {
            actor->colChkInfo.damageEffect = (u8)payload["damageEffect"].get<int>();
        }
        if (payload.contains("atHitEffect")) {
            actor->colChkInfo.atHitEffect = (u8)payload["atHitEffect"].get<int>();
        }
        SPDLOG_INFO("[DamageEnemy] Applied directly (no ext) netId={} damage={} preHp={}",
                    netId, (int)damage, (int)actor->colChkInfo.health);
    } else {
        u8 newPending = (u8)((u32)mut->pendingSyncDamage + damage);
        if (newPending < mut->pendingSyncDamage) newPending = 0xFF;  // saturating-add overflow guard
        mut->pendingSyncDamage = newPending;
        if (payload.contains("damageEffect")) {
            mut->pendingSyncDamageEffect = (u8)payload["damageEffect"].get<int>();
        }
        if (payload.contains("atHitEffect")) {
            mut->pendingSyncAtHitEffect = (u8)payload["atHitEffect"].get<int>();
        }
        SPDLOG_INFO("[DamageEnemy] Queued netId={} +{} → pending={} (drains on next ShouldActorUpdate)",
                    netId, (int)damage, (int)mut->pendingSyncDamage);
    }

    // Q I Tier 2 — record who dealt this damage. Synchronous because
    // kill attribution is keyed on the LATEST damager seen for the
    // netId; if the actor dies before the queued damage drains, we
    // still want the attribution to reflect this packet's sender.
    uint32_t senderId = payload.value("clientId", (uint32_t)0);
    if (senderId != 0) {
        EnemyStateSync::HostBookkeeping::Instance().RecordDamager(netId, senderId);
    }
}

// Per-actor AC_HIT setter switch — mirrors the original
// HandlePacket_DamageEnemy switch. Called from DrainPendingSyncDamage when
// queued damage is applied. Kept as a separate helper so the receive-side
// queue path stays terse and the actor-id dispatch stays in one place.
//
// Each branch sets the AC_HIT bit on the collider the actor's damage code
// checks. For Boss_Goma (which reads BUMP_HIT not AC_HIT) we set bumperFlags
// + synthesise a static ColliderInfo with sword dmgFlags so Goma's stun /
// patience / sword-damage paths register the synthetic hit.
//
// ============================================================================
// ADMISSION REQUIREMENTS (freeze lifted 2026-06-08; audit discipline retained)
// ============================================================================
//
// The 2026-06-08 freeze on new admissions to this switch is LIFTED. The audit
// discipline that motivated the freeze is now structurally enforced via the
// SYNTHESIS CONTRACT and CASE COMMENT FORMAT blocks below, so the procedural
// freeze is no longer needed to back it.
//
// Background: the synthetic AC_HIT bit-set causes a class of crashes for any
// actor whose damage code derefs `collider.base.ac->X` (typically
// `->toucher.dmgFlags` for weapon-type branching). The synthetic path sets
// only the bit; the AC pointer remains whatever it was last frame, often
// NULL. Crash class surfaced as En_St / En_Ssh / En_Ik / En_Bili / En_Hintnuts
// regressions over a 2-week window in 2026-05/06 (~12 reactive fix commits).
// The audit-before-admit discipline was added in response and is now the
// canonical pattern; the freeze that bought time to write the discipline is
// no longer load-bearing.
//
// Before adding ANY new actor case below, the following are REQUIRED:
//
//   1. Read the actor's damage path (typically inside its Update or a
//      dedicated UpdateDamage / CheckHitFront helper).
//   2. Grep for `base.ac->` and `acHitInfo->` derefs WITHIN that damage
//      path's reach. Any such site must be null-guarded in the actor's
//      `.c` file IN THE SAME COMMIT that adds the case here, OR the case
//      must be omitted.
//   3. Verify the actor's damage code only reads fields listed in the
//      SYNTHESIS CONTRACT's "POPULATED" section. If it reads any field
//      from the "NOT POPULATED" section without a null-guard, the case
//      must not be admitted (or the actor's code must be modified to
//      tolerate the missing field).
//   4. Document the audit result in the case-comment per CASE COMMENT
//      FORMAT below.
//   5. Cross-check against
//      `Testing/ApplySyncAcHitToActor_BaseAcAudit_2026-06-07.md` for
//      existing per-actor verdicts and the canonical audit pattern.
//
// RECOMMENDED before a large batch of admissions: run the alpha-demo
// regression pass per `Testing/AlphaDemo_Regression_Plan_2026-06-08.md` to
// verify existing admissions stay stable. Not a hard blocker for individual
// admissions that follow the audit format — single new actors can land
// independently if they comply with steps 1-5.
//
// Pre-audit cases that predate the format standard remain in their
// prose-equivalent comments; back-fitting them to the structured tags can
// happen opportunistically when those cases are next touched, or as a
// dedicated cleanup chore. Not a blocker for new admissions.
//
// Architectural alternative if this mechanism ever needs to be retired:
// an explicit `Anchor_ApplyExternalDamage(actor, damage, dmgFlags)` entry
// point per admitted actor instead of synthesising collider state. Higher
// per-actor authoring cost, zero null-deref crash class. Not the current
// direction — consistency of this established pattern was chosen as the
// more important property (see #205 + the conversation thread that landed
// 4b845c58a / 1772be0d1).
// ============================================================================
//
// ============================================================================
// SYNTHESIS CONTRACT — what this function lies about vs. tells the truth
// ============================================================================
//
// This function is the boundary between the network-driven external damage
// pipeline (DAMAGE_ENEMY packets from peers) and the vanilla OoT damage
// pipeline. It synthesises some collider state to make the actor's own
// damage code "think" a real vanilla collision happened. That synthesis is
// incomplete by design — populating every field a real vanilla collision
// would populate is too expensive. The fields BELOW are explicitly NOT
// populated; any actor's damage path that reads them needs a null-guard
// (or the actor must be omitted from this switch entirely).
//
// POPULATED before / by this function:
//
//   actor->colChkInfo.damage          — set by HandlePacket_DamageEnemy
//                                       caller from the wire payload's
//                                       "damage" field.
//   actor->colChkInfo.damageEffect    — set from the wire payload's
//                                       "damageEffect" field (schema 2,
//                                       per #174/#175).
//   actor->colChkInfo.atHitEffect     — set from the wire payload's
//                                       "atHitEffect" field (schema 2).
//   actor->collider.base.acFlags
//       & AC_HIT                       — set by this function per case.
//   actor->collider.base.bumperFlags
//       & BUMP_HIT                     — set ONLY for Boss_Goma (special
//                                       case; reads BUMP_HIT not AC_HIT).
//   static fake ColliderInfo + sword
//   dmgFlags                           — set ONLY for Boss_Goma so its
//                                       acHitInfo->toucher.dmgFlags
//                                       deref doesn't crash and resolves
//                                       to "sword damage."
//
// NOT POPULATED (NULL or stale from last frame's vanilla collision pass):
//
//   actor->collider.base.ac            — pointer to the AT collider that
//                                       "hit" us. Vanilla actors may
//                                       deref `->toucher.dmgFlags` to
//                                       branch on weapon type, or
//                                       `->world.rot.y` for knockback
//                                       direction. Per-actor null-guards
//                                       in the actor's `.c` file are
//                                       required for any deref.
//   actor->collider.info.acHitInfo     — alias of `.base.ac` for older
//                                       decomp style. Same null-deref
//                                       crash shape. Same per-actor
//                                       null-guard required.
//   ColliderJntSph / ColliderQuad
//   per-element ac / acHitInfo         — for multi-element colliders, the
//                                       per-element pointers are NULL on
//                                       this path. Actors that index by
//                                       element (En_St / En_Ssh front-
//                                       shield cyl [2]) must either
//                                       explicitly OMIT specific elements
//                                       from this function's bit-set OR
//                                       null-guard the per-element deref
//                                       in the actor's `.c`.
//   collider.base.acFlags & AC_BOUNCED — set by vanilla CollisionCheck
//                                       when an AT bounces off a hard
//                                       AC. Not set here. Actors that
//                                       branch on AC_BOUNCED for shield-
//                                       hit response are not synced via
//                                       this path; the SHIELD_BOUNCE_
//                                       PLAYER pipeline handles that
//                                       slice (see Packets/Player/).
//   collider.base.acFlags & AC_TYPE_*  — vanilla collision sets the AC
//                                       TYPE bits from the AT collider's
//                                       TYPE field (PLAYER / ENEMY /
//                                       OTHER). Not set here. Actors
//                                       that branch on AC_TYPE_PLAYER
//                                       vs. AC_TYPE_ENEMY (typically
//                                       for friendly-fire vs. hostile
//                                       differentiation) MUST be
//                                       audited; consider whether the
//                                       wire schema should carry the
//                                       attacker type.
//   colChkInfo.damageReaction          — reaction-type byte resolved
//                                       from the AC's damage table by
//                                       vanilla `CollisionCheck_SetAT
//                                       vsACDamage`. The wire payload
//                                       doesn't carry it. Actors that
//                                       branch on damageReaction for
//                                       vanilla hit-anim selection MUST
//                                       be audited.
//
// DIAGNOSTIC: every call to this function emits a SPDLOG_DEBUG line at
// the entry point with the actorId + damage. Filter `[DamageEnemy.synth]`
// in dev builds to observe synthesis events. Compiles out in Release.
// ============================================================================
//
// ============================================================================
// CASE COMMENT FORMAT — required for every new case below
// ============================================================================
//
// Every new `case ACTOR_EN_XXX:` MUST carry a comment with these three tags
// (in this order) so a future agent can audit the synthesis at a glance.
// Existing cases predating this standard may use prose-equivalent format;
// they cover the same information in narrative form. New cases follow the
// structured tags.
//
//   case ACTOR_EN_XXX:
//       // Audit: <verdict>
//       //   <commit-or-doc citation>
//       //
//       // Acks: <list of fields the damage path reads that this synthesis
//       //   populates — e.g. "acFlags & AC_HIT", "colChkInfo.damage",
//       //   "colChkInfo.damageEffect">
//       //
//       // Skips: <fields the damage path reads that this synthesis does
//       //   NOT populate, with how each is handled — null-guarded inline,
//       //   protected by upstream branch, etc. "none" if the damage path
//       //   only reads Acks-listed fields>
//       ((EnXxx*)actor)->collider.base.acFlags |= AC_HIT;
//       break;
//
// Audit verdict canonical strings:
//
//   "no base.ac / acHitInfo derefs in damage path; safe synthetic bit-set"
//   "base.ac->X deref at <file>:<line>; null-guard landed in <commit>"
//   "acHitInfo->Y deref at <file>:<line>; null-guard landed in <commit>"
//   "INTENTIONALLY EXCLUDED: see body comment for why" (case body is empty;
//   leaves the explicit case label so future readers see this was
//   considered, e.g. ACTOR_EN_HINTNUTS at L539)
//
// Examples of cases that already match (or approximate) this format:
//   ACTOR_EN_TITE   (around L748) — cites the audit doc + grep verdict
//   ACTOR_EN_CROW   (around L465) — cites the source read + grep verdict
//   ACTOR_EN_VALI   (around L590) — cites the audit commit + grep verdict
//
// The CASE COMMENT FORMAT is enforceable at code-review time: any new
// case missing one of the three tags is rejected. Combined with the
// FREEZE NOTICE above, future admissions become mechanical — the case-
// comment IS the audit deliverable.
// ============================================================================
static void ApplySyncAcHitToActor(Actor* actor, u8 damage) {
    SPDLOG_DEBUG("[DamageEnemy.synth] synthetic AC_HIT actorId=0x{:04X} damage={} "
                 "(see helper header for what is/isn't populated)",
                 actor->id, damage);

    // NPC Invader uses a runtime-allocated actor id (gEnInvaderId), so we
    // can't put it in the switch's case labels. Check up-front.
    //
    // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe synthetic
    //   AC_HIT bit-set. EnInvader's body collider gates its health drain on
    //   AC_HIT (z_en_invader.c:152 tests `(acFlags & AC_HIT) &&
    //   colChkInfo.damage > 0`).
    // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage.
    // Skips: none — damage path reads only Acks-listed fields. Same shape
    //   as ACTOR_EN_SKB. The gEnInvaderId != 0 guard is defensive in case
    //   the dynamic-actor registration hasn't completed yet.
    if (gEnInvaderId != 0 && actor->id == gEnInvaderId) {
        ((EnInvader*)actor)->collider.base.acFlags |= AC_HIT;
        return;
    }
    switch (actor->id) {
        case ACTOR_EN_DEKUBABA:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Back-fitted 2026-06-08 via
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_dekubaba.c` → 0
            //   matches.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            ((EnDekubaba*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_KAREBABA:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Back-fitted 2026-06-08 via
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_karebaba.c` → 0
            //   matches. Damage gates read bodyCollider AC_HIT at
            //   z_en_karebaba.c:369 and :419.
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            ((EnKarebaba*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_FIREFLY:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Back-fitted 2026-06-08 via
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_firefly.c` → 0
            //   matches.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            ((EnFirefly*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_CROW:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read of EnCrow_UpdateDamage (z_en_crow.c:424) — reads only
            //   damageEffect / damage / colorFilterParams.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Single JntSph collider.
            ((EnCrow*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_SW:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Back-fitted 2026-06-08 via
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_sw.c` → 0 matches.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Single JntSph collider.
            ((EnSw*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_ST: {
            // Audit: ONE acHitInfo-> deref in EnSt_CheckHitBackside,
            //   null-guarded at z_en_st.c:460+ (predates the standard).
            //   Safe synthetic AC_HIT on cyl [0]/[1]; flags |= 0 falls
            //   through to standard melee → BounceAround. #90 / log 431+
            //   433 fix.
            // Acks: colCylinder[0].base.acFlags & AC_HIT (body),
            //   colCylinder[1].base.acFlags & AC_HIT (legs),
            //   colChkInfo.damage, colChkInfo.damageEffect.
            // Skips: colCylinder[2] (front shield) INTENTIONALLY EXCLUDED
            //   from base bit-set. EnSt_CheckColliders (z_en_st.c:519)
            //   calls EnSt_CheckHitFrontside FIRST and returns "armored
            //   reaction" (sway anim, no damage) when [2]'s AC_HIT is set.
            //   Setting [2] here would make EVERY peer hit register as
            //   armored bounce — damage never applies, HP never decrements,
            //   Skulltula stays alive forever (log 433 regression). Front-
            //   shield AC_HIT is set ONLY by the armored-hit receiver
            //   branch above (handles isArmoredHit=true wire flag).
            //
            // Sway anim cross-machine sync for peer's armored-side hits
            // (log 433 bug 2) is NOT solved by this fix — peer's local
            // Skulltula fires sway locally, but host's local copy
            // doesn't. Would need wire-side info about which cylinder
            // was hit OR ENEMY_STATE broadcasting swayTimer /
            // playSwayFlag / swayAngle fields. Acceptable cosmetic gap
            // until a follow-up sync pass.
            EnSt* st = (EnSt*)actor;
            st->colCylinder[0].base.acFlags |= AC_HIT;
            st->colCylinder[1].base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_WF: {
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read — damage gate at z_en_wf.c:1287 reads body + tail
            //   cylinder AC_HIT only.
            // Acks: colliderCylinderBody.base.acFlags & AC_HIT,
            //   colliderCylinderTail.base.acFlags & AC_HIT,
            //   colChkInfo.damage, colChkInfo.damageEffect.
            // Skips: none.
            //
            // Multi-collider: body + tail cylinders. Set AC_HIT on both
            // so peer damage routes correctly regardless of which segment
            // was hit on peer's side.
            EnWf* wf = (EnWf*)actor;
            wf->colliderCylinderBody.base.acFlags |= AC_HIT;
            wf->colliderCylinderTail.base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_REEBA:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read — z_en_reeba.c:545.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Single cylinder collider.
            ((EnReeba*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_IK:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read — z_en_ik.c:301 and :710 read bodyCollider AC_HIT.
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Multi-collider: bodyCollider (AC) + axe AT colliders. AT
            // colliders are host-authoritative for outgoing axe hits and
            // are not touched by this path.
            ((EnIk*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_HINTNUTS:
            // Audit: INTENTIONALLY EXCLUDED — log 438 P1 crash. Case body
            //   is a no-op. EnHintnuts_ColliderCheck (z_en_hintnuts.c:536)
            //   derefs `this->collider.base.ac->id` to discriminate nutball
            //   vs. weapon hits — synthetic AC_HIT without a valid `.ac`
            //   pointer crashes with 0xC0000005. Hintnut sync does NOT need
            //   this drain (see Skips for the alternative paths).
            // Acks: none.
            // Skips: ALL fields — case body is empty by design.
            //
            // Two reasons Hintnut sync works without this drain:
            //  1. PROJECTILE_HIT_ENEMY is the canonical host-authoritative
            //     path for Hintnut state transitions (calls
            //     HitByScrubProjectile1/2 directly on host, then
            //     broadcasts the resulting state via ENEMY_STATE).
            //  2. Race-B clamp neutralises the damage value to 0 by the
            //     time the peer's DAMAGE_ENEMY wire packet arrives, so no
            //     health decrement is intended here anyway.
            //
            // Leave the explicit case label in place so a future reader
            // sees "yes this was considered, and yes it's deliberately
            // empty" rather than the default fall-through being mistaken
            // for an oversight.
            break;
        case ACTOR_EN_PEEHAT: {
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read — z_en_peehat.c checks colCylinder (grounded) and
            //   colJntSph (flying) for AC_HIT in different state branches.
            // Acks: colCylinder.base.acFlags & AC_HIT (grounded variant),
            //   colJntSph.base.acFlags & AC_HIT (flying variant),
            //   colChkInfo.damage, colChkInfo.damageEffect.
            // Skips: none.
            //
            // Multi-collider: variant-dependent. Set AC_HIT on both so
            // peer damage routes correctly regardless of variant/state at
            // wire-receive time.
            EnPeehat* ph = (EnPeehat*)actor;
            ph->colCylinder.base.acFlags |= AC_HIT;
            ph->colJntSph.base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_POH:
            // Audit: TWO derefs at z_en_poh.c:308 inside func_80ADE28C —
            //   `colliderCyl.info.acHitInfo->toucher.dmgFlags` AND
            //   `colliderCyl.base.ac->world.rot.y`. Null-guards landed in
            //   the same commit as this case addition.
            // Acks: colliderCyl.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: colliderCyl.info.acHitInfo + colliderCyl.base.ac —
            //   read only inside the null-guarded site at z_en_poh.c:308;
            //   safe under the guard (returns early on NULL).
            ((EnPoh*)actor)->colliderCyl.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_BILI:
            // Audit: ONE acHitInfo-> deref at z_en_bili.c:604 inside
            //   EnBili_UpdateDamage —
            //   `collider.info.acHitInfo->toucher.dmgFlags`. Null-guard
            //   landed in the same commit as this case addition.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: collider.info.acHitInfo — read only inside the
            //   null-guarded site at z_en_bili.c:604; safe under the
            //   guard.
            ((EnBili*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_VALI:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. EnVali_UpdateDamage at
            //   z_en_vali.c:508 reads ONLY bodyCollider.base.acFlags +
            //   colChkInfo.damageEffect/damage (audit in ad01a739b
            //   commit body). #126.
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            ((EnVali*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_ZF:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path;
            //   safe synthetic AC_HIT bit-set. Verified by agent
            //   `feature/sync-en-zf` Phase 1 (commit ad065c183):
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_zf.c` → 0 matches.
            //   Damage path reads `bodyCollider.base.acFlags & AC_HIT` +
            //   `colChkInfo.damage` / `colChkInfo.damageEffect` only.
            //
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            //
            // Skips: none — damage path does not read any field listed in
            //   the SYNTHESIS CONTRACT "NOT POPULATED" section.
            //
            // Multi-collider note: En_Zf has bodyCollider (cyl) + swordCollider
            //   (quad AT). Only bodyCollider receives damage; the sword AT
            //   collider is host-authoritative for its own AT hits. Set
            //   AC_HIT on bodyCollider only.
            ((EnZf*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_MB:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path;
            //   safe synthetic AC_HIT bit-set. Verified by agent
            //   `feature/sync-en-mb` Phase 1 (commit 7f210411a):
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_mb.c` → 0 matches.
            //   Damage path reads `hitbox.base.acFlags & AC_HIT` +
            //   `colChkInfo.damageEffect` / `colChkInfo.damage` only.
            //
            // Acks: hitbox.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            //
            // Skips: none — damage path does not read any field listed in
            //   the SYNTHESIS CONTRACT "NOT POPULATED" section.
            //
            // Multi-collider note: En_Mb has hitbox (cyl, AC) + attackCollider
            //   (quad, AT) + frontShielding (tris, deflection). Only hitbox
            //   receives damage cross-machine. The attackCollider is host-
            //   authoritative for outgoing AT hits. frontShielding is a
            //   front-side deflection collider; AC_HIT on it would not
            //   register damage anyway. Set AC_HIT on hitbox only.
            ((EnMb*)actor)->hitbox.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_BIGOKUTA:
            // Audit: ONE acHitInfo-> deref found and null-guarded same-commit.
            //   Verified by agent `feature/sync-en-bigokuta` Phase 1
            //   (commit 14ad5a154):
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_bigokuta.c` → 1 match
            //   at line 343 (now 354 post-edit) inside `func_809BD524`
            //   (Stunned deku-nut setup) — derefs
            //   `this->collider.elements->info.acHitInfo->toucher.dmgFlags & 1`.
            //   Null-guarded in same commit (z_en_bigokuta.c:354) following
            //   the z_en_bili.c:621 pattern. When acHitInfo is NULL
            //   (synthetic cross-machine AC_HIT path), the guarded branch
            //   falls through to the long-stun behavior — matches vanilla
            //   "no DMG_DEKU_NUT toucher" path.
            //
            // Acks: collider.elements[*].base.acFlags & AC_HIT,
            //   colChkInfo.damage, colChkInfo.damageEffect.
            //
            // Skips: collider.base.ac (or per-element acHitInfo) — read
            //   only inside the null-guarded site at z_en_bigokuta.c:354;
            //   safe under the guard.
            //
            // Big Octo uses a ColliderJntSph; setting AC_HIT on the
            // shared base header covers all elements.
            ((EnBigokuta*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_FD:
            // Audit: ONE pre-existing acHitInfo-> deref in damage path,
            //   already null-guarded.
            //   Verified by agent `feature/sync-en-fd` Phase 1
            //   (commit 3a60cf5ea): `grep -nE "base\.ac->|acHitInfo->"
            //   z_en_fd.c` → 1 match at line 298 inside `EnFd_ColliderCheck`,
            //   PRE-EXISTING `acHitInfo != NULL` guard precedes the deref.
            //   No new null-guard required for synthetic AC_HIT.
            //
            // Acks: collider.elements[*].base.acFlags & AC_HIT,
            //   colChkInfo.damage, colChkInfo.damageEffect.
            //
            // Skips: collider.base.ac per-element acHitInfo — read only
            //   inside the pre-existing null-guarded site at
            //   z_en_fd.c:298; safe under the guard.
            //
            // Sibling actor note: En_Fd_Fire (the flame projectile cluster
            //   spawned by EnFd_SpinAndSpawnFire) is short-lived and not
            //   admitted to this switch — it self-kills via Actor_Kill in
            //   EnFdFire_Disappear (Type 8 projectile class). En_Fw (the
            //   Flare Dancer core/wisp child) owns the actual loot drop
            //   and is currently UNSYNCED; admit En_Fw when synced.
            //
            // ColliderJntSph; setting AC_HIT on shared base covers all
            // elements.
            ((EnFd*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_GELDB:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified by agent
            //   `feature/sync-en-geldb` Phase 1 (commit 0455ca871):
            //   `grep -nE "base\.ac->|acHitInfo->" z_en_geldb.c` → 0 matches.
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Multi-collider: bodyCollider (AC, cyl) + swordCollider (AT,
            // quad) + blockCollider (deflection, tris). The swordCollider
            // is host-authoritative for outgoing AT hits; blockCollider
            // is shield-deflection only. Set AC_HIT on bodyCollider only.
            ((EnGeldB*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_PO_FIELD:
            // Audit: TWO acHitInfo->/base.ac-> derefs at z_en_po_field.c:
            //   276/277 inside EnPoField_SetupDamage. Null-guards landed in
            //   the same commit as Phase 1 (3805a6281) — mirror of En_Poh's
            //   func_80ADE28C pattern at z_en_poh.c:315-323. Three-branch
            //   flow: dual-null-check + flag, single-null-check fallback,
            //   no-attacker fallback uses `shape.rot.y + 0x8000` facing flip.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: collider.info.acHitInfo + collider.base.ac — read only
            //   inside the null-guarded site at z_en_po_field.c:276-277;
            //   safe under the guard.
            //
            // Multi-collider: collider (AC) + flameCollider (AT, separate
            // small flame projectile). AC_HIT on collider only; flame is
            // host-side outgoing AT.
            ((EnPoField*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_BB:
            // Audit: TWO acHitInfo-> derefs at z_en_bb.c:1164 and :1173
            //   inside EnBb_CollisionCheck cases 7 (Fire arrow) and 6
            //   (Ice arrow) — `collider.elements[0].info.acHitInfo->
            //   toucher.damage` populates freezeTimer. Null-guards landed
            //   in the same commit as this case addition.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: collider.elements[0].info.acHitInfo — read only
            //   inside the null-guarded sites at z_en_bb.c:1164/:1173;
            //   safe under the guard.
            //
            // ColliderJntSph — `base.acFlags` is the shared header so a
            // single write covers all elements.
            ((EnBb*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_DEKUNUTS:
            // Audit: ONE acHitInfo-> deref at z_en_dekunuts.c:213 —
            //   `collider.info.acHitInfo->toucher.dmgFlags`
            //   unconditionally. Null-guard landed in the same commit
            //   as this case addition.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: collider.info.acHitInfo — read only inside the
            //   null-guarded site at z_en_dekunuts.c:213; safe under
            //   the guard.
            ((EnDekunuts*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_SSH: {
            // Audit: no acHitInfo deref in EnSsh_CheckHitBack or
            //   EnSsh_CheckHitFront; safe synthetic AC_HIT bit-set on
            //   cyl [0]/[1]. Verified z_en_ssh.c:510-537 and :492-508.
            // Acks: colCylinder[0].base.acFlags & AC_HIT (body),
            //   colCylinder[1].base.acFlags & AC_HIT (legs),
            //   colChkInfo.damage, colChkInfo.damageEffect.
            // Skips: colCylinder[2] (front shield) INTENTIONALLY EXCLUDED
            //   from base bit-set — same shape as En_St. EnSsh_Collision
            //   Check calls EnSsh_CheckHitFront FIRST (z_en_ssh.c:543) and
            //   short-circuits when [2]'s AC_HIT is set. Front-shield
            //   AC_HIT is set ONLY by the armored-hit receiver branch
            //   above (handles isArmoredHit=true wire flag for sway anim
            //   sync).
            //
            // Per user direction: EnSsh actors are intended as cursed-
            // human NPCs and shouldn't normally die. This fix ensures
            // damage/sway visual consistency between clients when they
            // are hit; doesn't change the gameplay design that these
            // actors aren't part of the standard kill flow.
            EnSsh* ssh = (EnSsh*)actor;
            ssh->colCylinder[0].base.acFlags |= AC_HIT;
            ssh->colCylinder[1].base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_TEST:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified 2026-06-07 via source
            //   read — z_en_test.c:1666 reads bodyCollider AC_HIT.
            // Acks: bodyCollider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Multi-collider: bodyCollider (AC) + swordCollider (AT) +
            // shieldCollider (AT). The AT colliders are for Stalfos
            // hitting Link and are not touched by this path.
            ((EnTest*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_GOMA:
            // Audit: acHitInfo deref inside EnGoma_UpdateHit, now NULL-
            //   guarded so the synthetic flag without an AT collider
            //   won't crash. Larva's damage block at z_en_goma.c:647
            //   reads colCyl2.base.acFlags. When acHitInfo is NULL the
            //   path falls through to the swordDamage branch and
            //   decrements health by 1.
            // Acks: colCyl2.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: colCyl2 acHitInfo — read inside null-guarded site
            //   in EnGoma_UpdateHit; safe under the guard.
            ((EnGoma*)actor)->colCyl2.base.acFlags |= AC_HIT;
            break;
        case ACTOR_BOSS_GOMA: {
            // Audit: SPECIAL CASE — synthesises BOTH BUMP_HIT AND a static
            //   acHitInfo with sword dmgFlags for the ceiling-state branch
            //   (BossGoma_UpdateHit at z_boss_goma.c:1823 reads
            //   acHitInfo->toucher.dmgFlags); floor-state branch bypasses
            //   the vanilla collision pipeline entirely and writes
            //   `boss->actor.colChkInfo.health` directly.
            // Acks: collider.elements[0].info.bumperFlags & BUMP_HIT
            //   (ceiling-state path), static fake ColliderInfo with sword
            //   dmgFlags (ceiling-state path), boss->actor.colChkInfo
            //   .health (floor-state direct write).
            // Skips: standard collider.base.acFlags & AC_HIT path NOT
            //   used — Boss Goma reads BUMP_HIT not AC_HIT in its damage
            //   handler. The synthetic acHitInfo IS populated (only
            //   admission that does so) so the deref at z_boss_goma.c:
            //   1823 sees a valid ColliderInfo with sword damage flags.
            //
            // Boss Gohma damage from non-host (#67 follow-up, log 299).
            //
            // Hybrid approach split by Goma's actionFunc state:
            //
            //   - **Ceiling state** (CeilingMoveToCenter / CeilingIdle /
            //     CeilingPrepareSpawnGohmas — state-index 0x0B-0x0D):
            //     synthesise BUMP_HIT + acHitInfo. BossGoma_UpdateHit's
            //     ceiling branch (z_boss_goma.c:1867) fires
            //     SetupFallStruckDown — this is the slingshot eye-shot
            //     knockdown. The ceiling actionFuncs are sustained for
            //     many frames so timing is reliable; vanilla path works.
            //     This is the SAME synthetic-collider approach the
            //     previous code used for all states.
            //
            //   - **Floor state** (any non-ceiling, non-CeilingSpawnGohmas
            //     state): direct HP decrement, bypassing
            //     BossGoma_UpdateHit's FloorStunned-only damage gate.
            //     Vanilla's gate requires `actionFunc == FloorStunned` at
            //     the moment the synthetic BUMP_HIT is processed; the
            //     stun window is just 4 frames (~200ms) before
            //     transitioning to FloorDamaged or FloorMain. Network
            //     latency (50-150ms RTT) plus host/peer state-machine
            //     drift means peer hits while peer's Goma is stunned,
            //     but the packet arrives at host AFTER host's Goma has
            //     already exited FloorStunned. The synthetic BUMP_HIT
            //     then routes to the patience-stun branch (line 1886)
            //     which re-stuns Goma instead of damaging her, AND
            //     consumes the bumper bit. invincibilityFrames=10 set
            //     by patience-stun blocks any subsequent hit for 10
            //     frames. Net effect: peer's hits never damaged, only
            //     stunned. Log 299 confirms preHp=10 across ~200
            //     received DAMAGE_ENEMY packets.
            //
            //     Direct HP write trusts the peer's local hit detection
            //     — peer's UpdateHit gates damage broadcast on its own
            //     state machine, so if peer broadcast DAMAGE_ENEMY,
            //     peer saw a legitimate hit. Apply damage authoritatively
            //     on host. visualState transition handled by
            //     SetupFloorDamaged for the hit-reaction; SetupDefeated
            //     fires the boss-defeat cycle when HP reaches 0.
            //
            //   - **CeilingSpawnGohmas (state 0x0E)**: vanilla blocks
            //     damage entirely during this state (z_boss_goma.c:1863
            //     `actionFunc != BossGoma_CeilingSpawnGohmas`). Drop the
            //     packet — Goma is mid-egg-laying and intentionally
            //     invulnerable.
            BossGoma* boss = (BossGoma*)actor;
            s16 stateIdx = BossGoma_GetStateIndex(boss);
            const bool inCeilingState = (stateIdx == 0x0B || stateIdx == 0x0C ||
                                         stateIdx == 0x0D);
            const bool inSpawnGohmas  = (stateIdx == 0x0E);

            if (inSpawnGohmas) {
                break;  // intentionally invulnerable; matches vanilla
            }

            if (inCeilingState) {
                // Slingshot eye-shot knockdown via vanilla path.
                static ColliderInfo sBossGomaSynthAcHitInfo = {};
                sBossGomaSynthAcHitInfo.toucher.dmgFlags = 0x1;  // sword bit
                sBossGomaSynthAcHitInfo.toucher.damage   = (u8)damage;
                boss->collider.elements[0].info.acHitInfo    = &sBossGomaSynthAcHitInfo;
                boss->collider.elements[0].info.bumperFlags |= BUMP_HIT;
                break;
            }

            // Floor-state direct HP write. Skip if invincibility is
            // currently active so back-to-back peer hits don't double-
            // damage past the vanilla cooldown.
            if (boss->invincibilityFrames > 0) {
                break;
            }
            s8 newHp = (s8)boss->actor.colChkInfo.health - (s8)damage;
            boss->invincibilityFrames = 10;  // matches vanilla post-hit cooldown
            if (newHp > 0) {
                boss->actor.colChkInfo.health = newHp;
                BossGoma_SetupFloorDamaged(boss);
            } else {
                boss->actor.colChkInfo.health = 0;
                BossGoma_SetupDefeated(boss, gPlayState);
                Enemy_StartFinishingBlow(gPlayState, &boss->actor);
                GameInteractor_ExecuteOnBossDefeat(&boss->actor);
            }
            SPDLOG_INFO("[DamageEnemy] Boss_Goma direct HP {}→{} (state=0x{:02X}, damage={})",
                        (int)(newHp + (s8)damage), (int)newHp, (int)stateIdx, (int)damage);
            break;
        }
        case ACTOR_EN_SKB:
            // Audit: no acHitInfo->toucher deref in damage path; safe
            //   synthetic AC_HIT bit-set. Stalchild damage block
            //   (z_en_skb.c:456) branches on colChkInfo.damageEffect
            //   only.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Fixes peer→host damage not registering when peer hits a
            // Stalchild on Hyrule Field at night.
            ((EnSkb*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_TITE:
            // Audit: no base.ac->/acHitInfo-> derefs in damage path; safe
            //   synthetic AC_HIT bit-set. Verified per
            //   `Plans/ApplySyncAcHitToActor_BaseAcAudit_2026-06-07.md`
            //   pattern. EnTite_CheckDamage at z_en_tite.c:850 reads
            //   collider.base.acFlags & AC_HIT.
            // Acks: collider.base.acFlags & AC_HIT, colChkInfo.damage,
            //   colChkInfo.damageEffect.
            // Skips: none.
            //
            // Single ColliderJntSph; colliderItem is the JntSph element
            // (same physical collider — setting AC_HIT on the base flags
            // is sufficient). Covers both red (TEKTITE_RED, land+water)
            // and blue (TEKTITE_BLUE, water-surface) variants uniformly.
            ((EnTite*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        default:
            // No AC_HIT setter for this actor type. Damage is delivered via
            // colChkInfo only — works for enemies that don't gate on AC_HIT,
            // breaks silently for those that do. Add new entries above as
            // they're identified during testing.
            break;
    }
}

// #190 — drain queued DAMAGE_ENEMY damage onto the actor's colChkInfo +
// AC_HIT. Called from the host-side ShouldActorUpdate hook every frame
// the actor's update is about to run, so the synthetic hit gets consumed
// by UpdateDamage on the same frame the world resumes from a freeze.
//
// No-op when ext->pendingSyncDamage == 0. Discards queued damage when the
// actor has died (actor->update == NULL or health == 0) — prevents
// applying to a dead-but-not-yet-cleaned-up actor.
void Anchor::DrainPendingSyncDamage(Actor* actor) {
    if (actor == nullptr) return;
    EnemyNetId* ext = const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
    if (ext == nullptr || ext->pendingSyncDamage == 0) return;

    if (actor->update == nullptr || actor->colChkInfo.health == 0) {
        ext->pendingSyncDamage = 0;
        ext->pendingSyncDamageEffect = 0;
        ext->pendingSyncAtHitEffect = 0;
        return;
    }

    u8 damage = ext->pendingSyncDamage;
    u8 damageEffect = ext->pendingSyncDamageEffect;
    u8 atHitEffect = ext->pendingSyncAtHitEffect;
    ext->pendingSyncDamage = 0;
    ext->pendingSyncDamageEffect = 0;
    ext->pendingSyncAtHitEffect = 0;

    u8 preHp = (u8)actor->colChkInfo.health;
    actor->colChkInfo.damage += damage;
    if (damageEffect != 0) actor->colChkInfo.damageEffect = damageEffect;
    if (atHitEffect != 0) actor->colChkInfo.atHitEffect = atHitEffect;
    ApplySyncAcHitToActor(actor, damage);

    SPDLOG_INFO("[DamageEnemy] Drained pending damage actorId={} damage={} preHp={} "
                "accumDmg={} (UpdateDamage consumes this frame)",
                actor->id, (int)damage, (int)preHp, (int)actor->colChkInfo.damage);
}
