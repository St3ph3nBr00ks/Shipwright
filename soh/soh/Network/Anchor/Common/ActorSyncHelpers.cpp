#include "ActorSyncHelpers.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/Network/Anchor/Anchor.h"

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
        case ACTOR_EN_NUTSBALL: return true;  // En_Hintnuts deku-nut projectile (PROP).
                                              // Host's spawn fires ENEMY_SPAWN; peer spawns
                                              // matching copy. Both sides run deterministic
                                              // Actor_MoveXZGravity locally and detect
                                              // collisions independently, advancing each
                                              // peer's local sPuzzleCounter consistently.
        case ACTOR_EN_HINTNUTS: return true;  // #180 Compound Room scrubs. Default category
                                              // is ENEMY (admitted via the default branch
                                              // already), but EnHintnuts_HitByScrubProjectile1
                                              // (z_en_hintnuts.c:128-133) calls
                                              // Actor_ChangeCategory(... ACTORCAT_BG) on the
                                              // third correct puzzle hit (sPuzzleCounter==2)
                                              // and on any non-puzzle scrub (params==0).
                                              // Without explicit allowlist entry the
                                              // category change drops the actor out of
                                              // OnActorUpdate's IsSyncableActor gate, so
                                              // Run-state ENEMY_STATE traffic stops on the
                                              // host and the "scrub running around the room"
                                              // animation never syncs to peers.

        // Push-block / lift-and-throw actors (Pillar C realtime extension).
        // FLAG_SCENE_SWITCH already replicates the *final* resting position
        // via WorldStateSync, but the user-visible push/lift/throw motion
        // wasn't synced — peer saw the block stay put until the puzzle
        // completed, then teleport to its final position. Admitting these
        // to IsSyncedWorldActor enables ENEMY_STATE world.pos sync at
        // ~20pps so peer sees the motion in realtime.
        //
        // Host-authoritative: only the host's player can push/lift the
        // block; peer follows via position sync. Each actor is a Dyna or
        // collision-bearing actor — DynaPoly's mesh update reads
        // actor.world.pos each frame, so collision tracks the synced
        // position automatically (peer's local Link can still walk on
        // top of the block as it moves under host authority).
        case ACTOR_OBJ_OSHIHIKI:   return true;  // Generic dungeon push block
                                                 // (DynaPoly, ACTORCAT_PROP).
        case ACTOR_BG_HEAVY_BLOCK: return true;  // Death Mountain large block
                                                 // (Golden Gauntlets,
                                                 // ACTORCAT_BG).
        case ACTOR_EN_ISHI:        return true;  // Small/large gray rocks
                                                 // (lift + throw,
                                                 // ACTORCAT_PROP).

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

uint32_t EncodeUniqueDynamicNetId(Actor* actor) {
    if (gPlayState == nullptr) return EncodeEnemyNetId(actor);

    uint32_t netId = EncodeEnemyNetId(actor);
    const uint32_t base = netId & 0xFFFFFF00;  // top 24 bits stay fixed

    // Linear-probe the low 8 bits for an unclaimed posHash slot. Walks
    // every syncable actor category looking for an existing EnemyNetId
    // extension whose netId matches; bumps and retries on collision.
    for (int probe = 0; probe < 256; probe++) {
        bool collides = false;
        for (size_t i = 0; i < kSyncableActorCategoriesCount && !collides; i++) {
            Actor* a = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
            while (a != nullptr) {
                if (a != actor) {
                    const EnemyNetId* ext =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(a);
                    if (ext != nullptr && ext->netId == netId) {
                        collides = true;
                        break;
                    }
                }
                a = a->next;
            }
        }
        if (!collides) return netId;
        // Bump posHash by 1 (mod 256) and retry.
        netId = base | (((netId + 1) & 0xFF));
    }
    // 256 collisions — extreme overflow, return whatever we have.
    return netId;
}
