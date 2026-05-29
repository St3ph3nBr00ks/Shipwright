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
    // Known non-skeletal actor types — return nullptr without probing the
    // memory at offset 0x14C. The generic limbCount heuristic below can
    // false-positive on DynaPolyActor variants: at the SkelAnime offset
    // they store `dyna.bgId` (s32), and when bgId byte 0 happens to be a
    // small positive value (e.g. Bg_Ydan_Hasi observed as 2) the bogus
    // limbCount passes the range check while the bogus `jointTable`
    // points at heap memory past end-of-struct. ENEMY_STATE serialize
    // then dereferences that garbage pointer → access violation.
    // Obj_Oshihiki / Bg_Heavy_Block / En_Ishi happened to bypass the
    // heuristic correctly only because their bgId byte 0 was 0.
    switch (actor->id) {
        case ACTOR_BG_YDAN_HASI:
        case ACTOR_BG_HEAVY_BLOCK:
        case ACTOR_OBJ_OSHIHIKI:
        case ACTOR_EN_ISHI:
        case ACTOR_EN_GOROIWA:
        case ACTOR_OBJ_SYOKUDAI:
        // #193 Phase 4 v2 — env actors with no SkelAnime substruct.
        // En_Kusa / Obj_Mure store an actionFunc pointer at 0x14C
        // (high bits could plausibly bypass the limbCount heuristic);
        // Obj_Bombiwa / Obj_Hamishi store ColliderCylinder.base which
        // could similarly false-positive. Explicit reject.
        case ACTOR_EN_KUSA:
        case ACTOR_OBJ_MURE:
        case ACTOR_OBJ_BOMBIWA:
        case ACTOR_OBJ_HAMISHI:
        // #193 Phase 4 v2 extension — pots / crates. None have
        // SkelAnime; Obj_Tsubo / Obj_Kibako store collider data at
        // 0x14C; Obj_Kibako2 is a DynaPolyActor whose dyna struct
        // overlaps the same range. Same false-positive risk as the
        // grass / rock family above.
        case ACTOR_OBJ_TSUBO:
        case ACTOR_OBJ_KIBAKO:
        case ACTOR_OBJ_KIBAKO2:
        // #193 Phase 4 v3 — six more breakable env actors. None have
        // SkelAnime substruct; all store collider/state data at the
        // SkelAnime offset and would false-positive the limbCount
        // heuristic.
        case ACTOR_BG_HAKA_TUBO:       // Shadow Temple giant skull jar (DynaPoly).
        case ACTOR_BG_JYA_IRONOBJ:     // Spirit Temple Iron Knuckle pillar / throne (DynaPoly).
        case ACTOR_BG_SPOT18_BASKET:   // Goron City prize basket.
        case ACTOR_OBJ_COMB:           // Beehive (Lost Woods, Lake Hylia).
        case ACTOR_EN_TUBO_TRAP:       // Flying pot enemy (Forest Temple).
        case ACTOR_EN_WONDER_ITEM:     // Invisible collectible spot (Hyrule Field secrets).
            return nullptr;
        default: break;
    }

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
        case ACTOR_EN_MD:       return true;  // #184 Mido (Generic NPC State Sync Phase 1, Team scope)
        case ACTOR_BG_YDAN_HASI: return true;  // #185 Inside Deku Tree B1 floating platform (Phase 2, Global scope; HASI_WATER_BLOCK variant is the primary concern, but the actor-id allowlist also covers HASI_WATER + 2F blocks variants — flag-sync handles their state already, world.pos broadcast is additive smoothing).
        // ACTOR_BG_YDAN_MARUTA (rotating spike log + falling ladder) deliberately NOT in this allowlist. The spike log's damage volume is at world.pos which doesn't move; rotation phase is cosmetic. Falling ladder transitions on Flags_SetSwitch which already syncs. Add later if visible drift surfaces in field testing.
        case ACTOR_OBJ_SYOKUDAI: return true;  // Per-torch lit-state sync. Existing puzzle-completion `Flags_SetSwitch` covers "all torches lit"; this admits per-instance `litTimer` so partial puzzle progress (P1 lit 2 of 4) is visible to P2. Global scope.
        case ACTOR_EN_HINTNUTS: return true;  // Compound Room puzzle scrubs.
                                              // State-machine sync is HOST-AUTHORITATIVE
                                              // (room host runs the AI; peers receive
                                              // ENEMY_STATE and apply via ApplyNetState).
                                              // Bidirectional Option A was tried and
                                              // produced state oscillation between
                                              // clients (logs 208 / 213) — peer's local
                                              // AI kept overriding host's authoritative
                                              // broadcasts; reverted to host-only
                                              // broadcaster.
                                              //
                                              // sPuzzleCounter is also synced via
                                              // ENEMY_STATE so the puzzle's branch
                                              // logic resolves identically on both
                                              // clients. Under host-authoritative,
                                              // host is the only writer to its local
                                              // counter; peer's counter is mirror-only.
                                              //
                                              // Projectiles (En_Nutsball) remain
                                              // local-AI / local-spawn — each client
                                              // spawns its own nutsball aimed at its
                                              // local nearest player at frame 6 of
                                              // the synced ThrowNut animation, runs
                                              // reflect physics against its local
                                              // Link's shield. When peer's reflected
                                              // nutsball lands on peer's local
                                              // hintnut, peer fires PROJECTILE_HIT_ENEMY
                                              // instead of running HitByScrubProjectile1+2
                                              // locally. Host receives, applies the
                                              // hit-response on its local actor, and
                                              // broadcasts the resulting state via
                                              // ENEMY_STATE. RTT cost: ~50ms between
                                              // visual nutball impact on peer and
                                              // peer seeing the hintnut transition.

        // ACTOR_EN_NUTSBALL: deliberately NOT admitted. Projectile is
        // intrinsically per-client (reflect physics needs local Link's
        // shield). See logs 195-206 for the full investigation that led
        // to this split: state machine = synced, projectile = local.
        // Same shape applies to other projectile-throwing enemies as
        // they're implemented (Mad Scrub already admitted, but the
        // projectile spawned by EnDekunuts is also EnNutsball — kept
        // out by the same exclusion).

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

        // #193 Phase 4 v2 — env-actor host-authoritative sync. Admits
        // grass shrubs, grass clusters, and bombable rocks so their
        // alive/dead state propagates via ENEMY_DEFEATED. Peer's local
        // cuts are kept (vanilla cosmetic — animation, particle effect,
        // Actor_Kill) but peer suppresses its local Item_DropCollectible
        // call and notifies host via ENV_ACTOR_DROP. Host runs the drop
        // on its side and broadcasts ITEM_DROP_SYNC. Result: one drop
        // per cut event regardless of which client triggered it,
        // eliminating the Phase 4 v1 simultaneous-cut double-drop.
        //
        // Obj_Mure (grass cluster) spawns child En_Kusa actors at runtime
        // — admitting it lets host broadcast its child-spawn pattern so
        // peers see the same cluster layout. Obj_Bombiwa / Obj_Hamishi
        // are bombable rocks; vanilla they have no direct drop calls
        // (the rock itself doesn't drop), but admitting the actor id
        // ensures their alive/dead state syncs so peers' rocks stay in
        // lockstep with host's destruction.
        case ACTOR_EN_KUSA:        return true;  // Single grass shrub.
        case ACTOR_OBJ_MURE:       return true;  // Grass cluster (spawns En_Kusa children).
        case ACTOR_OBJ_BOMBIWA:    return true;  // Bombable rock.
        case ACTOR_OBJ_HAMISHI:    return true;  // Bombable rock variant.

        // #193 Phase 4 v2 extension — breakable container family.
        // Pots and crates have direct `Item_DropCollectible` calls
        // that previously fell through to Phase 4 v1 (host-only
        // broadcast; peer's local cut produced a non-broadcast drop,
        // simultaneous-cut → double-drop on peer). Promoting to v2
        // routes through `Anchor_DropCollectibleEnvActor` for
        // host-authoritative drop attribution.
        case ACTOR_OBJ_TSUBO:      return true;  // Breakable clay pot.
        case ACTOR_OBJ_KIBAKO:     return true;  // Small wooden crate.
        case ACTOR_OBJ_KIBAKO2:    return true;  // Large wooden crate (DynaPoly).

        // #193 Phase 4 v3 — six more breakable env actors with direct
        // `Item_DropCollectible*` calls. Admitting these to the synced
        // allowlist gives their drop calls a netId, which is what
        // `Anchor_DropCollectibleEnvActor` / `*Random*` use to route
        // peer-side cut events to the host instead of producing a
        // local duplicate drop alongside host's broadcast.
        //
        // Two related breakable actors were intentionally NOT admitted
        // and need follow-up design work (filed as separate trackers):
        //   - ACTOR_OBJ_MURE3 (Tower of Rupees) — uses
        //     `Item_DropCollectible2` and captures the spawned
        //     EnItem00* into a per-instance slot array for its own
        //     Flags_SetSwitch tracking; the peer-suppress path would
        //     leave the array NULL and break host's switch-flag firing
        //     when peer is the picker. Needs a separate sync shape.
        //   - ACTOR_OBJ_BEAN (bean stalk fairies) — drops
        //     ITEM00_FLEXIBLE which routes to ACTOR_EN_ELF, not
        //     EN_ITEM00. Fairies are intentionally per-player anyway
        //     (each player's stalk grants their own heal); no sync
        //     needed.
        case ACTOR_BG_HAKA_TUBO:       return true;  // Shadow Temple giant skull jar.
        case ACTOR_BG_JYA_IRONOBJ:     return true;  // Spirit Temple Iron Knuckle pillar/throne.
        case ACTOR_BG_SPOT18_BASKET:   return true;  // Goron City prize basket.
        case ACTOR_OBJ_COMB:           return true;  // Beehive.
        case ACTOR_EN_TUBO_TRAP:       return true;  // Flying pot enemy (Forest Temple).
        case ACTOR_EN_WONDER_ITEM:     return true;  // Invisible collectible spot (Hyrule Field secrets).

        // Carry / held-actor sync (Plans/carry_held_actor_sync.md Step 0).
        // Pickup-and-throw actors that the upcoming carry-sync work needs to
        // look up by netId when attaching to a remote DummyPlayer. Side
        // benefit: admission alone gives peer-visible destroy / respawn sync
        // for small rocks via the existing ENEMY_STATE pipeline.
        case ACTOR_EN_ISHI:            return true;  // Small/large throwable rocks (ACTOR_FLAG_THROW_ONLY).
        case ACTOR_OBJ_MURE2:          return true;  // Rock/bush cluster spawner (spawns En_Ishi children).

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

