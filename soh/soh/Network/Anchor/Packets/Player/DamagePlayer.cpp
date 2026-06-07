#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <unordered_map>
#include <unordered_set>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
void func_80838280(Player* player);
}

/**
 * DAMAGE_PLAYER
 *
 * Implements the **Vanilla Mirror Pattern** for enemy AT damage +
 * knockback. See session_state.md → "Vanilla Mirror Pattern" for
 * the canonical 4-step recipe. This packet is the "Path A" instance
 * of the pattern; ShieldBouncePlayer.cpp is another instance.
 *
 * Sender-side surface: DummyPlayer.cpp's AC_HIT detection +
 *   Common/EnemyKnockbackTable.cpp (per-attacker constants).
 * Vanilla origin: enemy AT_HIT branch calls func_8002F6D4 / _71C /
 *   _758 / _7A0 → 5 knockback fields on GET_PLAYER → Player_Update
 *   z_player.c:4795 consumes them on the next tick.
 * Receiver mirror: HandlePacket_DamagePlayer sets the same 5 fields
 *   on self (local Link) via the "knockback" payload block, letting
 *   Player_Update reproduce vanilla animation + iframes + damage
 *   application by construction.
 */

// Overload chain — three signatures, all funnel through the bottom one:
//   (clientId, effect, damage)                            — minimal; PvP friendly-fire
//   (..., attackerPos)                                    — adds attacker position for yaw
//   (..., attackerPos, kbType, kbSpeed, kbYVel, kbDamage) — full Path A vanilla-knockback
// The minimal and attackerPos-only overloads default-zero the kb fields
// (kbType=0 sentinel) → receiver routes through legacy func_80837C0C.
void Anchor::SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage,
                                     const Vec3f* attackerPos) {
    SendPacket_DamagePlayer(clientId, damageEffect, damage, attackerPos,
                            0 /* no knockback */, 0.0f, 0.0f, 0);
}

void Anchor::SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage) {
    SendPacket_DamagePlayer(clientId, damageEffect, damage, nullptr);
}

