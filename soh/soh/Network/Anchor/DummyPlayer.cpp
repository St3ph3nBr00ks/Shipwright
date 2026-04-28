#include "Anchor.h"
#include "soh/Network/Anchor/Common/SceneMultiplayerConfig.h"
#include "soh/Enhancements/nametag.h"
#include <unordered_map>
#include <unordered_set>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);

// Face-texture bindings used by Player_DrawImpl via gSPSegment(0x08/0x09, ...).
// Declared at file scope in z_player_lib.c:975/990 with external linkage under
// MODDING/_MSC_VER/__GNUC__.  We swap the populated slots around each
// DummyPlayer's Player_Draw call so remote packs get their own face textures
// (Coop Test 17 fix).
extern void* sEyeTextures[2][8];
extern void* sMouthTextures[2][4];
}

// KB-19 diagnostic CVars — see DummyPlayer_Draw / DummyPlayer_Init / DummyPlayer_Update.
//   gAnchor.Debug.SkipDummyDraw   (default 0): when 1, DummyPlayer_Draw returns
//                                  after the gSaveContext.linkAge swap-set/restore
//                                  WITHOUT calling Player_Draw. Originally used
//                                  to isolate KB-19 (R1/R2/R3 control tests
//                                  2026-04-27 confirmed the pause-menu /
//                                  DummyPlayer collision). The narrower
//                                  pauseCtx.state != 0 gate below is now the
//                                  permanent fix; this CVar is retained as an
//                                  emergency override / future bisection probe.
//   gAnchor.Debug.LogSwapWindows  (default 0): when 1, every gSaveContext.linkAge
//                                  swap entry/exit in DummyPlayer.cpp is logged
//                                  with clientId, savedAge, swappedAge so frames
//                                  showing distortion can be correlated with the
//                                  exact swap pattern.
#define CVAR_ANCHOR_DEBUG_SKIP_DUMMY_DRAW   "gAnchor.Debug.SkipDummyDraw"
#define CVAR_ANCHOR_DEBUG_LOG_SWAP_WINDOWS  "gAnchor.Debug.LogSwapWindows"

static inline bool DebugSkipDummyDraw() {
    return CVarGetInteger(CVAR_ANCHOR_DEBUG_SKIP_DUMMY_DRAW, 0) != 0;
}
static inline bool DebugLogSwapWindows() {
    return CVarGetInteger(CVAR_ANCHOR_DEBUG_LOG_SWAP_WINDOWS, 0) != 0;
}

static DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Hack to account for usage of gSaveContext in Player_Init
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    if (DebugLogSwapWindows()) {
        SPDLOG_INFO("[KB19][SwapEnter:Init] clientId={} savedAge={} swappedTo={}",
                    clientId, originalAge, client.linkAge);
    }

    // #region modeled after EnTorch2_Init and Player_Init
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[client.linkAge]);

    play->func_11D54(player, play);
    // #endregion

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    gSaveContext.linkAge = originalAge;
    if (DebugLogSwapWindows()) {
        SPDLOG_INFO("[KB19][SwapExit:Init] clientId={} restoredAge={}", clientId, originalAge);
    }

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), {});
    }

    // Step 6 — apply the remote player's custom character model skeleton if they have one
    SPDLOG_INFO("[CoopModel] DummyPlayer_Init clientId={}: linkAge={} customModel=\"{}\"",
                clientId, client.linkAge, client.customModelFilename);
    if (!client.customModelFilename.empty()) {
        bool isAdult = (client.linkAge != LINK_AGE_CHILD);
        client.customSkeleton = nullptr;
        // KB-15 fix (issue #110): if this client already held a bakedModel (possible
        // when RefreshClientActors spawns a replacement DummyPlayer), retire it
        // instead of dropping it — the old DummyPlayer's final draw frame may still
        // be in flight and its Gfx commands still reference pathStrings / bakedDLs /
        // eyeTexKeys owned by the old bakedModel.
        client.RetireBakedModel();
        client.bakedModel = std::make_unique<SOH::BakedPlayerModel>();
        client.lastAppliedModelFilename = "";  // reset so DummyPlayer_Update will re-apply
        SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
            &player->skelAnime, isAdult, (uint8_t)client.currentTunic,
            client.customModelFilename, client.customSkeleton, *client.bakedModel);
        client.lastAppliedModelFilename = client.customModelFilename;
    }
}