bool IsSyncedBossExit(int16_t sourceSceneNum, int16_t destEntranceIndex) {
    // Allowlist of (boss-scene, dungeon-clear-entrance) pairs. Checked
    // by the BOSS_EXIT_TEAM_WARP send-side hook to gate which transition
    // edges pull teammates along. Each scene typically has TWO valid
    // exit entrances: the first-time-clear path (carries the post-fight
    // cutscene) and the subsequent-visit path (no cutscene). z_door_warp1
    // chooses between them based on whether the dungeon-clear EVENTCHKINF
    // flag has already been set for the boss.
    switch (sourceSceneNum) {
        case SCENE_DEKU_TREE_BOSS:  // 0x12 — Queen Gohma
            return destEntranceIndex == ENTR_KOKIRI_FOREST_0 ||
                   destEntranceIndex == ENTR_KOKIRI_FOREST_DEKU_TREE_BLUE_WARP;
        // Future bosses — add as IsSyncedBossActor entries land:
        // case SCENE_DODONGOS_CAVERN_BOSS:  // 0x13 — King Dodongo
        //     return destEntranceIndex == ENTR_DEATH_MOUNTAIN_TRAIL_BOTTOM_EXIT ||
        //            destEntranceIndex == ENTR_DEATH_MOUNTAIN_TRAIL_DODONGO_BLUE_WARP;
        // case SCENE_JABU_JABU_BOSS:  // 0x14 — Barinade
        //     return destEntranceIndex == ENTR_ZORAS_FOUNTAIN_JABU_JABU_BLUE_WARP;
        default:
            return false;
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
