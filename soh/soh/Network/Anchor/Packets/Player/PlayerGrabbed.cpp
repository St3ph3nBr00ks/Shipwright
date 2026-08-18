#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// PLAYER_GRABBED — host → peer notification when a host-owned hostile
// NPC (Moblin, Like Like, Dead Hand arm, Redead, Wallmaster,
// Floormaster, Morpha) successfully grabbed a peer's DummyPlayer via
// `play->grabPlayer(play, dummyPeer)`. Peer's receive handler
// replicates the vanilla grab effect on the peer's own local Link so
// the grabbed player is actually stuck, matches the vanilla stateFlags2
// PLAYER_STATE2_GRABBED_BY_ENEMY, and gets the "held" voice sfx.
//
// Implements the **Vanilla Mirror Pattern** for grab acquisition. See
// session_state.md → "Vanilla Mirror Pattern" for the canonical
// 4-step recipe. Sibling: PLAYER_RELEASED (release + throw).
//
// Vanilla origin (Moblin pilot at z_en_mb.c:961-963):
//   if (play->grabPlayer(play, player)) {
//       player->actor.parent = &this->actor;
//   }
// where `play->grabPlayer` is the vtable-exposed
// `func_80852F38(PlayState*, Player*)` at `z_player.c:16749-16762`:
//   - guards on !Player_InBlockingCsMode + invincibilityTimer >= 0 +
//     !func_8008F128 + !PLAYER_STATE3_FLYING_WITH_HOOKSHOT
//   - clears cstate (func_80832564), sets Player_Action_8084F308
//   - plays gPlayerAnim_link_normal_re_dead_attack
//   - sets PLAYER_STATE2_GRABBED_BY_ENEMY on stateFlags2
//   - resets weapon hold (func_80832224)
//   - plays NA_SE_VO_LI_HELD voice sfx
//
// Receiver mirror: peer's own gPlayState->grabPlayer vtable pointer
// points to the same func_80852F38 (set in Player_Init), so we can
// simply call it. It writes to peer's own Player struct which is the
// correct target — peer's DummyPlayer on host is just the sync
// representation; on peer's machine, the ACTUAL grabbed entity IS
// peer's Player actor.
//
// Positioning while grabbed: host's Moblin sync (ENEMY_STATE) already
// broadcasts world.pos + shape.rot. Peer's local Bg attaches to its
// local Moblin replica via actor.parent, and the vanilla
// z_en_mb.c:971-980 positioning code (which runs on peer's local
// Moblin) will place peer's grabbed Link relative to it every frame.
// No per-frame wire needed.
//
// Payload:
//   targetClientId : u32   — the peer being grabbed
//   attackerNetId  : u32   — the host enemy's EnemyNetId; peer looks
//                            up its local replica to set actor.parent
//   damage         : u8    — optional pre-grab damage (Moblin: 8);
//                            receiver applies via damagePlayer(-damage)
//                            before grab. 0 = no pre-damage.

void Anchor::SendPacket_PlayerGrabbed(u32 clientId, u32 attackerNetId, u8 damage) {
    if (!IsSaveLoaded()) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = PLAYER_GRABBED;
    payload["targetClientId"] = clientId;
    payload["attackerNetId"] = attackerNetId;
    payload["damage"] = damage;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_PlayerGrabbed(nlohmann::json payload) {
    Player* self = GET_PLAYER(gPlayState);
    if (self == nullptr) {
        return;
    }

    const u32 attackerNetId = payload.value("attackerNetId", (u32)0);
    const u8 damage = payload.value("damage", (u8)0);

    // Pre-grab damage first (vanilla runs damagePlayer BEFORE grabPlayer
    // at z_en_mb.c:955, so peer's HP has already dropped by the time the
    // grab lands). Guard against negative-invincibility state matching
    // vanilla's z_en_mb.c:950-957 shape.
    if (damage != 0 && self->invincibilityTimer >= -39) {
        self->invincibilityTimer = 0;
        gPlayState->damagePlayer(gPlayState, -(s32)damage);
    }

    // Vanilla grabPlayer via vtable. Same function pointer as host's
    // (Player_Init on each client sets play->grabPlayer = func_80852F38).
    // Returns false when peer is in a state that vanilla protects from
    // grab (cutscene, invincibility, magic-spell action, flying with
    // hookshot). If false, drop actor.parent assignment — leaves peer
    // free. If true, look up the local attacker replica and attach.
    if (!gPlayState->grabPlayer(gPlayState, self)) {
        SPDLOG_INFO("[PlayerGrabbed] grabPlayer refused — peer in "
                    "protected state (cutscene/invincible/spell/flying)");
        return;
    }

    // Look up the local replica of the attacker so peer's actor.parent
    // points at it. The parent pointer drives vanilla positioning
    // (z_en_mb.c:971-980 positions grabbed Link relative to Moblin
    // when player->actor.parent == &Moblin->actor). If the attacker
    // isn't loaded on peer (e.g. host and peer in different rooms of
    // the same scene), skip the parent link — peer is grabbed
    // (stateFlags2 bit set) but won't be dragged. PLAYER_RELEASED will
    // still clear the grab state normally.
    if (attackerNetId != 0) {
        Actor* attacker = FindActorByNetId(gPlayState, attackerNetId);
        if (attacker != nullptr) {
            self->actor.parent = attacker;
            SPDLOG_INFO("[PlayerGrabbed] applied — attackerNetId=0x{:X} "
                        "damage={}", attackerNetId, damage);
        } else {
            SPDLOG_INFO("[PlayerGrabbed] applied without parent — "
                        "attackerNetId=0x{:X} not loaded locally", attackerNetId);
        }
    }
}

// C-callable bridge — invoked from actor-side sender integration.
// Gated on isConnected so non-MP builds are a no-op. The sender-site
// design (detect AT_HIT against DummyPlayer specifically, extract the
// target clientId from the DummyPlayer's extension, extract attacker's
// EnemyNetId::netId) mirrors the Path A DAMAGE_PLAYER pattern. Not
// currently wired to any actor — reserved for the Moblin (En_Mb)
// pilot per Plans/pitfall_28_phase2_vmp_walkthrough_2026-06-15.md §2.3.
extern "C" void Anchor_NotifyPlayerGrabbed(uint32_t targetClientId,
                                           uint32_t attackerNetId,
                                           uint8_t damage) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    Anchor::Instance->SendPacket_PlayerGrabbed(targetClientId, attackerNetId, damage);
}
