#include "soh/Network/Anchor/Anchor.h"
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
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_En_St/z_en_st.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Dekunuts/z_en_dekunuts.h"
// Bug B — En_Goma (Boss_Goma's larvae) needs AC_HIT for its damage block to
// fire. Larva is null-guarded in z_en_goma.c so synthetic AC_HIT without
// acHitInfo no longer crashes (treated as dmgFlags=0 → falls through to the
// sword-damage path, decrements health by 1, plays SetupHurt).
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
// En_Skb (Stalchild) — Hyrule Field at night spawner output. Damage gate
// at z_en_skb.c:456 reads collider.base.acFlags & 2 (AC_HIT).
#include "src/overlays/actors/ovl_En_Skb/z_en_skb.h"
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
    if (damage == 0) {
        return;
    }

    // Walk every syncable actor category looking for the netId match.
    // Widened beyond ACTORCAT_ENEMY so non-host damage to non-ENEMY synced
    // actors (allowlisted via IsSyncedWorldActor or mid-transition BOSS /
    // ITEMACTION / MISC instances) doesn't silently drop.
    Actor* actor = nullptr;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (actor != nullptr) {
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext != nullptr && ext->netId == netId) {
                goto damage_target_found;
            }
            actor = actor->next;
        }
    }
    actor = nullptr;
damage_target_found:
    if (actor == nullptr) {
        return;
    }
    // Skip dead actors — actor->update == NULL after Actor_Kill.
    if (actor->update == nullptr || actor->colChkInfo.health == 0) {
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
        case ACTOR_EN_SW:
            ((EnSw*)actor)->collider.base.acFlags |= AC_HIT;
            break;
        // ACTOR_EN_ST (Skulltula) and ACTOR_EN_DEKUNUTS (Mad Scrub) are
        // INTENTIONALLY OMITTED — both read `info.acHitInfo->toucher.dmgFlags`
        // unconditionally when AC_HIT fires (z_en_st.c:433/440,
        // z_en_dekunuts.c:451). CollisionCheck_AT normally populates
        // acHitInfo alongside the AC_HIT bit; setting only the bit synthetic-
        // ally leaves acHitInfo as a stale or null pointer, and the dereference
        // crashes the host (verified in log 115 — exception 0xC0000005 on
        // Skulltula damage receive). Until we synthesise a fake AT collider
        // OR patch these actors with a null-check guard, leave AC_HIT off for
        // them. damageEffect / atHitEffect propagation still happens — they
        // just won't run their hit-reaction code on remote-only damage.
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
