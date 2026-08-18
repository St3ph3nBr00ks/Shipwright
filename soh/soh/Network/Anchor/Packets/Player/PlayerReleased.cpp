#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// PLAYER_RELEASED — host → peer notification when a host-owned enemy
// releases + throws a previously-grabbed peer. Sibling to
// PLAYER_GRABBED. Second half of the VMP Phase 2 grab-and-throw pair.
//
// Vanilla origin (Moblin pilot at z_en_mb.c:621-627, mirrored at
// :985-990 for endCharge release):
//   player->stateFlags2 &= ~PLAYER_STATE2_GRABBED_BY_ENEMY;
//   player->actor.parent = NULL;
//   player->av2.actionVar2 = 200;
//   func_8002F71C(play, &this->actor, 4.0f, this->actor.world.rot.y, 4.0f);
//
// Three effects:
//   1. Clear grab state (stateFlags2 bit + parent pointer + actionVar2).
//   2. Apply the throw knockback (func_8002F71C — vanilla small-KB
//      helper that sets player->pushedSpeed / pushedYaw for the next
//      Player_Update tick to consume).
//   3. actionVar2=200 is the post-release animation frame counter
//      that drives the "thrown backward" animation.
//
// Receiver mirror: peer's Player_Update naturally consumes the
// pushedSpeed/pushedYaw values written by func_8002F71C. We call the
// same primitive directly on peer using the wire-shipped knockback
// params (from host's `world.rot.y` at broadcast time).
//
// The attackerNetId identity check ensures we only clear the grab
// state if peer is actually grabbed BY this specific attacker —
// prevents a stale release packet from an old enemy from disrupting
// a fresh grab by a different one.
//
// Payload:
//   targetClientId : u32   — the peer being released
//   attackerNetId  : u32   — attacker identity for validation
//   kbSpeed        : f32   — throw speed (Moblin: 4.0 or 6.0)
//   kbYaw          : s16   — throw yaw (host's shape.rot.y at release)
//   kbYVel         : f32   — vertical throw component
//   actionVar2     : u8    — post-release anim index (Moblin: 200)

void Anchor::SendPacket_PlayerReleased(u32 clientId, u32 attackerNetId,
                                       f32 kbSpeed, s16 kbYaw, f32 kbYVel,
                                       u8 actionVar2) {
    if (!IsSaveLoaded()) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PLAYER_RELEASED;
    payload["targetClientId"] = clientId;
    payload["attackerNetId"] = attackerNetId;
    payload["kbSpeed"] = kbSpeed;
    payload["kbYaw"] = kbYaw;
    payload["kbYVel"] = kbYVel;
    payload["actionVar2"] = actionVar2;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_PlayerReleased(nlohmann::json payload) {
    Player* self = GET_PLAYER(gPlayState);
    if (self == nullptr) {
        return;
    }

    const u32 attackerNetId = payload.value("attackerNetId", (u32)0);

    // Identity check — only release if peer is currently grabbed BY
    // this specific attacker. Prevents stale/replayed release packets
    // from disrupting a fresh grab-by-a-different-enemy.
    if (!(self->stateFlags2 & PLAYER_STATE2_GRABBED_BY_ENEMY)) {
        SPDLOG_INFO("[PlayerReleased] ignored — peer not grabbed");
        return;
    }
    if (attackerNetId != 0 && self->actor.parent != nullptr) {
        Actor* expectedAttacker = FindActorByNetId(gPlayState, attackerNetId);
        if (expectedAttacker != nullptr && self->actor.parent != expectedAttacker) {
            SPDLOG_INFO("[PlayerReleased] ignored — attackerNetId=0x{:X} "
                        "doesn't match current parent",
                        attackerNetId);
            return;
        }
    }

    const f32 kbSpeed = payload.value("kbSpeed", 4.0f);
    const s16 kbYaw = payload.value("kbYaw", (s16)0);
    const f32 kbYVel = payload.value("kbYVel", 4.0f);
    const u8 actionVar2 = payload.value("actionVar2", (u8)200);

    // Vanilla clear-grab writes (mirror of z_en_mb.c:622-624).
    self->stateFlags2 &= ~PLAYER_STATE2_GRABBED_BY_ENEMY;
    self->actor.parent = NULL;
    self->av2.actionVar2 = actionVar2;

    // Vanilla throw knockback via func_8002F71C → func_8002F6D4 →
    // func_8002F698 which writes GET_PLAYER(play)->knockback{Type,Rot,
    // Speed,YVelocity,Damage} — Player_Update consumes those on the
    // next tick. The `actor` parameter is passed through unread; we
    // supply &self->actor as a stand-in. Yaw+speed+yVel come from the
    // wire (host's world.rot.y at the release site).
    func_8002F71C(gPlayState, &self->actor, kbSpeed, kbYaw, kbYVel);

    SPDLOG_INFO("[PlayerReleased] applied — attackerNetId=0x{:X} "
                "kbSpeed={:.1f} kbYaw=0x{:04X} kbYVel={:.1f} actionVar2={}",
                attackerNetId, kbSpeed, (uint16_t)kbYaw, kbYVel, actionVar2);
}

// C-callable bridge — sibling to Anchor_NotifyPlayerGrabbed. Same
// deferred-integration story: sender site fires from the actor's
// release code path (Moblin: z_en_mb.c:621 stunned-release AND :989
// endCharge-release, both currently unwired).
extern "C" void Anchor_NotifyPlayerReleased(uint32_t targetClientId,
                                            uint32_t attackerNetId,
                                            float kbSpeed, short kbYaw,
                                            float kbYVel, uint8_t actionVar2) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    Anchor::Instance->SendPacket_PlayerReleased(targetClientId, attackerNetId,
                                                kbSpeed, kbYaw, kbYVel, actionVar2);
}
