#include "soh/Network/Anchor/Anchor.h"
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
            SPDLOG_DEBUG("[DamageEnemy] Sent netId={} damage={}", netId, (int)damage);
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
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    uint32_t netId = payload.value("netId", (uint32_t)0);
    u8       damage = (u8)payload.value("damage", 0);
    if (damage == 0) {
        return;
    }

    // Walk the enemy actor list and find the matching actor by netId.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (actor != nullptr) {
        const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext != nullptr && ext->netId == netId) {
            // Skip dead actors — actor->update == NULL after Actor_Kill.
            if (actor->update == nullptr || actor->colChkInfo.health == 0) {
                break;
            }

            // Accumulate into colChkInfo.damage. The enemy's own update() will read
            // this on the next frame (same code path as a local sword hit) and call
            // Actor_ApplyDamage, decrement health, play hit reactions, and potentially
            // fire GameInteractor_ExecuteOnEnemyDefeat → ENEMY_DEFEATED.
            // Using += handles the rare case where two DAMAGE_ENEMY packets arrive in
            // the same processing window (multi-hit same frame from the non-host).
            actor->colChkInfo.damage += damage;

            SPDLOG_DEBUG("[DamageEnemy] Applied damage={} to netId={} health={} (will be processed next frame)",
                         (int)damage, netId, (int)actor->colChkInfo.health);
            break;
        }
        actor = actor->next;
    }
}
