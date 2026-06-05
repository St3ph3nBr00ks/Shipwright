#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "macros.h"
#include "variables.h"  // gSaveContext — needed for [Bug1.Diag] health-before/after log (Pitfall 9)
#include "functions.h"
extern PlayState* gPlayState;
void func_80838280(Player* player);
}

/**
 * DAMAGE_PLAYER
 */

// Bug 3 fix (2026-06-05) — overload carrying the attacker's world
// position so the receive-side knockback yaw is computed from the
// actual attacker (e.g. Invader actor) instead of from the sender's
// player. The legacy two-arg overload kept for callers that don't
// have an attacker actor handy (PvP friendly-fire from the sender's
// sword — the sender's player IS the attacker).
void Anchor::SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage,
                                     const Vec3f* attackerPos) {
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

    SendJsonToRemote(payload);
}

void Anchor::SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage) {
    SendPacket_DamagePlayer(clientId, damageEffect, damage, nullptr);
}

void Anchor::HandlePacket_DamagePlayer(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId) || clients[clientId].player == nullptr) {
        SPDLOG_INFO("[Bug1.Diag] RECV DAMAGE_PLAYER REJECTED early — clientId={} "
                    "(unknown sender or null peer player)", clientId);
        return;
    }

    AnchorClient& anchorClient = clients[clientId];
    Player* otherPlayer = anchorClient.player;
    Player* self = GET_PLAYER(gPlayState);

    u8 damageEffect = payload.at("damageEffect").get<u8>();
    u8 damage = payload.at("damage").get<u8>();
    const uint64_t curFrame = (Anchor::Instance != nullptr)
        ? Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed) : 0;

    // [Bug1.Diag] (2026-06-05) — log every incoming DAMAGE_PLAYER with the
    // local Link's current state so we can correlate receive timing
    // against the host's send log. If the peer reports "must be hit
    // twice", we want to see: (a) how many packets actually arrive per
    // host-swing, (b) whether the cutscene gate below silently drops
    // them, (c) Link's iframe / state at the moment each one applies.
    SPDLOG_INFO("[Bug1.Diag] RECV DAMAGE_PLAYER from senderClientId={} "
                "curFrame={} damage={} damageEffect={} "
                "selfInvincibilityTimer={} selfStateFlags1=0x{:X} "
                "selfStateFlags2=0x{:X} hasAttackerPos={}",
                clientId, curFrame, (int)damage, (int)damageEffect,
                (int)self->invincibilityTimer,
                self->stateFlags1, self->stateFlags2,
                payload.contains("attackerPos"));

    // Prevent incoming damage during cutscenes or item get sequences
    if (Player_InBlockingCsMode(gPlayState, self) || self->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS ||
        self->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) {
        SPDLOG_INFO("[Bug1.Diag] RECV DAMAGE_PLAYER DROPPED — cutscene / item-get "
                    "block. blockingCs={} state1_IN_ITEM_CS={} "
                    "state1_GETTING_ITEM={}",
                    Player_InBlockingCsMode(gPlayState, self),
                    (self->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS) != 0,
                    (self->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) != 0);
        return;
    }

    self->actor.colChkInfo.damage = damage * 8; // Arbitrary number currently, need to fine tune

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

    // Bug 3 fix (2026-06-05) — knockback yaw from the actual attacker if
    // the sender included an attackerPos field (e.g. Invader's
    // world.pos), else fall back to "yaw from sender's player toward
    // self" (legacy behaviour — PvP friendly-fire still uses this).
    s16 knockbackYaw;
    if (payload.contains("attackerPos")) {
        const auto& ap = payload["attackerPos"];
        const f32 apx = ap.value("x", 0.0f);
        const f32 apy = ap.value("y", 0.0f);
        const f32 apz = ap.value("z", 0.0f);
        const f32 dx = self->actor.world.pos.x - apx;
        const f32 dz = self->actor.world.pos.z - apz;
        // Bug A fix (2026-06-05) — OoT's Math_Atan2 family takes (z, x),
        // NOT (x, z). The prior order produced a yaw rotated ~90° which
        // sometimes pushed the player INTO the attacker (field-test 413
        // — "P1 knocked forward into the Goroiwa 3D model"). Mirror
        // Actor_WorldYawTowardActor / Math_Vec3f_Yaw which use
        // Math_Atan2S(z_delta, x_delta).
        knockbackYaw = Math_FAtan2F(dz, dx);
        // [Bug1.Diag] (2026-06-05) — break down the yaw computation so
        // we can verify direction is correct from the payload values.
        SPDLOG_INFO("[Bug1.Diag] RECV knockbackYaw computation: "
                    "self.pos=({:.0f},{:.0f},{:.0f}) "
                    "attackerPos=({:.0f},{:.0f},{:.0f}) "
                    "dx={:.1f} dz={:.1f} yaw=0x{:04X}",
                    self->actor.world.pos.x, self->actor.world.pos.y,
                    self->actor.world.pos.z, apx, apy, apz,
                    dx, dz, (uint16_t)knockbackYaw);
    } else {
        knockbackYaw = Actor_WorldYawTowardActor(&otherPlayer->actor, &self->actor);
        SPDLOG_INFO("[Bug1.Diag] RECV knockbackYaw fallback (no attackerPos): "
                    "self.pos=({:.0f},{:.0f},{:.0f}) "
                    "otherPlayer.pos=({:.0f},{:.0f},{:.0f}) yaw=0x{:04X}",
                    self->actor.world.pos.x, self->actor.world.pos.y,
                    self->actor.world.pos.z, otherPlayer->actor.world.pos.x,
                    otherPlayer->actor.world.pos.y, otherPlayer->actor.world.pos.z,
                    (uint16_t)knockbackYaw);
    }
    const u8 healthBefore = gSaveContext.health;
    func_80837C0C(gPlayState, self, damageEffect, 4.0f, 5.0f, knockbackYaw, 20);
    SPDLOG_INFO("[Bug1.Diag] RECV DAMAGE_PLAYER APPLIED — func_80837C0C done. "
                "colChkInfo.damage={} healthBefore={} healthAfter={} "
                "knockbackYaw=0x{:04X}",
                (int)self->actor.colChkInfo.damage,
                (int)healthBefore, (int)gSaveContext.health,
                (uint16_t)knockbackYaw);
}
