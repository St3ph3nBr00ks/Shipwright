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
#include "macros.h" // AMMO / CUR_CAPACITY / INV_CONTENT — follower item-pickup need-gating
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
    // Adding an ID here admits the actor into the sync pipeline regardless of
    // its current `actor->category` — so default-category and any mid-life
    // category-transition instances both pass the admission gate.
    switch (actorId) {
        case ACTOR_EN_GOROIWA:  return true;  // #153 (PROP)
        case ACTOR_EN_SW:       return true;  // #148 Skullwalltula (gold variant → NPC)
        case ACTOR_EN_DEKUNUTS: return true;  // #135 Mad Scrub (ITEMACTION projectile transition)
        default:                return false;
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

bool Anchor::IsLocalPlayerClimbing() const {
    if (gPlayState == nullptr) { return false; }
    Player* p = GET_PLAYER(gPlayState);
    if (p == nullptr) { return false; }
    u32 sf1 = p->stateFlags1;
    return (sf1 & PLAYER_STATE1_CLIMBING_LADDER)   ||
           (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) ||
           (sf1 & PLAYER_STATE1_CLIMBING_LEDGE);
}

// Option B — follower item override system. See Anchor.h for full
// design. Touches gSaveContext.equips.{buttonItems[1..3], cButtonSlots[0..2]}.
// B-slot (sword) is never modified.
u8 Anchor::FollowerTryEquipRangedWeapon() {
    // Gate on CVar.
    if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems"), 0)) {
        return 0xFF;
    }
    // Idempotent — if already overridden, just report the active slot.
    if (followerItemOverrideActive) {
        return followerActiveCSlot;
    }
    // Pick slingshot (child) or bow (adult), whichever is in inventory.
    u8 item = ITEM_NONE;
    u8 invSlot = 0;
    if (gSaveContext.inventory.items[SLOT_SLINGSHOT] == ITEM_SLINGSHOT) {
        item = ITEM_SLINGSHOT;
        invSlot = SLOT_SLINGSHOT;
    } else if (gSaveContext.inventory.items[SLOT_BOW] == ITEM_BOW) {
        item = ITEM_BOW;
        invSlot = SLOT_BOW;
    } else {
        SPDLOG_INFO("[Follower] FollowerTryEquipRangedWeapon: no slingshot or bow in inventory");
        return 0xFF;
    }
    // Snapshot C-button loadout (indices 1..3 of buttonItems; indices 0..2
    // of cButtonSlots). Skip B-button.
    for (int i = 1; i <= 3; i++) {
        savedButtonItems[i] = gSaveContext.equips.buttonItems[i];
    }
    for (int i = 0; i < 3; i++) {
        savedCButtonSlots[i] = gSaveContext.equips.cButtonSlots[i];
    }
    // Override C-left (buttonItems index 1; cButtonSlots index 0).
    gSaveContext.equips.buttonItems[1]  = item;
    gSaveContext.equips.cButtonSlots[0] = invSlot;
    followerItemOverrideActive          = true;
    followerActiveCSlot                 = 0; // C-left
    SPDLOG_INFO("[Follower] Item override: equipped {} (invSlot={}) to C-left; "
                "saved prior C-items ({:#04x},{:#04x},{:#04x})",
                (item == ITEM_SLINGSHOT ? "slingshot" : "bow"), (int)invSlot,
                (int)savedButtonItems[1], (int)savedButtonItems[2], (int)savedButtonItems[3]);
    return 0;
}

void Anchor::FollowerRestoreItems() {
    if (!followerItemOverrideActive) { return; }
    for (int i = 1; i <= 3; i++) {
        gSaveContext.equips.buttonItems[i] = savedButtonItems[i];
    }
    for (int i = 0; i < 3; i++) {
        gSaveContext.equips.cButtonSlots[i] = savedCButtonSlots[i];
    }
    followerItemOverrideActive = false;
    followerActiveCSlot        = 0xFF;
    SPDLOG_INFO("[Follower] Item override: restored original C-button loadout");
}

void Anchor::SetFollowerActive(bool active) {
    bool changed = (followerActive != active);
    followerActive = active;
    if (active) {
        followerAIState     = FollowerAIState::IDLE;
        followerStateFrames = 0;
        followerStuckFrames = 0;
        followerTargetEnemy = nullptr;
        followerLeaderClientId = 0;
        followerOverrunFrames = 0;
        followerStuckCycleCount = 0;
        followerStuckCycleResetFrames = 0;
        hasPendingTransition = false;
        pendingTransitionTimeoutFrames = 0;
        followerDoorHandoff = false;
        followerDoorHandoffFrames = 0;
        followerClimbDismountFrames = 0;
        SPDLOG_INFO("[Follower] Activated (menu)");
    } else {
        hasPendingTransition = false;
        pendingTransitionTimeoutFrames = 0;
        followerDoorHandoff = false;
        followerDoorHandoffFrames = 0;
        followerClimbDismountFrames = 0;
        // Safety: always restore the player's C-button loadout on any
        // deactivation path (menu toggle, joystick cancel, scene boundary,
        // leash timeout, …). FollowerRestoreItems is a no-op when no
        // override is active.
        FollowerRestoreItems();
        // Bug 8 — defensive input cleanup. The follower hook OR-s buttons into
        // input each frame while active; if deactivation happens mid-frame
        // (after injection but before Player_Update consumes them), the
        // residual bits can trigger a stray action on Link. Clear every
        // button and stick axis the follower ever injects.
        if (gPlayState != nullptr) {
            Input& input = gPlayState->state.input[0];
            constexpr u16 kFollowerButtons = BTN_A | BTN_B | BTN_Z | BTN_R |
                                             BTN_CLEFT | BTN_CDOWN | BTN_CRIGHT;
            input.press.button &= ~kFollowerButtons;
            input.cur.button   &= ~kFollowerButtons;
            input.press.stick_x = 0;
            input.press.stick_y = 0;
            input.cur.stick_x   = 0;
            input.cur.stick_y   = 0;
        }
        SPDLOG_INFO("[Follower] Deactivated (menu)");
    }
    if (changed && isConnected) {
        SendPacket_UpdateClientState();
    }
}

