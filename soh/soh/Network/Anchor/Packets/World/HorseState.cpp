/*
 * HORSE_STATE — owner → all peers (room broadcast, ~10 Hz throttled).
 *
 * Per-tick state stream for in-scope horse instances owned by the local
 * client. Receivers apply pos / shape.rot / numBoosts directly to the
 * matching local horse instance; actionState routes through the
 * EnemyNetId-style cached state pattern (lastAppliedActionState on
 * HorseNetId), with vanilla state-machine code playing the corresponding
 * animation locally on receivers.
 *
 * Critical-edge: actionState changes bypass the throttle so animation
 * transitions land immediately. Matches PLAYER_UPDATE's heldActorNetId
 * critical-edge pattern.
 *
 * Plan: Plans/horse_sync_plan.md §"HORSE_STATE" / §"Animation sync
 * model".
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/HorseNetId.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"

#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "objects/object_horse/object_horse.h"  // gEpona*Anim — #257 animationIdx fix
extern PlayState* gPlayState;
}

void Anchor::SendPacket_HorseState(Actor* horse) {
    if (!isConnected) return;
    if (!IsHorseSyncEnabled()) return;
    if (horse == nullptr) return;

    const HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(horse);
    if (ext == nullptr || ext->netId == 0) return;
    // Owners only — peers never send authoritative state for horses
    // they don't own.
    if (ext->isPeerOwned) return;

    EnHorse* en            = (EnHorse*)horse;
    int32_t  actionState   = (int32_t)en->action;
    int32_t  animationIdx  = (int32_t)en->animationIdx;  // #257 — drives peer-side anim
    float    playSpeed     = en->skin.skelAnime.playSpeed;  // #260 — drives anim magnitude + sign
    uint8_t  numBoosts     = en->numBoosts;

    nlohmann::json payload;
    payload["type"]         = HORSE_STATE;
    payload["netId"]        = ext->netId;
    payload["actionState"]  = actionState;
    payload["animationIdx"] = animationIdx;
    payload["playSpeed"]    = playSpeed;
    payload["speedXZ"]      = horse->speedXZ;
    payload["pos"]          = { horse->world.pos.x, horse->world.pos.y, horse->world.pos.z };
    payload["shapeRot"]     = { (int)horse->shape.rot.x,
                                 (int)horse->shape.rot.y,
                                 (int)horse->shape.rot.z };
    payload["numBoosts"]    = (int)numBoosts;
    payload["quiet"]        = true;  // suppress relay-side logging for high-rate packet
    PacketTimeline::SetTimelineField(payload);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_HorseState(nlohmann::json payload) {
    if (!IsHorseSyncEnabled()) return;
    if (gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    uint32_t netId = payload.value("netId", (uint32_t)0);
    if (netId == 0) return;

    // Find the local replica for this netId. mPeerHorses is keyed by
    // netId; we also fall through to a category walk via
    // FindActorByNetId so that mid-scene-transition + late-join races
    // (where the replica exists in the actor list but mPeerHorses
    // hasn't been re-populated yet) still apply state.
    Actor* horse = nullptr;
    auto it = mPeerHorses.find(netId);
    if (it != mPeerHorses.end() && it->second != nullptr &&
        it->second->update != nullptr) {
        horse = it->second;
    } else {
        horse = FindActorByNetId(gPlayState, netId);
        // Cache for future ticks.
        if (horse != nullptr) mPeerHorses[netId] = horse;
    }
    if (horse == nullptr) return;  // late-join race; HORSE_SPAWN will materialise it

    HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(horse);
    if (ext == nullptr) return;

    Vec3f pos = horse->world.pos;
    auto posArr = payload.value("pos", std::vector<float>{});
    if (posArr.size() >= 3) {
        pos.x = posArr[0]; pos.y = posArr[1]; pos.z = posArr[2];
    }
    horse->world.pos = pos;

    auto rotArr = payload.value("shapeRot", std::vector<int>{});
    if (rotArr.size() >= 3) {
        horse->shape.rot.x = (s16)rotArr[0];
        horse->shape.rot.y = (s16)rotArr[1];
        horse->shape.rot.z = (s16)rotArr[2];
    }

    horse->speedXZ = payload.value("speedXZ", 0.0f);

    ext->lastAppliedActionState = (int16_t)payload.value("actionState", -1);

    EnHorse* en = (EnHorse*)horse;
    en->numBoosts = (uint8_t)payload.value("numBoosts", 0);

    // #257 — apply animation from synced animationIdx so the peer-owned
    // horse animates instead of freezing in ENHORSE_ANIM_IDLE. The
    // playerControlled = 1 mount-side gate (HorseHooks.cpp) keeps
    // EnHorse_Update in ENHORSE_ACT_FROZEN which never advances the
    // animation; we drive it manually here. Switch table mirrors the
    // title-screen mapping at DummyPlayer.cpp ~600 (gallop / walk /
    // trot / rear / jump / etc.). Owner's animationIdx is the
    // ENHORSE_ANIM_* enum index — apply only when it changes to avoid
    // resetting the loop phase every packet.
    const int32_t syncedAnimIdx = payload.value("animationIdx", (int32_t)-1);
    if (syncedAnimIdx >= 0 && syncedAnimIdx != en->animationIdx) {
        AnimationHeader* anim = nullptr;
        switch (syncedAnimIdx) {
            case ENHORSE_ANIM_IDLE:
                anim = (AnimationHeader*)&gEponaIdleAnim; break;
            case ENHORSE_ANIM_WHINNEY:
                anim = (AnimationHeader*)&gEponaWhinnyAnim; break;
            case ENHORSE_ANIM_STOPPING:
                // Asset name is "Refuse" but the runtime enum index 2 is
                // STOPPING (per z_en_horse.h). Same surprise documented
                // in DummyPlayer.cpp:605 — preserved for consistency.
                anim = (AnimationHeader*)&gEponaRefuseAnim; break;
            case ENHORSE_ANIM_REARING:
                anim = (AnimationHeader*)&gEponaRearingAnim; break;
            case ENHORSE_ANIM_WALK:
                anim = (AnimationHeader*)&gEponaWalkingAnim; break;
            case ENHORSE_ANIM_TROT:
                anim = (AnimationHeader*)&gEponaTrottingAnim; break;
            case ENHORSE_ANIM_GALLOP:
                anim = (AnimationHeader*)&gEponaGallopingAnim; break;
            case ENHORSE_ANIM_LOW_JUMP:
                anim = (AnimationHeader*)&gEponaJumpingAnim; break;
            case ENHORSE_ANIM_HIGH_JUMP:
                anim = (AnimationHeader*)&gEponaJumpingHighAnim; break;
            default:
                anim = (AnimationHeader*)&gEponaIdleAnim; break;
        }
        Animation_PlayLoop(&en->skin.skelAnime, anim);
        // Cache the synced index so the next packet only re-plays the
        // anim on transitions (same shape as the title-screen path).
        en->animationIdx = (s32)syncedAnimIdx;
    }

    // #260 — apply owner's playSpeed every packet. Animation_PlayLoop
    // hard-codes playSpeed = 1.0f (z_skelanime.c:1801-1803), so the
    // anim-transition path above resets it. Vanilla EnHorse_Mounted*
    // action handlers (z_en_horse.c:1267 walk / :1314 trot / :1382
    // gallop / :1567 rearing+reverse) re-compute playSpeed every frame
    // from speedXZ × per-action coefficient. Peer-owned horses are
    // locked in ENHORSE_ACT_IDLE (commit 4e1d76763) so none of those
    // action handlers run on receivers — playSpeed stays at 1.0f
    // forever without this write.
    //
    // Owner-authoritative: receiver mirrors owner's playSpeed
    // directly. Carries:
    //   - magnitude (gallop ~3.6 vs idle 1.0 — fixes #260 slow gallop)
    //   - sign (reverse is negative — fixes #260 backward-walk bug:
    //     vanilla encodes direction in playSpeed sign, not via a
    //     separate reverse animation)
    //
    // Apply each packet (not just on anim change) because owner's
    // playSpeed drifts continuously during locomotion as stick input
    // / speedXZ change. Default 1.0f if field is absent (legacy
    // schema-1 senders fall back to a still pose, matching pre-fix
    // behaviour).
    en->skin.skelAnime.playSpeed = payload.value("playSpeed", 1.0f);
}