void Anchor::SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage,
                                     const Vec3f* attackerPos,
                                     u32 knockbackType, f32 knockbackSpeed,
                                     f32 knockbackYVelocity, u32 knockbackDamage) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = DAMAGE_PLAYER;
    payload["targetClientId"] = clientId;
    payload["damageEffect"] = damageEffect;
    payload["damage"] = damage;
    if (attackerPos != nullptr) {
        payload["attackerPos"] = {
            { "x", attackerPos->x },
            { "y", attackerPos->y },
            { "z", attackerPos->z },
        };
    }
    if (knockbackType != 0) {
        payload["knockback"] = {
            { "type", knockbackType },
            { "speed", knockbackSpeed },
            { "yVelocity", knockbackYVelocity },
            { "damage", knockbackDamage },
        };
    }

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_DamagePlayer(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId) || clients[clientId].player == nullptr) {
        SPDLOG_INFO("[DamagePlayer] RECV REJECTED — unknown clientId={}", clientId);
        return;
    }

    AnchorClient& anchorClient = clients[clientId];
    Player* otherPlayer = anchorClient.player;
    Player* self = GET_PLAYER(gPlayState);

    u8 damageEffect = payload.at("damageEffect").get<u8>();
    u8 damage = payload.at("damage").get<u8>();

    SPDLOG_INFO("[DamagePlayer] RECV from clientId={} damage={} effect={} "
                "iframes={} stateFlags1=0x{:X} hasAttackerPos={}",
                clientId, (int)damage, (int)damageEffect,
                (int)self->invincibilityTimer, self->stateFlags1,
                payload.contains("attackerPos"));

    // Cutscene / item-get block — match vanilla suppression behavior.
    if (Player_InBlockingCsMode(gPlayState, self) || self->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS ||
        self->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) {
        SPDLOG_INFO("[DamagePlayer] RECV DROPPED — cutscene / item-get");
        return;
    }

    // Wire damage is raw AT damage in HP units. DummyPlayer.cpp reads
    // cylinder.info.acHitInfo->toucher.damage and ships unchanged. See
    // Plans/dummy_player_damage_table_audit.md.
    self->actor.colChkInfo.damage = damage;

    if (damageEffect == DUMMY_PLAYER_HIT_RESPONSE_FIRE) {
        for (int i = 0; i < ARRAY_COUNT(self->bodyFlameTimers); i++) {
            self->bodyFlameTimers[i] = Rand_S16Offset(0, 200);
        }
        self->bodyIsBurning = true;
    } else if (damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
        self->actor.freezeTimer = 20;
        Actor_SetColorFilter(&self->actor, 0, 0xFF, 0, 24);
        return;
    }

    // Vanilla Mirror Pattern — candidate-queue gap instrumentation for
    // hit-response effects that arrive via DAMAGE_PLAYER's damageEffect
    // dispatch but lack full vanilla parity. Currently handled via
    // func_80837C0C-direct (loses vanilla ice-trap freeze animation,
    // electric-shock recoil chain, etc.). See session_state.md →
    // "Vanilla Mirror Pattern" → candidate queue entry for "Ice Trap
    // / Electric Shock effects." Once per damageEffect value globally.
    {
        struct HitResponseGap {
            const char* name;
            const char* missingEffect;
        };
        static const std::unordered_map<u8, HitResponseGap> sHitResponseGaps = {
            { PLAYER_HIT_RESPONSE_ICE_TRAP,
              { "Ice Trap",
                "vanilla ice-cube freeze anim + func_80832224 + frozen state" } },
            { PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK,
              { "Electric Shock",
                "vanilla shock-jitter anim + bodyShockTimer flow" } },
        };
        static std::unordered_set<u8> sLoggedHitResponseGap;
        auto gapIt = sHitResponseGaps.find(damageEffect);
        if (gapIt != sHitResponseGaps.end() &&
            sLoggedHitResponseGap.find(damageEffect) == sLoggedHitResponseGap.end()) {
            sLoggedHitResponseGap.insert(damageEffect);
            SPDLOG_WARN("[VanillaMirror.gap] damageEffect={} ({}) arrived via DAMAGE_PLAYER "
                        "but vanilla parity for — {} — is NOT fully mirrored. "
                        "Current receive uses func_80837C0C-direct which produces an "
                        "approximation; full vanilla flow requires invoking the same "
                        "Player_Update branches the local hit would take. See "
                        "session_state.md → 'Vanilla Mirror Pattern' for the recipe. "
                        "Logged once per damageEffect value per session.",
                        (int)damageEffect, gapIt->second.name, gapIt->second.missingEffect);
        }
    }

    // Knockback yaw — from sender's `attackerPos` (preferred — accurate
    // direction-of-impact for any attacker that supplies it) or fall
    // back to "yaw from sender's player to self" (legacy behavior, used
    // by PvP friendly-fire where sender's player IS the attacker).
    //
    // OoT's Math_Atan2 family takes (z, x) order, NOT (x, z). And use
    // Math_Atan2S (s16 binary angle) not Math_FAtan2F (f32 radians) —
    // the float-to-s16 truncation collapses every angle to 0..3.
    s16 knockbackYaw;
    if (payload.contains("attackerPos")) {
        const auto& ap = payload["attackerPos"];
        const f32 dx = self->actor.world.pos.x - ap.value("x", 0.0f);
        const f32 dz = self->actor.world.pos.z - ap.value("z", 0.0f);
        knockbackYaw = Math_Atan2S(dz, dx);
    } else {
        knockbackYaw = Actor_WorldYawTowardActor(&otherPlayer->actor, &self->actor);
    }

    // Path A — vanilla-knockback flow. When sender includes a
    // "knockback" block, set the 5 knockback fields on GET_PLAYER
    // (mirrors func_8002F698 at z_actor.c:2216-2224); Player_Update at
    // z_player.c:4795 consumes them next tick, calls func_80837C0C
    // internally with the right args. Damage application + iframes via
    // Player_SetIntangibility + knockback animation all match vanilla
    // by construction because vanilla owns the code path.
    if (payload.contains("knockback")) {
        const auto& kb     = payload["knockback"];
        self->knockbackType     = kb.value("type", 0u);
        self->knockbackSpeed    = kb.value("speed", 0.0f);
        self->knockbackYVelocity = kb.value("yVelocity", 0.0f);
        self->knockbackDamage   = kb.value("damage", 0u);
        self->knockbackRot      = knockbackYaw;

        SPDLOG_INFO("[DamagePlayer] APPLIED vanilla-kb path. kbType={} kbSpeed={:.1f} "
                    "colChkInfo.damage={} yaw=0x{:04X} (Player_Update will process)",
                    (uint32_t)self->knockbackType, self->knockbackSpeed,
                    (int)self->actor.colChkInfo.damage, (uint16_t)knockbackYaw);
        return;
    }

    // Legacy fallback — sender didn't supply knockback block (PvP
    // friendly-fire, attackers not in EnemyKnockbackTable). Direct
    // func_80837C0C call loses some vanilla parity but works.
    func_80837C0C(gPlayState, self, damageEffect, 4.0f, 5.0f, knockbackYaw, 20);
    SPDLOG_INFO("[DamagePlayer] APPLIED legacy path. colChkInfo.damage={} yaw=0x{:04X}",
                (int)self->actor.colChkInfo.damage, (uint16_t)knockbackYaw);
}
