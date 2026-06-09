/*
 * Horse-sync spawn primitives + CVar gate helper.
 *
 * Plan: Plans/horse_sync_plan.md §"Horse-spawn primitives".
 *
 * One private file-scope helper Anchor_SpawnHorseInternal performs the
 * actual Actor_Spawn + HorseNetId attach + isSpawningNetworkActor
 * bracketing. Two public entry points wrap it:
 *
 *   - Anchor_SpawnPeerHorse (gameplay receive path; reads ambient
 *     gPlayState — safe inside packet handlers per Pitfall 27)
 *   - Anchor_SpawnHorseForTitleScreen (title-screen consumer; caller
 *     provides PlayState* because the title-screen runs pre-save-load
 *     and ambient gPlayState semantics differ)
 *
 * Both bypass EnHorse_Init's save-state Actor_Kill branches by spawning
 * with variantParams = 0 (rideable idle Epona), which the audit
 * verified survives Init in all 5 valid horse scenes and stays
 * ACTORCAT_PROP throughout (no category mutation that would invalidate
 * IsSyncedWorldActor admission).
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/HorseNetId.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------------
// CVar gate
// ---------------------------------------------------------------------------

bool Anchor::IsHorseSyncEnabled() {
    return CVarGetInteger(CVAR_ENHANCEMENT("Anchor.HorseSyncEnabled"), 0) != 0;
}

// ---------------------------------------------------------------------------
// Private helper — the single Actor_Spawn site for peer-owned horses
// ---------------------------------------------------------------------------

static Actor* Anchor_SpawnHorseInternal(PlayState* play,
                                         uint32_t netId,
                                         uint32_t ownerClientId,
                                         int16_t variantParams,
                                         Vec3f pos,
                                         int16_t rotY) {
    if (play == nullptr || Anchor::Instance == nullptr) return nullptr;

    // Bracket the spawn so OnActorSpawn's host-broadcast path treats this
    // as a network-driven spawn and skips re-broadcasting. Pattern mirrors
    // EnemyStateSync (Anchor.h:171 / EnemyState.cpp:2437).
    Anchor::Instance->isSpawningNetworkActor = true;
    Actor* horse = Actor_Spawn(&play->actorCtx, play,
                                ACTOR_EN_HORSE,
                                pos.x, pos.y, pos.z,
                                0 /*rotX*/, rotY, 0 /*rotZ*/,
                                variantParams);
    Anchor::Instance->isSpawningNetworkActor = false;

    if (horse == nullptr) {
        SPDLOG_WARN("[HorseSpawn] Actor_Spawn(ACTOR_EN_HORSE, params={}) returned null "
                    "for netId={} owner={}",
                    (int)variantParams, netId, ownerClientId);
        return nullptr;
    }

    // Attach the HorseNetId extension. EnHorse_Init has already run by
    // this point (Actor_Spawn is synchronous through Init). We're
    // tagging post-Init so the next OnActorUpdate / OnActorKill / draw
    // pass sees the extension and routes through peer-owned semantics.
    HorseNetId* ext = ObjectExtension::GetInstance().Get<HorseNetId>(horse);
    if (ext != nullptr) {
        ext->netId         = netId;
        ext->ownerClientId = ownerClientId;
        ext->isPeerOwned   = (ownerClientId != Anchor::Instance->ownClientId);
    } else {
        SPDLOG_WARN("[HorseSpawn] ObjectExtension::Get<HorseNetId>() returned null for "
                    "netId={} owner={}",
                    netId, ownerClientId);
    }

    return horse;
}

// ---------------------------------------------------------------------------
// Public entry points (extern "C" for C-side decomp callers + title-screen
// agent)
// ---------------------------------------------------------------------------

extern "C" Actor* Anchor_SpawnPeerHorse(uint32_t netId,
                                         uint32_t ownerClientId,
                                         int16_t variantParams,
                                         Vec3f pos,
                                         int16_t rotY) {
    return Anchor_SpawnHorseInternal(gPlayState, netId, ownerClientId,
                                      variantParams, pos, rotY);
}

extern "C" Actor* Anchor_SpawnHorseForTitleScreen(PlayState* play,
                                                   uint32_t netId,
                                                   uint32_t ownerClientId,
                                                   int16_t variantParams,
                                                   Vec3f pos,
                                                   int16_t rotY) {
    return Anchor_SpawnHorseInternal(play, netId, ownerClientId,
                                      variantParams, pos, rotY);
}

// ---------------------------------------------------------------------------
// NetId construction helper — exposed for both owner-side (HORSE_SPAWN
// send) and the title-screen agent (so its netId matches the gameplay
// scheme).
// ---------------------------------------------------------------------------

uint32_t Anchor::MakeHorseNetId(uint32_t ownerClientId, int16_t sceneNum,
                                  int16_t variantParams) {
    return ((uint32_t)ownerClientId << 24)
         | ((uint32_t)(uint16_t)sceneNum << 8)
         | (uint16_t)(uint8_t)variantParams;
}
