#include "Anchor.h"
#include <chrono>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/frame_interpolation.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Bg_Bombwall/z_bg_bombwall.h"
#include "src/overlays/actors/ovl_Bg_Breakwall/z_bg_breakwall.h"
#include "src/overlays/actors/ovl_Bg_Haka_Zou/z_bg_haka_zou.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hamstep/z_bg_hidan_hamstep.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hrock/z_bg_hidan_hrock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Shelter/z_bg_ice_shelter.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombchuiwa/z_bg_jya_bombchuiwa.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombiwa/z_bg_jya_bombiwa.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Bwall/z_bg_mizu_bwall.h"
#include "src/overlays/actors/ovl_Bg_Spot08_Bakudankabe/z_bg_spot08_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot11_Bakudankabe/z_bg_spot11_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot17_Bakudankabe/z_bg_spot17_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Maruta/z_bg_ydan_maruta.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Si/z_en_si.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_Item_B_Heart/z_item_b_heart.h"
#include "src/overlays/actors/ovl_Obj_Bombiwa/z_obj_bombiwa.h"
#include "src/overlays/actors/ovl_Obj_Hamishi/z_obj_hamishi.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Dalm/z_bg_hidan_dalm.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Kowarerukabe/z_bg_hidan_kowarerukabe.h"
#include "objects/gameplay_keep/gameplay_keep.h"
// Enemy struct headers for SkelAnime offset exceptions (see GetEnemySkelAnime below)
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
#include "src/overlays/actors/ovl_En_Karebaba/z_en_karebaba.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "src/overlays/actors/ovl_En_Wf/z_en_wf.h"
#include "src/overlays/actors/ovl_En_Mb/z_en_mb.h"
// Issue #153 — En_Goroiwa is ACTORCAT_PROP, the first non-ENEMY actor synced.
#include "src/overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"

extern PlayState* gPlayState;
extern MapData* gMapData;

void func_8086ED70(BgBombwall* bgBombwall, PlayState* play);
void BgBreakwall_Wait(BgBreakwall* bgBreakwall, PlayState* play);
void func_80883000(BgHakaZou* bgHakaZou, PlayState* play);
void func_808887C4(BgHidanHamstep* bgHidanHamstep, PlayState* play);
void func_808896B8(BgHidanHrock* bgHidanHrock, PlayState* play);
void BgIceShelter_Idle(BgIceShelter* bgIceShelter, PlayState* play);
void BgIceShelter_SetupMelt(BgIceShelter* bgIceShelter);
void ObjBombiwa_Break(ObjBombiwa* objBombiwa, PlayState* play);
void ObjHamishi_Break(ObjHamishi* objHamishi, PlayState* play);
void BgJyaBombchuiwa_WaitForExplosion(BgJyaBombchuiwa* bgJyaBombchuiwa, PlayState* play);
void BgMizuBwall_Idle(BgMizuBwall* bgMizuBwall, PlayState* play);
void func_808B6BC0(BgSpot17Bakudankabe* bgSpot17Bakudankabe, PlayState* play);
void func_808BF078(BgYdanMaruta* bgYdanMaruta, PlayState* play);
void BgYdanSp_FloorWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_WallWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_BurnWeb(BgYdanSp* bgYdanSp, PlayState* play);
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
float OTRGetDimensionFromLeftEdge(float v);
float OTRGetDimensionFromRightEdge(float v);
}

/**
 * Returns the SkelAnime* for an enemy actor, or nullptr if unsupported.
 *
 * Most enemies in OoT place SkelAnime immediately after the base Actor struct.
 * A minority have other fields in between. Known exceptions are handled via
 * explicit struct casts; everything else uses the generic layout with validation.
 *
 * Generic layout (used by ~117 of ~156 animated enemies):
 *   struct GenericEnemy { Actor actor; SkelAnime skelAnime; };
 *
 * Exception pattern (e.g. Redead, Wolfos, Stalfos):
 *   struct { Actor actor; Vec3s bodyPartsPos[N]; SkelAnime skelAnime; };
 *   — handled by casting to the specific enemy struct.
 */
static SkelAnime* GetEnemySkelAnime(Actor* actor) {
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

// Issue #153 — admission predicate for non-ACTORCAT_ENEMY actors that should
// participate in the sync pipeline (ENEMY_UPDATE, ENEMY_DEFEATED, etc.).
//
// The original three gate sites (OnActorSpawn / OnActorUpdate / ShouldActorUpdate)
// hard-checked `category == ACTORCAT_ENEMY`. That excludes hostile/world actors
// in PROP / BG / NPC / SWITCH / MISC, even when they affect cross-client gameplay
// (rolling boulders, scripted-path NPCs, eye-switch traps, etc.).
//
// Adding actor IDs here joins them to the sync pipeline without disturbing the
// existing ACTORCAT_ENEMY-only flow. Per-actor sync logic (payload extension,
// re-apply behaviour) still has to be implemented case by case in the relevant
// hook bodies and packet handlers.
//
// Pending future allowlist entries surfaced in research:
//   ACTOR_EN_PO_DESERT     — Desert Poe / Guide Poe   (#124, ACTORCAT_BG)
//   ACTOR_EN_PO_RELAY      — Dampé's Ghost            (#125, ACTORCAT_NPC)
//   ACTOR_EN_ANUBICE_TAG   — Anubis spawn marker      (#116, ACTORCAT_SWITCH)
static bool IsSyncedWorldActor(int16_t actorId) {
    switch (actorId) {
        case ACTOR_EN_GOROIWA: return true;
        default: return false;
    }
}

// True when the actor should be considered for sync. Called from each filter
// site to keep the gate logic identical everywhere.
static inline bool IsSyncableActor(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || IsSyncedWorldActor(actor->id);
}

// Returns the Actor* of the nearest player-type actor to `enemy`.
// Considers the local player and all live DummyPlayer actors.
// DummyPlayers that are out-of-scene are already at (-9999,-9999,-9999) by
// DummyPlayer_Update, so they are naturally excluded by the distance comparison.
static Actor* FindNearestPlayerActor(Actor* enemy, PlayState* play) {
    Player* localPlayer = GET_PLAYER(play);

    // Seed with the pre-computed squared distance to the local player so we
    // avoid an extra sqrt and stay consistent with the automatic field values.
    float nearestDistSq = SQ(enemy->xzDistToPlayer) + SQ(enemy->yDistToPlayer);
    Actor* nearest = &localPlayer->actor;

    Actor* npc = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            float dx = enemy->world.pos.x - npc->world.pos.x;
            float dy = enemy->world.pos.y - npc->world.pos.y;
            float dz = enemy->world.pos.z - npc->world.pos.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearest = npc;
            }
        }
        npc = npc->next;
    }

    return nearest;
}

// C-callable wrapper so enemy C-code files (e.g. z_en_dekubaba.c) can query the
// nearest player actor without pulling in C++ headers. Returns the nearest
// player-type Actor* (local player or closest DummyPlayer). Safe to call any time
// gPlayState is valid; falls back to local player when Anchor is not active.
extern "C" Actor* Anchor_GetNearestPlayerActor(Actor* enemy, PlayState* play) {
    return FindNearestPlayerActor(enemy, play);
}

// C-callable: returns true when a Karebaba's natural death cycle is running on this
// (non-host) client so that its stick drop should be suppressed (no duplicate item).
// Called from EnKarebaba_DeadItemDrop in z_en_karebaba.c.
extern "C" bool Anchor_ShouldSuppressKarebabaDrop(Actor* actor) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return ext != nullptr && ext->pendingNaturalDeath;
}

// C-callable: non-host tells host that its local Link was just hit by this enemy
// so the host can reverse/update its authoritative copy (En_Goroiwa, issue #153
// Phase 2). No-op when Anchor is disconnected, when this client is the host, or
// when the actor lacks an EnemyNetId extension (never reached the sync pipeline).
extern "C" void Anchor_NotifyEnemyHitPlayer(Actor* actor) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    if (Anchor::Instance->roomState.ownerClientId == Anchor::Instance->ownClientId) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_EnemyHitPlayer(ext->netId);
}

void Anchor::SetFollowerActive(bool active) {
    followerActive = active;
    if (active) {
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        followerStuckFrames = 0;
        followerTargetEnemy = nullptr;
        SPDLOG_INFO("[Follower] Activated (menu)");
    } else {
        SPDLOG_INFO("[Follower] Deactivated (menu)");
    }
}