void Anchor::RegisterHooks() {

    // #region Hooks that are required for basic Anchor functionality

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        // Bump before sending so the host's HandlePacket_UpdateClientState sees the
        // new epoch and fires the dead-enemy replay even when sceneNum and isSaveLoaded
        // are both unchanged (Game Over continue, void-out in the same scene).
        sceneSpawnEpoch++;
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

        // G1/G2 — broadcast climbing-state edge changes so remote followers can
        // teleport-and-ride. Edge-only (not every frame): UPDATE_CLIENT_STATE is
        // a heavy packet and climbing transitions are infrequent.
        static bool sLastClimbing = false;
        bool nowClimbing = IsLocalPlayerClimbing();
        if (nowClimbing != sLastClimbing) {
            sLastClimbing = nowClimbing;
            SPDLOG_INFO("[Follower] LocalPlayer isClimbing edge: {} -> {}",
                        !nowClimbing, nowClimbing);
            SendPacket_UpdateClientState();
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

        // Phase C — SCENE_TRANSITION_HANDOFF leader-side broadcast.
        // Every client runs this: on the rising edge of transitionTrigger
        // (OFF → START), capture our current position and destination
        // entrance, broadcast to other clients. A client with follower mode
        // active will use the data to follow us through the transition
        // (SceneTransitionHandoff.cpp HandlePacket_… stashes it pending; the
        // follower state machine below consumes it once within proximity).
        if (IsSaveLoaded() && gPlayState != nullptr) {
            s32 curTrigger = gPlayState->transitionTrigger;
            if (curTrigger == TRANS_TRIGGER_START &&
                prevTransitionTrigger == TRANS_TRIGGER_OFF) {
                Player* localPlayer = GET_PLAYER(gPlayState);
                if (localPlayer != nullptr) {
                    s16   fromScene   = (s16)gPlayState->sceneNum;
                    s16   toEntrance  = (s16)gPlayState->nextEntranceIndex;
                    Vec3f triggerPos  = localPlayer->actor.world.pos;
                    s16   triggerRotY = localPlayer->actor.shape.rot.y;
                    SendPacket_SceneTransitionHandoff(fromScene, toEntrance,
                                                      triggerPos, triggerRotY);
                }
            }
            prevTransitionTrigger = curTrigger;
        } else {
            prevTransitionTrigger = TRANS_TRIGGER_OFF;
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
                //
                // INVARIANT — every `input.press.button |= X` site in this file MUST
                // have a matching mask entry below for the state(s) that inject X.
                // If a new injection is added without its mask, the follower will
                // self-cancel on the frame it fires. Symptom: log shows
                // `Deactivated (input pressed=0xNNNN state=...)` with the NNNN bit
                // matching the newly injected button and the state being one that
                // just started injecting it (Test 4 log 70 caught this for BTN_Z
                // in ENGAGE/ATTACK when Bug D added the lock-on hold without the
                // mask; fixed by the block below).
                //
                // Current mask table:
                //   ENGAGE/ATTACK         → BTN_Z   (lock-on, Bug D)
                //   ATTACK                → BTN_B   (sword swing cycle)
                //   BLOCK                 → BTN_R   (shield plant)
                //   RANGED_ATTACK         → BTN_Z | BTN_A | C-slot
                //   GETTING_ITEM/TALKING  → BTN_A   (text-box dismiss)
                //   DO_ACTION_CLIMB/ENTER → BTN_A   (ladder + door auto-press)
                if (followerActive) {
                    u16 pressed = gPlayState->state.input[0].press.button;
                    u16 deactivateCheck = pressed;
                    // Mask off buttons WE inject — otherwise our own input would
                    // cancel follower mode the frame after we inject it.
                    if (followerAIState == FollowerAIState::ATTACK) {
                        deactivateCheck &= ~BTN_B;
                    }
                    if (followerAIState == FollowerAIState::BLOCK) {
                        deactivateCheck &= ~BTN_R;
                    }
                    // Bug D — BTN_Z lock-on edge-pressed on ENGAGE entry (and
                    // held via cur through ATTACK). Mask from cancel-check
                    // so the ENGAGE entry frame doesn't self-cancel.
                    if (followerAIState == FollowerAIState::ENGAGE ||
                        followerAIState == FollowerAIState::ATTACK) {
                        deactivateCheck &= ~BTN_Z;
                    }
                    // Item pickup — while OoT is showing an item-get text box
                    // (PLAYER_STATE1_GETTING_ITEM or PLAYER_STATE1_TALKING),
                    // we inject BTN_A every 20 frames to dismiss. Mask so
                    // our own press doesn't self-cancel follower mode.
                    if (player != nullptr &&
                        (player->stateFlags1 &
                         (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_TALKING))) {
                        deactivateCheck &= ~BTN_A;
                    }
                    if (followerAIState == FollowerAIState::RANGED_ATTACK) {
                        deactivateCheck &= ~(BTN_Z | BTN_A);
                        // If we also injected a C-button for item draw
                        // (Option B), mask that too so our own press doesn't
                        // cancel follower mode.
                        switch (followerActiveCSlot) {
                            case 0: deactivateCheck &= ~BTN_CLEFT;  break;
                            case 1: deactivateCheck &= ~BTN_CDOWN;  break;
                            case 2: deactivateCheck &= ~BTN_CRIGHT; break;
                            default: break;
                        }
                    }
                    // DO_ACTION_CLIMB triggers BTN_A injection regardless of state
                    // (ledge-hang and water-exit climb-out). DO_ACTION_ENTER
                    // (Phase A) does the same for doors. Mask BTN_A in either
                    // case so our own injection doesn't cancel follower mode.
                    if (player != nullptr &&
                        (player->stateFlags2 &
                         (PLAYER_STATE2_DO_ACTION_CLIMB | PLAYER_STATE2_DO_ACTION_ENTER))) {
                        deactivateCheck &= ~BTN_A;
                    }
                    if (deactivateCheck != 0) {
                        // Include state name + the UNMASKED residue so future
                        // self-cancel regressions are easy to diagnose: any
                        // bit in `check` is a button we either didn't mask or
                        // user genuinely pressed. If the bit matches a known
                        // injection (BTN_Z, BTN_A, BTN_B, BTN_R, C-slot) and
                        // the state should be injecting that button, the mask
                        // table above is missing an entry for that state.
                        const char* stateStr = "?";
                        switch (followerAIState) {
                            case FollowerAIState::IDLE:          stateStr = "IDLE";          break;
                            case FollowerAIState::FOLLOW:        stateStr = "FOLLOW";        break;
                            case FollowerAIState::STUCK:         stateStr = "STUCK";         break;
                            case FollowerAIState::ENGAGE:        stateStr = "ENGAGE";        break;
                            case FollowerAIState::ATTACK:        stateStr = "ATTACK";        break;
                            case FollowerAIState::RETURN:        stateStr = "RETURN";        break;
                            case FollowerAIState::CLIMBING:      stateStr = "CLIMBING";      break;
                            case FollowerAIState::BLOCK:         stateStr = "BLOCK";         break;
                            case FollowerAIState::RANGED_ATTACK: stateStr = "RANGED_ATTACK"; break;
                            case FollowerAIState::STANDBY:       stateStr = "STANDBY";       break;
                            case FollowerAIState::COLLECT_ITEM:  stateStr = "COLLECT_ITEM";  break;
                        }
                        SetFollowerActive(false);
                        SPDLOG_INFO("[Follower] Deactivated (input pressed=0x{:04X} check=0x{:04X} state={})",
                                    pressed, deactivateCheck, stateStr);
                        return;
                    }
                }

                if (!followerActive) { return; }

                // Monotonic per-Anchor tick counter. Advances once per
                // follower-active OnGameFrameUpdate tick. Used for
                // grace-period tracking in the item-pickup scan — must
                // not be followerStateFrames (which resets on state
                // change).
                followerTickCounter++;

                // G18 — full cutscene suspension. csCtx.state == CS_STATE_IDLE means
                // no cutscene; anything else is an active CS frame and we must not
                // touch the player's state machine. Stick suppression alone (in
                // ShouldActorUpdate) is not enough — running the state machine here
                // can still write shape.rot.y or trigger state transitions that
                // collide with cutscene scripts.
                if (gPlayState->csCtx.state != CS_STATE_IDLE) {
                    return;
                }

                // G12 — tick the stuck-cycle reset window. When the window expires,
                // the cycle counter clears so isolated STUCK events don't accumulate
                // across long sessions. Counter is incremented at FOLLOW→STUCK below.
                if (followerStuckCycleResetFrames > 0) {
                    followerStuckCycleResetFrames--;
                    if (followerStuckCycleResetFrames == 0) {
                        followerStuckCycleCount = 0;
                    }
                }

                // --- AI follower state machine ---
                // Constants (do not change follow offset — set by prior session).
                static constexpr f32 kFollowOffset       = 50.0f;  // world +X from leader
                static constexpr f32 kFollowThreshold    = 100.0f; // dist to switch FOLLOW↔IDLE
                static constexpr f32 kEngageRange        = 350.0f; // enemy detection radius (XZ)
                static constexpr f32 kAttackRange        = 80.0f;  // melee-contact radius (XZ)
                static constexpr f32 kMaxYDelta          = 120.0f; // reject enemies on a different floor
                static constexpr f32 kMaxLeash           = 800.0f; // abandon ENGAGE if leader this far
                static constexpr f32 kMoveSpeed          = 4.0f;   // units/frame for STUCK fallback nudge only
                static constexpr int kStuckCheckInterval = 20;     // frames between stuck checks
                static constexpr f32 kStuckMinProgress   = 5.0f;   // min units per check interval
                static constexpr int kStuckRecovery      = 25;     // frames of strafe before retry
                static constexpr int kAttackDuration     = 60;     // frames per ATTACK cycle
                // G10 — leash-timeout teleport thresholds.
                static constexpr f32 kTeleportThreshold   = 1200.0f; // sustained XZ overrun that triggers teleport
                static constexpr int kTeleportDelayFrames = 120;     // ~2s at 60fps; debounces brief overshoots
                // G12 — STUCK escalation: N STUCK entries within window → teleport.
                static constexpr int kStuckCycleEscalation = 3;     // count threshold
                static constexpr int kStuckCycleWindow     = 300;   // frames; resets count if exceeded
                // Phase B (Bug 7) — door handoff timeout. After leader crosses a
                // room boundary, the follower has this many frames to navigate
                // to the door / cross the threshold itself. On timeout, teleport.
                static constexpr int kDoorHandoffTimeout   = 360;   // ~6 s at 60fps
                // Bug C (log 69) — dismount forward-hold. After CLIMBING→IDLE,
                // hold stick forward at the climb-exit yaw for this many frames
                // so Link walks inward past the rim before other state machine
                // logic can re-point him backward toward a leader standing at
                // the edge. ~1 s at 60fps (matches user request "~1 more second").
                static constexpr int kClimbDismountHoldFrames = 60;
                // Item pickup (Claude/Plans/ai_follower_item_pickup.md).
                // kItemProximity — XZ radius of the ACTORCAT_MISC scan.
                //     User-specified 200 units: far enough to catch most
                //     enemy-drop distances, short enough not to distract.
                // kItemGraceFrames — human-first-pick window. A drop isn't
                //     eligible until it has been observed for this many
                //     ticks; lets the leader grab it if they want to.
                // kItemCollectTimeout — walking timeout inside COLLECT_ITEM.
                //     Walking from kEngageRange (350) → item at ~4 u/frame
                //     is < 90 frames; 300 gives plenty of slack for
                //     collision mishaps. Drops back to RETURN on expiry.
                static constexpr f32 kItemProximity      = 200.0f;
                static constexpr int kItemGraceFrames    = 180;
                static constexpr int kItemCollectTimeout = 300;
                // G13 — boss scenes that warrant pre-emptive teleport on leader entry.
                // Only Deku Tree boss is in scope for the first dungeon demo (#167);
                // extend this list as later dungeons land.
                static constexpr s16 kBossScenes[] = { /* SCENE_DEKU_TREE_BOSS */ 0x11 };
                auto IsBossScene = [&](s16 scene) -> bool {
                    for (s16 s : kBossScenes) { if (s == scene) return true; }
                    return false;
                };
                // G4 — enemies that require shield-reflect to defeat. ENGAGE routes
                // to BLOCK instead of ATTACK when the target is one of these.
                static constexpr s16 kShieldReflectEnemyIds[] = { ACTOR_EN_DEKUNUTS };
                auto IsShieldReflectEnemy = [&](s16 id) -> bool {
                    for (s16 e : kShieldReflectEnemyIds) { if (e == id) return true; }
                    return false;
                };
                // G6/G7/G8 — enemies that require ranged attack (slingshot/bow).
                // ENGAGE routes to RANGED_ATTACK when the target is one of these
                // AND the target is above Link's sword vertical reach (see Fix 2,
                // 2026-04-22). Previously gated on |Δy| >= kMaxYDelta=120, which
                // was far too loose — Link's sword vertical reach is ~30 units,
                // so a Skullwalltula at Δy=118 still slipped through into ATTACK
                // and the follower swung at empty air for 60 frames (P2 log 67,
                // 15:21:03).
                static constexpr f32 kSwordVerticalReach = 40.0f;
                static constexpr s16 kRangedRequiredEnemyIds[] = {
                    ACTOR_BOSS_GOMA, // Queen Gohma — ceiling phase
                    ACTOR_EN_GOMA,   // Gohma larvae on the ceiling
                    ACTOR_EN_SW,     // Skullwalltula on a wall vine
                    ACTOR_EN_ST,     // Skulltula hanging from ceiling on its thread (Fix 2)
                };
                auto IsRangedRequiredEnemy = [&](s16 id) -> bool {
                    for (s16 e : kRangedRequiredEnemyIds) { if (e == id) return true; }
                    return false;
                };

                // Bug D (combat upgrade) — per-enemy approach distance.
                // Default kAttackRange (80) stops Link at sword-tip contact,
                // which is fine for Stalfos-class melee but walks the
                // follower straight into the lunge arc of enemies whose
                // damage volume sits ahead of world.pos (Karebaba head,
                // Deku Baba stem-tip, Bari body-AoE). Override per actor id.
                // The override is used BOTH for ENGAGE→ATTACK admission and
                // for the point-blank shield trigger (see SwingReach below).
                auto GetAttackRangeForEnemy = [](s16 id) -> f32 {
                    switch (id) {
                        case ACTOR_EN_KAREBABA: return 110.0f; // head lunges ~40 u
                        case ACTOR_EN_DEKUBABA: return 100.0f; // stem-tip head
                        case ACTOR_EN_VALI:     return 120.0f; // body AoE discharge
                        default:                return 80.0f;  // kAttackRange
                    }
                };
                // Sword arc reach — Link's effective swing distance. Inside
                // this radius we switch to shield-up-between-swings; outside
                // it, the follower walks forward during the swing-cycle gap.
                static constexpr f32 kSwingReach = 50.0f;

                // Item pickup — need-gated whitelist. Returns true only if
                // the follower has a legitimate use for this ITEM00_* type
                // AND it's not a class reserved for the leader (keys,
                // ammo, heart pieces, etc.). After EnItem00_Init runs,
                // actor.params is masked down to the ITEM00_* enum value
                // directly (z_en_item00.c:363 — `this->actor.params &= 0xFF`);
                // still AND with 0xFF to handle the one-frame window
                // between spawn and Init when the 0x3F00 collectible-flag
                // and 0x8000 spawn-type bits are still set.
                auto FollowerWantsItem = [](Actor* item) -> bool {
                    if (item == nullptr || item->id != ACTOR_EN_ITEM00 ||
                        item->update == nullptr) {
                        return false;
                    }
                    s16 itemType = (s16)(item->params & 0xFF);
                    switch (itemType) {
                        // --- Always-collect: rupees are capacity-capped at
                        // the wallet level, so surplus just no-ops.
                        case ITEM00_RUPEE_GREEN:
                        case ITEM00_RUPEE_BLUE:
                        case ITEM00_RUPEE_RED:
                        case ITEM00_RUPEE_ORANGE:
                        case ITEM00_RUPEE_PURPLE:
                            return true;
                        // --- Need-gated recovery.
                        case ITEM00_HEART:
                            return gSaveContext.health < gSaveContext.healthCapacity;
                        case ITEM00_MAGIC_SMALL:
                        case ITEM00_MAGIC_LARGE:
                            return gSaveContext.isMagicAcquired &&
                                   gSaveContext.magic < gSaveContext.magicCapacity;
                        // --- Consumable ammo. Gate on (a) player owns the
                        // weapon/upgrade (CUR_CAPACITY > 0 ⇒ bag acquired)
                        // AND (b) ammo < capacity (OoT silently discards
                        // drops when the bag is full — picking them up
                        // would deprive the human for no gain). Plentiful
                        // in the first dungeon so the human rarely loses
                        // something meaningful.
                        case ITEM00_STICK:
                            return CUR_CAPACITY(UPG_STICKS) > 0 &&
                                   AMMO(ITEM_STICK) < CUR_CAPACITY(UPG_STICKS);
                        case ITEM00_NUTS:
                            return CUR_CAPACITY(UPG_NUTS) > 0 &&
                                   AMMO(ITEM_NUT) < CUR_CAPACITY(UPG_NUTS);
                        case ITEM00_SEEDS:
                            return CUR_CAPACITY(UPG_BULLET_BAG) > 0 &&
                                   AMMO(ITEM_SLINGSHOT) < CUR_CAPACITY(UPG_BULLET_BAG);
                        case ITEM00_ARROWS_SINGLE:
                        case ITEM00_ARROWS_SMALL:
                        case ITEM00_ARROWS_MEDIUM:
                        case ITEM00_ARROWS_LARGE:
                            return CUR_CAPACITY(UPG_QUIVER) > 0 &&
                                   AMMO(ITEM_BOW) < CUR_CAPACITY(UPG_QUIVER);
                        case ITEM00_BOMBS_A:
                        case ITEM00_BOMBS_B:
                        case ITEM00_BOMBS_SPECIAL:
                            return CUR_CAPACITY(UPG_BOMB_BAG) > 0 &&
                                   AMMO(ITEM_BOMB) < CUR_CAPACITY(UPG_BOMB_BAG);
                        case ITEM00_BOMBCHU:
                            // Bombchus have no upgrade slot (fixed 50-cap).
                            // Gate on "player has bombchus in inventory".
                            return INV_CONTENT(ITEM_BOMBCHU) != ITEM_NONE &&
                                   AMMO(ITEM_BOMBCHU) < 50;
                        // --- Reserved for human: progression items, shields,
                        // tunics, keys, heart pieces, flexible-drop resolver.
                        default:
                            return false;
                    }
                };

                // Item pickup — scan ACTORCAT_MISC for eligible En_Item00
                // drops. Maintains itemFirstSeenFrame (grace-period tracker)
                // and returns the nearest eligible in-range item whose
                // grace window has elapsed. Called once per tick from the
                // IDLE/FOLLOW state bodies. Pointer-reuse is handled by
                // purging entries whose key is no longer in the current
                // MISC list.
                auto ScanForItemCandidate = [&]() -> Actor* {
                    // Pass 1: collect current live EN_ITEM00 pointers.
                    std::unordered_set<Actor*> liveItems;
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
                    while (cand != nullptr) {
                        if (cand->id == ACTOR_EN_ITEM00 && cand->update != nullptr) {
                            liveItems.insert(cand);
                        }
                        cand = cand->next;
                    }
                    // Pass 2: purge itemFirstSeenFrame entries whose key is
                    // no longer in the MISC list (item was collected / unloaded).
                    for (auto it = itemFirstSeenFrame.begin();
                         it != itemFirstSeenFrame.end();) {
                        if (liveItems.find(it->first) == liveItems.end()) {
                            it = itemFirstSeenFrame.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    // Pass 3: register newly-seen items + evaluate eligibility.
                    Vec3f selfPos = player->actor.world.pos;
                    Actor* bestItem  = nullptr;
                    f32    bestDistSq = kItemProximity * kItemProximity;
                    for (Actor* item : liveItems) {
                        auto firstIt = itemFirstSeenFrame.find(item);
                        if (firstIt == itemFirstSeenFrame.end()) {
                            itemFirstSeenFrame[item] = followerTickCounter; // arm grace
                            continue;
                        }
                        // Grace check first — cheap int compare before physics math.
                        if (followerTickCounter - firstIt->second < kItemGraceFrames) {
                            continue;
                        }
                        if (!FollowerWantsItem(item)) {
                            continue;
                        }
                        // Same-floor gate (mirrors enemy-target Y gate).
                        if (fabsf(item->world.pos.y - selfPos.y) >= kMaxYDelta) {
                            continue;
                        }
                        // Room-equality check disabled: player->actor.room is
                        // stale across room transitions. See earlier banner.
                        f32 dx = item->world.pos.x - selfPos.x;
                        f32 dz = item->world.pos.z - selfPos.z;
                        f32 d2 = dx * dx + dz * dz;
                        if (d2 < bestDistSq) {
                            bestDistSq = d2;
                            bestItem   = item;
                        }
                    }
                    return bestItem;
                };

                // -----------------------------------------------------------------
                // Room-equality check — DISABLED 2026-04-21.
                //
                // What it did (four sites: IsEligibleLeader, IDLE enemy scan,
                // ENGAGE off-floor/room guard, ATTACK off-floor/room guard):
                // reject any candidate whose actor->room did not match the local
                // player's actor->room. Added originally alongside kMaxYDelta to
                // keep the follower from targeting enemies in a different logical
                // room — e.g. an enemy in the pit beneath the Great Deku Tree
                // entrance, where XZ distance is short but they are physically
                // unreachable.
                //
                // Why it broke combat:
                // OoT's Actor_Spawn (z_actor.c:3394) assigns actor->room =
                // roomCtx.curRoom.num AT SPAWN TIME and never updates it. The
                // Player actor is spawned once per scene and persists across
                // TransitionActor room changes — nothing in the decomp writes
                // to player->actor.room after the initial spawn (verified by
                // searching soh/src for any such assignment: zero hits). So
                // in any multi-room scene (Hyrule Field quadrants, most
                // dungeons past room 0), the Player's room number is stale
                // the moment the player walks through the first transition,
                // and every enemy spawned in a subsequent room fails the
                // equality test. Observed regression: Hyrule Field with 5
                // Karebabas within 80-unit attack range, zero IDLE→ENGAGE
                // events (P2 log 52, 2026-04-21).
                //
                // The kMaxYDelta gate alone handles the original floor-below
                // bug that motivated this check — OoT floor-to-floor vertical
                // separation is always ≫ 120 units in practice.
                //
                // When it would be useful again: single-floor scenes where
                // two rooms are physically adjacent at the same Y level and
                // could be mistakenly targeted through a thin wall within
                // the 350-unit engage range. If such a case surfaces, the
                // correct fix is to compare against a live room source,
                // NOT player->actor.room. Candidates:
                //   - gPlayState->roomCtx.curRoom.num (authoritative current
                //     room number; accept actor->room == -1 as well since
                //     that is the documented "persistent across rooms"
                //     sentinel — see z64actor.h:215).
                //   - A SoH-side room tracker updated from a TransitionActor
                //     hook, stored on the Anchor instance.
                // With either, the four sites below should read e.g.:
                //   s8 curRoom = (s8)gPlayState->roomCtx.curRoom.num;
                //   bool roomOk = (cand->room == curRoom || cand->room == -1);
                // Until then, the lines are commented out rather than
                // deleted so the intent and re-enable path stay discoverable.
                // -----------------------------------------------------------------

                // Movement is driven by stick input injected in ShouldActorUpdate
                // (mirrors how BTN_B drives sword swings). Link's own Player_Update
                // then handles locomotion, wall collisions, ledge-climb, swim,
                // cutscene suspension, etc. The state machine here only computes
                // `followerMoveTarget` — the world-space point ShouldActorUpdate
                // steers toward — and never writes to player->actor.world.pos
                // except in the STUCK fallback (see that case below for rationale).

                // --- Pick a leader DummyPlayer ---
                // Prefer the previously chosen leader (stickiness) if it is still
                // eligible; otherwise scan the DummyPlayer list for the nearest
                // eligible one. Eligibility: same room as the follower, within the
                // vertical gate, not parked out-of-scene at (-9999,-9999,-9999),
                // and the remote client is not itself in follower mode.
                auto IsEligibleLeader = [&](Actor* cand) -> bool {
                    if (cand == nullptr || cand->update != (ActorFunc)DummyPlayer_Update) {
                        return false;
                    }
                    if (cand->id != ACTOR_EN_OE2) { return false; }
                    if (cand->world.pos.x < -9000.0f) { return false; } // out-of-scene sentinel
                    // Room-equality check DISABLED — see banner note above the state machine.
                    // if (cand->room != player->actor.room) { return false; }
                    uint32_t cid = GetDummyPlayerClientId(cand);
                    if (cid == 0) { return false; }
                    auto it = clients.find(cid);
                    if (it != clients.end() && it->second.followerActive) {
                        return false; // don't follow another follower
                    }
                    // Bug 6 (2026-04-22) — Y-eligibility check REMOVED.
                    // Previously gated on |Δy| < kMaxYDelta with a Fix 1
                    // carve-out for isClimbing leaders. The carve-out was
                    // necessary because tall ladders/vines lift the leader
                    // out of the band within ~1 s, dropping the leader and
                    // stranding the follower (log 67). Removing the gate
                    // entirely is cleaner: the follower's stick-driven
                    // navigation will hit walls / floors / ceilings naturally
                    // (Link's collisions stop him), and G10 (now 3D-distance)
                    // / G12 (stuck-cycle) catch the unreachable case.
                    // kMaxYDelta still gates ENGAGE/IDLE enemy targeting.
                    return true;
                };

                Actor* leaderActor = nullptr;
                if (followerLeaderClientId != 0) {
                    // Sticky path: re-find last leader's actor and check eligibility.
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (cand != nullptr) {
                        if (cand->id == ACTOR_EN_OE2 &&
                            cand->update == (ActorFunc)DummyPlayer_Update &&
                            GetDummyPlayerClientId(cand) == followerLeaderClientId) {
                            if (IsEligibleLeader(cand)) { leaderActor = cand; }
                            break;
                        }
                        cand = cand->next;
                    }
                    if (leaderActor == nullptr) {
                        followerLeaderClientId = 0; // release stickiness, re-scan below
                    }
                }
                if (leaderActor == nullptr) {
                    // Scan for nearest eligible DummyPlayer (any client, not just host).
                    Actor* nearestLeader  = nullptr;
                    f32    nearestDistSq  = 1.0e18f; // effectively unbounded for XZ world distances
                    Vec3f  selfPos        = player->actor.world.pos;
                    Actor* cand = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
                    while (cand != nullptr) {
                        if (IsEligibleLeader(cand)) {
                            f32 dx = cand->world.pos.x - selfPos.x;
                            f32 dz = cand->world.pos.z - selfPos.z;
                            f32 d2 = dx * dx + dz * dz;
                            if (d2 < nearestDistSq) {
                                nearestDistSq = d2;
                                nearestLeader = cand;
                            }
                        }
                        cand = cand->next;
                    }
                    if (nearestLeader != nullptr) {
                        leaderActor = nearestLeader;
                        followerLeaderClientId = GetDummyPlayerClientId(nearestLeader);
                        SPDLOG_INFO("[Follower] Leader selected clientId={} pos=({:.0f},{:.0f},{:.0f})",
                                    followerLeaderClientId,
                                    nearestLeader->world.pos.x,
                                    nearestLeader->world.pos.y,
                                    nearestLeader->world.pos.z);
                    }
                }
                // No eligible leader — stay in IDLE and wait. Do not cancel
                // follower mode; the user may be the only active player.
                if (leaderActor == nullptr) {
                    if (followerAIState != FollowerAIState::IDLE) {
                        followerAIState     = FollowerAIState::IDLE;
                        followerStateFrames = 0;
                        SPDLOG_INFO("[Follower] No eligible leader — reverting to IDLE");
                    }
                    return;
                }

                Actor* dummyActor = leaderActor;                          // preserved name for downstream reads
                Vec3f  leaderPos  = leaderActor->world.pos;
                Vec3f  sideTarget = { leaderPos.x + kFollowOffset, leaderPos.y, leaderPos.z };

                // Yaw toward (dx, dz).  Math_Atan2S(x, y) with OoT param order.
                auto YawToward = [](f32 dx, f32 dz) -> s16 {
                    return Math_Atan2S(dz, dx); // z first, x second — OoT convention
                };

                // Bug B (log 69) — cross-room teleport helper. Plain
                // world.pos=leaderPos teleports do not update OoT's
                // roomCtx.curRoom.num, so teleporting into a different room
                // leaves the game's collision/actor context stuck in the
                // old room. Every subsequent frame G11 re-detects the
                // divergence and re-arms the handoff, producing the
                // infinite-loop symptom from log 69.
                //
                // This helper decides:
                //   (a) Same room or no leader clients entry — plain
                //       world.pos write suffices (no room transition needed).
                //   (b) Different room / scene — drive OoT through its
                //       respawn pipeline (RESPAWN_MODE_DOWN + respawnFlag=1 +
                //       same-scene TRANS_TRIGGER_START). func_8009728C reads
                //       roomIndex from respawn[respawnFlag-1], and
                //       Player_Init copies pos/yaw. Well-exercised engine
                //       path (void-out / Farore's Wind). Handles Deku Tree
                //       basement / Mad Scrub / other non-entrance-accessible
                //       rooms that a raw entrance-index reload cannot reach.
                //
                // Returns true if a scene transition was triggered (caller
                // should return from the follower hook since OoT owns the
                // next frames).
                auto TeleportToLeader = [&](const char* reason) -> bool {
                    Vec3f destPos = leaderPos;
                    s16   destYaw = leaderActor->shape.rot.y;
                    auto  it      = clients.find(followerLeaderClientId);
                    s8    ourRoom    = (s8)gPlayState->roomCtx.curRoom.num;
                    s8    leaderRoom = (it != clients.end()) ? it->second.curRoomNum : ourRoom;
                    bool  roomsDiffer = (leaderRoom != -1 && ourRoom != -1 && leaderRoom != ourRoom);
                    if (!roomsDiffer) {
                        player->actor.world.pos = destPos;
                        player->actor.prevPos   = destPos;
                        SPDLOG_INFO("[Follower] Teleport world.pos ({}) — same room {}", reason, (int)ourRoom);
                        return false;
                    }
                    SPDLOG_WARN("[Follower] Teleport scene-reload ({}) — ours-room={} leader-room={} "
                                "pos={:.0f},{:.0f},{:.0f}",
                                reason, (int)ourRoom, (int)leaderRoom,
                                destPos.x, destPos.y, destPos.z);
                    // Play_SetRespawnData is static to z_play.c; inline the
                    // struct writes rather than plumb a forward declaration.
                    RespawnData* rd = &gSaveContext.respawn[RESPAWN_MODE_DOWN];
                    rd->entranceIndex    = gSaveContext.entranceIndex;
                    rd->roomIndex        = (s16)leaderRoom;
                    rd->pos              = destPos;
                    rd->yaw              = destYaw;
                    rd->playerParams     = 0x0DFF;  // normal-spawn player-params
                    rd->tempSwchFlags    = gPlayState->actorCtx.flags.tempSwch;
                    rd->tempCollectFlags = gPlayState->actorCtx.flags.tempCollect;
                    gSaveContext.respawnFlag        = 1; // RESPAWN_MODE_DOWN + 1
                    gPlayState->transitionTrigger   = TRANS_TRIGGER_START;
                    gPlayState->nextEntranceIndex   = gSaveContext.entranceIndex;
                    gPlayState->transitionType      = TRANS_TYPE_FADE_BLACK;
                    return true;
                };

                // p2Pos is a READ-ONLY snapshot of the follower's current position,
                // taken at the top of the state-machine block for distance/transition
                // checks. Under stick-input movement, the state machine no longer
                // writes p2Pos back to player->actor.world.pos — Link's own
                // Player_Update moves him in response to the stick injected in
                // ShouldActorUpdate. The only path that now writes to
                // player->actor.world.pos is the STUCK fallback (see that case).
                Vec3f p2Pos = player->actor.world.pos;

                // -----------------------------------------------------------------
                // Top-of-hook safety nets (Batch A — G10, G11, G12 escalation, G13).
                //
                // Run BEFORE the state machine so they apply uniformly regardless
                // of which state the follower is in. Each writes player->actor.world.pos
                // directly under specific failure conditions — these are bounded
                // exceptions to the "STUCK is the only world.pos writer" rule
                // documented in the state machine block below.
                // -----------------------------------------------------------------

                // Phase C — pending SCENE_TRANSITION_HANDOFF replay.
                // Runs BEFORE G11 so the follower doesn't get deactivated while
                // navigating toward the trigger point. Three outcomes:
                //   (a) our sceneNum has already changed to (or past) the
                //       leader's — packet is stale; clear and fall through.
                //   (b) still in the from-scene AND within proximity of the
                //       trigger — fire our own transition (set
                //       nextEntranceIndex + transitionTrigger) and clear.
                //   (c) still in the from-scene but too far from the trigger —
                //       point followerMoveTarget at triggerPos so the state
                //       machine walks us there. Decrement timeout.
                bool pendingTransitionInFlight = false;
                if (hasPendingTransition) {
                    s16 ourScene = (s16)gPlayState->sceneNum;
                    if (ourScene != pendingTransitionFromScene) {
                        // We moved on without using the handoff (user walked
                        // manually, or we already fired the transition last
                        // frame). Drop it.
                        SPDLOG_INFO("[Follower] Pending transition cleared — scene already changed "
                                    "(ours=0x{:02X} packet.fromScene=0x{:02X})",
                                    (int)ourScene, (int)pendingTransitionFromScene);
                        hasPendingTransition           = false;
                        pendingTransitionTimeoutFrames = 0;
                    } else {
                        static constexpr f32 kHandoffProximity = 60.0f;
                        f32 dx = pendingTransitionPos.x - p2Pos.x;
                        f32 dz = pendingTransitionPos.z - p2Pos.z;
                        f32 d2 = dx * dx + dz * dz;
                        if (d2 < kHandoffProximity * kHandoffProximity) {
                            SPDLOG_INFO("[Follower] Pending transition firing — entering scene via "
                                        "entrance 0x{:04X} (from scene 0x{:02X})",
                                        (int)(u16)pendingTransitionEntrance,
                                        (int)pendingTransitionFromScene);
                            gPlayState->nextEntranceIndex = pendingTransitionEntrance;
                            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
                            hasPendingTransition           = false;
                            pendingTransitionTimeoutFrames = 0;
                            return; // scene load owns the next frames
                        } else {
                            // Navigate to the trigger. Force the state machine
                            // to walk toward the door/trigger point by routing
                            // through FOLLOW with an overridden move target.
                            pendingTransitionInFlight = true;
                            followerMoveTarget = pendingTransitionPos;
                            if (followerAIState == FollowerAIState::IDLE) {
                                followerAIState     = FollowerAIState::FOLLOW;
                                followerStateFrames = 0;
                                followerLastPos     = p2Pos;
                                SPDLOG_INFO("[Follower] IDLE→FOLLOW (toward pending transition trigger at "
                                            "{:.0f},{:.0f},{:.0f}, dist={:.0f})",
                                            pendingTransitionPos.x, pendingTransitionPos.y,
                                            pendingTransitionPos.z, sqrtf(d2));
                            }
                        }
                        if (pendingTransitionTimeoutFrames > 0) {
                            pendingTransitionTimeoutFrames--;
                            if (pendingTransitionTimeoutFrames == 0) {
                                SPDLOG_WARN("[Follower] Pending transition TIMEOUT — leader is gone, "
                                            "can't reach trigger. Deactivating.");
                                hasPendingTransition = false;
                                SetFollowerActive(false);
                                return;
                            }
                        }
                    }
                }

                // G11/G13 — leader crossed a scene or room boundary.
                // Leader's scene/room is broadcast via UPDATE_CLIENT_STATE; if it
                // diverges from ours, we either teleport (boss scene per G13),
                // deactivate (different scene per G11), or initiate a door-
                // handoff walk-through (same scene different room, Bug 7 phase B).
                //
                // SUPPRESSED when a pending SCENE_TRANSITION_HANDOFF is in
                // flight (phase C): the packet already tells us exactly where
                // to go and which entrance to use. Deactivating here would
                // stop the navigation before we reach the trigger.
                if (!pendingTransitionInFlight) {
                    auto it = clients.find(followerLeaderClientId);
                    if (it != clients.end() && it->second.isSaveLoaded) {
                        s16 leaderScene = it->second.sceneNum;
                        s8  leaderRoom  = it->second.curRoomNum;
                        s16 ourScene    = (s16)gPlayState->sceneNum;
                        s8  ourRoom     = (s8)gPlayState->roomCtx.curRoom.num;

                        // Shadow-track the leader's position while we share a
                        // room. When they cross a door and leave the room, the
                        // follower walks toward this cached point to find the
                        // same door, then teleports on timeout if it fails.
                        if (leaderScene == ourScene && leaderRoom == ourRoom) {
                            followerLeaderLastInOurRoom = leaderPos;
                            // Rooms re-synced while a handoff was in flight:
                            // our follower crossed the door successfully.
                            if (followerDoorHandoff) {
                                SPDLOG_INFO("[Follower] Door handoff complete — room re-synced (ours={})",
                                            (int)ourRoom);
                                followerDoorHandoff       = false;
                                followerDoorHandoffFrames = 0;
                            }
                        }

                        if (leaderScene != ourScene) {
                            if (IsBossScene(leaderScene)) {
                                // G13 — historically we deactivated here. With
                                // SCENE_TRANSITION_HANDOFF active, the leader's
                                // handoff packet is what carries the follower
                                // through the boss door. G13 only fires now if
                                // the leader entered the boss scene WITHOUT
                                // the handoff packet reaching us (packet
                                // dropped, or leader's build predates the
                                // packet). In that case, deactivate with the
                                // same fallback behaviour as before.
                                SPDLOG_WARN("[Follower] Leader entered boss scene 0x{:02X} without handoff — "
                                            "deactivating (walk through the door manually, then re-enable)",
                                            leaderScene);
                            } else {
                                SPDLOG_WARN("[Follower] Leader in different scene (ours=0x{:02X} leader=0x{:02X}) "
                                            "— deactivating; walk through the door manually",
                                            ourScene, leaderScene);
                            }
                            SetFollowerActive(false);
                            return;
                        }

                        // Same scene, different room: Bug 7 phase B — initiate
                        // a door handoff. The follower walks toward the leader's
                        // last-seen position in our room (usually a door
                        // threshold) and Phase A's DO_ACTION_ENTER injection
                        // triggers the door animation. If we can't reach the
                        // door within kDoorHandoffTimeout frames, teleport to
                        // the leader (they may already be deep in the next room).
                        if (leaderRoom != ourRoom && leaderRoom != -1 && ourRoom != -1) {
                            if (!followerDoorHandoff) {
                                followerDoorHandoff       = true;
                                followerDoorHandoffFrames = kDoorHandoffTimeout;
                                SPDLOG_INFO("[Follower] Leader crossed room boundary (ours={} leader={}) "
                                            "— door handoff armed (target={:.0f},{:.0f},{:.0f}, timeout={} frames)",
                                            (int)ourRoom, (int)leaderRoom,
                                            followerLeaderLastInOurRoom.x,
                                            followerLeaderLastInOurRoom.y,
                                            followerLeaderLastInOurRoom.z,
                                            kDoorHandoffTimeout);
                            }

                            // Route the follower to the door threshold. Reuse
                            // FOLLOW as the navigation state; its approach
                            // logic already knows how to walk toward a target.
                            followerMoveTarget = followerLeaderLastInOurRoom;
                            if (followerAIState == FollowerAIState::IDLE) {
                                followerAIState     = FollowerAIState::FOLLOW;
                                followerStateFrames = 0;
                                followerLastPos     = p2Pos;
                            }

                            if (followerDoorHandoffFrames > 0) {
                                followerDoorHandoffFrames--;
                                if (followerDoorHandoffFrames == 0) {
                                    SPDLOG_WARN("[Follower] Door handoff TIMEOUT "
                                                "(ours-room={} leader-room={})",
                                                (int)ourRoom, (int)leaderRoom);
                                    bool triggered = TeleportToLeader("G11 handoff timeout");
                                    followerDoorHandoff       = false;
                                    followerOverrunFrames     = 0;
                                    followerAIState           = FollowerAIState::IDLE;
                                    followerStateFrames       = 0;
                                    if (triggered) {
                                        return; // scene transition owns the next frames
                                    }
                                }
                            }
                        }
                    }
                }

                // G10 — leash-timeout teleport. If the follower has been more
                // than kTeleportThreshold from the leader for kTeleportDelayFrames
                // continuous frames, teleport to the leader. Catches stuck-in-
                // geometry / fell-behind / can't-traverse scenarios that the
                // state machine couldn't recover from.
                //
                // Bug 6 (2026-04-22): now uses 3D distance (was XZ-only).
                // With Y-eligibility removed from IsEligibleLeader, the
                // "leader on a different floor" case falls through to here
                // — Δy can be the dominant component. Treating XZ-only would
                // miss that case entirely (log 68: P1 on floor above P2,
                // 30 + s, no teleport fired).
                {
                    f32 dxL = leaderPos.x - p2Pos.x;
                    f32 dyL = leaderPos.y - p2Pos.y;
                    f32 dzL = leaderPos.z - p2Pos.z;
                    if (dxL * dxL + dyL * dyL + dzL * dzL > kTeleportThreshold * kTeleportThreshold) {
                        followerOverrunFrames++;
                        if (followerOverrunFrames >= kTeleportDelayFrames) {
                            // Bug B (log 69) — route through TeleportToLeader
                            // so cross-room overruns use the scene-reload path
                            // rather than a raw world.pos write.
                            bool triggered = TeleportToLeader("G10 3D leash overrun");
                            followerOverrunFrames = 0;
                            followerAIState       = FollowerAIState::IDLE;
                            followerStateFrames   = 0;
                            if (triggered) {
                                return;
                            }
                        }
                    } else {
                        followerOverrunFrames = 0;
                    }
                }

                // G12 — STUCK escalation teleport. If the follower has entered
                // STUCK kStuckCycleEscalation times within kStuckCycleWindow,
                // bail to a teleport. Counter is incremented at the FOLLOW→STUCK
                // transition below; window is reset when we reach IDLE cleanly.
                if (followerStuckCycleCount >= kStuckCycleEscalation) {
                    // Bug B (log 69) — route through TeleportToLeader for
                    // cross-room-safe teleport. Always return regardless of
                    // mode: STUCK escalation is a terminal reset.
                    TeleportToLeader("G12 stuck-cycle escalation");
                    followerStuckCycleCount       = 0;
                    followerStuckCycleResetFrames = 0;
                    followerOverrunFrames         = 0;
                    followerAIState               = FollowerAIState::IDLE;
                    followerStateFrames           = 0;
                    return;
                }

                // G1/G2 — leader is climbing a vine/ladder. Bug 2 redesign
                // (2026-04-22): no longer teleport-and-ride. Instead, teleport
                // follower to leader's XZ (the ladder base) at follower's
                // current Y, then let the CLIMBING state inject stick_y so
                // Link's own state machine grabs the ladder and climbs
                // naturally. This produces the real climb animation, real
                // physics, and avoids the gravity-fight "hover slightly below
                // P1" symptom from log 68.
                //
                // Edge-triggered: only enter CLIMBING if we aren't already
                // there. The XZ-only teleport makes follower adjacent to the
                // ladder rim; subsequent stick_y forward injection causes the
                // ladder collider to attach Link.
                {
                    auto it = clients.find(followerLeaderClientId);
                    if (it != clients.end() && it->second.isClimbing &&
                        followerAIState != FollowerAIState::CLIMBING) {
                        // Snap to leader's XZ but keep follower's Y. If
                        // follower is already higher (leader climbing down to
                        // us), we don't drop them; if follower is lower (the
                        // common case), they're now at the ladder base.
                        Vec3f ladderXz = { leaderPos.x, p2Pos.y, leaderPos.z };
                        player->actor.world.pos = ladderXz;
                        player->actor.prevPos   = ladderXz;
                        followerAIState     = FollowerAIState::CLIMBING;
                        followerStateFrames = 0;
                        SPDLOG_INFO("[Follower] Leader started climbing → CLIMBING "
                                    "(snap to ladder XZ at {:.0f},{:.0f},{:.0f})",
                                    ladderXz.x, ladderXz.y, ladderXz.z);
                        // Refresh p2Pos snapshot since we just moved.
                        p2Pos = player->actor.world.pos;
                    }
                }

                followerStateFrames++;

                // Periodic heartbeat: log state + positions every 60 frames.
                if (followerStateFrames % 60 == 0) {
                    f32 toTarget = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                    const char* stateStr = "?";
                    switch (followerAIState) {
                        case FollowerAIState::IDLE:          stateStr = "IDLE";          break;
                        case FollowerAIState::FOLLOW:        stateStr = "FOLLOW";        break;
                        case FollowerAIState::STUCK:         stateStr = "STUCK";         break;
                        case FollowerAIState::ENGAGE:        stateStr = "ENGAGE";        break;
                        case FollowerAIState::ATTACK:        stateStr = "ATTACK";        break;
                        case FollowerAIState::RETURN:        stateStr = "RETURN";        break;
                        case FollowerAIState::CLIMBING:      stateStr = "CLIMBING";      break;
                        case FollowerAIState::BLOCK:         stateStr = "BLOCK";         break;
                        case FollowerAIState::RANGED_ATTACK: stateStr = "RANGED_ATTACK"; break;
                        case FollowerAIState::STANDBY:       stateStr = "STANDBY";       break;
                        case FollowerAIState::COLLECT_ITEM:  stateStr = "COLLECT_ITEM";  break;
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
                        // Reject enemies on a different vertical level — the
                        // follower only moves in XZ, so targets on another floor
                        // (e.g. a room below the Deku Tree entrance) otherwise
                        // cause it to walk into walls and swing at air.
                        // (Room-equality check disabled — see banner note above.)
                        Actor* nearest    = nullptr;
                        f32    nearDistSq = kEngageRange * kEngageRange;
                        Actor* eActor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
                        while (eActor != nullptr) {
                            if (eActor->update != nullptr &&
                                /* eActor->room == player->actor.room && */
                                fabsf(eActor->world.pos.y - p2Pos.y) < kMaxYDelta) {
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
                            break;
                        }
                        // Item pickup — no enemy to engage; scan for eligible drops.
                        // Grace/filter/Y-gate are all handled inside ScanForItemCandidate.
                        {
                            Actor* item = ScanForItemCandidate();
                            if (item != nullptr) {
                                followerTargetItem = item;
                                followerCollectItemTimeoutFrames = kItemCollectTimeout;
                                followerAIState     = FollowerAIState::COLLECT_ITEM;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] IDLE→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                                            (int)(item->params & 0xFF),
                                            item->world.pos.x, item->world.pos.y, item->world.pos.z);
                                break;
                            }
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
                                // Stick input failed to make progress. Enter the
                                // STUCK fallback, which nudges the follower
                                // directly toward followerMoveTarget via
                                // position override until kStuckRecovery frames
                                // elapse. (followerStuckDir is no longer used:
                                // the perpendicular strafe pattern was dropped
                                // when movement switched to stick input. Field
                                // kept in the header for a future strafe variant.)
                                followerAIState     = FollowerAIState::STUCK;
                                followerStuckFrames = 0;
                                followerStateFrames = 0;
                                // G12 — count this entry; arm the reset window.
                                // The top-of-hook check escalates to teleport when
                                // count >= kStuckCycleEscalation within the window.
                                followerStuckCycleCount++;
                                followerStuckCycleResetFrames = kStuckCycleWindow;
                                SPDLOG_INFO("[Follower] FOLLOW→STUCK (stick input stalled, cycle={})",
                                            followerStuckCycleCount);
                                break;
                            }
                        }
                        // Item pickup — scan every 10 frames inside FOLLOW (less
                        // frequent than IDLE; we're actively traversing so a
                        // tight scan window is less useful). On finding an
                        // eligible drop, abandon FOLLOW and divert to COLLECT_ITEM.
                        if (followerStateFrames % 10 == 0) {
                            Actor* item = ScanForItemCandidate();
                            if (item != nullptr) {
                                followerTargetItem = item;
                                followerCollectItemTimeoutFrames = kItemCollectTimeout;
                                followerAIState     = FollowerAIState::COLLECT_ITEM;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] FOLLOW→COLLECT_ITEM item=0x{:02X} at ({:.0f},{:.0f},{:.0f})",
                                            (int)(item->params & 0xFF),
                                            item->world.pos.x, item->world.pos.y, item->world.pos.z);
                                break;
                            }
                        }
                        followerMoveTarget = sideTarget;
                        {
                            f32 dist = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                            // Stick injection in ShouldActorUpdate drives actual movement;
                            // here we just transition when we're close enough.
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
                        }
                        break;
                    }

                    case FollowerAIState::STUCK: {
                        // Fallback path: stick-input hit a wall / corner / doorway
                        // the simulation can't navigate. Apply a small position nudge
                        // directly toward followerMoveTarget for up to kStuckRecovery
                        // frames. This bypasses Link's physics just enough to get
                        // past the obstacle. Stick injection stays active in this
                        // state (see ShouldActorUpdate) so Link's legs still try to
                        // walk — the nudge is additive, not a replacement.
                        // This is the ONLY path in the follower state machine that
                        // writes to player->actor.world.pos in the stick-input design.
                        followerStuckFrames++;
                        f32 ndx = followerMoveTarget.x - player->actor.world.pos.x;
                        f32 ndz = followerMoveTarget.z - player->actor.world.pos.z;
                        f32 nd  = sqrtf(ndx * ndx + ndz * ndz);
                        if (nd > 0.001f) {
                            f32 step = (nd < kMoveSpeed) ? nd : kMoveSpeed;
                            player->actor.world.pos.x += ndx / nd * step;
                            player->actor.world.pos.z += ndz / nd * step;
                        }
                        if (followerStuckFrames >= kStuckRecovery) {
                            followerAIState     = FollowerAIState::FOLLOW;
                            followerLastPos     = player->actor.world.pos;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] STUCK→FOLLOW (fallback nudge complete)");
                        }
                        break;
                    }

                    case FollowerAIState::ENGAGE: {
                        // Abandon if leader is too far or target is gone.
                        {
                            f32 ldx = leaderPos.x - p2Pos.x;
                            f32 ldz = leaderPos.z - p2Pos.z;
                            if (ldx * ldx + ldz * ldz > kMaxLeash * kMaxLeash) {
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RETURN (leader too far)");
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
                        // Vertical-reach handling. Three layered checks:
                        //  1. Cross-floor (|Δy| >= kMaxYDelta, 120 units): target
                        //     is on a different logical level. If it's ranged-
                        //     required, route to RANGED_ATTACK; otherwise bail.
                        //  2. Above sword reach but same-floor (Δy > kSwordVerticalReach,
                        //     40 units) AND ranged-required: route to RANGED_ATTACK.
                        //     Fix 2 (2026-04-22) — before this check existed, a
                        //     Skullwalltula at Δy=118 (just under kMaxYDelta) was
                        //     routed to ATTACK and the follower whiffed for the
                        //     full 60-frame cycle (P2 log 67, 15:21:03).
                        //  3. Otherwise fall through to XZ close + ATTACK.
                        // (Room-equality side of this check disabled — see banner note above.)
                        {
                            f32 dy = followerTargetEnemy->world.pos.y - p2Pos.y;
                            if (fabsf(dy) >= kMaxYDelta) {
                                if (IsRangedRequiredEnemy(followerTargetEnemy->id)) {
                                    FollowerTryEquipRangedWeapon();
                                    followerAIState     = FollowerAIState::RANGED_ATTACK;
                                    followerStateFrames = 0;
                                    SPDLOG_INFO("[Follower] ENGAGE→RANGED_ATTACK (off-floor target id={})",
                                                followerTargetEnemy->id);
                                    break;
                                }
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RETURN (enemy off-floor)");
                                break;
                            }
                            if (dy > kSwordVerticalReach &&
                                IsRangedRequiredEnemy(followerTargetEnemy->id)) {
                                FollowerTryEquipRangedWeapon();
                                followerAIState     = FollowerAIState::RANGED_ATTACK;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→RANGED_ATTACK (above sword reach Δy={:.0f} target id={})",
                                            dy, followerTargetEnemy->id);
                                break;
                            }
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        f32   edx      = enemyPos.x - p2Pos.x;
                        f32   edz      = enemyPos.z - p2Pos.z;
                        f32   distSq   = edx * edx + edz * edz;
                        // Bug D — per-enemy attackRange keeps the follower
                        // outside lunge arcs of enemies whose damage volume
                        // sits ahead of world.pos.
                        f32   attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
                        if (distSq < attackRange * attackRange) {
                            // G4 — Mad Scrub class: shield first, then swing on
                            // the stunned scrub. BLOCK→ATTACK is wired in BLOCK.
                            if (IsShieldReflectEnemy(followerTargetEnemy->id)) {
                                followerAIState     = FollowerAIState::BLOCK;
                                followerStateFrames = 0;
                                SPDLOG_INFO("[Follower] ENGAGE→BLOCK (shield-reflect target id={})",
                                            followerTargetEnemy->id);
                                break;
                            }
                            followerAIState     = FollowerAIState::ATTACK;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ENGAGE→ATTACK enemy=({:.0f},{:.0f},{:.0f}) dist={:.0f} "
                                        "range={:.0f} id={}",
                                        enemyPos.x, enemyPos.y, enemyPos.z, sqrtf(distSq),
                                        attackRange, followerTargetEnemy->id);
                            break;
                        }
                        // Every 20 frames log distance to enemy so we can see approach progress.
                        if (followerStateFrames % 20 == 0) {
                            SPDLOG_INFO("[Follower] ENGAGE progress: distToEnemy={:.0f} p2=({:.0f},{:.0f})",
                                        sqrtf(distSq), p2Pos.x, p2Pos.z);
                        }
                        followerMoveTarget = enemyPos;
                        // Stick injection in ShouldActorUpdate drives actual movement.
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
                        // Task 3 — stop swinging when the target is defeated.
                        // Two complementary signals because OoT doesn't have one
                        // universal "dead" field:
                        //   (a) colChkInfo.health <= 0 — catches actors that
                        //       decrement their own health (Dekubaba, En_Ba,
                        //       most bosses — ~14 overlays total).
                        //   (b) EnemyNetId::hasLocalDeath / pendingNaturalDeath —
                        //       covers the AC_HIT-only pattern (Karebaba,
                        //       En_Firefly, En_St, most Phase-4A enemies) where
                        //       health is initialised once in sColCheckInfoInit
                        //       and never written again. Their death is signalled
                        //       by the collision AC_HIT flag driving SetupDying,
                        //       and our OnEnemyDefeat / HandlePacket_EnemyDefeated
                        //       paths flip these flags on the EnemyNetId extension.
                        // Initial Task 3 implementation used only (a) and was a
                        // no-op for Karebaba (health stays at 1 through the entire
                        // Dying cycle, P2 log 62 2026-04-21).
                        bool targetDefeated = (followerTargetEnemy->colChkInfo.health <= 0);
                        if (!targetDefeated) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr &&
                                (ext->hasLocalDeath || ext->pendingNaturalDeath)) {
                                targetDefeated = true;
                            }
                        }
                        if (targetDefeated) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy dead)");
                            break;
                        }
                        // Room-equality side of this check disabled — see banner note above.
                        if (/* followerTargetEnemy->room != player->actor.room || */
                            fabsf(followerTargetEnemy->world.pos.y - p2Pos.y) >= kMaxYDelta) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] ATTACK→RETURN (enemy off-floor)");
                            break;
                        }
                        Vec3f enemyPos = followerTargetEnemy->world.pos;
                        // Bug D — point followerMoveTarget at a standoff
                        // offset from enemyPos instead of enemyPos itself.
                        // Stopping radius is attackRange - kSwingReach:
                        // sword can still reach (kSwingReach), but Link
                        // holds outside the enemy's damage volume. For
                        // Karebaba (range=110, swing=50), standoff is 60 u
                        // from root — outside the head's lunge arc. For
                        // Stalfos-class (range=80, swing=50), standoff is
                        // 30 u — the original sword-tip contact distance.
                        f32 attackRange = GetAttackRangeForEnemy(followerTargetEnemy->id);
                        f32 standoff    = attackRange - kSwingReach;
                        if (standoff < 20.0f) standoff = 20.0f; // sanity floor
                        {
                            f32 edx      = enemyPos.x - p2Pos.x;
                            f32 edz      = enemyPos.z - p2Pos.z;
                            f32 enemyDistSq = edx * edx + edz * edz;
                            f32 enemyDist   = sqrtf(enemyDistSq);
                            if (enemyDist > 1.0f) {
                                // Move target = enemyPos pulled back toward
                                // the follower by `standoff` units. Avoids
                                // walking into the damage volume even while
                                // the enemy walks toward us.
                                f32 shrink = (enemyDist > standoff)
                                             ? (enemyDist - standoff) / enemyDist
                                             : 0.0f;
                                followerMoveTarget.x = p2Pos.x + edx * shrink;
                                followerMoveTarget.y = enemyPos.y;
                                followerMoveTarget.z = p2Pos.z + edz * shrink;
                            } else {
                                followerMoveTarget = enemyPos;
                            }
                            if (followerStateFrames % 10 == 0) {
                                SPDLOG_INFO("[Follower] ATTACK frame={} distToEnemy={:.0f} "
                                            "standoff={:.0f} p2=({:.0f},{:.0f})",
                                            followerStateFrames, enemyDist, standoff,
                                            p2Pos.x, p2Pos.z);
                            }
                            if (enemyDistSq > 1.0f) {
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
                        f32 dist = sqrtf(SQ(sideTarget.x - p2Pos.x) + SQ(sideTarget.z - p2Pos.z));
                        // Stick injection in ShouldActorUpdate drives actual movement.
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

                    // G1/G2 — leader is climbing. Bug 2 redesign (2026-04-22):
                    // instead of writing world.pos = leaderPos every frame
                    // (which fights gravity between actor-update and our hook,
                    // producing the "hover slightly below leader" symptom),
                    // we point followerMoveTarget at leader's XZ at follower's
                    // current Y (the ladder base / current rung) and let the
                    // ShouldActorUpdate stick injection drive Link.
                    //
                    // The state machine sets a flag (followerOnLadderTarget)
                    // so the stick-inject hook knows to use raw stick_y for
                    // up/down rather than camera-relative XZ projection.
                    // Once Link's PLAYER_STATE1_CLIMBING_LADDER fires (Link
                    // physically grabbed the ladder), stick_y direction
                    // toggles based on Δy to leader: positive (up) if leader
                    // is higher, negative (down) if lower, zero when within
                    // tolerance. OoT plays the real climb animation natively.
                    //
                    // Exit when leader's isClimbing flips back to false. The
                    // top-of-hook re-arm only fires on rising edge so we
                    // don't loop back into CLIMBING if leader's still
                    // sticky-eligible.
                    case FollowerAIState::CLIMBING: {
                        auto it = clients.find(followerLeaderClientId);
                        if (it == clients.end() || !it->second.isClimbing) {
                            // Bug C (log 69) — arm the dismount-forward-hold.
                            // Immediately after CLIMBING→IDLE, follower is on
                            // the rim of the top/bottom floor. Without this
                            // hold, the next-frame state machine recomputes
                            // the move target around leaderPos — and leader
                            // often stands right at the climb exit, so the
                            // follower's stick-math points BACKWARD off the
                            // rim. Snapshot the current facing (set every
                            // frame during CLIMBING to leaderActor->shape.rot.y),
                            // arm the hold counter, then IDLE.
                            followerClimbDismountYaw    = player->actor.shape.rot.y;
                            followerClimbDismountFrames = kClimbDismountHoldFrames;
                            followerAIState     = FollowerAIState::IDLE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] CLIMBING→IDLE (leader stopped climbing); "
                                        "armed dismount forward-hold {} frames at yaw={}",
                                        kClimbDismountHoldFrames,
                                        (int)followerClimbDismountYaw);
                            break;
                        }
                        // followerMoveTarget = leader's XZ at the leader's
                        // current Y. ShouldActorUpdate's CLIMBING-aware
                        // injection reads this for direction (leader.y vs
                        // p2Pos.y).
                        followerMoveTarget = leaderPos;
                        // Match leader's facing so dismount looks clean.
                        player->actor.shape.rot.y = leaderActor->shape.rot.y;
                        break;
                    }

                    // G4 — shield reflect. Inject BTN_R while ENGAGE target is a
                    // known shield-reflect class (Mad Scrub). Movement freezes
                    // (no stick) so Link plants the shield. Returns to ENGAGE
                    // when target leaves the reflect-class window or is defeated.
                    case FollowerAIState::BLOCK: {
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] BLOCK→RETURN (target gone)");
                            break;
                        }
                        // shape.rot.y points at target so the shield faces the
                        // incoming projectile. Position is held by zeroed stick
                        // (see ShouldActorUpdate isMoving exclusion).
                        f32 ex = followerTargetEnemy->world.pos.x - p2Pos.x;
                        f32 ez = followerTargetEnemy->world.pos.z - p2Pos.z;
                        if (ex * ex + ez * ez > 1.0f) {
                            player->actor.shape.rot.y = YawToward(ex, ez);
                        }
                        // Hold the shield for kAttackDuration frames per cycle,
                        // then drop to ATTACK to swing on the (now-stunned) scrub.
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::ATTACK;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] BLOCK→ATTACK (shield cycle complete)");
                        }
                        break;
                    }

                    // G6/G7/G8 — ranged attack. Inject BTN_Z + BTN_A while ENGAGE
                    // target is a known ranged-required class (Gohma ceiling, larvae,
                    // Skullwalltulas on vines). Movement freezes so Link aims.
                    case FollowerAIState::RANGED_ATTACK: {
                        if (followerTargetEnemy == nullptr ||
                            followerTargetEnemy->update == nullptr) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (target gone)");
                            break;
                        }
                        // Two-signal defeat check, mirroring the ATTACK state.
                        bool defeated = (followerTargetEnemy->colChkInfo.health <= 0);
                        if (!defeated) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr &&
                                (ext->hasLocalDeath || ext->pendingNaturalDeath)) {
                                defeated = true;
                            }
                        }
                        if (defeated) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (target dead)");
                            break;
                        }
                        // Face the target so the slingshot aim line is correct.
                        f32 ex = followerTargetEnemy->world.pos.x - p2Pos.x;
                        f32 ez = followerTargetEnemy->world.pos.z - p2Pos.z;
                        if (ex * ex + ez * ez > 1.0f) {
                            player->actor.shape.rot.y = YawToward(ex, ez);
                        }
                        if (followerStateFrames >= kAttackDuration) {
                            FollowerRestoreItems();
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK→RETURN (cycle complete)");
                        }
                        break;
                    }

                    // Reserved — placeholder for G19 (Gohma weak-point window).
                    // No transitions wired today; ENGAGE never picks STANDBY.
                    case FollowerAIState::STANDBY: {
                        if (followerStateFrames >= kAttackDuration) {
                            followerAIState     = FollowerAIState::ENGAGE;
                            followerStateFrames = 0;
                            SPDLOG_INFO("[Follower] STANDBY→ENGAGE (window expired)");
                        }
                        break;
                    }

                    // Item pickup (Claude/Plans/ai_follower_item_pickup.md).
                    // Walks toward followerTargetItem until pickup fires
                    // (En_Item00 is collision-triggered; contact → collect).
                    // Exit paths:
                    //   - target actor gone (collected by us OR by leader) → RETURN
                    //   - timeout elapsed (couldn't reach) → RETURN
                    //   - leader beyond leash → RETURN (follow takes priority)
                    //   - leader started climbing → let top-of-hook G1/G2 take over
                    //   - item on a different floor (|Δy| ≥ kMaxYDelta) → RETURN
                    case FollowerAIState::COLLECT_ITEM: {
                        if (followerTargetItem == nullptr ||
                            followerTargetItem->update == nullptr) {
                            SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item gone — collected or unloaded)");
                            followerTargetItem  = nullptr;
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            break;
                        }
                        // Leader leash — don't stray too far from the leader
                        // just for a rupee.
                        {
                            f32 lx = leaderPos.x - p2Pos.x;
                            f32 lz = leaderPos.z - p2Pos.z;
                            if (lx * lx + lz * lz > kMaxLeash * kMaxLeash) {
                                SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (leader beyond leash)");
                                followerTargetItem  = nullptr;
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                break;
                            }
                        }
                        // Y-gate — item ended up on a different floor (bounce
                        // off a ledge between grace expiry and pickup start).
                        if (fabsf(followerTargetItem->world.pos.y - p2Pos.y) >= kMaxYDelta) {
                            SPDLOG_INFO("[Follower] COLLECT_ITEM→RETURN (item off-floor)");
                            followerTargetItem  = nullptr;
                            followerAIState     = FollowerAIState::RETURN;
                            followerStateFrames = 0;
                            break;
                        }
                        // Timeout — couldn't reach the item in kItemCollectTimeout
                        // frames (geometry / collision mishap).
                        if (followerCollectItemTimeoutFrames > 0) {
                            followerCollectItemTimeoutFrames--;
                            if (followerCollectItemTimeoutFrames == 0) {
                                SPDLOG_WARN("[Follower] COLLECT_ITEM→RETURN (timeout)");
                                followerTargetItem  = nullptr;
                                followerAIState     = FollowerAIState::RETURN;
                                followerStateFrames = 0;
                                break;
                            }
                        }
                        // Drive ShouldActorUpdate toward the item. En_Item00's
                        // own collision handler attaches to Link on contact —
                        // no BTN_A or other interaction needed for pickup.
                        followerMoveTarget = followerTargetItem->world.pos;
                        {
                            f32 idx = followerTargetItem->world.pos.x - p2Pos.x;
                            f32 idz = followerTargetItem->world.pos.z - p2Pos.z;
                            if (idx * idx + idz * idz > 1.0f) {
                                player->actor.shape.rot.y = YawToward(idx, idz);
                            }
                        }
                        break;
                    }
                }

                // End-of-block position override was intentionally removed when
                // the follower switched to stick-input movement. The only path
                // that now writes to player->actor.world.pos is the STUCK state
                // fallback above — see that case's comment block for rationale.
            });
        }
    }

    // Follower input injection (non-host only).
    //
    // Fires via ShouldActorUpdate immediately BEFORE the player actor's update()
    // so the player's own action state machine sees synthetic input and moves /
    // swings / climbs in response. (OnGameFrameUpdate fires too late — after
    // update() — so inputs written there would miss the current frame.)
    //
    // This hook is the PRIMARY driver of follower movement. The state machine
    // in OnGameFrameUpdate computes `followerMoveTarget`; this hook projects
    // that target into camera-relative stick input and lets Link's own
    // Player_Update carry him there — respecting walls, slopes, ledges,
    // water, cutscenes, and every other state transition OoT handles natively.
    //
    // Walk/run: stick is deflected toward followerMoveTarget with magnitude
    // scaled by distance (sprint > 250 units, run > 60, walk > 30, zero
    // within 30 so Link's own deceleration handles the last few units).
    //
    // State guard: stick is zeroed when Link is in a state that can't accept
    // free movement (ladder climb, ledge hang / climb-up, water, cutscene,
    // hit-react, talking, input disabled). Injecting during these can corrupt
    // the associated state machine.
    //
    // Ledge-climb: BTN_A is injected whenever PLAYER_STATE1_HANGING_OFF_LEDGE
    // is set — the follower runs up to a tall ledge, Link hangs, we press A,
    // Link hoists up. This replaces the old position-override-through-geometry
    // behaviour that clipped through ledges.
    //
    // Attack: BTN_B as an edge-press every 20 frames while in ATTACK state.
    // Stick is ALSO driven during ATTACK so the follower keeps closing the
    // gap between kAttackRange (80) and actual sword reach (~30-40 units);
    // without it the follower stops at 80 and swings at empty air. The stick
    // points at enemyPos, agreeing with shape.rot.y, so swing direction is
    // unambiguous regardless of which field OoT consults on the BTN_B frame.
    //
    // Timing note: ShouldActorUpdate sees followerStateFrames from the PREVIOUS
    // OnGameFrameUpdate (one frame before the next increment).  BTN_B is injected
    // when followerStateFrames % 20 == 0, which corresponds to frame 1, 21, 41
    // inside the ATTACK state after the next increment. The sword swing takes
    // ~20 frames, matching the cycle period.
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
                    // States where we drive locomotion via stick input.
                    // ATTACK included: under stick-input movement the follower
                    // needs to close the last few tens of units between
                    // kAttackRange (80) and actual sword reach (~40). Without
                    // stick injection during ATTACK the follower stops at 80
                    // and swings into empty air (observed 2026-04-21, P2 log 64
                    // — 60-frame cycles with distToEnemy 75-85). Stick points
                    // at enemyPos, so the swing also goes toward the enemy
                    // — OoT reads stick on the BTN_B edge-press frame to set
                    // swing direction, which agrees with the approach direction.
                    bool isMoving = (followerAIState == FollowerAIState::FOLLOW       ||
                                     followerAIState == FollowerAIState::STUCK        ||
                                     followerAIState == FollowerAIState::ENGAGE       ||
                                     followerAIState == FollowerAIState::ATTACK       ||
                                     followerAIState == FollowerAIState::RETURN       ||
                                     followerAIState == FollowerAIState::CLIMBING     ||
                                     followerAIState == FollowerAIState::COLLECT_ITEM);

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

                    // --- State guard — don't inject stick while Link is in a
                    // non-walkable state. Injecting during these can corrupt the
                    // ladder/cutscene state machines. Button presses (BTN_A for
                    // climb) are handled below, separately from stick.
                    //
                    // IN_WATER is intentionally NOT blocked — swimming uses the
                    // same camera-relative stick input as walking, and the
                    // follower needs to be able to swim forward into a ledge
                    // to trigger the water-exit climb-out animation. (Observed
                    // 2026-04-21: blocking IN_WATER left the follower sliding
                    // along the water's edge unable to exit.)
                    Player* player = (Player*)actor;
                    u32 sf1 = player->stateFlags1;
                    u32 sf2 = player->stateFlags2;
                    bool nowOnLadder = (sf1 & PLAYER_STATE1_CLIMBING_LADDER) != 0;
                    // CLIMBING_LADDER is normally blocked (stick during a real
                    // climb would spam OoT's input). Bug 2 (2026-04-22): when
                    // our follower state is CLIMBING, we WANT stick injection
                    // to drive Link up/down the ladder. The CLIMBING-aware
                    // injection block below handles it via a different code
                    // path; here we just exempt CLIMBING_LADDER from the
                    // blocked list when we're actively driving.
                    bool blockedByPlayerState =
                        (sf1 & PLAYER_STATE1_HANGING_OFF_LEDGE) ||
                        (sf1 & PLAYER_STATE1_CLIMBING_LEDGE)    ||
                        (sf1 & PLAYER_STATE1_IN_CUTSCENE)       ||
                        (sf1 & PLAYER_STATE1_DAMAGED)           ||
                        (sf1 & PLAYER_STATE1_TALKING)           ||
                        (sf1 & PLAYER_STATE1_INPUT_DISABLED);
                    if (nowOnLadder && followerAIState != FollowerAIState::CLIMBING) {
                        // On a ladder but our state machine isn't in CLIMBING:
                        // user manually grabbed it, or we mis-entered from a
                        // non-climbing state. Block stick injection — let the
                        // human resume control via the joystick-cancel path.
                        blockedByPlayerState = true;
                    }

                    // --- Walk/run: camera-relative stick toward followerMoveTarget ---
                    // OoT's movement pipeline: worldYaw = Camera_GetInputDirYaw(cam) + stickAngle,
                    // where stickAngle = Math_Atan2S(relY, -relX).  To move in world direction
                    // (dx, dz), invert that pipeline:
                    //   worldYaw    = Math_Atan2S(dz, dx)          [OoT convention: z first]
                    //   stickAngle  = worldYaw - inputDirYaw
                    //   relY        = Math_CosS(stickAngle) * mag
                    //   relX        = -Math_SinS(stickAngle) * mag
                    // Magnitude is distance-scaled: sprint when far, walk when
                    // close, zero within the stop radius so Link's own
                    // deceleration carries him the last few units.
                    static bool sAnimHookLogged = false;
                    if (!sAnimHookLogged) {
                        SPDLOG_INFO("[Follower] animHook firing for ACTOR_PLAYER");
                        sAnimHookLogged = true;
                    }

                    // --- Crawlspace override (2026-04-22) ---
                    // When Link is in PLAYER_STATE2_CRAWLING, the camera is
                    // locked to the tunnel axis and input is simplified to
                    // forward/back along that axis. Our camera-relative stick
                    // projection may or may not land on that axis cleanly, so
                    // we hardcode full forward (stick_y = 127) while the flag
                    // is set — crawlspaces in OoT are always "push forward to
                    // advance, press backward to back out". Zero X because X
                    // input during crawl is ignored anyway.
                    //
                    // Edge-logged: one log entry on entry into CRAWLING, one
                    // on exit — so we can tell from the test log whether this
                    // path fired. Not per-frame (would flood the log).
                    static bool sWasCrawling = false;
                    bool nowCrawling = (sf2 & PLAYER_STATE2_CRAWLING) != 0;
                    if (nowCrawling && !sWasCrawling) {
                        SPDLOG_INFO("[Follower] Crawlspace override ENTER "
                                    "(PLAYER_STATE2_CRAWLING set) — forcing stick_y=127");
                    } else if (!nowCrawling && sWasCrawling) {
                        SPDLOG_INFO("[Follower] Crawlspace override EXIT "
                                    "(PLAYER_STATE2_CRAWLING cleared)");
                    }
                    sWasCrawling = nowCrawling;

                    if (isMoving && nowCrawling) {
                        input.cur.stick_x = 0;
                        input.cur.stick_y = 127;
                        input.rel.stick_x = 0;
                        input.rel.stick_y = 127;
                    } else if (followerClimbDismountFrames > 0 && !blockedByPlayerState) {
                        // Bug C (log 69) — ladder/vine dismount forward-hold.
                        // Project the held world-space yaw (captured at the
                        // CLIMBING→IDLE transition as Link's shape.rot.y,
                        // which matches the leader's facing per the CLIMBING
                        // state body) into camera-relative stick axes. Full
                        // magnitude so Link walks briskly inward past the
                        // ledge rim. Counter decrements every frame; when it
                        // reaches zero, the normal move logic resumes.
                        Camera* cam = GET_ACTIVE_CAM(gPlayState);
                        s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                        s16 stickAngle  = followerClimbDismountYaw - inputDirYaw;
                        s8  stickY = (s8)( Math_CosS(stickAngle) * 127.0f);
                        s8  stickX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                        input.cur.stick_x = stickX;
                        input.cur.stick_y = stickY;
                        input.rel.stick_x = stickX;
                        input.rel.stick_y = stickY;
                        followerClimbDismountFrames--;
                        if (followerClimbDismountFrames == 0) {
                            SPDLOG_INFO("[Follower] Dismount forward-hold complete");
                        }
                    } else if (followerAIState == FollowerAIState::CLIMBING) {
                        // Bug 2 (2026-04-22): natural ladder grab + climb.
                        // Two phases:
                        //   (a) Not on ladder yet (nowOnLadder == false):
                        //       follower is approaching the ladder from the
                        //       side. Drive stick forward toward
                        //       followerMoveTarget (= leader's XZ at leader's
                        //       Y) using the standard camera-relative
                        //       projection so OoT's collision sees Link
                        //       walking into the ladder face-first and
                        //       attaches him.
                        //   (b) On ladder (nowOnLadder == true): OoT uses
                        //       raw stick_y for vertical motion. Direction
                        //       comes from comparing leader.y to follower.y:
                        //       leader higher → up, lower → down, within
                        //       tolerance → zero (we've reached them).
                        //       Stick_x is irrelevant during climb.
                        Vec3f p2w = actor->world.pos;
                        if (nowOnLadder) {
                            f32 dyL = followerMoveTarget.y - p2w.y;
                            static constexpr f32 kClimbYTolerance = 8.0f;
                            s8  ladderY = 0;
                            if (dyL >  kClimbYTolerance)      ladderY =  127;
                            else if (dyL < -kClimbYTolerance) ladderY = -127;
                            input.cur.stick_x = 0;
                            input.cur.stick_y = ladderY;
                            input.rel.stick_x = 0;
                            input.rel.stick_y = ladderY;
                        } else {
                            // Walk toward ladder. Reuse the standard
                            // camera-relative inversion (smaller copy here so
                            // we can ignore the magnitude curve — full
                            // forward into the ladder gets the grab).
                            f32 dx = followerMoveTarget.x - p2w.x;
                            f32 dz = followerMoveTarget.z - p2w.z;
                            if (dx * dx + dz * dz > 1.0f) {
                                Camera* cam = GET_ACTIVE_CAM(gPlayState);
                                s16 inputDirYaw = Camera_GetInputDirYaw(cam);
                                s16 worldYaw    = Math_Atan2S(dz, dx);
                                s16 stickAngle  = worldYaw - inputDirYaw;
                                s8  stickY = (s8)( Math_CosS(stickAngle) * 127.0f);
                                s8  stickX = (s8)(-Math_SinS(stickAngle) * 127.0f);
                                input.cur.stick_x = stickX;
                                input.cur.stick_y = stickY;
                                input.rel.stick_x = stickX;
                                input.rel.stick_y = stickY;
                            } else {
                                input.cur.stick_x = 0; input.cur.stick_y = 0;
                                input.rel.stick_x = 0; input.rel.stick_y = 0;
                            }
                        }
                    } else if (isMoving && !blockedByPlayerState) {
                        Vec3f p2w = actor->world.pos;
                        f32 dx = followerMoveTarget.x - p2w.x;
                        f32 dz = followerMoveTarget.z - p2w.z;
                        f32 distSq = dx * dx + dz * dz;
                        if (distSq > 1.0f) {
                            f32 dist = sqrtf(distSq);
                            f32 magF;
                            if      (dist > 250.0f) magF = 127.0f; // sprint — leader far ahead
                            else if (dist >  60.0f) magF = 100.0f; // run
                            else if (dist >  30.0f) magF =  60.0f; // walk (decelerate)
                            else                    magF =   0.0f; // coast to a stop
                            Camera* cam = GET_ACTIVE_CAM(gPlayState);
                            s16 inputDirYaw  = Camera_GetInputDirYaw(cam);
                            s16 worldYaw     = Math_Atan2S(dz, dx); // z first per OoT convention
                            s16 stickAngle   = worldYaw - inputDirYaw;
                            s8  stickY = (s8)( Math_CosS(stickAngle) * magF);
                            s8  stickX = (s8)(-Math_SinS(stickAngle) * magF);
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

                    // --- Auto-press A when the "Climb" action is available ---
                    // PLAYER_STATE2_DO_ACTION_CLIMB is the flag the engine uses
                    // to show "Climb" on the A-button prompt. It covers:
                    //   - Link hanging off a land ledge (PLAYER_STATE1_HANGING_OFF_LEDGE
                    //     is also set; the two flags agree).
                    //   - Link swimming at a water-exit ledge where the engine
                    //     accepts an A-press to climb out of the water.
                    // Injecting BTN_A whenever DO_ACTION_CLIMB is set handles
                    // both cases without needing to distinguish land vs water.
                    // (Observed 2026-04-21: relying on HANGING_OFF_LEDGE alone
                    // left the follower stuck at the water's edge.)
                    if (sf2 & PLAYER_STATE2_DO_ACTION_CLIMB) {
                        input.press.button |= BTN_A;
                        input.cur.button   |= BTN_A;
                        SPDLOG_INFO("[Follower] BTN_A climb (DO_ACTION_CLIMB)");
                    }

                    // --- Phase A — auto-press A when OoT prompts "Enter" ---
                    // PLAYER_STATE2_DO_ACTION_ENTER is the flag the engine
                    // sets to display "Enter" on the A-button prompt — fires
                    // whenever Link is adjacent to an openable door / passage
                    // that accepts A. OoT handles the actor-specific detection
                    // (En_Door trigger volume, Door_Shutter cylinder, grotto
                    // Door_Ana, certain transition actors) for us; we just
                    // inject the press.
                    //
                    // Doesn't solve the G11 "leader in different room"
                    // deactivation on its own — Phase B (#169, deferred) is
                    // the handoff that keeps the follower active long enough
                    // to WALK to the door. Phase A is still valuable standalone:
                    // any time the follower is naturally near an openable
                    // passage (FOLLOW toward a leader beside an open doorway,
                    // RETURN pathway, or manual user re-activate after G11
                    // placed the follower near a door), the door opens without
                    // user intervention.
                    //
                    // Edge-logged so the test log shows when this fires. Not
                    // per-frame (would flood when follower is idle near a door).
                    {
                        bool enterPromptActive = (sf2 & PLAYER_STATE2_DO_ACTION_ENTER) != 0;
                        static bool sWasAtDoor = false;
                        if (enterPromptActive && !sWasAtDoor) {
                            SPDLOG_INFO("[Follower] BTN_A door ENTER prompt (Phase A — DO_ACTION_ENTER)");
                        } else if (!enterPromptActive && sWasAtDoor) {
                            SPDLOG_INFO("[Follower] BTN_A door EXIT (prompt cleared)");
                        }
                        sWasAtDoor = enterPromptActive;
                        if (enterPromptActive) {
                            input.press.button |= BTN_A;
                            input.cur.button   |= BTN_A;
                        }
                    }

                    // Bug D — L-target lock-on during ENGAGE/ATTACK. Holding
                    // BTN_Z while approaching means OoT's camera tracks the
                    // target, Link's facing stays auto-oriented, and swing
                    // direction is taken from the locked target rather than
                    // the raw stick angle. This corrects BTN_B swings that
                    // previously flew into empty air when the follower's
                    // facing drifted during approach. Edge-press on the
                    // ENGAGE entry; hold via cur through ATTACK. No release
                    // needed — OoT drops lock-on when the target dies or
                    // when ATTACK→RETURN clears the Z hold next frame.
                    //
                    // RANGED_ATTACK has its own BTN_Z cycle (see below);
                    // the two are mutually exclusive states so there's no
                    // double-hold.
                    if (followerAIState == FollowerAIState::ENGAGE ||
                        followerAIState == FollowerAIState::ATTACK) {
                        if (followerAIState == FollowerAIState::ENGAGE &&
                            followerStateFrames == 0) {
                            input.press.button |= BTN_Z;
                        }
                        input.cur.button |= BTN_Z;
                    }

                    // --- Attack: face enemy + inject BTN_B at start of each charge phase ---
                    if (followerAIState == FollowerAIState::ATTACK) {
                        // Keep shape.rot.y facing the enemy here (BEFORE Player_Update) so
                        // that when BTN_B is processed by OoT this frame, the swing direction
                        // is current.  OnGameFrameUpdate also sets it (after Player_Update) to
                        // maintain facing during the animation; both assignments are consistent.
                        // Task 3: suppress both the facing update and the BTN_B injection
                        // when the target is dead or dying. The state-machine RETURN
                        // transition above catches it one frame earlier on the next
                        // OnGameFrameUpdate; this gate prevents a final rogue swing in the
                        // gap between the killing hit and the state transition.
                        //
                        // Mirrors the two-signal check in the ATTACK state (see banner
                        // comment there): colChkInfo.health catches actors that decrement
                        // their own health; EnemyNetId::hasLocalDeath / pendingNaturalDeath
                        // catches AC_HIT-only actors whose health never moves (Karebaba
                        // and most simple enemies).
                        bool targetAlive = (followerTargetEnemy != nullptr &&
                                            followerTargetEnemy->update != nullptr &&
                                            followerTargetEnemy->colChkInfo.health > 0);
                        if (targetAlive) {
                            const EnemyNetId* ext =
                                ObjectExtension::GetInstance().Get<EnemyNetId>(followerTargetEnemy);
                            if (ext != nullptr &&
                                (ext->hasLocalDeath || ext->pendingNaturalDeath)) {
                                targetAlive = false;
                            }
                        }
                        if (targetAlive) {
                            f32 ex = followerTargetEnemy->world.pos.x - actor->world.pos.x;
                            f32 ez = followerTargetEnemy->world.pos.z - actor->world.pos.z;
                            f32 eDistSq = ex * ex + ez * ez;
                            if (eDistSq > 1.0f) {
                                actor->shape.rot.y = Math_Atan2S(ez, ex); // z first per OoT convention
                            }
                            if (followerStateFrames % 20 == 0) {
                                input.press.button |= BTN_B;
                                input.cur.button   |= BTN_B;
                                SPDLOG_INFO("[Follower] ATTACK injecting BTN_B (stateFrames={})",
                                            followerStateFrames);
                            } else if (eDistSq < 50.0f * 50.0f) {  // kSwingReach
                                // Bug D — point-blank shield between swings.
                                // Non-swing frames while Link is within
                                // sword arc (enemy is mid-range retaliation
                                // window). BTN_R raises the shield to tank
                                // Karebaba lunges / Deku Baba bites. OoT
                                // plants Link in place with shield up when
                                // lock-on + BTN_R + no stick — intentional:
                                // we stop walking into the damage volume.
                                input.cur.button |= BTN_R;
                            }
                        }
                    }

                    // G4 — BLOCK: hold BTN_R to plant the shield. OoT treats
                    // R-hold as a continuous shielding input, so set both .cur
                    // and .press every frame.
                    if (followerAIState == FollowerAIState::BLOCK) {
                        input.press.button |= BTN_R;
                        input.cur.button   |= BTN_R;
                    }

                    // Item pickup — dismiss item-get and talking text boxes
                    // with BTN_A every 20 frames. PLAYER_STATE1_GETTING_ITEM
                    // is set during the "raised-item" cutscene (first-time
                    // pickups of bombs, arrows, keys, heart pieces); TALKING
                    // catches the text-advance portion. Matches the BTN_B
                    // swing cadence so we're not slamming BTN_A every frame.
                    // Fires regardless of follower state (pickup can occur
                    // during COLLECT_ITEM, but also in any other state if
                    // Link steps on an item by accident).
                    if (sf1 & (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_TALKING)) {
                        if (followerStateFrames % 20 == 0) {
                            input.press.button |= BTN_A;
                            input.cur.button   |= BTN_A;
                        }
                    }

                    // G6/G7/G8 — RANGED_ATTACK: draw weapon, aim, release-to-fire.
                    //
                    // Bug 4 (2026-04-22) — release-to-fire cycle. Prior code
                    // pressed Z + C-button + A every frame. Three problems:
                    //   1. Setting input.press.button every frame = OoT sees
                    //      "just pressed" every frame, so the slingshot draw
                    //      animation never settles into ready-to-fire.
                    //   2. A-press before Link is fully drawn = roll/jump
                    //      attack, not fire (matches user's "rolled instead").
                    //   3. The natural OoT firing path is "release the
                    //      C-button to auto-fire the primed shot" — A-press
                    //      is the secondary path.
                    //
                    // New cycle: hold Z + C-button (cur only, press on entry
                    // edge), drop the C-button for one frame every kFireCycleFrames
                    // to trigger auto-fire. Re-press the next frame to re-draw.
                    //
                    // Option B — the C-button press is only meaningful if
                    // followerActiveCSlot != 0xFF (CVar enabled AND player has
                    // a slingshot/bow). Otherwise the C-button block is
                    // skipped; Z is still held but no fire happens.
                    if (followerAIState == FollowerAIState::RANGED_ATTACK) {
                        static constexpr int kFireCycleFrames = 60;
                        // Z: edge-press on entry, hold via cur thereafter.
                        if (followerStateFrames == 0) {
                            input.press.button |= BTN_Z;
                        }
                        input.cur.button |= BTN_Z;

                        u16 cBtn = 0;
                        switch (followerActiveCSlot) {
                            case 0: cBtn = BTN_CLEFT;  break;
                            case 1: cBtn = BTN_CDOWN;  break;
                            case 2: cBtn = BTN_CRIGHT; break;
                            default: break;
                        }
                        if (cBtn != 0) {
                            int phase = followerStateFrames % kFireCycleFrames;
                            // Phase 0: edge-press the C-button (start draw)
                            // Phase 1 .. (kFireCycleFrames-2): hold via cur (aim/prime)
                            // Phase (kFireCycleFrames-1): RELEASE for one frame
                            //   (don't set cur, don't set press) → OoT auto-fires
                            //   the primed shot.
                            if (phase == 0) {
                                input.press.button |= cBtn;
                                input.cur.button   |= cBtn;
                                SPDLOG_INFO("[Follower] RANGED_ATTACK draw cycle (cSlot={})",
                                            (int)followerActiveCSlot);
                            } else if (phase == kFireCycleFrames - 1) {
                                // Release frame — explicitly do NOT add cBtn
                                // to either cur or press. This is the fire.
                                SPDLOG_INFO("[Follower] RANGED_ATTACK release-to-fire (cSlot={})",
                                            (int)followerActiveCSlot);
                            } else {
                                input.cur.button |= cBtn;
                            }
                        }

                        // BTN_A as a backup fire path. Only inject when
                        // PLAYER_STATE1_READY_TO_FIRE is set — that's OoT's
                        // signal that the slingshot/bow is fully drawn and
                        // primed. Pressing A before this would do roll /
                        // jump-attack instead.
                        if ((sf1 & PLAYER_STATE1_READY_TO_FIRE) &&
                            (followerStateFrames % 20 == 0)) {
                            input.press.button |= BTN_A;
                            input.cur.button   |= BTN_A;
                            SPDLOG_INFO("[Follower] RANGED_ATTACK BTN_A fire (READY_TO_FIRE set)");
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

        // Bug 1 (2026-04-22, log 68) — host respawn guard.
        //
        // OoT unloads / reloads room actors on every room transition, even
        // within a single scene visit. When the host briefly leaves a room
        // (e.g., Deku Tree Room 0 → Room 1 (Mad Scrub) → Room 0 again,
        // ~6 s round trip), all room-0 actors get freshly re-spawned via
        // their normal Init path. Their EnemyNetIds are recomputed
        // deterministically (same scene + actor id + posHash → same netId
        // values as before). Without this guard, the host's fresh Dekubabas
        // come back alive, host sends ENEMY_UPDATE with health > 0, and
        // non-host clients that had already killed them are forced to choose
        // between (a) overwriting their dead state (visually revives the
        // enemy) or (b) keeping it dead via hasLocalDeath (mismatch between
        // clients).
        //
        // Fix: on host, check deadEnemiesByScene[sceneNum] for the freshly-
        // computed netId. If present, the player previously killed this
        // enemy this scene-visit — kill it again on respawn. Same Karebaba /
        // non-Karebaba split as the pendingKillNetIds branch below; for
        // Karebaba the deferredDeadItemDrop flag is set so OnActorInit can
        // apply SetupDeadItemDrop AFTER actor->init() runs (otherwise
        // EnKarebaba_Init resets actionFunc back to Idle on Frame 1).
        // deadEnemiesByScene is cleared on OnSceneSpawnActors, so this
        // guard only suppresses same-scene-visit revivals — leaving and
        // re-entering the scene proper still respawns enemies as expected.
        if (roomState.ownerClientId == ownClientId) {
            auto deadIt = deadEnemiesByScene.find(gPlayState->sceneNum);
            if (deadIt != deadEnemiesByScene.end() && deadIt->second.count(netId)) {
                SPDLOG_INFO("[EnemySpawn] deadEnemiesByScene hit for netId={} on host — "
                            "suppressing same-scene respawn (id={})",
                            netId, actor->id);
                if (actor->id == ACTOR_EN_KAREBABA) {
                    EnemyNetId* extPtr = const_cast<EnemyNetId*>(
                        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
                    if (extPtr != nullptr) {
                        extPtr->hasLocalDeath        = true;
                        extPtr->pendingNaturalDeath  = true;
                        extPtr->defeatPacketSent     = true;
                        // OnActorInit applies SetupDeadItemDrop after init() runs.
                        // Gated on deferredDeadItemDrop, not on host/non-host.
                        extPtr->deferredDeadItemDrop = true;
                    }
                } else {
                    isKillingNetworkActor = true;
                    Actor_Kill(actor);
                    isKillingNetworkActor = false;
                }
                return;
            }
        }

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
        // 2026-04-22 (Bug 1): host now also sets deferredDeadItemDrop in the
        // OnActorSpawn deadEnemiesByScene-respawn-guard branch. Gate on the
        // flag itself rather than on host/non-host so both code paths reach
        // the same SetupDeadItemDrop application below.
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
