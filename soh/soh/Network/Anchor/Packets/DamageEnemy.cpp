#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
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

    // Send only to the host — it is the authority on enemy health.
    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && clientId == roomState.ownerClientId) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
            // Test 15 (log 85) — raised DEBUG→INFO so the next test log reveals
            // whether the send side fires per hit. Will demote back to DEBUG
            // once client→host health sync is verified working.
            SPDLOG_INFO("[DamageEnemy] Sent netId={} damage={} damageEffect={} atHitEffect={}",
                        netId, (int)damage, (int)damageEffect, (int)atHitEffect);
            break;
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

    // Accumulate into colChkInfo.damage. The enemy's own update() will read
    // this on the next frame (same code path as a local sword hit) and call
    // Actor_ApplyDamage, decrement health, play hit reactions, and potentially
    // fire GameInteractor_ExecuteOnEnemyDefeat → ENEMY_DEFEATED.
    // Using += handles the rare case where two DAMAGE_ENEMY packets arrive in
    // the same processing window (multi-hit same frame from the non-host).
    // Test 15 diagnostic: capture HP before and log both.
    u8 preHp = (u8)actor->colChkInfo.health;
    actor->colChkInfo.damage += damage;

    // Schema 2 (#174/#175): replay damageEffect + atHitEffect so enemies that
    // branch on these fields recognise the synthetic hit. damageEffect lives
    // on the enemy itself (set by the damage-table lookup); atHitEffect lives
    // on the *attacker*, but we mirror it onto the enemy here as Option A from
    // Plans/damage_enemy_propagation_fix.md — the per-enemy override table
    // (Phase 3) can refine this if any enemy reads it via a different path.
    //
    if (payload.contains("damageEffect")) {
        actor->colChkInfo.damageEffect = (u8)payload["damageEffect"].get<int>();
    }
    if (payload.contains("atHitEffect")) {
        actor->colChkInfo.atHitEffect = (u8)payload["atHitEffect"].get<int>();
    }

    // Phase 3 of #174/#175 — set AC_HIT on the actor's AC collider so its
    // update() recognises the synthetic hit. Many enemies branch on
    // `collider.base.acFlags & AC_HIT` BEFORE consulting colChkInfo.damage
    // (e.g. EnDekubaba_UpdateDamage, EnKarebaba lunge handler, EnFirefly hit
    // detection). Without AC_HIT, the damage value is set but never read.
    //
    // The collider lives on each actor's own struct at a per-overlay-specific
    // field path. There's no generic Actor::collider pointer, so we dispatch
    // by actor->id. If a synced enemy is added that gates damage on AC_HIT,
    // add it here.
    //
    // For multi-collider actors (Skulltula's 6-cylinder body, Stalfos' body
    // vs sword vs shield, Karebaba's head vs body), we set the bit on the
    // collider that the actor's damage code checks — verified in each
    // actor's source.
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
        default:
            // No AC_HIT setter for this actor type. Damage is delivered via
            // colChkInfo only — works for enemies that don't gate on AC_HIT,
            // breaks silently for those that do. Add new entries above as
            // they're identified during testing.
            break;
    }

    SPDLOG_INFO("[DamageEnemy] Received netId={} damage={} damageEffect={} atHitEffect={} "
                "preHp={} accumDmg={} (actor->update will consume next frame)",
                netId, (int)damage,
                (int)payload.value("damageEffect", 0),
                (int)payload.value("atHitEffect", 0),
                (int)preHp, (int)actor->colChkInfo.damage);

    // Q I Tier 2 — record who dealt this damage so SendPacket_EnemyDefeated
    // can populate killerClientId on the outgoing kill packet. Sender is
    // identified by the auto-injected payload["clientId"] field set by
    // SendJsonToRemote on the originating client.
    uint32_t senderId = payload.value("clientId", (uint32_t)0);
    if (senderId != 0) {
        EnemyStateSync::HostBookkeeping::Instance().RecordDamager(netId, senderId);
    }
}
