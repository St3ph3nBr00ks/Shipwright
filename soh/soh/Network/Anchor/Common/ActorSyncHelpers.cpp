#include "ActorSyncHelpers.h"

extern "C" {
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

SkelAnime* GetEnemySkelAnime(Actor* actor) {
    // Explicit exceptions: enemies with fields between Actor and SkelAnime.
    switch (actor->id) {
        case ACTOR_EN_DEKUBABA: return &((EnDekubaba*)actor)->skelAnime;
        case ACTOR_EN_TEST:     return &((EnTest*)actor)->skelAnime;
        case ACTOR_EN_RD:       return &((EnRd*)actor)->skelAnime;
        case ACTOR_EN_WF:       return &((EnWf*)actor)->skelAnime;
        case ACTOR_EN_MB:       return &((EnMb*)actor)->skelAnime;
        default: break;
    }

    // Generic case: SkelAnime immediately follows Actor.
    struct GenericEnemy { Actor actor; SkelAnime skelAnime; };
    SkelAnime* ska = &((GenericEnemy*)actor)->skelAnime;

    // Validate before trusting it — for the ~39 non-default enemies whose data
    // at this offset is NOT a SkelAnime, limbCount is typically 0 or out of range.
    if (ska->limbCount == 0 || ska->limbCount > 30 || ska->jointTable == nullptr) {
        return nullptr;
    }
    return ska;
}

bool IsSyncedWorldActor(int16_t actorId) {
    switch (actorId) {
        case ACTOR_EN_GOROIWA:  return true;  // #153 (PROP)
        case ACTOR_EN_SW:       return true;  // #148 Skullwalltula (gold variant → NPC)
        case ACTOR_EN_DEKUNUTS: return true;  // #135 Mad Scrub (ITEMACTION projectile transition)
        default:                return false;
    }
}

bool IsSyncedBossActor(int16_t actorId) {
    switch (actorId) {
        // First Dungeon Demo (#167 Step 9) — paired with boss_goma_sync_plan.md
        // implementation. Until the per-actor sync logic lands, admission
        // produces silent ENEMY_STATE traffic (receiver discards the payload),
        // which is harmless — it lets us verify admission alone.
        case ACTOR_BOSS_GOMA:    return true;  // Queen Gohma (#67)
        // Future allowlist additions go here as their trackers land. Each one
        // should land in the SAME PR as that boss's per-actor sync logic.
        // case ACTOR_BOSS_DODONGO:    return true;  // #68
        // case ACTOR_BOSS_VA:         return true;  // #69
        // case ACTOR_BOSS_FD:         return true;  // #70
        // case ACTOR_BOSS_FD2:        return true;  // #70
        // case ACTOR_BOSS_MO:         return true;  // #71
        // case ACTOR_BOSS_SST:        return true;  // #72
        // case ACTOR_BOSS_TW:         return true;  // #73
        // case ACTOR_BOSS_GANON:      return true;  // #74
        // case ACTOR_BOSS_GANONDROF:  return true;  // #112
        // case ACTOR_BOSS_GANON2:     return true;  // #75
        default:                 return false;
    }
}

uint32_t EncodeEnemyNetId(Actor* actor) {
    uint8_t posHash = (uint8_t)((int16_t)actor->home.pos.x) ^
                      (uint8_t)((int16_t)actor->home.pos.y >> 2) ^  // #162 Proposal A
                      (uint8_t)((int16_t)actor->home.pos.z >> 1) ^
                      (uint8_t)actor->room;
    // Pillar B Phase 2 — high bit of the scene field carries linkAge so
    // a child-timeline actor and an adult-timeline actor at the same
    // (scene, id, posHash) get distinct netIds.
    uint32_t scenePart = (uint32_t)(uint16_t)gPlayState->sceneNum & 0x7FFF;
    scenePart |= ((uint32_t)gSaveContext.linkAge & 0x1) << 15;
    return (scenePart << 16) |
           ((uint32_t)(uint16_t)actor->id << 8) |
           posHash;
}
