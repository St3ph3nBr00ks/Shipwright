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
