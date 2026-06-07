#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.h"
extern PlayState* gPlayState;
}

// SHIELD_BOUNCE_PLAYER — host → peer notification when host's hostile
// NPC AT was bounced by the peer's DummyPlayer shield.
//
// Implements the **Vanilla Mirror Pattern** for shield-bounce
// side-effects. See session_state.md → "Vanilla Mirror Pattern" for
// the canonical 4-step recipe. This packet + DAMAGE_PLAYER (Path A)
// are the two current instances of the pattern.
//
// Vanilla origin: Player_Update at z_player.c:4813 reads
//   `sp64 = (shieldQuad.acFlags & AC_BOUNCED) != 0` and applies
//   linearVelocity = -18; CollisionCheck_HitSolid at
//   z_collision_check.c:1597 spawns the hitmark/particles/sfx
//   branched on collider->colType (per-shield via shieldColTypes[]
//   at z_player_lib.c:1525).
//
// Receiver mirror: this handler sets self->linearVelocity = -18 (so
// Player_Update consumes it naturally) and calls the same
// EffectSsHitMark / shield-particle / sfx primitives with the
// peer's local-shield colType — pick branch derived from
// self->currentShield, which is already synced via PLAYER_UPDATE.
//
// Closes three vanilla-parity gaps from the cross-machine shield-block
// path (log 426 field-test):
//
//   Bug 1 — peer's local Link didn't get the small backward push
//           vanilla applies on shield-bounce (z_player.c:4856 sets
//           linearVelocity = -18). Receiver writes the same field
//           so peer's next Player_Update tick processes the push
//           naturally through vanilla movement code.
//
//   Bug 2 — peer didn't see the spark particle / shield-bounce sfx
//           that fires locally on host. Receiver spawns the same
//           EffectSsHitMark_SpawnFixedScale + shield-particle calls
//           on peer's machine at the equivalent local-shield height.
//
//   Bug 3 — the wrong-position particle on host (appearing at feet
//           instead of shield) — fixed at the host send site by
//           spawning the particle at the actual shieldQuad bumper
//           hitPos there. This packet doesn't address the host-side
//           wrong-position artifact; that fix lives in DummyPlayer.cpp
//           alongside the SHIELD_BOUNCE_PLAYER send.

void Anchor::SendPacket_ShieldBouncePlayer(u32 clientId, u16 attackerId, f32 hitOffsetY) {
    if (!IsSaveLoaded()) {
        return;
    }
    nlohmann::json payload;
    payload["type"] = SHIELD_BOUNCE_PLAYER;
    payload["targetClientId"] = clientId;
    payload["attackerId"] = attackerId;
    payload["hitOffsetY"] = hitOffsetY;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ShieldBouncePlayer(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId)) {
        SPDLOG_INFO("[ShieldBouncePlayer] RECV REJECTED — unknown clientId={}", clientId);
        return;
    }

    Player* self = GET_PLAYER(gPlayState);
    if (self == nullptr) {
        return;
    }

    // Don't apply shield-bounce side-effects if peer is in a state where
    // they would be inappropriate (cutscene, item-get, damage anim already
    // playing, etc.). Same gates as DAMAGE_PLAYER.
    if (Player_InBlockingCsMode(gPlayState, self) ||
        self->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS ||
        self->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) {
        SPDLOG_INFO("[ShieldBouncePlayer] RECV DROPPED — cutscene / item-get");
        return;
    }

    const u16 attackerId = payload.value("attackerId", (u16)0);
    const f32 hitOffsetY = payload.value("hitOffsetY", 30.0f);

    // Bug 1 — apply vanilla shield-bounce backward push. Player_Update
    // consumes linearVelocity on the next tick via the standard
    // movement code path; no need to call func_8002F* here. Yaw faces
    // forward (away from attacker) per vanilla z_player.c:4856-4858.
    if (!(self->stateFlags1 & (PLAYER_STATE1_HANGING_OFF_LEDGE |
                               PLAYER_STATE1_CLIMBING_LEDGE |
                               PLAYER_STATE1_CLIMBING_LADDER))) {
        self->linearVelocity = -18.0f;
        self->yaw = self->actor.shape.rot.y;
    }

    // Bug 2 — spawn local particle + sfx at peer's local shield position.
    // We don't have the exact local shield-quad bumper hitPos handy
    // (peer's shield collider isn't necessarily mid-process when this
    // packet arrives), so approximate using world.pos + hitOffsetY +
    // a small forward offset (20u in shape.rot.y direction).
    Vec3f localHitPos;
    f32 forwardX = Math_SinS(self->actor.shape.rot.y) * 20.0f;
    f32 forwardZ = Math_CosS(self->actor.shape.rot.y) * 20.0f;
    localHitPos.x = self->actor.world.pos.x + forwardX;
    localHitPos.y = self->actor.world.pos.y + hitOffsetY;
    localHitPos.z = self->actor.world.pos.z + forwardZ;

    // Branch on peer's local shield type — peer IS the one shielding,
    // so its own currentShield is the source of truth for what visual
    // and sound the shield bounce should produce. Mirrors vanilla
    // CollisionCheck_HitSolid (z_collision_check.c:1597) which switches
    // on collider->colType — and player shield colType is per-shield
    // (z_player_lib.c:1525-1530 shieldColTypes[] = METAL/WOOD/METAL/METAL).
    const bool isWoodShield = (self->currentShield == PLAYER_SHIELD_DEKU);
    if (isWoodShield) {
        EffectSsHitMark_SpawnFixedScale(gPlayState, EFFECT_HITMARK_DUST, &localHitPos);
        Audio_PlaySoundGeneral(NA_SE_IT_REFLECTION_WOOD, &self->actor.projectedPos, 4,
                               &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                               &gSfxDefaultReverb);
    } else {
        EffectSsHitMark_SpawnFixedScale(gPlayState, EFFECT_HITMARK_METAL, &localHitPos);
        CollisionCheck_SpawnShieldParticlesMetalSound(gPlayState, &localHitPos, &self->actor.projectedPos);
    }

    SPDLOG_INFO("[ShieldBouncePlayer] APPLIED clientId={} attackerId=0x{:04X} "
                "linearVelocity={:.1f} yaw=0x{:04X} hitPos=({:.1f},{:.1f},{:.1f}) "
                "shieldType={} ({})",
                clientId, attackerId, self->linearVelocity,
                (uint16_t)self->yaw,
                localHitPos.x, localHitPos.y, localHitPos.z,
                (int)self->currentShield, isWoodShield ? "wood" : "metal");
}