void Anchor::RegisterHooks() {

    // #region Hooks that are required for basic Anchor functionality

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        SendPacket_UpdateClientState();
        // Request current state from all other clients so we pick up their
        // dayTime if they are in a time-advancing scene and we were not.
        // Responses arrive as UpdateClientState packets and are applied via
        // the forward-only bidirectional time sync in HandlePacket_UpdateClientState.
        if (IsSaveLoaded()) {
            SendPacket_RequestTeamState();
        }

        if (IsSaveLoaded()) {
            // Clear the dead-enemy list for this scene: the scene just (re-)loaded
            // so any previously dead enemies have respawned fresh.
            if (roomState.ownerClientId == ownClientId) {
                deadEnemiesByScene.erase(gPlayState->sceneNum);
            }
            // Clear the per-scene-visit send-dedup set so that enemies in the new
            // scene can have their ENEMY_DEFEATED broadcast normally.
            sentDefeatThisScene.clear();
            // Clear buffered kills that belong to scenes OTHER than the one we are
            // entering.  Kills for the destination scene must survive so that
            // OnActorSpawn can call SetupDeadItemDrop when those actors spawn.
            // (Clearing unconditionally caused Fix 35 to fail when P2 entered the
            // target scene from a different scene — the pending kill was wiped just
            // before the Karebaba spawned, so it appeared alive.)
            {
                uint16_t newScene = gPlayState ? (uint16_t)gPlayState->sceneNum : 0xFFFF;
                for (auto it = pendingKillNetIds.begin(); it != pendingKillNetIds.end(); ) {
                    if ((uint16_t)(*it >> 16) != newScene) {
                        it = pendingKillNetIds.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            RefreshClientActors();
        }
    });

    COND_HOOK(OnPresentFileSelect, isConnected, [&]() { SendPacket_UpdateClientState(); });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;

        if (spawningDummyPlayerForClientId != 0) {
            SetDummyPlayerClientId(actor, spawningDummyPlayerForClientId);

            // By the time we get here, the actor was already added to the ACTORCAT_PLAYER list, so we need to move it
            Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
            actor->id = ACTOR_EN_OE2;
            actor->category = ACTORCAT_NPC;
            actor->init = DummyPlayer_Init;
            actor->update = DummyPlayer_Update;
            actor->draw = DummyPlayer_Draw;
            actor->destroy = DummyPlayer_Destroy;
        }
    });

    COND_HOOK(OnPlayerUpdate, isConnected, [&]() {
        if (justLoadedSave) {
            justLoadedSave = false;
            SendPacket_RequestTeamState();
        }

        if (shouldRefreshActors) {
            shouldRefreshActors = false;
            RefreshClientActors();
        }

        // Diagnostic: log when local player's skelAnime.skeleton pointer changes.
        // Helps identify if something overwrites the skeleton AFTER UpdateCustomSkeletons sets it.
        static void* sLastLocalSkeleton = nullptr;
        if (gPlayState != nullptr) {
            Player* localPlayer = GET_PLAYER(gPlayState);
            if (localPlayer != nullptr) {
                void* curSkel = (void*)localPlayer->skelAnime.skeleton;
                if (curSkel != sLastLocalSkeleton) {
                    SPDLOG_INFO("[CoopModel] LocalPlayer skelAnime.skeleton changed: {} -> {}",
                                sLastLocalSkeleton, curSkel);
                    sLastLocalSkeleton = curSkel;
                }
            }
        }

        SendPacket_PlayerUpdate();
    });

    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        ProcessIncomingPacketQueue();

        // KB-15 / issue #110 — retire counter tick.
        // Every client that retired a BakedPlayerModel this frame or in recent
        // frames has a non-zero retireFrameCounter. Decrement; when it reaches
        // 0, destroy the retiree — by this point every Gfx frame that could
        // have referenced it has been fully consumed by the renderer. See
        // AnchorClient::RetireBakedModel and kRetireFrames in Anchor.h.
        for (auto& [id, client] : clients) {
            if (client.retireFrameCounter > 0) {
                client.retireFrameCounter--;
                if (client.retireFrameCounter == 0) {
                    client.retiredBakedModel = nullptr;
                }
            }
        }

        // Issue #82 — sibling retire-slot tick for local-player baked skeletons.
        // UpdateCustomSkeletonFromFolder moves the outgoing bakedModel into the
        // SkeletonPatchInfo's retire slot on pack switch (same reasoning as the
        // AnchorClient loop above).
        for (auto& skel : SOH::SkeletonPatcher::skeletons) {
            if (skel.retireFrameCounter > 0) {
                skel.retireFrameCounter--;
                if (skel.retireFrameCounter == 0) {
                    skel.retiredBakedModel = nullptr;
                }
            }
        }
    });

    // Follower mode (non-host only): override local player position to trail the host.
    //
    // Activation: toggled via the Anchor settings menu (AI Follower checkbox).
    // Any controller input while active immediately cancels it and returns manual control.
    //
    // Position source: the host's DummyPlayer actor (ACTORCAT_NPC, id=ACTOR_EN_OE2,
    // update=DummyPlayer_Update, clientId==roomState.ownerClientId). Its world.pos is
    // updated every frame by DummyPlayer_Update to the host's authoritative position.
    //
    // Offset: fixed units along the world +X axis from the host. P2's shape.rot.y is
    // also set to match the host so both players face the same direction.
    //
    // Note: COND_HOOK cannot be used here — the lambda body contains brace-initializer
    // lists (e.g. Vec3f sideTarget = { a, b, c }), and the C preprocessor does NOT
    // treat {} as grouping, so their commas split the macro's argument list.
    {
        static HOOK_ID followerHookId = 0;
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnGameFrameUpdate>(followerHookId);
        followerHookId = 0;
        if (isConnected) {
            followerHookId = GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>([&]() {
                // Only run on non-host clients with a save loaded.
                if (roomState.ownerClientId == ownClientId) { return; }
                if (!IsSaveLoaded()) { return; }
                if (gPlayState == nullptr) { return; }

                Player* player = GET_PLAYER(gPlayState);
                if (player == nullptr) { return; }

                // Any real input cancels follower mode.
                // During ATTACK the animation hook injects BTN_B into press.button —
                // exclude it so our own injection doesn't cancel follower mode.
                if (followerActive) {
                    u16 pressed = gPlayState->state.input[0].press.button;
                    u16 deactivateCheck = pressed;
                    if (followerAIState == FollowerAIState::ATTACK) {
                        deactivateCheck &= ~BTN_B;
                    }
                    if (deactivateCheck != 0) {
                        SetFollowerActive(false);
                        SPDLOG_INFO("[Follower] Deactivated (input pressed=0x{:04X})", pressed);
                        return;
                    }
                }

                if (!followerActive) { return; }

                // --- Find the host's DummyPlayer actor ---
                Actor* dummyActor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
                while (dummyActor != nullptr) {
                    if (dummyActor->id == ACTOR_EN_OE2 &&
                        dummyActor->update == (ActorFunc)DummyPlayer_Update &&
                        GetDummyPlayerClientId(dummyActor) == roomState.ownerClientId) {
                        break;
                    }
                    dummyActor = dummyActor->next;
                }
                if (dummyActor == nullptr) { return; } // Host DummyPlayer not found.

                // --- AI follower state machine ---
                // Constants (do not change follow offset — set by prior session).
                static constexpr f32 kFollowOffset       = 50.0f;  // world +X from host
                static constexpr f32 kFollowThreshold    = 100.0f; // dist to switch FOLLOW↔IDLE
                static constexpr f32 kEngageRange        = 350.0f; // enemy detection radius
                static constexpr f32 kAttackRange        = 80.0f;  // melee-contact radius
                static constexpr f32 kMaxLeash           = 800.0f; // abandon ENGAGE if P1 this far
                static constexpr f32 kMoveSpeed          = 4.0f;   // units/frame toward target
                static constexpr int kStuckCheckInterval = 20;     // frames between stuck checks
                static constexpr f32 kStuckMinProgress   = 5.0f;   // min units per check interval
                static constexpr int kStuckRecovery      = 25;     // frames of strafe before retry
                static constexpr int kAttackDuration     = 60;     // frames per ATTACK cycle

                Vec3f hostPos    = dummyActor->world.pos;
                Vec3f sideTarget = { hostPos.x + kFollowOffset, hostPos.y, hostPos.z };

                // Move p2Pos toward 'to' by at most 'speed' units in the XZ plane.
                // Y is not moved — caller sets Y explicitly.  Returns XZ distance before step.
                auto MoveXZ = [](Vec3f& pos, const Vec3f& to, f32 speed) -> f32 {
                    f32 dx   = to.x - pos.x;
                    f32 dz   = to.z - pos.z;
                    f32 dist = sqrtf(dx * dx + dz * dz);
                    if (dist < 0.001f) { return 0.0f; }
                    f32 step = (dist < speed) ? dist : speed;
                    pos.x   += dx / dist * step;
                    pos.z   += dz / dist * step;
                    return dist;
                };

                // Yaw toward (dx, dz).  Math_Atan2S(x, y) with OoT param order.
                auto YawToward = [](f32 dx, f32 dz) -> s16 {
                    return Math_Atan2S(dz, dx); // z first, x second — OoT convention
                };

                // Read P2's current position.  Y is NOT overridden — let OoT's floor
                // detection handle vertical position each frame.  Overriding Y to P1's
                // height fought the physics system on slopes: going downhill put P2 below
                // the surface, physics pushed them back up, and XZ movement was disrupted.
                Vec3f p2Pos = player->actor.world.pos;

                followerStateFrames++;

                // Periodic heartbeat: log state + positions every 60 frames.
                if (followerStateFrames % 60 == 0) {
                    f32 toTarget = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                    const char* stateStr = "?";
                    switch (followerAIState) {
                        case FollowerAIState::IDLE:   stateStr = "IDLE";   break;
                        case FollowerAIState::FOLLOW: stateStr = "FOLLOW"; break;
                        case FollowerAIState::STUCK:  stateStr = "STUCK";  break;
                        case FollowerAIState::ENGAGE: stateStr = "ENGAGE"; break;
                        case FollowerAIState::ATTACK: stateStr = "ATTACK"; break;
                        case FollowerAIState::RETURN: stateStr = "RETURN"; break;
                    }
                    SPDLOG_INFO("[Follower] state={} p2=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) distToTarget={:.0f}",
                                stateStr,
                                p2Pos.x, p2Pos.y, p2Pos.z,
                                sideTarget.x, sideTarget.y, sideTarget.z,
                                toTarget);
                }

                switch (followerAIState) {

                    case FollowerAIState::IDLE: {
                        // Drift back to side-target if P1 moved.
                        f32 dx = sideTarget.x - p2Pos.x;
                        f32 dz = sideTarget.z - p2Pos.z;
                        if (dx * dx + dz * dz > kFollowThreshold * kFollowThreshold) {
                            followerAIState     = FollowerAIState::FOLLOW;
                            followerStateFrames = 0;
                            followerLastPos     = p2Pos;
                            SPDLOG_INFO("[Follower] IDLE→FOLLOW p2=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f}) dist={:.0f}",
                                        p2Pos.x, p2Pos.y, p2Pos.z,
                                        sideTarget.x, sideTarget.y, sideTarget.z,
                                        sqrtf(dx * dx + dz * dz));
                            break;
                        }
                        // Scan for the nearest live enemy within ENGAGE range.
                        Actor* nearest    = nullptr;
                        f32    nearDistSq = kEngageRange * kEngageRange;
                        Actor* eActor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
                        while (eActor != nullptr) {
                            if (eActor->update != nullptr) {
                                f32 edx     = eActor->world.pos.x - p2Pos.x;
                                f32 edz     = eActor->world.pos.z - p2Pos.z;
                                f32 eDistSq = edx * edx + edz * edz;
                                if (eDistSq < nearDistSq) {
                                    nearDistSq = eDistSq;
                                    nearest    = eActor;
                                }
                            }
                            eActor = eActor->next;
                        }
                        if (nearest != nullptr) {
                            followerTargetEnemy = nearest;
                            followerAIState     = FollowerAIState::ENGAGE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] IDLE→ENGAGE enemy id={} at ({:.0f},{:.0f},{:.0f}) dist={:.0f}",
                                        nearest->id,
                                        nearest->world.pos.x, nearest->world.pos.y, nearest->world.pos.z,
                                        sqrtf(nearDistSq));
                        }
                        // In IDLE, match P1's facing direction.
                        player->actor.shape.rot.y = dummyActor->shape.rot.y;
                        // Pre-populate move target so the first FOLLOW frame's
                        // ShouldActorUpdate sees the correct direction immediately.
                        followerMoveTarget = sideTarget;
                        break;
                    }

                    case FollowerAIState::FOLLOW: {
                        // Stuck detection: every kStuckCheckInterval frames check progress.
                        if (followerStateFrames % kStuckCheckInterval == 0) {
                            f32 progDx   = p2Pos.x - followerLastPos.x;
                            f32 progDz   = p2Pos.z - followerLastPos.z;
                            f32 progress = sqrtf(progDx * progDx + progDz * progDz);
                            f32 toTarget = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                            SPDLOG_INFO("[Follower] FOLLOW check: progress={:.1f} distToTarget={:.0f} "
                                        "p2=({:.0f},{:.0f}) last=({:.0f},{:.0f}) target=({:.0f},{:.0f})",
                                        progress, toTarget,
                                        p2Pos.x, p2Pos.z,
                                        followerLastPos.x, followerLastPos.z,
                                        sideTarget.x, sideTarget.z);
                            followerLastPos = p2Pos; // update checkpoint
                            if (progress < kStuckMinProgress) {
                                // Compute strafe direction perpendicular to travel.
                                f32 tdx = sideTarget.x - p2Pos.x;
                                f32 tdz = sideTarget.z - p2Pos.z;
                                f32 len = sqrtf(tdx * tdx + tdz * tdz);
                                if (len > 0.001f) {
                                    followerStuckDir = { -tdz / len, 0.0f, tdx / len };
                                } else {
                                    followerStuckDir = { 1.0f, 0.0f, 0.0f };
                                }
                                followerAIState     = FollowerAIState::STUCK;
                                followerStuckFrames = 0;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] FOLLOW→STUCK strafeDir=({:.2f},{:.2f})",
                                            followerStuckDir.x, followerStuckDir.z);
                                break;
                            }
                        }
                        followerMoveTarget = sideTarget;
                        f32 dist = MoveXZ(p2Pos, sideTarget, kMoveSpeed);
                        if (dist > 0.001f) {
                            player->actor.shape.rot.y = YawToward(
                                sideTarget.x - player->actor.world.pos.x,
                                sideTarget.z - player->actor.world.pos.z);
                        }
                        if (dist < kFollowThreshold) {
                            followerAIState     = FollowerAIState::IDLE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] FOLLOW→IDLE dist={:.1f}", dist);
                        }
                        break;
                    }

                    case FollowerAIState::STUCK: {
                        followerStuckFrames++;
                        p2Pos.x += followerStuckDir.x * kMoveSpeed;
                        p2Pos.z += followerStuckDir.z * kMoveSpeed;
                        if (followerStuckFrames >= kStuckRecovery) {
                            followerAIState     = FollowerAIState::FOLLOW;
                            followerLastPos     = p2Pos;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] STUCK→FOLLOW p2=({:.0f},{:.0f})",
                                        p2Pos.x, p2Pos.z);
                        }
                        break;
                    }

                    case FollowerAIState::ENGAGE: {
                        // Abandon if P1 is too far or target is gone.
                        {
                            f32 ldx = hostPos.x - p2Pos.x;
                            f32 ldz = hostPos.z - p2Pos.z;
                            if (ldx * ldx + ldz * ldz > kMaxLeash * kMaxLeash) {
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RETURN (P1 too far)");
                                break;
                            }
                        }
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ENGAGE→RETURN (enemy gone)");
                            break;
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        f32   edx      = enemyPos.x - p2Pos.x;
                        f32   edz      = enemyPos.z - p2Pos.z;
                        f32   distSq   = edx * edx + edz * edz;
                        if (distSq < kAttackRange * kAttackRange) {
                            followerAIState     = FollowerAIState::ATTACK;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ENGAGE→ATTACK enemy=({:.0f},{:.0f},{:.0f}) dist={:.0f}",
                                        enemyPos.x, enemyPos.y, enemyPos.z, sqrtf(distSq));
                            break;
                        }
                        // Every 20 frames log distance to enemy so we can see approach progress.
                        if (followerStateFrames % 20 == 0) {
                            SPDLOG_INFO("[Follower] ENGAGE progress: distToEnemy={:.0f} p2=({:.0f},{:.0f})",
                                        sqrtf(distSq), p2Pos.x, p2Pos.z);
                        }
                        followerMoveTarget = enemyPos;
                        MoveXZ(p2Pos, enemyPos, kMoveSpeed);
                        if (distSq > 1.0f) {
                            player->actor.shape.rot.y = YawToward(edx, edz);
                        }
                        break;
                    }

                    case FollowerAIState::ATTACK: {
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy gone)");
                            break;
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        // Charge/retreat cycle: 10 frames toward enemy, 10 frames back.
                        // Note: this moves P2 into the enemy's hitbox area but does NOT
                        // trigger a sword swing — P2 slides without a swing animation.
                        // Injecting BTN_B before the player update is needed to trigger
                        // actual melee — deferred as future work.
                        bool chargePhase = ((followerStateFrames / 10) % 2) == 0;
                        {
                            f32 edx      = enemyPos.x - p2Pos.x;
                            f32 edz      = enemyPos.z - p2Pos.z;
                            f32 enemyDist = sqrtf(edx * edx + edz * edz);
                            if (followerStateFrames % 10 == 0) {
                                SPDLOG_INFO("[Follower] ATTACK frame={} phase={} distToEnemy={:.0f} p2=({:.0f},{:.0f})",
                                            followerStateFrames,
                                            chargePhase ? "CHARGE" : "RETREAT",
                                            enemyDist, p2Pos.x, p2Pos.z);
                            }
                            MoveXZ(p2Pos, chargePhase ? enemyPos : sideTarget, kMoveSpeed);
                            if (edx * edx + edz * edz > 1.0f) {
                                player->actor.shape.rot.y = YawToward(edx, edz);
                            }
                        }
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (cycle complete)");
                        }
                        break;
                    }

                    case FollowerAIState::RETURN: {
                        followerMoveTarget = sideTarget;
                        f32 dist = MoveXZ(p2Pos, sideTarget, kMoveSpeed);
                        if (dist > 0.001f) {
                            player->actor.shape.rot.y = YawToward(
                                sideTarget.x - player->actor.world.pos.x,
                                sideTarget.z - player->actor.world.pos.z);
                        }
                        if (dist < kFollowThreshold) {
                            followerAIState     = FollowerAIState::IDLE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RETURN→IDLE dist={:.1f}", dist);
                        }
                        break;
                    }
                }

                player->actor.world.pos   = p2Pos;
                player->actor.prevPos     = p2Pos;
            });
        }
    }

    // Follower animation injection (non-host only).
    //
    // Fires via ShouldActorUpdate immediately BEFORE the player actor's update()
    // so the player's own action state machine sees synthetic input and plays the
    // correct animations.  (OnGameFrameUpdate fires too late — after update().)
    //
    // Walk/run: inject stick_y=80 (camera-forward) when the follower is moving.
    //   The player enters walk/run action → correct leg animation plays.
    //   Actual position is still controlled by our world.pos override in the
    //   OnGameFrameUpdate hook above, so stick direction does not affect where
    //   P2 ends up.
    //
    // Attack: inject BTN_B as an edge-press at the start of each charge phase.
    //   The player's action state machine processes the press → real sword swing
    //   animation plays.  shape.rot.y (set toward the enemy in the state machine
    //   above) ensures P2 faces the enemy during the swing.
    //
    // Timing note: ShouldActorUpdate sees followerStateFrames from the PREVIOUS
    // OnGameFrameUpdate (one frame before the next increment).  BTN_B is injected
    // when followerStateFrames % 20 == 0, which corresponds to frame 1, 21, 41
    // inside the ATTACK state after the next increment — the first frame of each
    // charge phase.  The sword swing takes ~20 frames, matching the cycle period.
    {
        static HOOK_ID followerAnimHookId = 0;
        GameInteractor::Instance->UnregisterGameHook<GameInteractor::ShouldActorUpdate>(followerAnimHookId);
        followerAnimHookId = 0;
        if (isConnected) {
            followerAnimHookId = GameInteractor::Instance->RegisterGameHook<GameInteractor::ShouldActorUpdate>(
                [&](void* refActor, bool* should) {
                    (void)should; // we never block; only inject input
                    if (!followerActive)        { return; }
                    if (gPlayState == nullptr)  { return; }
                    Actor* actor = static_cast<Actor*>(refActor);
                    if (actor->id != ACTOR_PLAYER) { return; }

                    Input& input = gPlayState->state.input[0];
                    bool isMoving = (followerAIState == FollowerAIState::FOLLOW ||
                                     followerAIState == FollowerAIState::ENGAGE ||
                                     // ATTACK excluded: OoT determines swing direction from stick,
                                     // overriding shape.rot.y. No stick → Link attacks in the
                                     // direction shape.rot.y points (set toward enemy each frame).
                                     followerAIState == FollowerAIState::RETURN);

                    // --- Joystick cancel ---
                    // Read hardware values BEFORE we inject anything. OoT resets input.cur
                    // from hardware at the start of each frame, so these are the real values.
                    {
                        s8 hwX = input.cur.stick_x;
                        s8 hwY = input.cur.stick_y;
                        if ((s32)hwX * hwX + (s32)hwY * hwY > 25 * 25) {
                            SetFollowerActive(false);
                            SPDLOG_INFO("[Follower] Deactivated (joystick hw=({}, {}))", hwX, hwY);
                            return;
                        }
                    }

                    // --- Walk/run animation: camera-relative stick toward followerMoveTarget ---
                    // OoT's movement pipeline: worldYaw = Camera_GetInputDirYaw(cam) + stickAngle,
                    // where stickAngle = Math_Atan2S(relY, -relX).  To move in world direction
                    // (dx, dz), invert that pipeline:
                    //   worldYaw    = Math_Atan2S(dz, dx)          [OoT convention: z first]
                    //   stickAngle  = worldYaw - inputDirYaw
                    //   relY        = Math_CosS(stickAngle) * 60
                    //   relX        = -Math_SinS(stickAngle) * 60
                    // This is exact — no floating-point projection or sign guessing required.
                    static bool sAnimHookLogged = false;
                    if (!sAnimHookLogged) {
                        SPDLOG_INFO("[Follower] animHook firing for ACTOR_PLAYER");
                        sAnimHookLogged = true;
                    }
                    if (isMoving) {
                        Vec3f p2w = actor->world.pos;
                        f32 dx = followerMoveTarget.x - p2w.x;
                        f32 dz = followerMoveTarget.z - p2w.z;
                        if (dx * dx + dz * dz > 1.0f) {
                            Camera* cam = GET_ACTIVE_CAM(gPlayState);
                            s16 inputDirYaw  = Camera_GetInputDirYaw(cam);
                            s16 worldYaw     = Math_Atan2S(dz, dx); // z first per OoT convention
                            s16 stickAngle   = worldYaw - inputDirYaw;
                            s8  stickY = (s8)(Math_CosS(stickAngle) * 60.0f);
                            s8  stickX = (s8)(-Math_SinS(stickAngle) * 60.0f);
                            input.cur.stick_x = stickX;
                            input.cur.stick_y = stickY;
                            input.rel.stick_x = stickX;
                            input.rel.stick_y = stickY;
                        } else {
                            // Already at target — no stick
                            input.cur.stick_x = 0; input.cur.stick_y = 0;
                            input.rel.stick_x = 0; input.rel.stick_y = 0;
                        }
                    } else {
                        input.cur.stick_x = 0; input.cur.stick_y = 0;
                        input.rel.stick_x = 0; input.rel.stick_y = 0;
                    }

                    // --- Attack: face enemy + inject BTN_B at start of each charge phase ---
                    if (followerAIState == FollowerAIState::ATTACK) {
                        // Keep shape.rot.y facing the enemy here (BEFORE Player_Update) so
                        // that when BTN_B is processed by OoT this frame, the swing direction
                        // is current.  OnGameFrameUpdate also sets it (after Player_Update) to
                        // maintain facing during the animation; both assignments are consistent.
                        if (followerTargetEnemy != nullptr &&
                            followerTargetEnemy->update != nullptr) {
                            f32 ex = followerTargetEnemy->world.pos.x - actor->world.pos.x;
                            f32 ez = followerTargetEnemy->world.pos.z - actor->world.pos.z;
                            if (ex * ex + ez * ez > 1.0f) {
                                actor->shape.rot.y = Math_Atan2S(ez, ex); // z first per OoT convention
                            }
                        }
                        if (followerStateFrames % 20 == 0) {
                            input.press.button |= BTN_B;
                            input.cur.button   |= BTN_B;
                            SPDLOG_INFO("[Follower] ATTACK injecting BTN_B (stateFrames={})",
                                        followerStateFrames);
                        }
                    }
                });
        }
    }

    // #region Enemy sync hooks (Phase 1 — visibility)

    // Assign a deterministic netId to every enemy actor on spawn so both clients
    // can refer to the same enemy without any handshake. Also store the SkelAnime
    // pointer so the send/receive path can sync joint tables without re-deriving
    // the offset every frame.
    //
    // Dynamic spawn detection: gPlayState->numSetupActors > 0 while the engine is
    // iterating the scene's setup actor list (z_actor.c Actor_UpdateAll). It is
    // zeroed immediately after that loop completes, before OnSceneSpawnActors fires.
    // Any OnActorSpawn with numSetupActors == 0 is therefore a runtime (dynamic)
    // spawn (e.g. Stalchild from En_Encount1, Peahat Larva). This check is reliable
    // across scene transitions — unlike the old sceneActorsLoaded flag which stayed
    // true from the previous scene and incorrectly suppressed static actors on P2.
    //
    // Host broadcasts a dynamic spawn via ENEMY_SPAWN; non-host kills the locally-
    // spawned actor and waits to receive the host's canonical copy. Actors spawned
    // in response to HandlePacket_EnemySpawn are exempt (isSpawningNetworkActor).
    COND_HOOK(OnActorSpawn, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            // Log any actor type that might be expected to sync but has wrong category.
            // This catches e.g. Deku Baba spawning with an unexpected initial category.
            if (actor->id == ACTOR_EN_DEKUBABA) {
                SPDLOG_WARN("[EnemySpawn] OnActorSpawn: ACTOR_EN_DEKUBABA skipped — cat={} (expected ACTORCAT_ENEMY={})",
                            (int)actor->category, (int)ACTORCAT_ENEMY);
            }
            return;
        }
        if (!IsSaveLoaded()) {
            return;
        }

        bool isDynamicSpawn = (gPlayState->numSetupActors == 0);
        if (isDynamicSpawn) {
            if (roomState.ownerClientId == ownClientId) {
                // Host: broadcast so non-host clients can spawn a matching actor.
                // SendPacket_EnemySpawn runs after the netId block below so the
                // actor already has a valid extension when the send path reads it.
                // We defer the actual send to after netId assignment — see below.
            } else if (!isSpawningNetworkActor) {
                // Non-host: kill locally-generated dynamic actors immediately.
                // The host's canonical copy arrives via ENEMY_SPAWN and is spawned
                // by HandlePacket_EnemySpawn (which sets isSpawningNetworkActor).
                SPDLOG_INFO("[EnemySpawn] Suppressing dynamic spawn actorId={} on non-host", actor->id);
                Actor_Kill(actor);
                return;
            }
        }

        // Deterministic netId: scene | actorId | hash(home.pos, room).
        // Both clients spawn the same set of actors in the same scene so the
        // same home.pos/room combination always identifies the same enemy,
        // eliminating the spawnCounter divergence seen with dynamic spawns.
        uint8_t posHash = (uint8_t)((int16_t)actor->home.pos.x) ^
                          (uint8_t)((int16_t)actor->home.pos.z >> 1) ^
                          (uint8_t)actor->room;
        uint32_t netId = ((uint32_t)(uint16_t)gPlayState->sceneNum << 16) |
                         ((uint32_t)(uint16_t)actor->id << 8) |
                         posHash;

        EnemyNetId ext;
        ext.netId = netId;
        ext.skelAnime = GetEnemySkelAnime(actor);
        ext.limbCount = ext.skelAnime ? ext.skelAnime->limbCount : 0;
        ObjectExtension::GetInstance().Set<EnemyNetId>(actor, std::move(ext));

        SPDLOG_INFO("[EnemySpawn] Extension assigned: actorId={} netId={} ptr={} home=({:.0f},{:.0f},{:.0f}) posHash=0x{:02X} limbCount={} {}",
                    actor->id, netId, (void*)actor,
                    actor->home.pos.x, actor->home.pos.y, actor->home.pos.z,
                    (int)posHash, (int)ext.limbCount,
                    isDynamicSpawn ? "dynamic" : "static");

        // If an ENEMY_DEFEATED for this netId arrived before the scene finished
        // loading (race between scene load and packet delivery), kill it now.
        // Karebaba: use the natural death cycle so it can respawn later, same as
        // HandlePacket_EnemyDefeated. Other enemies: direct Actor_Kill is fine.
        if (pendingKillNetIds.count(netId)) {
            // Karebaba: do NOT erase from pendingKillNetIds yet (Fix 35).
            // The actor moves to ACTORCAT_MISC for ~420 frames at 20fps. If the
            // player exits and re-enters the room during that window, OoT destroys
            // the ACTORCAT_MISC actor (room unload) and spawns a fresh one on
            // re-entry. Without the netId still in pendingKillNetIds, the fresh
            // actor starts alive. The erase is deferred to the non-host respawn
            // detection in OnActorUpdate, which fires when the actor returns to
            // ACTORCAT_ENEMY in a living state after completing the full cycle.
            // Non-Karebaba enemies are killed instantly so their erase is immediate.
            if (actor->id == ACTOR_EN_KAREBABA) {
                SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} (Karebaba) ptr={} — deferring dead state to OnActorInit (Fix 38)",
                            netId, (void*)actor);
                EnemyNetId* extPtr = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
                if (extPtr != nullptr) {
                    extPtr->hasLocalDeath        = true;
                    extPtr->pendingNaturalDeath  = true;
                    extPtr->defeatPacketSent     = true;
                    // Fix 38: defer SetupDeadItemDrop to OnActorInit (non-host only).
                    // OnActorSpawn fires BEFORE actor->init() is called by Actor_UpdateAll
                    // (z_actor.c:3409 vs 2638). Calling SetupDeadItemDrop here causes
                    // EnKarebaba_Init (Frame 1) to override actionFunc=DeadItemDrop back
                    // to actionFunc=Idle. The next update() (Frame 2) then runs
                    // EnKarebaba_Idle, detects the player, and calls SetupAwaken — making
                    // the Karebaba appear alive for one frame on P2.
                    // OnActorInit (z_actor.c:2641) fires AFTER actor->init() has run,
                    // so SetupDeadItemDrop can override actionFunc without being undone.
                    // CL-01: host manages its own Karebaba state naturally via respawn
                    // detection — deferredDeadItemDrop is a non-host-only mechanism and
                    // OnActorInit returns early for the host anyway.
                    if (roomState.ownerClientId != ownClientId) {
                        extPtr->deferredDeadItemDrop = true;
                    }
                }
            } else {
                SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} — killing actor immediately", netId);
                pendingKillNetIds.erase(netId); // instant kill — safe to release now
                isKillingNetworkActor = true;
                Actor_Kill(actor);
                isKillingNetworkActor = false;
            }
            return;
        }

        // Host deferred broadcast: send ENEMY_SPAWN for dynamic actors now that
        // the netId extension is in place.
        if (isDynamicSpawn && roomState.ownerClientId == ownClientId) {
            SendPacket_EnemySpawn(actor);
        }
    });

    // Fix 38 — apply deferred dead state after EnKarebaba_Init has run.
    // OnActorSpawn fires before actor->init() (z_actor.c:3409 vs 2638). Setting
    // actionFunc=DeadItemDrop there is immediately overridden by EnKarebaba_Init in
    // Frame 1, which resets actionFunc=Idle. The next update() then calls SetupAwaken.
    // OnActorInit fires at z_actor.c:2641, after actor->init() and before the first
    // update(), so we can safely override here without being overwritten.
    COND_ID_HOOK(OnActorInit, ACTOR_EN_KAREBABA, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Only non-host needs this — host never sets deferredDeadItemDrop.
        if (roomState.ownerClientId == ownClientId) {
            return;
        }
        if (!IsSaveLoaded() || gPlayState == nullptr) {
            return;
        }
        EnemyNetId* ext = const_cast<EnemyNetId*>(
            ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
        if (ext == nullptr || !ext->deferredDeadItemDrop) {
            return;
        }
        ext->deferredDeadItemDrop = false;
        SPDLOG_INFO("[EnemySpawn] Pending kill for netId={} (Karebaba) ptr={} — SetupDeadItemDrop after init (Fix 38)",
                    ext->netId, (void*)actor);
        // Set the same flags EnKarebaba_SetupDying sets (natural precursor to
        // DeadItemDrop). EnKarebaba_Init (via Actor_ProcessInitChain) resets flags
        // to standard enemy flags; we must re-apply the death flags now that init
        // has run. SetupDeadItemDrop itself clears DRAW_CULLING_DISABLED.
        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
        actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
        EnKarebaba_SetupDeadItemDrop((EnKarebaba*)actor, gPlayState);
        SPDLOG_INFO("[EnemySpawn] After SetupDeadItemDrop: ptr={} category={}",
                    (void*)actor, (int)actor->category);
    });

    // Host sends enemy positions every frame to all clients in the same scene.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            return;
        }
        if (!IsSaveLoaded()) {
            return;
        }

        if (roomState.ownerClientId == ownClientId) {
            // Host: send current state to all clients in scene.
            const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            if (ext == nullptr) {
                // Log once per actor pointer to avoid per-frame spam.
                static std::unordered_set<const Actor*> warnedNoExt;
                if (warnedNoExt.insert(actor).second) {
                    SPDLOG_WARN("[EnemyUpdate] Host OnActorUpdate: no extension for actorId={} cat={} — skipping update",
                                actor->id, (int)actor->category);
                }
                return;
            }
            // Karebaba respawn detection (host path, Fix 24):
            // When a Karebaba returns to Idle (stateIndex=1) after a kill cycle,
            // reset all death tracking so the next kill can be broadcast correctly.
            //
            // Two cases:
            //   defeatPacketSent=true  — host killed it locally; sentDefeatThisScene and
            //                            deadEnemiesByScene were written at kill time.
            //   pendingNaturalDeath=true — host received ENEMY_DEFEATED from a non-host
            //                             client and ran the natural death cycle itself;
            //                             HandlePacket also wrote deadEnemiesByScene.
            //                             Without this branch, pendingNaturalDeath stays
            //                             set forever and subsequent kills from non-host
            //                             are silently ignored as "already dying".
            if (actor->id == ACTOR_EN_KAREBABA &&
                (ext->defeatPacketSent || ext->pendingNaturalDeath)) {
                EnemyNetId* extMut = const_cast<EnemyNetId*>(ext);
                s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                // Detect respawn-complete: any living state (>= 0, not a death/regrow state).
                // Death states: 5=Dying, 6=DeadItemDrop, 8=Dead, 9=Regrow.
                // We cannot rely on curState==1 (Idle) because when a player is
                // nearby, the Idle update immediately calls SetupAwaken in the same
                // frame — our OnActorUpdate hook fires AFTER update(), so by the time
                // we check, the state is already Awaken (2), never Idle.
                bool isDeathState = (curState == 5 || curState == 6 || curState == 8 || curState == 9);
                // Grow (state=0) is the initial spawn state when OoT re-creates the
                // actor after Actor_Kill. A freshly-spawned actor in Grow that inherits
                // a stale extension (pendingNaturalDeath=true) must NOT be treated as
                // a post-death respawn — the actor hasn't completed its cycle yet and
                // deadEnemiesByScene should remain set so late joiners get the replay.
                bool isGrowState  = (curState == 0);
                if (curState >= 0 && !isDeathState && !isGrowState) {
                    // Notify non-hosts to skip their remaining countdown and jump to
                    // Regrow. Send before clearing flags so the receive-side guard
                    // (pendingNaturalDeath check) still holds when the packet arrives.
                    SendPacket_EnemyRespawn(extMut->netId);
                    extMut->defeatPacketSent    = false;
                    extMut->hasLocalDeath       = false;
                    extMut->pendingNaturalDeath = false;
                    sentDefeatThisScene.erase(extMut->netId);
                    deadEnemiesByScene[gPlayState->sceneNum].erase(extMut->netId);
                    SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} respawned (state={}) (host) — defeat tracking cleared",
                                extMut->netId, curState);
                }
            }
            SendPacket_EnemyUpdate(ext->netId, actor);
        } else {
            // Non-host: re-apply last received network state after AI update ran.
            // This keeps the enemy at the host-authoritative position/health while
            // still allowing the update() to register collision shapes every frame.
            EnemyNetId* ext = const_cast<EnemyNetId*>(ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
            if (ext == nullptr) {
                return;
            }

            // Detect sword hits by the local (non-host) player this frame and forward
            // them to the host for authoritative damage application.
            //
            // Timing: CollisionCheck_Damage ran BEFORE actor->update() this frame and
            // populated colChkInfo.damage. actor->update() called Actor_ApplyDamage
            // (reducing health locally) but did NOT clear colChkInfo.damage —
            // CollisionCheck_ResetDamage runs AFTER this hook. So colChkInfo.damage
            // still holds the total damage dealt this frame.
            //
            // Guard: skip on local kill (hasLocalDeath). On the killing blow,
            // actor->update() fires OnEnemyDefeat before OnActorUpdate runs, which
            // sets hasLocalDeath = true. ENEMY_DEFEATED already handles the kill;
            // sending DAMAGE_ENEMY for the final hit would be redundant.
            if (!ext->hasLocalDeath && actor->colChkInfo.damage > 0) {
                SendPacket_DamageEnemy(ext->netId, (u8)actor->colChkInfo.damage);
            }

            // Karebaba respawn detection (non-host path, Fix 24 + Fix 30c):
            // Must run BEFORE the hasNetState gate. When a Karebaba is killed via
            // pendingKillNetIds at scene load (host's actor is in ACTORCAT_MISC so
            // no ENEMY_UPDATE arrives), hasNetState stays false forever. The old
            // placement inside the hasNetState gate meant this detection never fired,
            // leaving pendingNaturalDeath=true permanently and blocking all future kills.
            //
            // Also guards against Grow (state=0): when OoT re-creates the actor after
            // Actor_Kill, the new instance may inherit a stale extension with
            // pendingNaturalDeath=true. Grow is the initial spawn state — it is NOT
            // a completed respawn. Skip until the actor reaches a non-death, non-Grow
            // state (Idle=1 or higher living state).
            if (actor->id == ACTOR_EN_KAREBABA &&
                (ext->pendingNaturalDeath || ext->defeatPacketSent)) {
                s16 curState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                bool isDeathState = (curState == 5 || curState == 6 || curState == 8 || curState == 9);
                bool isGrowState  = (curState == 0);
                if (curState >= 0 && !isDeathState && !isGrowState) {
                    // Fix 33: non-host was the killer (not the receiver of a host kill).
                    // pendingNaturalDeath=false means we killed it locally;
                    // defeatPacketSent=true means we sent ENEMY_DEFEATED (not a dedup skip).
                    // Notify the host to skip its remaining countdown — symmetric to how
                    // the host sends ENEMY_RESPAWN to us after a host-side kill (Fix 32).
                    if (!ext->pendingNaturalDeath && ext->defeatPacketSent) {
                        SendPacket_EnemyRespawn(ext->netId);
                    }

                    // Fix 36 — stacked kill: a second ENEMY_DEFEATED arrived while the
                    // actor was already mid-cycle. Instead of restoring to live state,
                    // immediately re-trigger the death cycle so the stacked kill is
                    // honoured. The actor stays in pendingKillNetIds for room re-entry.
                    bool doStalledKill = ext->stalledKillPending;

                    ext->pendingNaturalDeath  = false;
                    ext->hasLocalDeath        = false;
                    ext->defeatPacketSent     = false;
                    ext->stalledKillPending   = false;
                    ext->netStateIndex        = -1;
                    // Clear hasNetState (Fix 25): prevents stale scale/rot from the
                    // host's last packet being re-applied during the first few frames
                    // after respawn (caused "missing heads" visual). The actor runs
                    // free AI until the next ENEMY_UPDATE from the host arrives and
                    // sets hasNetState=true again; if the host is absent it stays
                    // false and the actor runs free AI permanently — correct behavior.
                    ext->hasNetState = false;

                    sentDefeatThisScene.erase(ext->netId);

                    if (doStalledKill) {
                        // Re-trigger the death cycle immediately for the stacked kill.
                        SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} stalled kill — re-triggering death cycle (non-host)",
                                    ext->netId);
                        ext->hasLocalDeath       = true;
                        ext->pendingNaturalDeath = true;
                        ext->defeatPacketSent    = true;
                        actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
                        actor->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
                        EnKarebaba_SetupDeadItemDrop((EnKarebaba*)actor, gPlayState);
                        // Keep in pendingKillNetIds for room re-entry persistence
                    } else {
                        // Release the deferred pendingKillNetIds entry (Fix 35).
                        pendingKillNetIds.erase(ext->netId);
                        SPDLOG_INFO("[EnemyDefeated] Karebaba netId={} respawned (state={}) (non-host) — sync re-enabled",
                                    ext->netId, curState);
                    }
                }
            }

            if (!ext->hasNetState) {
                return;
            }
            // Re-derive skelAnime if it was null or empty at spawn time.
            // Dormant Deku Babas (and similar actors) may not have a valid
            // SkelAnime until after their first activation (grow/wake animation).
            // Checking each frame is cheap — GetEnemySkelAnime just reads a
            // struct field and validates limbCount/jointTable.
            if (ext->skelAnime == nullptr || ext->limbCount == 0) {
                SkelAnime* ska = GetEnemySkelAnime(actor);
                if (ska != nullptr && ska->limbCount > 0 && ska->jointTable != nullptr) {
                    ext->skelAnime = ska;
                    ext->limbCount = ska->limbCount;
                }
            }
            // Skip world.pos/rot/shape.rot re-apply when the actor is in a local death
            // animation (hasLocalDeath=true). The death code drives these fields each
            // frame (e.g. BounceAround modifies world.rot for Gold Skulltula).
            // Overwriting with stale cached host values corrupts the animation.
            // Scale and health have their own hasLocalDeath guards further below.
            if (!ext->hasLocalDeath) {
                // Both En_Dekubaba and En_Karebaba compute world.pos each frame from
                // animated angles rather than using a stable model root:
                //   En_Dekubaba: head-tip position derived from home.pos + stemSectionAngles
                //   En_Karebaba: in Spin state, world.pos = f(home.pos, shape.rot) trig
                // Overriding world.pos OR shape.rot causes the stem base to drift/wobble.
                // Both are skipped here and in HandlePacket_EnemyUpdate; the state machine
                // and jointTable sync keep each actor visually correct without overrides.
                if (actor->id != ACTOR_EN_DEKUBABA && actor->id != ACTOR_EN_KAREBABA) {
                    actor->world.pos = ext->netPos;
                    actor->shape.rot = ext->netShapeRot;
                }
                actor->world.rot = ext->netRot;
            }
            // Skip health re-apply after a local kill so the host's stale health > 0
            // packets don't revive the dying actor on this client (hasLocalDeath guard).
            // Multi-hit guard: only re-apply if local health hasn't been reduced below the
            // network value; otherwise we'd undo locally-dealt damage on multi-hit enemies.
            if (!ext->hasLocalDeath && actor->colChkInfo.health >= ext->netHealth) {
                actor->colChkInfo.health = ext->netHealth;
            }
            // Karebaba: pre-compute local state and active/dormant flags here so they
            // can guard BOTH the scale re-apply below and the state-machine sync.
            // Computed even when netStateIndex < 0 or hasLocalDeath so the scale guard
            // variable has a defined value; the sync block is skipped in those cases.
            bool karebabaDormantOverride = false;
            s16  karebabaLocalState      = -1;
            if (actor->id == ACTOR_EN_KAREBABA && ext->netStateIndex >= 0 && !ext->hasLocalDeath) {
                karebabaLocalState = EnKarebaba_GetStateIndex((EnKarebaba*)actor);
                bool kNetDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1 ||
                                     ext->netStateIndex == 2 || ext->netStateIndex == 9);
                bool kLocalActive = (karebabaLocalState == 2 || karebabaLocalState == 3 ||
                                     karebabaLocalState == 4 || karebabaLocalState == 7);
                karebabaDormantOverride = kNetDormant && kLocalActive;
            }

            // Scale sync: enemies like En_Karebaba change actor->scale throughout their
            // state machine (0 when dormant, growing to 0.01 when fully emerged). Without
            // this re-apply the non-host always sees the actor at its spawn-time scale.
            // Guards:
            //   hasLocalDeath — skip while pendingNaturalDeath/respawn cycle is active.
            //     Without this, P1's Idle scale=0 overwrites P2's Regrow animation.
            //   karebabaDormantOverride — skip when P1's Karebaba is dormant (scale=0.005)
            //     but P2's is active (Upright/Spin, scale=0.01). Without this guard,
            //     P1's dormant scale overwrites P2's active scale every frame, making
            //     the Karebaba appear tiny while P2 is standing next to it (Fix 26).
            if (!ext->hasLocalDeath && !karebabaDormantOverride) {
                actor->scale = ext->netScale;
            }

            // Karebaba state machine sync: if the host's current state differs from ours,
            // drive the local actor into the matching state. Called after update() so any
            // self-transition that ran this frame is immediately corrected.
            //
            // Guards:
            //   hasLocalDeath — never override state after a local kill; the host keeps
            //     sending its pre-death state for several frames which would un-kill the
            //     actor here, causing a SetupDying↔SetupUpright oscillation loop.
            //
            //   Retract (7) blocked unconditionally — Retract is distance-driven in
            //     EnKarebaba_Upright via Anchor_GetNearestPlayerActor. Syncing P1's
            //     Retract to P2 forces P2's Karebaba to retract even when P2 is still
            //     nearby. Worse: SetupRetract doesn't reset world.pos.y, so if the
            //     actor hasn't risen from home.pos.y+14, the Retract animation completes
            //     immediately (StepTo hits target in frame 1) → SetupIdle → OnActorUpdate
            //     re-applies Retract again → rapid Retract→Idle→Retract loop every frame
            //     (~50ms per iteration, visible as oscillation in logs) (Fix 26).
            //
            //   dormant-to-active protection — if the host just entered the room its
            //     enemies start at Idle (1) while this client's were already activated.
            //     Block regression to dormant states (Grow=0/Idle=1/Awaken=2/Regrow=9)
            //     when the local enemy is already in a fully active state
            //     (Awaken=2/Upright=3/Spin=4/Retract=7). Awaken(2) is in both sets:
            //     it blocks host-sent Idle from resetting a locally-Awaken actor, AND
            //     is itself blocked from overriding already-active (Upright/Spin) actors.
            if (actor->id == ACTOR_EN_KAREBABA && ext->netStateIndex >= 0 && !ext->hasLocalDeath) {
                s16 curState = karebabaLocalState; // pre-computed above
                if (curState != ext->netStateIndex && ext->netStateIndex != 7) {
                    // Intra-attack guard (Fix 29): when both the host and local Karebaba are
                    // in the active bite cycle (Upright=3 / Spin=4), let the local state-machine
                    // timers drive the Upright↔Spin transitions rather than forcing the host's
                    // exact sub-state every packet.  Syncing here causes phase-mismatch
                    // oscillation: local actor finishes Spin → SetupUpright, then ApplyNetState
                    // immediately sets Spin again because the host's last packet still said Spin
                    // (confirmed in Test 25 P2 logs: SetupUpright + SetupSpin within 50ms for
                    // the same actor).  The dormant-to-active filter below still handles the
                    // Idle→Awaken→Upright activation boundary correctly.
                    bool netIsAttacking   = (ext->netStateIndex == 3 || ext->netStateIndex == 4);
                    // Retract (7) is the natural wind-down of the attack cycle. When the local
                    // actor is retracting and the host is still in Upright/Spin, ApplyNetState
                    // would reset the actor back to Upright mid-retract — world.pos.y is already
                    // at home.pos.y+14 so the retract completes in one frame, immediately loops
                    // (confirmed in Test 27 P2 logs: rapid Upright↔Retract every 50ms).
                    bool localIsAttacking = (curState == 3 || curState == 4 || curState == 7);
                    if (!(netIsAttacking && localIsAttacking)) {
                        bool netIsDormant  = (ext->netStateIndex == 0 || ext->netStateIndex == 1 ||
                                              ext->netStateIndex == 2 || ext->netStateIndex == 9);
                        bool localIsActive = (curState == 2 || curState == 3 || curState == 4 || curState == 7);
                        if (!(netIsDormant && localIsActive)) {
                            EnKarebaba_ApplyNetState((EnKarebaba*)actor, ext->netStateIndex, ext->netActorParams);
                        }
                    }
                }
            }

            // Goroiwa state-machine sync — issue #153.
            // The local action func runs every frame for collision registration but
            // its waypoint advance can drift across frame-rate boundaries (P2 on
            // VirtualBox at 20fps vs P1 native at 60fps). Cached waypoint state was
            // already applied directly in HandlePacket_EnemyUpdate; this block only
            // resets actionFunc when the host's state diverges from local. Position
            // is host-authoritative via ext->netPos (re-applied above).
            if (actor->id == ACTOR_EN_GOROIWA && ext->netStateIndex >= 0) {
                EnGoroiwa* boulder = (EnGoroiwa*)actor;
                s16 curState = EnGoroiwa_GetStateIndex(boulder);
                if (curState != ext->netStateIndex) {
                    EnGoroiwa_ApplyNetState(boulder, gPlayState, ext->netStateIndex);
                }
            }

        }
    });

    // Non-host clients: allow enemy update() to run so collision registration
    // (CollisionCheck_SetAC/OC) executes every frame, enabling P2 to hit enemies.
    // Position/health drift from the free-running AI is corrected in OnActorUpdate
    // below by re-applying the last state received from the host.
    // (Previously this suppressed the update entirely with *should = false.)

    // Host: before each enemy AI update runs, patch the 4 pre-computed targeting
    // fields to point at the nearest player (local or DummyPlayer). All enemy AI
    // that reads xzDistToPlayer, yDistToPlayer, xyzDistToPlayerSq, or
    // yawTowardsPlayer — including Actor_IsFacingPlayer and
    // Actor_IsFacingAndNearPlayer — will then target the correct player with no
    // per-enemy changes required.
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        if (roomState.ownerClientId != ownClientId) {
            return;
        }
        Actor* actor = static_cast<Actor*>(refActor);
        // Issue #153 — gate accepts ACTORCAT_ENEMY OR an allowlisted world-actor id.
        if (!IsSyncableActor(actor)) {
            return;
        }
        if (!IsSaveLoaded() || gPlayState == nullptr) {
            return;
        }

        Player* localPlayer = GET_PLAYER(gPlayState);
        Actor* nearest = FindNearestPlayerActor(actor, gPlayState);

        // Only overwrite when a DummyPlayer is closer. If the local player is
        // nearest, the automatic calculation (z_actor.c:2665-2669) is already
        // correct and we leave the fields untouched.
        if (nearest != &localPlayer->actor) {
            actor->xzDistToPlayer    = Actor_WorldDistXZToActor(actor, nearest);
            actor->yDistToPlayer     = Actor_HeightDiff(actor, nearest);
            actor->xyzDistToPlayerSq = SQ(actor->xzDistToPlayer) + SQ(actor->yDistToPlayer);
            actor->yawTowardsPlayer  = Actor_WorldYawTowardActor(actor, nearest);
        }
        // *should is not modified — enemy AI runs normally
    });

    // Any client: when an enemy dies locally, broadcast the defeat so all other
    // clients kill the matching actor. Both host and non-host send this — after
    // Fix 4 non-host clients have live colliders and can land killing blows.
    // OnEnemyDefeat fires from within each enemy's death animation code, so the
    // death animation has already played on the killing client before we notify.
    COND_HOOK(OnEnemyDefeat, isConnected, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Diagnostic: log every OnEnemyDefeat invocation so we can confirm
        // which actors fire this hook and whether their extension is found.
        SPDLOG_INFO("[EnemyDefeated] OnEnemyDefeat hook: actor ptr={} id={} cat={}", (void*)actor, actor->id, actor->category);
        if (!IsSaveLoaded()) {
            SPDLOG_WARN("[EnemyDefeated] OnEnemyDefeat skipped — save not loaded (actor id={})", actor->id);
            return;
        }
        EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext == nullptr) {
            SPDLOG_WARN("[EnemyDefeated] OnEnemyDefeat: no extension for actor id={} — netId assignment missed", actor->id);
            return;
        }
        // Scene-visit dedup: skip if this netId was already broadcast during the
        // current scene visit (e.g. second Actor_Kill on a recycled actor pointer,
        // or a Karebaba being killed again before its previous respawn cycle cleared
        // the dedup set).
        //
        // Even when skipping the send, set hasLocalDeath and defeatPacketSent so that:
        //   1. ENEMY_UPDATE does not revive the dying actor on this frame.
        //   2. The non-host respawn detection (which checks defeatPacketSent) can fire
        //      when the actor returns to Idle, clearing the dedup entry for the next kill.
        if (sentDefeatThisScene.count(ext->netId)) {
            SPDLOG_INFO("[EnemyDefeated] OnEnemyDefeat: netId={} already sent this scene visit — skipping duplicate",
                        ext->netId);
            ext->hasLocalDeath    = true;
            ext->defeatPacketSent = true;
            return;
        }
        sentDefeatThisScene.insert(ext->netId);
        // Mark that ENEMY_DEFEATED was sent via the normal death path so that
        // the OnActorKill hook (Fix 12) does not send a duplicate packet when
        // this same actor later calls Actor_Kill on itself.
        ext->defeatPacketSent = true;
        // Prevent ENEMY_UPDATE from overwriting health > 0 after a local kill.
        // The host keeps sending health=alive for a few frames while this packet
        // travels; without this flag the dying enemy flickers back to alive state.
        ext->hasLocalDeath = true;
        // Host tracks kills for join-time replay (Fix 6).
        if (roomState.ownerClientId == ownClientId) {
            deadEnemiesByScene[gPlayState->sceneNum].insert(ext->netId);
        }
        SendPacket_EnemyDefeated(ext->netId);
    });

    // Fix 12 — Actor_Kill death path: ENEMY_DEFEATED for enemies that skip OnEnemyDefeat.
    //
    // Some enemies die by calling Actor_Kill directly inside their death animation
    // (e.g., ACTOR_EN_SKB at dawn) rather than going through the standard health-zero →
    // OnEnemyDefeat → Actor_Kill sequence. Because OnEnemyDefeat never fires for these
    // actors, no ENEMY_DEFEATED packet is sent and the actor persists on remote clients.
    //
    // This hook fires for every Actor_Kill. It sends ENEMY_DEFEATED for any actor
    // that has an EnemyNetId extension (meaning it was ACTORCAT_ENEMY/BOSS at spawn)
    // but did NOT already send a packet through OnEnemyDefeat (guarded by
    // defeatPacketSent). The isKillingNetworkActor flag prevents echo loops when
    // HandlePacket_EnemyDefeated is the one calling Actor_Kill.
    //
    // Room-unload guard: OoT calls Actor_Kill on every actor in a room that is
    // being unloaded during a room transition. These are NOT real deaths — the
    // actors respawn when the room is re-entered. After Room Init fires for the
    // new room, gPlayState->curRoom.num reflects the destination; actors in the
    // old room have actor->room != curRoom.num, so we can detect and skip them.
    COND_HOOK(OnActorKill, isConnected, [&](void* refActor) {
        if (isKillingNetworkActor) {
            return; // This kill originated from a received ENEMY_DEFEATED — do not echo.
        }
        Actor* actor = static_cast<Actor*>(refActor);
        if (!IsSaveLoaded()) {
            return;
        }
        // Skip room-unload kills. When OoT transitions to a different room within
        // the same scene, it calls Actor_Kill on every actor in the departing room
        // after updating curRoom to the destination. Remote clients that are still
        // in the old room must NOT receive ENEMY_DEFEATED for these actors — the
        // enemies are not dead, just unloaded for the host. Detect by comparing
        // the actor's assigned room to the room the host is now in.
        if (gPlayState != nullptr && actor->room != gPlayState->roomCtx.curRoom.num) {
            return;
        }
        // Diagnostic: log every kill for ACTORCAT_ENEMY/BOSS/MISC actors so we can
        // confirm which enemies reach OnActorKill and why some skip sending ENEMY_DEFEATED.
        // MISC is included because Deku Baba stems change category before Actor_Kill fires.
        if (actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS ||
            actor->category == ACTORCAT_MISC) {
            EnemyNetId* diagExt = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
            SPDLOG_INFO("[EnemyDefeated] OnActorKill: id={} cat={} ext={} netId={} defeatSent={} inDedup={}",
                        actor->id, (int)actor->category,
                        (diagExt != nullptr ? "found" : "NULL"),
                        (diagExt != nullptr ? diagExt->netId : 0u),
                        (diagExt != nullptr ? diagExt->defeatPacketSent : false),
                        (diagExt != nullptr ? (bool)sentDefeatThisScene.count(diagExt->netId) : false));
        }
        // Do NOT filter by actor->category here — Deku Baba stems call
        // Actor_ChangeCategory(ACTORCAT_MISC) in EnDekubaba_SetupDeadStickDrop
        // before Actor_Kill fires, so they appear as ACTORCAT_MISC at this point.
        // Instead rely on the EnemyNetId extension: it is only assigned in OnActorSpawn
        // to ACTORCAT_ENEMY/BOSS actors, so its presence is sufficient proof.
        EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
        if (ext == nullptr || ext->netId == 0) {
            return;
        }
        if (ext->defeatPacketSent) {
            return; // Already sent — either via OnEnemyDefeat or a prior OnActorKill fire.
        }
        // Scene-visit dedup: skip if this netId was already broadcast during the
        // current scene visit (e.g. room-transition re-allocates an actor at the same
        // address, giving it a fresh extension with defeatPacketSent=false but the same
        // netId as an actor that already died this scene visit).
        if (sentDefeatThisScene.count(ext->netId)) {
            SPDLOG_INFO("[EnemyDefeated] Actor_Kill path: netId={} already sent this scene visit — skipping duplicate",
                        ext->netId);
            ext->defeatPacketSent = true; // Prevent future OnActorKill fires on this instance.
            return;
        }
        // Actor died via Actor_Kill without firing OnEnemyDefeat.
        // Broadcast ENEMY_DEFEATED so remote clients remove the actor.
        // Set defeatPacketSent immediately so re-entrant or repeated Actor_Kill calls
        // (e.g. OoT calling Actor_Kill twice on the same actor, or multiple actors sharing
        // a netId via posHash collision) do not emit duplicate packets.
        sentDefeatThisScene.insert(ext->netId);
        ext->defeatPacketSent = true;
        ext->hasLocalDeath = true;
        SPDLOG_INFO("[EnemyDefeated] Actor_Kill path: sending defeat for actor id={} netId={}",
                    actor->id, ext->netId);
        if (roomState.ownerClientId == ownClientId) {
            deadEnemiesByScene[gPlayState->sceneNum].insert(ext->netId);
        }
        SendPacket_EnemyDefeated(ext->netId);
    });

    // #endregion

    COND_HOOK(OnPlayerSfx, isConnected, [&](u16 sfxId) { SendPacket_PlayerSfx(sfxId); });
    COND_HOOK(OnOcarinaNote, isConnected,
              [&](uint8_t note, float modulator, int8_t bend) { SendPacket_OcarinaSfx(note, modulator, bend); });

    COND_HOOK(OnLoadGame, isConnected, [&](s16 fileNum) { justLoadedSave = true; });

    COND_HOOK(OnSaveFile, isConnected, [&](s16 fileNum, int sectionID) {
        if (sectionID == 0) {
            SendPacket_UpdateTeamState();
        }
    });

    COND_HOOK(OnFlagSet, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_SetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnFlagUnset, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_UnsetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnSceneFlagSet, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_SetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnSceneFlagUnset, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_UnsetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnRandoSetCheckStatus, isConnected, [&](RandomizerCheck rc, RandomizerCheckStatus status) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoSetIsSkipped, isConnected, [&](RandomizerCheck rc, bool isSkipped) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoEntranceDiscovered, isConnected,
              [&](u16 entranceIndex, u8 isReversedEntrance) { SendPacket_EntranceDiscovered(entranceIndex); });

    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, isConnected, [&](void* refActor) { SendPacket_GameComplete(); });

    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        // Handle vanilla dungeon items a bit differently
        if (itemEntry.modIndex == MOD_NONE &&
            (itemEntry.itemId >= ITEM_KEY_BOSS && itemEntry.itemId <= ITEM_KEY_SMALL)) {
            SendPacket_UpdateDungeonItems();
            return;
        }

        SendPacket_GiveItem(itemEntry.tableId, itemEntry.getItemId);
    });

    COND_HOOK(OnDungeonKeyUsed, isConnected, [&](uint16_t mapIndex) {
        // Handle vanilla dungeon items a bit differently
        SendPacket_UpdateDungeonItems();
    });

    COND_VB_SHOULD(VB_APPLY_TUNIC_COLOR, isConnected, {
        Actor* myPlayer = (Actor*)GET_PLAYER(gPlayState);
        Actor* actor = va_arg(args, Actor*);
        Color_RGB8* color = va_arg(args, Color_RGB8*);

        if (actor == myPlayer) {
            Color_RGBA8 ownColor = CVarGetColor(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
            color->r = ownColor.r;
            color->g = ownColor.g;
            color->b = ownColor.b;
            return;
        }

        uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

        if (!Anchor::Instance->clients.contains(clientId)) {
            return;
        }

        AnchorClient& client = Anchor::Instance->clients[clientId];
        color->r = client.color.r;
        color->g = client.color.g;
        color->b = client.color.b;
    });

    // #endregion

    // #region Hooks that are purely to sync actor states across the clients, not super essential

    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        EnItem00* actor = static_cast<EnItem00*>(refActor);

        if (Flags_GetCollectible(gPlayState, actor->collectibleFlag)) {
            Actor_Kill(&actor->actor);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BOMBWALL, isConnected, [&](void* refActor, bool* should) {
        BgBombwall* actor = static_cast<BgBombwall*>(refActor);

        if (actor->actionFunc == func_8086ED70 && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BREAKWALL, isConnected, [&](void* refActor, bool* should) {
        BgBreakwall* actor = static_cast<BgBreakwall*>(refActor);

        if (actor->actionFunc == BgBreakwall_Wait && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HAKA_ZOU, isConnected, [&](void* refActor, bool* should) {
        BgHakaZou* actor = static_cast<BgHakaZou*>(refActor);

        if (actor->actionFunc == func_80883000 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HAMSTEP, isConnected, [&](void* refActor, bool* should) {
        BgHidanHamstep* actor = static_cast<BgHidanHamstep*>(refActor);

        if (actor->actionFunc == func_808887C4 && Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0xFF)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HROCK, isConnected, [&](void* refActor, bool* should) {
        BgHidanHrock* actor = static_cast<BgHidanHrock*>(refActor);

        if (actor->actionFunc == func_808896B8 && Flags_GetSwitch(gPlayState, actor->unk_16A)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_ICE_SHELTER, isConnected, [&](void* refActor, bool* should) {
        BgIceShelter* actor = static_cast<BgIceShelter*>(refActor);

        if (actor->actionFunc == BgIceShelter_Idle && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            BgIceShelter_SetupMelt(actor);
            Audio_PlayActorSound2(&actor->dyna.actor, NA_SE_EV_ICE_MELT);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBCHUIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombchuiwa* actor = static_cast<BgJyaBombchuiwa*>(refActor);

        if (actor->actionFunc == BgJyaBombchuiwa_WaitForExplosion &&
            Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombiwa* actor = static_cast<BgJyaBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_MIZU_BWALL, isConnected, [&](void* refActor, bool* should) {
        BgMizuBwall* actor = static_cast<BgMizuBwall*>(refActor);

        if (actor->actionFunc == BgMizuBwall_Idle &&
            Flags_GetSwitch(gPlayState, ((u16)actor->dyna.actor.params >> 8) & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT08_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot08Bakudankabe* actor = static_cast<BgSpot08Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT11_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot11Bakudankabe* actor = static_cast<BgSpot11Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT17_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot17Bakudankabe* actor = static_cast<BgSpot17Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            func_808B6BC0(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->dyna.actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            Actor_Kill(&actor->dyna.actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_MARUTA, isConnected, [&](void* refActor, bool* should) {
        BgYdanMaruta* actor = static_cast<BgYdanMaruta*>(refActor);

        if (actor->actionFunc == func_808BF078 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_SP, isConnected, [&](void* refActor, bool* should) {
        BgYdanSp* actor = static_cast<BgYdanSp*>(refActor);

        if ((actor->actionFunc == BgYdanSp_FloorWebIdle || actor->actionFunc == BgYdanSp_WallWebIdle) &&
            Flags_GetSwitch(gPlayState, actor->isDestroyedSwitchFlag)) {
            BgYdanSp_BurnWeb(actor, gPlayState);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_SHUTTER, isConnected, [&](void* refActor, bool* should) {
        DoorShutter* actor = static_cast<DoorShutter*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            DECR(actor->unlockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_DOOR, isConnected, [&](void* refActor, bool* should) {
        EnDoor* actor = static_cast<EnDoor*>(refActor);

        if (actor->actionFunc == EnDoor_Idle && Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            DECR(actor->lockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SI, isConnected, [&](void* refActor, bool* should) {
        EnSi* actor = static_cast<EnSi*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SW, isConnected, [&](void* refActor, bool* should) {
        EnSw* actor = static_cast<EnSw*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ITEM_B_HEART, isConnected, [&](void* refActor, bool* should) {
        ItemBHeart* actor = static_cast<ItemBHeart*>(refActor);

        if (Flags_GetCollectible(gPlayState, 0x1F)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        ObjBombiwa* actor = static_cast<ObjBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjBombiwa_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 80, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_HAMISHI, isConnected, [&](void* refActor, bool* should) {
        ObjHamishi* actor = static_cast<ObjHamishi*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjHamishi_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_HAMMER_TOTEM_BREAK, isConnected, {
        BgHidanDalm* actor = va_arg(args, BgHidanDalm*);

        if (Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            *should = true;
        }
    });

    COND_VB_SHOULD(VB_FIRE_TEMPLE_BOMBABLE_WALL_BREAK, isConnected, {
        BgHidanKowarerukabe* actor = va_arg(args, BgHidanKowarerukabe*);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0x3F)) {
            *should = true;
        }
    });

    // #endregion

    // #region Hooks for visual effects that don't affect gameplay

    struct CompassIcon {
        Vec3f pos;
        Vec3s rot;
        float scale;
        Color_RGB8 color;
    };

    COND_HOOK(OnMinimapDrawCompassIcons, isConnected, [&]() {
        if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("ShowOtherPlayersOnMinimap"), 1) ||
            Anchor::Instance->roomState.showLocationsMode == 0) {
            return;
        }

        std::vector<CompassIcon> compassIcons;

        bool isInDungeon = gPlayState->sceneNum == SCENE_DEKU_TREE || gPlayState->sceneNum == SCENE_DODONGOS_CAVERN ||
                           gPlayState->sceneNum == SCENE_JABU_JABU || gPlayState->sceneNum == SCENE_FOREST_TEMPLE ||
                           gPlayState->sceneNum == SCENE_FIRE_TEMPLE || gPlayState->sceneNum == SCENE_WATER_TEMPLE ||
                           gPlayState->sceneNum == SCENE_SPIRIT_TEMPLE || gPlayState->sceneNum == SCENE_SHADOW_TEMPLE ||
                           gPlayState->sceneNum == SCENE_BOTTOM_OF_THE_WELL || gPlayState->sceneNum == SCENE_ICE_CAVERN;
        std::string teamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");

        // When transitioning to a new room via a door, curRoom.num updates immediately but the minimap still shows the
        // previous room while fading out
        s8 displayedRoomNum =
            gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

        for (auto& [clientId, client] : Anchor::Instance->clients) {
            // Show compass icons for other players in the current scene. Also require them to be in the current room
            // within dungeons. If showLocationsMode isn't all players (2), only show compass icons for players of the
            // same team
            if (!client.self && client.online && client.player && client.sceneNum == gPlayState->sceneNum &&
                (!isInDungeon || client.curRoomNum == displayedRoomNum) &&
                (Anchor::Instance->roomState.showLocationsMode == 2 || client.teamId == teamId)) {
                compassIcons.push_back(
                    CompassIcon{ client.player->actor.world.pos, client.player->actor.shape.rot, 0.3f, client.color });
            }
        }

        // The local player's compass icon is always last so it gets drawn above the others
        Player* player = GET_PLAYER(gPlayState);
        compassIcons.push_back(CompassIcon{ player->actor.world.pos, player->actor.shape.rot, 0.4f,
                                            CVarGetColor24(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 }) });

        // Adapted internals of Minimap_DrawCompassIcons()
        s16 leftMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.L"), 0);
        s16 rightMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.R"), 0);
        s16 bottomMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.B"), 0);

        s16 xMarginsMinimap;
        s16 yMarginsMinimap;
        if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ORIGINAL_LOCATION) {
                xMarginsMinimap = rightMinimapMargin;
            }
            yMarginsMinimap = bottomMinimapMargin;
        } else {
            xMarginsMinimap = 0;
            yMarginsMinimap = 0;
        }

        s16 mapWidth = isInDungeon ? R_DGN_MINIMAP_X : R_OW_MINIMAP_X;
        s16 mapStartPosX = isInDungeon ? 96 : gMapData->owMinimapWidth[R_MAP_INDEX];

        OPEN_DISPS(gPlayState->state.gfxCtx);
        Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

        for (auto& compassIcon : compassIcons) {
            gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

            // The compass offset value is a factor of 10 compared to N64 screen pixels and originates in the center of
            // the screen Compute the additional mirror offset value by normalizing the original offset position and
            // taking it's distance to the center of the map, duplicating that result and casting back to a factor of 10
            s16 mirrorOffset =
                ((mapWidth / 2) - ((R_COMPASS_OFFSET_X / 10) - (mapStartPosX - SCREEN_WIDTH / 2))) * 2 * 10;

            s16 tempX = (s16)compassIcon.pos.x;
            s16 tempZ = (s16)compassIcon.pos.z;
            tempX /= R_COMPASS_SCALE_X * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            tempZ /= R_COMPASS_SCALE_Y;

            s16 tempXOffset =
                R_COMPASS_OFFSET_X + (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? mirrorOffset : 0);
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) != ORIGINAL_LOCATION) {
                if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_LEFT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = leftMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromLeftEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                     (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                    10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_RIGHT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = rightMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                      (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                     10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_NONE) {
                    Matrix_Translate(
                        (tempXOffset + tempX + (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10) / 10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                }
            } else {
                Matrix_Translate(OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX) / 10.0f),
                                 (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ) / 10.0f, 0.0f,
                                 MTXMODE_NEW);
            }
            Matrix_Scale(compassIcon.scale, compassIcon.scale, compassIcon.scale, MTXMODE_APPLY);
            Matrix_RotateX(-1.6f, MTXMODE_APPLY);
            s16 rotation = ((0x7FFF - compassIcon.rot.y) / 0x400) *
                           (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            Matrix_RotateY(rotation / 10.0f, MTXMODE_APPLY);
            gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, compassIcon.color.r, compassIcon.color.g, compassIcon.color.b,
                            255);
            gSPDisplayList(OVERLAY_DISP++, (Gfx*)gCompassArrowDL);
        }

        CLOSE_DISPS(gPlayState->state.gfxCtx);
    });

    // Re-apply remote players' custom skeletons when the local asset-alt prefix changes
    // (e.g. the player switches their own model, which may change which .o2r is open).
    // Also re-broadcasts our own UPDATE_CLIENT_STATE so remote clients pick up our new model.
    COND_HOOK(OnAssetAltChange, isConnected, [&]() {
        if (gPlayState == nullptr) return;
        Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (actor != nullptr) {
            if (actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update) {
                uint32_t clientId = GetDummyPlayerClientId(actor);
                if (clients.contains(clientId)) {
                    AnchorClient& client = clients[clientId];
                    if (!client.customModelFilename.empty()) {
                        bool isAdult = (client.linkAge != LINK_AGE_CHILD);
                        client.customSkeleton = nullptr;
                        client.bakedModel = std::make_unique<SOH::BakedPlayerModel>();
                        SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
                            &((Player*)actor)->skelAnime, isAdult,
                            (uint8_t)client.currentTunic,
                            client.customModelFilename, client.customSkeleton,
                            *client.bakedModel);
                    }
                }
            }
            actor = actor->next;
        }
        SendPacket_UpdateClientState();
    });

    // #endregion
}
