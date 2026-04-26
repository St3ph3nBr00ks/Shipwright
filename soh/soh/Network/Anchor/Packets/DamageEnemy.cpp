#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ReceiveValidator.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "z64.h"
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
 *   netId    - identifies the enemy that was hit
 *   sceneNum - guards against cross-scene application
 *   damage   - colChkInfo.damage value (total damage from all hits this frame)
 */

void Anchor::SendPacket_DamageEnemy(uint32_t netId, u8 damage) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]     = DAMAGE_ENEMY;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["netId"]    = netId;
    payload["damage"]   = damage;
    payload["quiet"]    = true;

    // Send only to the host — it is the authority on enemy health.
    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && clientId == roomState.ownerClientId) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
            // Test 15 (log 85) — raised DEBUG→INFO so the next test log reveals
            // whether the send side fires per hit. Will demote back to DEBUG
            // once client→host health sync is verified working.
            SPDLOG_INFO("[DamageEnemy] Sent netId={} damage={}", netId, (int)damage);
            break;
        }
    }
}

void Anchor::HandlePacket_DamageEnemy(nlohmann::json payload) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only the host applies incoming damage events.
    if (roomState.ownerClientId != ownClientId) {
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

    SPDLOG_INFO("[DamageEnemy] Received netId={} damage={} preHp={} accumDmg={} "
                "(actor->update will consume next frame)",
                netId, (int)damage, (int)preHp, (int)actor->colChkInfo.damage);
}
