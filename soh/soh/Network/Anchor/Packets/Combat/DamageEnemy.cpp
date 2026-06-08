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
static void ApplySyncAcHitToActor(Actor* actor, u8 damage) {
    // NPC Invader uses a runtime-allocated actor id (gEnInvaderId), so we
    // can't put it in the switch's case labels. Check up-front. EnInvader's
    // body collider gates its health drain on AC_HIT (z_en_invader.c:152
    // tests `(acFlags & AC_HIT) && colChkInfo.damage > 0`); setting the
    // bit here is sufficient. No acHitInfo deref in EnInvader_Update —
    // bit-set is safe without a fake AT collider (same shape as
    // ACTOR_EN_SKB). The gEnInvaderId != 0 guard is defensive in case the
    // dynamic-actor registration hasn't completed yet.
    if (gEnInvaderId != 0 && actor->id == gEnInvaderId) {
        ((EnInvader*)actor)->collider.base.acFlags |= AC_HIT;
        return;
    }
    switch (actor->id) {
        case ACTOR_EN_DEKUBABA:
            ((EnDekubaba*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_KAREBABA:
            // bodyCollider — z_en_karebaba.c:369/419 read AC_HIT here.
            ((EnKarebaba*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_FIREFLY:
            ((EnFirefly*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_CROW:
            // Guay — z_en_crow.c:424 reads `collider.base.acFlags & AC_HIT`
            // in EnCrow_UpdateDamage. No `base.ac->` deref in damage path
            // (verified by source read 2026-06-07 — only reads damageEffect
            // / damage / colorFilterParams). Single JntSph collider.
            ((EnCrow*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_SW:
            ((EnSw*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_ST: {
            // #90 / log 431+433 fix — host applies peer's damage via the
            // back-side body+legs cylinders ONLY ([0] and [1]).
            //
            // Cylinder [2] (front shield) is INTENTIONALLY EXCLUDED.
            // EnSt_CheckColliders (z_en_st.c:519) calls
            // EnSt_CheckHitFrontside FIRST and returns "armored reaction"
            // (sway anim, no damage) when [2]'s AC_HIT is set. Setting
            // [2] here makes EVERY peer hit register as an armored
            // bounce — damage never applies, HP never decrements,
            // Skulltula stays alive forever (regression observed in
            // log 433: drains showed preHp=2 on every hit, no kill).
            //
            // The acHitInfo deref in EnSt_CheckHitBackside is null-
            // guarded (z_en_st.c:460+) so synthetic bit-set on [0]/[1]
            // without a real AT collider is safe — flags |= 0 falls
            // through to the standard melee → BounceAround death path.
            // The earlier crash documented historically (log 115 —
            // 0xC0000005 on Skulltula damage receive) is resolved by
            // the null-guard.
            //
            // Sway anim cross-machine sync for peer's armored-side
            // hits (log 433 bug 2) is NOT solved by this fix — peer's
            // local Skulltula fires sway locally, but host's local
            // copy doesn't. Would need either wire-side info about
            // which cylinder was hit OR ENEMY_STATE broadcasting
            // swayTimer / playSwayFlag / swayAngle fields. Acceptable
            // cosmetic gap until a follow-up sync pass.
            EnSt* st = (EnSt*)actor;
            st->colCylinder[0].base.acFlags |= AC_HIT;
            st->colCylinder[1].base.acFlags |= AC_HIT;
            break;
        }
        // 2026-06-07 wave — same audit pass that fixed En_St (log 431).
        // The following actors all gate damage on `acFlags & AC_HIT` but
        // were missing from this switch — peer's DAMAGE_ENEMY arrived,
        // host's colChkInfo.damage incremented, but host's update path
        // never saw AC_HIT and so never called Actor_ApplyDamage. Each
        // case below either has NO acHitInfo deref in damage code
        // (safe direct add) or its deref is null-guarded in the same
        // commit landing this case.
        case ACTOR_EN_WF: {
            // Wolfos — z_en_wf.c:1287 checks body + tail cyl AC_HIT.
            // No acHitInfo deref in damage path; safe-easy add.
            EnWf* wf = (EnWf*)actor;
            wf->colliderCylinderBody.base.acFlags |= AC_HIT;
            wf->colliderCylinderTail.base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_REEBA:
            // Leever — z_en_reeba.c:545. No acHitInfo deref. Single cyl.
            ((EnReeba*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_IK:
            // Iron Knuckle — z_en_ik.c:301/710 reads bodyCollider AC_HIT.
            // No acHitInfo deref in damage path. AT colliders for axe are
            // host-authoritative (separate codepath).
            ((EnIk*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_HINTNUTS:
            // INTENTIONALLY EXCLUDED — log 438 P1 crash.
            //
            // `EnHintnuts_ColliderCheck` (z_en_hintnuts.c:536) derefs
            // `this->collider.base.ac->id` to discriminate nutball vs.
            // weapon hits. Setting AC_HIT WITHOUT populating
            // `collider.base.ac` (which only the real collision engine
            // does — z_collision_check.c:1739-1740 sets `ac->ac =
            // at->actor` atomically with the flag bit) leaves `ac` at
            // NULL → 0xC0000005 access violation when the actor's own
            // ColliderCheck runs on the next tick.
            //
            // Hintnut sync does NOT need this drain. Two reasons:
            //  1. PROJECTILE_HIT_ENEMY is the canonical host-authoritative
            //     path for Hintnut state transitions (it calls
            //     HitByScrubProjectile1/2 directly on host, then
            //     broadcasts the resulting state via ENEMY_STATE).
            //  2. Race-B clamp neutralises the damage value to 0 by the
            //     time the peer's DAMAGE_ENEMY wire packet arrives, so
            //     no health decrement is intended here anyway.
            //
            // Case body is a no-op. Leave the explicit case in place so
            // a future reader sees "yes this was considered, and yes
            // it's deliberately empty" rather than the default fall-
            // through being mistaken for an oversight.
            break;
        case ACTOR_EN_PEEHAT: {
            // Peahat — z_en_peehat.c checks colCylinder (grounded variant)
            // and colJntSph (flying variant) for AC_HIT in different
            // state branches. Set both so peer damage routes correctly
            // regardless of variant / state. No acHitInfo deref in
            // damage path.
            EnPeehat* ph = (EnPeehat*)actor;
            ph->colCylinder.base.acFlags |= AC_HIT;
            ph->colJntSph.base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_POH:
            // Poe — z_en_poh.c:308 derefs `colliderCyl.info.acHitInfo
            // ->toucher.dmgFlags` AND `colliderCyl.base.ac->world.rot.y`
            // unconditionally inside func_80ADE28C. Null-guards landed in
            // the same commit as this case addition. Safe to set AC_HIT.
            ((EnPoh*)actor)->colliderCyl.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_BILI:
            // Biri jellyfish — z_en_bili.c:604 derefs
            // `collider.info.acHitInfo->toucher.dmgFlags` unconditionally
            // inside EnBili_UpdateDamage. Null-guard landed in the same
            // commit as this case addition.
            ((EnBili*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_BB:
            // Bubble (flame skull) — z_en_bb.c:1164 and :1173 deref
            // `collider.elements[0].info.acHitInfo->toucher.damage` in
            // EnBb_CollisionCheck cases 7 (Fire arrow) and 6 (Ice arrow)
            // to populate freezeTimer. Null-guards landed in the same
            // commit as this case addition. ColliderJntSph collider —
            // `base.acFlags` is the shared header so a single write
            // covers all elements.
            ((EnBb*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_DEKUNUTS:
            // Mad Scrub — z_en_dekunuts.c:213 derefs
            // `collider.info.acHitInfo->toucher.dmgFlags` unconditionally.
            // Null-guard landed in the same commit as this case addition.
            ((EnDekunuts*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_SSH: {
            // En_Ssh — Skulltula sibling, identical multi-collider
            // pattern to En_St. Same fix shape: AC_HIT on cyl [0] (body)
            // and [1] (legs) ONLY — cyl [2] (front armor) is excluded
            // because EnSsh_CollisionCheck calls EnSsh_CheckHitFront
            // FIRST (z_en_ssh.c:543) and short-circuits when it fires.
            // Front-shield AC_HIT is set ONLY by the armored-hit
            // receiver branch above (handles isArmoredHit=true wire
            // flag for sway anim sync).
            //
            // No acHitInfo deref in EnSsh_CheckHitBack or
            // EnSsh_CheckHitFront — synthetic AC_HIT without real AT
            // collider is safe (verified z_en_ssh.c:510-537 + 492-508).
            //
            // Per user direction: EnSsh actors are intended as cursed-
            // human NPCs and shouldn't normally die. This fix ensures
            // damage/sway visual consistency between clients when they
            // are hit; it doesn't change the gameplay design that
            // these actors aren't part of the standard kill flow.
            EnSsh* ssh = (EnSsh*)actor;
            ssh->colCylinder[0].base.acFlags |= AC_HIT;
            ssh->colCylinder[1].base.acFlags |= AC_HIT;
            break;
        }
        case ACTOR_EN_TEST:
            // bodyCollider — z_en_test.c:1666 reads AC_HIT here. swordCollider
            // and shieldCollider are AT colliders (Stalfos hitting Link), not AC.
            ((EnTest*)actor)->bodyCollider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_GOMA:
            // Bug B — Larva's damage block (z_en_goma.c:647) reads
            // colCyl2.base.acFlags. acHitInfo is now NULL-guarded inside
            // EnGoma_UpdateHit so the synthetic flag without an AT collider
            // won't crash; the path falls through to the swordDamage branch
            // and decrements health by 1.
            ((EnGoma*)actor)->colCyl2.base.acFlags |= AC_HIT;
            break;
        case ACTOR_BOSS_GOMA: {
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
            // Stalchild damage block (z_en_skb.c:456) reads
            // `collider.base.acFlags & 2` (AC_HIT). No acHitInfo->toucher
            // deref — the actor branches on colChkInfo.damageEffect only,
            // so the synthetic AC_HIT bit is safe without a fake AT
            // collider. Fixes peer→host damage not registering when peer
            // hits a Stalchild on Hyrule Field at night.
            ((EnSkb*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        case ACTOR_EN_TITE:
            // Tektite — z_en_tite.c:850 (EnTite_CheckDamage) reads
            // `collider.base.acFlags & AC_HIT`. Single ColliderJntSph;
            // colliderItem is the JntSph element (same physical collider
            // — setting AC_HIT on the base flags is sufficient). No
            // `base.ac->X` derefs in damage path. Covers both red
            // (TEKTITE_RED, land+water) and blue (TEKTITE_BLUE,
            // water-surface) variants uniformly.
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