void Math_Vec3s_Copy(Vec3s* dest, Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&player->upperLimbRot, &client.upperLimbRot);
    Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
    Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
    player->skelAnime.jointTable = client.jointTable;
    player->skelAnime.movementFlags = client.movementFlags;
    Math_Vec3s_Copy(&player->skelAnime.prevTransl, &client.prevTransl);
    player->currentBoots = client.currentBoots;
    player->currentShield = client.currentShield;
    uint8_t prevTunic = player->currentTunic; // capture before overwrite for change detection
    player->currentTunic = client.currentTunic;

    // Step 7 — re-apply custom skeleton when:
    //   (a) the remote player's model changed since last apply, OR
    //   (b) the tunic changed (different skeleton variant needed).
    // Comparing lastAppliedModelFilename suppresses per-frame retries when the
    // archive lookup fails (it does NOT produce a null customSkeleton on success,
    // but the previous guard "customSkeleton == nullptr" was true on every frame
    // after a failed lookup, causing a per-frame archive search).
    if (!client.customModelFilename.empty()) {
        bool isAdult = (client.linkAge != LINK_AGE_CHILD);
        bool modelChanged  = (client.customModelFilename != client.lastAppliedModelFilename);
        bool tunicChanged  = (prevTunic != player->currentTunic);
        if (modelChanged || tunicChanged) {
            client.customSkeleton = nullptr;
            // KB-15 fix (issue #110): retire the outgoing bakedModel so in-flight
            // Gfx commands finish consuming it before destruction.
            client.RetireBakedModel();
            client.bakedModel = std::make_unique<SOH::BakedPlayerModel>();
            SOH::SkeletonPatcher::ApplyCustomSkeletonToDummyPlayer(
                &player->skelAnime, isAdult, (uint8_t)player->currentTunic,
                client.customModelFilename, client.customSkeleton, *client.bakedModel);
            client.lastAppliedModelFilename = client.customModelFilename;
        }
    }

    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->itemAction = client.itemAction;
    player->heldItemAction = client.heldItemAction;
    player->invincibilityTimer = client.invincibilityTimer;
    player->unk_862 = client.unk_862;
    player->unk_85C = client.unk_85C;
    player->av1.actionVar1 = client.actionVar1;

    // Apply animation movement (Copied from Player_ApplyAnimMovementScaledByAge)
    Vec3f diff;
    SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);

    if (player->skelAnime.movementFlags & 1) {
        if (!LINK_IS_ADULT) {
            diff.x *= 0.64f;
            diff.z *= 0.64f;
        }

        player->actor.world.pos.x += diff.x * player->actor.scale.x;
        player->actor.world.pos.z += diff.z * player->actor.scale.z;
    }

    if (player->skelAnime.movementFlags & 2) {
        if (!(player->skelAnime.movementFlags & 4)) {
            diff.y *= player->ageProperties->unk_08;
        }

        player->actor.world.pos.y += diff.y * player->actor.scale.y;
    }

    if (player->modelGroup != client.modelGroup) {
        // Hack to account for usage of gSaveContext
        s32 originalAge = gSaveContext.linkAge;
        gSaveContext.linkAge = client.linkAge;
        u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
        gSaveContext.equips.buttonItems[0] = client.buttonItem0;
        if (DebugLogSwapWindows()) {
            SPDLOG_INFO("[KB19][SwapEnter:Update] clientId={} savedAge={} swappedTo={} newModelGroup={}",
                        clientId, originalAge, client.linkAge, client.modelGroup);
        }
        Player_SetModelGroup(player, client.modelGroup);
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
        if (DebugLogSwapWindows()) {
            SPDLOG_INFO("[KB19][SwapExit:Update] clientId={} restoredAge={}", clientId, originalAge);
        }
    }

    // Pillar B Phase 3 — cross-timeline interaction gate (Q 4.B.4).
    // A child-timeline player and an adult-timeline player can occupy
    // the "same" scene (sceneNum) but their world-state is independent,
    // so any collision / lock-on / damage between them is meaningless.
    // Treat them as the pvpMode=0 case: disable lock-on and skip the
    // collider setup entirely. This runs BEFORE the PvP gate because
    // cross-timeline trumps every PvP mode (including FF).
    if (client.linkAge != gSaveContext.linkAge) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default")) ||
        SceneMultiplayerConfig::ShouldDisablePvP(gPlayState)) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0) {
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  player->actor.colChkInfo.damage);
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        return;
    }

    // Pillar B Phase 4 — cross-timeline render gate (Q 4.B.1 = ethereal).
    // v1 implementation: skip body draw entirely. The peer's name tag
    // (registered via NameTag_RegisterForActorWithOptions in DummyPlayer_Init)
    // still renders, so the player can see WHERE their cross-timeline peer
    // is without the body cluttering the scene's layout.
    //
    // Polish path (deferred): proper ethereal alpha-blended draw via a
    // hook in z_player.c that wraps Player_Draw with EnvColor.a override
    // + RM_AA_ZB_XLU_SURF (sibling of the existing
    // Anchor_LocalPlayerFaceSwapBegin/End hooks). Tracked in the Pillar B
    // implementation plan as a Phase 4 polish item.
    if (client.linkAge != gSaveContext.linkAge) {
        return;
    }

    // KB-19 history note: a `pauseCtx.state != 0` early-return used to live
    // here as the primary fix, because the pause menu's func_80091738 was
    // reconfiguring gSegments[4]/[6] for the rotating-Link preview. That
    // collided with DummyPlayer_Draw's own use of those segments and
    // crashed/distorted. The collision is now closed at the source: Option
    // C in z_kaleido_scope_PAL.c / z_kaleido_equipment.c gates the rotating-
    // Link load+draw on Anchor_PauseMenuFreezesWorld(), so segments 4 and 6
    // are never reconfigured in multiplayer. Combined with the Pillar G.i
    // gate around the R_PAUSE_MENU_MODE==3 captured-framebuffer branch in
    // z_play.c, remote-player rendering is now safe behind the pause UI.

    // Log skeleton pointer once per DummyPlayer lifetime so we can verify the
    // correct pack skeleton is active at render time (not a stale/wrong-pack skeleton).
    static std::unordered_map<uint32_t, void*> sLoggedSkeletons;
    void* curSkel = (void*)player->skelAnime.skeleton;
    if (sLoggedSkeletons[clientId] != curSkel) {
        SPDLOG_INFO("[CoopModel] DummyPlayer_Draw clientId={} skelAnime.skeleton changed: {} -> {} (customModel=\"{}\")",
                    clientId, sLoggedSkeletons[clientId], curSkel, client.customModelFilename);
        sLoggedSkeletons[clientId] = curSkel;
    }

    // Hack to account for usage of gSaveContext in Player_Draw
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = client.buttonItem0;
    if (DebugLogSwapWindows()) {
        // Rate-limited to a single line per (clientId, swappedAge) transition;
        // Draw runs every frame and would otherwise flood the log.
        static std::unordered_map<uint32_t, s32> sLastSwappedAge;
        auto it = sLastSwappedAge.find(clientId);
        if (it == sLastSwappedAge.end() || it->second != client.linkAge) {
            SPDLOG_INFO("[KB19][SwapEnter:Draw] clientId={} savedAge={} swappedTo={} (logged on age change)",
                        clientId, originalAge, client.linkAge);
            sLastSwappedAge[clientId] = client.linkAge;
        }
    }

    // KB-19 Diagnostic A — when gAnchor.Debug.SkipDummyDraw is on, skip Player_Draw
    // entirely. The swap window still opens and closes around this gate so that
    // any side-effects of writing gSaveContext.linkAge alone (without Player_Draw
    // executing) are still observed. If KB-19 vertex distortion on the LOCAL
    // player goes away with this gate enabled, the distortion source is inside
    // Player_Draw's read of segmented state during the swap window. If
    // distortion still occurs, the source is elsewhere (e.g. update-time
    // jointTable aliasing in DummyPlayer_Update line 155).
    if (DebugSkipDummyDraw()) {
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
        return;
    }

    // Test 16 #171 — defensive null-guard. If player->skelAnime.skeleton is
    // null (e.g. the DummyPlayer bake hasn't completed or was retired
    // without a replacement), Player_Draw dereferences it and crashes
    // (0xc0000005 at RIP=…DCA9 observed across 4 log occurrences). Log
    // and skip the draw this frame; the next frame will see a valid
    // skeleton or the actor will be killed by the upstream client-gone
    // check.
    if (player->skelAnime.skeleton == nullptr) {
        static std::unordered_set<uint32_t> sLoggedNullSkel;
        if (sLoggedNullSkel.insert(clientId).second) {
            SPDLOG_WARN("[CoopModel] DummyPlayer_Draw SKIPPED clientId={} — "
                        "skelAnime.skeleton is null (bake in flight or retired); "
                        "will retry next frame",
                        clientId);
        }
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
        return;
    }

    // Swap this DummyPlayer's baked face textures into the shared sEyeTextures /
    // sMouthTextures arrays for exactly the duration of Player_Draw.  Slots
    // where the pack did not ship an override keep their saved original value,
    // so partial packs still work (same acceptable bleed as the non-face case).
    // Scope is intentionally tight: any other actor that calls Player_Draw in
    // the same frame (or the local player's draw before us) must see the
    // vanilla/local-pack bindings.
    int faceAge = (client.linkAge == LINK_AGE_CHILD) ? 1 : 0;
    void* savedEye[8];
    void* savedMouth[4];
    bool swappedFace = false;
    if (client.bakedModel && client.bakedModel->isValid) {
        auto& bm = *client.bakedModel;
        for (int i = 0; i < 8; i++) savedEye[i]   = sEyeTextures[faceAge][i];
        for (int i = 0; i < 4; i++) savedMouth[i] = sMouthTextures[faceAge][i];
        for (int i = 0; i < 8; i++) {
            if (!bm.eyeTexKeys[faceAge][i].empty()) {
                sEyeTextures[faceAge][i] = (void*)bm.eyeTexKeys[faceAge][i].c_str();
            }
        }
        for (int i = 0; i < 4; i++) {
            if (!bm.mouthTexKeys[faceAge][i].empty()) {
                sMouthTextures[faceAge][i] = (void*)bm.mouthTexKeys[faceAge][i].c_str();
            }
        }
        swappedFace = true;
    }

    Player_Draw((Actor*)player, play);

    if (swappedFace) {
        for (int i = 0; i < 8; i++) sEyeTextures[faceAge][i]   = savedEye[i];
        for (int i = 0; i < 4; i++) sMouthTextures[faceAge][i] = savedMouth[i];
    }

    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
    // DummyPlayer Actors are initially spawned as ACTOR_PLAYER, but change their
    // ID shortly afterwards to ACTOR_EN_OE2. This would cause ACTOR_PLAYER's
    // ActorDB Entry's `numLoaded` to leak, which is mostly harmless but hits debug
    // asserts. Set the id back to ACTOR_PLAYER so that `numLoaded` will be decremented
    // correctly.
    actor->id = ACTOR_PLAYER;

    // Step 8 — release the custom skeleton shared_ptr so its memory can be freed.
    // Guard: only clear if this actor is still the active DummyPlayer for the client.
    // RefreshClientActors kills old DummyPlayers and spawns replacements in the same
    // call: DummyPlayer_Init for the NEW actor runs synchronously inside Actor_Spawn
    // (setting client.customSkeleton + client.player = newActor), but DummyPlayer_Destroy
    // for the OLD actor runs later (next frame).  Without this guard, Destroy would clear
    // the skeleton the new actor's skelAnime->skeleton already points to, leaving a
    // dangling pointer → STATUS_STACK_OVERFLOW in Player_Draw on first render.
    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);
    if (Anchor::Instance->clients.contains(clientId)) {
        if (Anchor::Instance->clients[clientId].player == (Player*)actor) {
            Anchor::Instance->clients[clientId].customSkeleton = nullptr;
            // KB-15 fix (issue #110): retire rather than destroy.
            // Actor_Delete can fire during scene transitions and similar points where
            // the last-submitted Gfx frame may still reference this bakedModel.
            Anchor::Instance->clients[clientId].RetireBakedModel();
        }
    }
}
