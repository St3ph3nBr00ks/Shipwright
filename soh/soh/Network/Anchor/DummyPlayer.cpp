#include "Anchor.h"
#include "EnemyNetId.h"  // #243.7.2 — EnemyNetId no longer transitive via Anchor.h
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/Common/SceneMultiplayerConfig.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"  // IsMyCurrentRoomHost — Bug B fix gate (2026-06-05)
#include "soh/Network/Anchor/Common/EnemyKnockbackTable.h"  // Path A vanilla knockback lookup (2026-06-05)
#include "soh/Network/Anchor/Common/GameTimeControllerBridge.h"
#include "soh/Enhancements/nametag.h"
#include <unordered_map>
#include <unordered_set>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.h"  // EFFECT_HITMARK_METAL
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"  // mounted-pose reconciliation
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

// Widen a DummyPlayer collider's AC type bits so cross-machine hostile
// NPCs (Invader and synced vanilla enemies like Goroiwa, etc.) can
// register hits against it. Vanilla Player colliders are
// AC_TYPE_PLAYER only (sized for PvP friendly-fire); cross-machine
// PvE damage requires AC_TYPE_ENEMY so AT_TYPE_ENEMY toucher flags
// from hostile NPCs match.
//
// Single point of truth for "which AC types should this DummyPlayer
// collider accept this frame." Called per-collider per-frame so the
// type bits stay in lockstep with the live PvP gate. PvP-on adds
// AC_TYPE_PLAYER on top so PvP-friendly-fire path still works.
//
// Apply to every AC-registering collider on the DummyPlayer. Today:
// body cylinder + shieldQuad. New collider additions (sword AT for
// PvP, hookshot grab, etc.) should call this same helper so we don't
// re-introduce the "shield doesn't block Invader" bug class for them.
static inline void WidenDummyAcForCrossMachine(Collider* base, bool pvpActive) {
    base->acFlags = (base->acFlags & ~AC_TYPE_ALL) | AC_TYPE_ENEMY;
    if (pvpActive) {
        base->acFlags |= AC_TYPE_PLAYER;
    }
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
    /* Unblockable   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),  // Bug 2 fix (2026-06-05) — Goroiwa, Bigokuta, falling rocks, etc. all use the Unblockable damage type (AT dmgFlags bit 29 = 0x20000000). Entry was accidentally zeroed during the original DummyPlayer table customization, silently filtering every direct-collision attacker to damage=0 → cross-machine DAMAGE_PLAYER shipped damage=0 → peer took no damage. See Plans/dummy_player_damage_table_audit.md.
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

// Verbatim duplicate of sUpperBodyLimbCopyMap from z_player.c:417-440.
// The vanilla AnimationContext_SetCopyTrue merge at z_player.c:3634 runs
// only inside Player_Update, which never fires for DummyPlayer actors
// (their update func is reassigned to DummyPlayer_Update at
// HookHandlers.cpp:1073). DummyPlayer_Update applies the merge manually
// using this table so synced carry / hookshot / bow-draw poses render
// on the upper body of remote players.
//
// If z_player.c diverges from this layout (PLAYER_LIMB_MAX changes, limb
// enum reorders, or sUpperBodyLimbCopyMap gains new "true" entries) this
// duplicate must be re-synced. PLAYER_LIMB_MAX = 22 verified
// z64player.h:196 (2026-05-29).
// See Plans/carry_held_actor_sync.md §3.1.
// Detach + kill the held actor on a DummyPlayer when the holder is no
// longer eligible to be holding it (left our scene, went offline, or
// the DummyPlayer itself is being destroyed). Used to avoid the log
// 317 break-on-detach bug: pots in ObjTsubo_LiftedUp state check
// Actor_HasNoParent every tick and transition to SetupThrown on
// parent-cleared, which immediately breaks the pot on floor contact.
// Killing the actor instead matches vanilla's "held pot vanishes when
// scene unloads for the holder" semantic. The isKillingNetworkActor
// flag suppresses the OnActorKill hook's ENEMY_DEFEATED broadcast —
// this is a passive "actor went away", not a defeat event.
static void AnchorDummyDetachAndKillHeldActor(Actor* dummyActor) {
    Player* player = (Player*)dummyActor;
    if (player->heldActor == nullptr) return;
    Actor* held = player->heldActor;
    if (held->parent == dummyActor) {
        held->parent = NULL;
    }
    player->heldActor = NULL;
    Anchor::Instance->KillNetworkActorSilently(held);
}

static constexpr u8 kAnchorUpperBodyLimbCopyMap[22] = {
    0, // PLAYER_LIMB_NONE
    0, // PLAYER_LIMB_ROOT
    0, // PLAYER_LIMB_WAIST
    0, // PLAYER_LIMB_LOWER
    0, // PLAYER_LIMB_R_THIGH
    0, // PLAYER_LIMB_R_SHIN
    0, // PLAYER_LIMB_R_FOOT
    0, // PLAYER_LIMB_L_THIGH
    0, // PLAYER_LIMB_L_SHIN
    0, // PLAYER_LIMB_L_FOOT
    1, // PLAYER_LIMB_UPPER
    1, // PLAYER_LIMB_HEAD
    1, // PLAYER_LIMB_HAT
    1, // PLAYER_LIMB_COLLAR
    1, // PLAYER_LIMB_L_SHOULDER
    1, // PLAYER_LIMB_L_FOREARM
    1, // PLAYER_LIMB_L_HAND
    1, // PLAYER_LIMB_R_SHOULDER
    1, // PLAYER_LIMB_R_FOREARM
    1, // PLAYER_LIMB_R_HAND
    1, // PLAYER_LIMB_SHEATH
    1, // PLAYER_LIMB_TORSO
};

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Title-screen peer branch (Phase 1 — Plans/title_screen_peer_actors.md).
    // Bypasses the gameplay-time isSaveLoaded gate below and drives position
    // from local Link's pos+rot + formation offset instead of client.posRot
    // (peer's gameplay-time wire data is invalid pre-save-load anyway).
    // Single-file formation behind local Link along his facing direction;
    // spacing 40u per formation index. Rationale: see camera audit Shot 5
    // analysis in the plan doc — only single-file is invariant across all
    // camera angles in the title cutscene.
    if (Anchor::Instance->IsDummyPlayerTitleMode(actor)) {
        if (client.online) {
            Player* localLink = GET_PLAYER(gPlayState);
            if (localLink != nullptr) {
                const uint8_t formationIdx =
                    Anchor::Instance->GetTitlePeerFormationIndex(actor);
                const float spacing = 40.0f * (float)(formationIdx + 1);
                const float heading =
                    (float)localLink->actor.world.rot.y / 32768.0f * (float)M_PI;
                actor->world.pos.x =
                    localLink->actor.world.pos.x - spacing * sinf(heading);
                actor->world.pos.y = localLink->actor.world.pos.y;
                actor->world.pos.z =
                    localLink->actor.world.pos.z - spacing * cosf(heading);
                actor->world.rot.y = localLink->actor.world.rot.y;
                actor->shape.rot.y = localLink->actor.world.rot.y;
                actor->shape.shadowAlpha = 255;
            }
        } else {
            // Peer went offline mid-title-cycle — hide until cleanup fires.
            actor->world.pos.x = -9999.0f;
            actor->world.pos.y = -9999.0f;
            actor->world.pos.z = -9999.0f;
            actor->shape.shadowAlpha = 0;
        }
        return;
    }

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        // Phase 2 follow-up — if we were carrying an actor on behalf of
        // this client and they just left our scene (or went offline),
        // drop the held actor properly so its LiftedUp state machine
        // doesn't trip Actor_HasNoParent → SetupThrown → break on next
        // tick. See AnchorDummyDetachAndKillHeldActor comment.
        AnchorDummyDetachAndKillHeldActor(actor);
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

    // Upper-body anim merge — replicate z_player.c:3631-3635
    // (AnimationContext_SetCopyTrue with sUpperBodyLimbCopyMap) manually
    // because Player_UpdateUpperBody never runs on DummyPlayer.
    //
    // Per-frame gate via upperMergeActiveThisFrame: the merge only runs
    // when the owner sent upperJointTable in THIS packet (i.e., they
    // were actively carrying an actor at send time). Without the gate,
    // stale upper joints overlay onto every frame's main jointTable and
    // visibly corrupt walk / run / attack anims — and the carry pose
    // would never release after throw because the stale carryB_wait
    // joints persist on the wire-cached upperJointTable.
    //
    // See Plans/carry_held_actor_sync.md §3.1.
    if (client.upperMergeActiveThisFrame) {
        player->upperSkelAnime.jointTable = client.upperJointTable;
        for (s32 i = 0; i < 22; i++) {
            if (kAnchorUpperBodyLimbCopyMap[i]) {
                player->skelAnime.jointTable[i] = player->upperSkelAnime.jointTable[i];
            }
        }
    }

    // Held-actor attach / detach — Phase 2 per-frame state-driven check
    // (Plans/pillar_c2_live_actor_snapshot.md §5).
    //
    // The §3.2 edge-trigger missed the late-join race: PLAYER_UPDATE
    // arrives at the joiner setting client.heldActorNetId = X before the
    // Phase 1 LiveSpawn replay creates the local copy of the actor. The
    // edge fired once with no local match (lookup returned null), and
    // subsequent identical-value PLAYER_UPDATEs never re-fired the edge.
    //
    // Replacement: compare DummyPlayer's currently-attached netId
    // against the expected netId from client state every frame; reconcile
    // any mismatch. Self-heals across the LiveSpawn-race-before-attach
    // case AND any future race involving disconnect/reconnect, scene
    // transitions, or held-then-thrown-mid-join sequences. Covers an
    // arbitrary number of clients (each DummyPlayer reconciles
    // independently against its own client.heldActorNetId).
    {
        uint32_t expectedNetId = client.heldActorNetId;
        uint32_t currentNetId  = 0;
        if (player->heldActor != nullptr) {
            const EnemyNetId* ext =
                ObjectExtension::GetInstance().Get<EnemyNetId>(player->heldActor);
            if (ext != nullptr) currentNetId = ext->netId;
        }

        if (currentNetId != expectedNetId) {
            // Detach whatever is currently attached.
            if (player->heldActor != nullptr) {
                if (player->heldActor->parent == actor) {
                    player->heldActor->parent = NULL;
                }
                player->heldActor = NULL;
            }
            // Attach the expected actor if we can find it. When the
            // local copy doesn't exist yet (Phase 1 LiveSpawn replay
            // hasn't arrived, or the actor was destroyed and the
            // network state hasn't caught up), the next frame's check
            // retries automatically.
            if (expectedNetId != 0) {
                Actor* held = FindActorByNetId(gPlayState, expectedNetId);
                if (held != nullptr) {
                    player->heldActor = held;
                    held->parent = actor;
                }
            }
        }
    }

    // Horse-sync mounted-pose reconciliation (Plans/horse_sync_plan.md
    // §"DummyPlayer mounted pose integration"). Sibling to the held-
    // actor block above. Per-frame state-driven check (not edge-trigger)
    // so late-join races, scene transitions, and reconnect/disconnect
    // all self-heal. When client.mountedHorseNetId is set, snap the
    // DummyPlayer to horse.world.pos + horse.riderPos — replicating
    // vanilla parent-child mount linkage on every receiver. When the
    // peer's mountedHorseNetId is 0 (dismounted) or the local horse
    // replica hasn't been spawned yet, this branch is a no-op and the
    // DummyPlayer falls through to standard ground-position handling.
    if (client.mountedHorseNetId != 0) {
        Actor* peerHorse = nullptr;
        if (Anchor::Instance != nullptr) {
            auto it = Anchor::Instance->mPeerHorses.find(client.mountedHorseNetId);
            if (it != Anchor::Instance->mPeerHorses.end() &&
                it->second != nullptr && it->second->update != nullptr) {
                peerHorse = it->second;
            }
        }
        if (peerHorse == nullptr) {
            peerHorse = FindActorByNetId(gPlayState, client.mountedHorseNetId);
        }
        if (peerHorse != nullptr && peerHorse->id == ACTOR_EN_HORSE) {
            EnHorse* horseAsHorse = (EnHorse*)peerHorse;
            actor->world.pos.x = peerHorse->world.pos.x + horseAsHorse->riderPos.x;
            actor->world.pos.y = peerHorse->world.pos.y + horseAsHorse->riderPos.y;
            actor->world.pos.z = peerHorse->world.pos.z + horseAsHorse->riderPos.z;
            actor->shape.rot.y = peerHorse->shape.rot.y;
            // Mark PLAYER_STATE1_ON_HORSE on the DummyPlayer's Player
            // struct so other systems that read it (collision skip at
            // DummyPlayer.cpp:1020, animation overlays) behave correctly
            // mid-ride. Cleared automatically next frame when peer
            // dismounts (mountedHorseNetId == 0 takes the else branch
            // below). (player and actor are the same struct via the
            // Player* cast at line 253; no need to write fields twice.)
            player->stateFlags1 |= PLAYER_STATE1_ON_HORSE;
        }
    } else {
        player->stateFlags1 &= ~PLAYER_STATE1_ON_HORSE;
    }
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
    player->unk_860 = client.unk_860;
    player->av1.actionVar1 = client.actionVar1;

    // Mirror the remote player's shield-hold pose. Sets rightHandType to
    // PLAYER_MODELTYPE_RH_SHIELD when stateFlags1 carries
    // PLAYER_STATE1_SHIELDING. The subsequent Player_OverrideLimbDraw
    // pass (during DummyPlayer_Draw → Player_Draw) checks rightHandType
    // and calls Player_UpdateShieldCollider, which registers the shield
    // quad's AC collider via CollisionCheck_SetAC. That's what lets
    // enemy projectiles like En_Nutsball read AT_BOUNCED off a peer's
    // DummyPlayer the same way they do off the local player.
    //
    // Side effects of Player_SetModelsForHoldingShield: it may flip
    // sheathType, modelAnimType, and write itemAction = -1. The
    // itemAction write is harmless because next frame's DummyPlayer_Update
    // restores it from client.itemAction.
    Player_SetModelsForHoldingShield(player);

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

    // Burning Deku Stick flame VFX — placed BEFORE the cross-timeline
    // and PvP early-returns because flame visibility is purely cosmetic
    // and unrelated to interaction gating. (End-of-function placement
    // is unreachable in cooperative play — pvpMode == 0 default
    // returns early at the PvP gate below.)
    //
    // Mirrors Player_UpdateBurningDekuStick (z_player.c:11630) on the
    // local owner — that function only runs in the local Player_Update
    // path, so a peer's DummyPlayer never spawned the flame. unk_860
    // is the burning countdown (0 = unlit, > 0 = burning); unk_85C is
    // the visual Y-scale that ramps to 0 during the final 20-frame
    // burn-out. Both are now synced via PLAYER_UPDATE.
    //
    // meleeWeaponInfo[0].tip is computed each draw by Player_Draw
    // (z_player_lib.c:1789) from the joint table, so it's valid for
    // DummyPlayer once the first draw cycle has run. One-frame lag is
    // invisible at 20fps logic.
    if (client.heldItemAction == PLAYER_IA_DEKU_STICK && client.unk_860 > 0) {
        static Vec3f kFlameVel   = { 0.0f, 0.5f, 0.0f };
        static Vec3f kFlameAccel = { 0.0f, 0.5f, 0.0f };
        static Color_RGBA8 kFlamePrim = { 255, 255, 100, 255 };
        static Color_RGBA8 kFlameEnv  = { 255,  50,   0,   0 };
        f32 temp = (client.unk_85C > 0.0f && client.unk_85C < 1.0f) ? client.unk_85C : 1.0f;
        func_8002836C(play, &player->meleeWeaponInfo[0].tip,
                      &kFlameVel, &kFlameAccel, &kFlamePrim, &kFlameEnv,
                      (s16)(temp * 200.0f), 0, 8);
    }

    // Pillar B Phase 3 — cross-timeline interaction gate (Q 4.B.4).
    // A child-timeline player and an adult-timeline player can occupy
    // the "same" scene (sceneNum) but their world-state is independent,
    // so any collision / lock-on / damage between them is meaningless.
    // Treat them as the pvpMode=0 case: disable lock-on and skip the
    // collider setup entirely. This runs BEFORE the AC-registration
    // block below because cross-timeline trumps every interaction
    // (including PvP-FF and hostile-NPC PvE).
    if (client.linkAge != gSaveContext.linkAge) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    // Hostile-NPC PvE damage path — AC registration + AC_HIT broadcast
    // fires regardless of PvP mode. The Invader (and any future
    // hostile-NPC actor) uses an AT_TYPE_ENEMY AT collider; we stamp
    // AC_TYPE_ENEMY unconditionally so AT_TYPE_ENEMY → AC_TYPE_ENEMY
    // hits register here naturally. PvP friendly-fire requires
    // AC_TYPE_PLAYER (since Player AT colliders are AT_TYPE_PLAYER)
    // — that bit is added only when PvP is active.
    //
    // Field-test log 359: with the pre-fix `AC_TYPE_PLAYER`-only AC
    // (set in DummyPlayer_Init line 119), Player swings damaged the
    // DummyPlayer in pvpMode=0 sessions (friendly fire), and Invader
    // swings (AT_TYPE_ENEMY) failed to register at all. Re-stamping
    // each frame fixes both: PvP-off → AC_TYPE_ENEMY only (Invader
    // hits, Player misses); PvP-on (and same-team in mode 1 not
    // active, etc.) → AC_TYPE_ENEMY | AC_TYPE_PLAYER (both hit).
    //
    // Without this fix, the original code returned at the PvP gate
    // below for pvpMode==0 sessions, leaving DummyPlayer's AC
    // unregistered — host's CollisionCheck never tested Invader AT
    // against DummyPlayer AC, so AC_HIT never fired, so no
    // DAMAGE_PLAYER broadcast reached the peer's local Link. Field-
    // test log 349: Invader swung sword ~30 times over 75s; P2's
    // Link took zero damage. See Plans/invader_field_test_log349_findings.md.
    const bool pvpActive =
        (Anchor::Instance->roomState.pvpMode != 0) &&
        !(Anchor::Instance->roomState.pvpMode == 1 &&
          client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default")) &&
        !SceneMultiplayerConfig::ShouldDisablePvP(gPlayState);

    // Widen AC type bits on every collider the DummyPlayer can register
    // as AC.
    //
    // Body cylinder: vanilla Player init is AC_TYPE_PLAYER only (PvP
    // friendly-fire). Stamping AC_TYPE_ENEMY each frame is what lets
    // cross-machine hostile NPCs (Invader, Goroiwa, etc.) register hits.
    // This is the load-bearing patch — without it, no DAMAGE_PLAYER
    // ever fires for cross-machine PvE.
    //
    // Shield quad: vanilla init at z_player.c:10700 is
    // `AC_ON | AC_HARD | AC_TYPE_ENEMY` — it ALREADY accepts
    // AT_TYPE_ENEMY. The widen call here is defensive symmetry only;
    // it's a no-op against vanilla today. The actual "shield blocks
    // hostile NPCs" fix is the AC_BOUNCED check at the AC_HIT gate
    // below, NOT the type bits.
    WidenDummyAcForCrossMachine(&player->cylinder.base, pvpActive);
    WidenDummyAcForCrossMachine(&player->shieldQuad.base, pvpActive);

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    // AC_HIT edge log — fires when collider transitions false→true.
    // Useful for verifying that a new attacker's AT actually registers
    // against the DummyPlayer's body cylinder. acFlags is bit-packed by
    // CollisionCheck_AT during the pre-update collision pass.
    static std::unordered_map<uint32_t, bool> sLastAcHitState;
    const bool acHitNow = (player->cylinder.base.acFlags & AC_HIT) != 0;
    if (acHitNow && !sLastAcHitState[clientId]) {
        const u16 acHitAttackerId = (player->cylinder.base.ac != nullptr) ? player->cylinder.base.ac->id : 0;
        SPDLOG_INFO("[DummyPlayer] AC_HIT edge clientId={} attackerId=0x{:04X} damage={} damageEffect={}",
                    clientId,
                    acHitAttackerId,
                    (int)player->actor.colChkInfo.damage,
                    (int)player->actor.colChkInfo.damageEffect);

        // Vanilla Mirror Pattern — candidate-queue gap instrumentation.
        // Fires once per (attackerId) globally so log volume stays
        // bounded. Catches when a candidate-queue attacker actually
        // reaches a DummyPlayer in field-test, signalling that the
        // missing Vanilla Mirror Pattern instance is now demand-driven
        // by real gameplay (not just theoretical).
        //
        // Each entry names the attacker + the missing vanilla effect.
        // The 4-step recipe to fix is in session_state.md → "Vanilla
        // Mirror Pattern". Implementation references existing Path A
        // (DamagePlayer.cpp) and ShieldBounce (ShieldBouncePlayer.cpp)
        // as templates.
        //
        // Edge-triggered (false→true of AC_HIT) so we don't re-log on
        // every AT-active frame. Combined with the per-attackerId-once
        // global rate-limit, the log fires at most once per attacker
        // per process lifetime.
        {
            struct VanillaMirrorCandidate {
                const char* name;
                const char* missingEffect;
            };
            static const std::unordered_map<u16, VanillaMirrorCandidate> sCandidates = {
                { ACTOR_EN_RR,
                  { "Like Like",
                    "equipment theft — vanilla calls Inventory_DeleteEquipment(EQUIP_TYPE_SHIELD/TUNIC)" } },
                { ACTOR_EN_WALLMAS,
                  { "Wallmaster",
                    "teleport to room entrance on grab" } },
                { ACTOR_EN_FLOORMAS,
                  { "Floormaster",
                    "teleport to room entrance on grab" } },
                { ACTOR_EN_RD,
                  { "Redead / Gibdo",
                    "grab freeze (sets freezeTimer + csCtx state)" } },
                { ACTOR_EN_DHA,
                  { "Dead Hand Arm",
                    "grab restrain + Dead Hand bite sequence" } },
                { ACTOR_BOSS_MO,
                  { "Morpha (boss)",
                    "nucleus grab + spin + position lock" } },
            };
            static std::unordered_set<u16> sLoggedCandidateGap;
            auto candIt = sCandidates.find(acHitAttackerId);
            if (candIt != sCandidates.end() &&
                sLoggedCandidateGap.find(acHitAttackerId) == sLoggedCandidateGap.end()) {
                sLoggedCandidateGap.insert(acHitAttackerId);
                SPDLOG_WARN("[VanillaMirror.gap] attackerId=0x{:04X} ({}) hit a DummyPlayer "
                            "but its vanilla side-effect — {} — is NOT mirrored "
                            "cross-machine. Path A (DAMAGE_PLAYER) delivers damage + "
                            "knockback only. Add a Vanilla Mirror Pattern instance for "
                            "this effect: see session_state.md → 'Vanilla Mirror Pattern' "
                            "(candidate queue lists this attacker; 4-step recipe + "
                            "DamagePlayer.cpp + ShieldBouncePlayer.cpp are the templates). "
                            "Logged once per attackerId per session.",
                            acHitAttackerId, candIt->second.name, candIt->second.missingEffect);
            }
        }
    }
    sLastAcHitState[clientId] = acHitNow;

    // Bug 1 fix (2026-06-05) — local post-hit suppression timer.
    // player->invincibilityTimer is overwritten from peer state at line 360
    // every tick, so the iframes we set below are stomped before the next
    // frame. Without a locally-owned timer, the AT collider's next active
    // frame re-triggers AC_HIT before the peer's iframes round-trip back —
    // result: 2+ DAMAGE_PLAYER packets per single swing (field-test log 406
    // showed two AC_HIT edges 100ms apart with both invincibilityTimer=0).
    //
    // Keep the local suppression keyed by clientId, decrement each tick,
    // gate the send on max(peer-synced timer, local suppression).
    static std::unordered_map<uint32_t, int> sLocalPostHitGuard;
    auto& localGuard = sLocalPostHitGuard[clientId];
    if (localGuard > 0) {
        --localGuard;
    }
    const bool acHitForGate     = (player->cylinder.base.acFlags & AC_HIT) != 0;
    const bool peerIframesOpen  = player->invincibilityTimer == 0;
    const bool localGuardOpen   = localGuard == 0;
    // Shield-bounce check — mirrors vanilla Player_Update (z_player.c:4813).
    // When an AT collides with both the shield quad AND the body cylinder
    // in the same frame, the shield's AC_HARD bumper sets AC_BOUNCED on the
    // shield. Vanilla Player reads this as "attack blocked, suppress body
    // damage." DummyPlayer.cpp doesn't run vanilla Player_Update, so we
    // mirror that check explicitly here. Without this, hostile NPC ATs
    // (Invader, hintnut nutsballs, etc.) hit the body AC, fire AC_HIT,
    // and DAMAGE_PLAYER broadcasts as if no shield was up.
    //
    // Note: the shield AC is registered every draw frame by vanilla
    // Player_UpdateShieldCollider (z_player_lib.c:1525) when stateFlags1
    // carries PLAYER_STATE1_SHIELDING. The state is mirrored from peer via
    // PLAYER_UPDATE and applied in DummyPlayer_Update's Player_SetModelsForHoldingShield
    // call — so as long as PLAYER_UPDATE is current, the shield AC exists
    // and AC_BOUNCED accurately reflects whether the AT was blocked.
    const bool shieldBounced = (player->shieldQuad.base.acFlags & AC_BOUNCED) != 0;
    const bool shieldBlockOpen = !shieldBounced;
    // Bug B fix (2026-06-05) — only the AUTHORITATIVE room host should
    // broadcast DAMAGE_PLAYER from a DummyPlayer AC_HIT. Without this
    // gate, peers also send DAMAGE_PLAYER whenever a synced enemy
    // (Goroiwa, etc.) hits their local DummyPlayer-of-the-host-or-
    // other-peer — which is just a replicated collider, not an
    // authoritative damage event. Field-test 413 confirmed this caused
    // P1 to be knocked back whenever Goroiwa hit P1's DummyPlayer on
    // P2's machine.
    //
    // Pillar A Phase 2's IsMyCurrentRoomHost() is the correct
    // authority: the room host owns BOTH the per-room hostile NPCs
    // (Invader) AND the synced vanilla enemies in that room.
    const bool authoritative    = ::SceneAuthority::IsMyCurrentRoomHost();

    // Per-client-local projectile attackers — actors whose damage outcome
    // is authoritatively decided on each client independently. Each
    // client's local-AI runs its own instance of the projectile aimed
    // at that client's local nearest player; the reflect-vs-damage
    // outcome is determined by THAT client's vanilla collision pass.
    // Host must NOT broadcast DAMAGE_PLAYER (or SHIELD_BOUNCE_PLAYER)
    // when host's own local projectile happens to also hit peer's
    // DummyPlayer body — peer's local instance already delivered the
    // correct outcome via vanilla collision (reflect if shield up, damage
    // if not). Cross-machine broadcast duplicates the outcome: log 441
    // showed P2 taking damage from host's wire packet ~30ms BEFORE P2's
    // local nutball even reached the shield, even though P2's local
    // reflect succeeded. Same architectural reason `shape.rot` is excluded
    // from sync for ACTOR_EN_HINTNUTS at HookHandlers.cpp:2066
    // (commit 52bb02634): per-client-local-AI semantics extend from aim
    // through damage application.
    //
    // Add an entry here when adding any future per-client-local-AI
    // projectile actor. Sibling concept: shape.rot exclusion in
    // HookHandlers.cpp's isAnimationDrivenPos.
    const u16 attackerIdNow = (player->cylinder.base.ac != nullptr)
                              ? player->cylinder.base.ac->id : 0;
    // Preemptive list — every confirmed AT_TYPE_ENEMY projectile actor
    // whose parent is currently in the sync pipeline (ACTORCAT_ENEMY
    // auto-admit). Each one is structurally susceptible to the log 441
    // wire-duplicate-damage bug if/when its parent fires at a player.
    //
    // Octorok: blanket-include ACTOR_EN_OKUTA is safe — parent
    // (params=0) has AT_NONE per z_en_okuta.c:61, only the projectile
    // form (params!=0, recategorised to PROP via Actor_ChangeCategory)
    // carries AT_TYPE_ENEMY (z_en_okuta.c:41). Parent never triggers
    // this gate even if included.
    //
    // Boss-spawned projectiles (En_Fhg_Fire, En_Vb_Ball, En_Bdfire)
    // are deliberately NOT added today — their parent bosses are NOT
    // in IsSyncedBossActor yet (only Boss_Goma is), so the bug can't
    // manifest. Add each to this list in the SAME PR that admits its
    // parent boss to IsSyncedBossActor.
    //
    // Sibling concept: shape.rot exclusion at HookHandlers.cpp:2066
    // (`isAnimationDrivenPos`). The two lists target different effects
    // (aim-direction vs damage-application) of the same per-client-
    // local-AI design principle. They don't have to match 1:1.
    const bool attackerIsPerClientProjectile =
        (attackerIdNow == ACTOR_EN_NUTSBALL)      ||
        (attackerIdNow == ACTOR_EN_ANUBICE_FIRE)  ||  // Anubis (Spirit Temple)
        (attackerIdNow == ACTOR_EN_FD_FIRE)       ||  // Flare Dancer (Fire Temple)
        (attackerIdNow == ACTOR_EN_FIRE_ROCK)     ||  // King Dodongo / Volvagia fire pillar rocks
        (attackerIdNow == ACTOR_EN_OKUTA)         ||  // Octorok rock spit (parent AT_NONE; safe)
        (attackerIdNow == ACTOR_EN_HONOTRAP);         // Honotrap flame children (FLAME_MOVE/_DROP).
                                                       // Eye variant has AT_NONE (tris collider
                                                       // line 91 of z_en_honotrap.c), so the
                                                       // attacker form is only the flame.
                                                       // Blanket-include by id is safe — the
                                                       // eye can't trigger this AC_HIT gate.

    const bool gateOpen         = acHitForGate && peerIframesOpen
                               && localGuardOpen && authoritative
                               && shieldBlockOpen
                               && !attackerIsPerClientProjectile;

    // Shield-block side effects on host + notification to peer. Triggers
    // when the shieldQuad's AC_BOUNCED is set this frame AND the body
    // would otherwise have been gated open by AC_HIT. The shield-block
    // event is reportable independent of peerIframesOpen / localGuard
    // (those are damage-rate-limiters; a shield bounce is a separate
    // class of event from a damage hit). Authoritative gate still
    // applies — non-host machines should not broadcast.
    //
    // Three vanilla-parity effects must fire:
    //   1. Host-local particle + sfx at the shield's actual hit position
    //      (Bug 3 — log 426 reported the wrong-position particle came
    //      from somewhere else; the correct spark at shieldQuad.bumper.hitPos
    //      should appear regardless of the wrong-position artifact).
    //   2. Peer-local particle + sfx at peer's own shield (Bug 2 — peer
    //      sees its OWN shield being struck).
    //   3. Peer-local knockback push (Bug 1 — vanilla z_player.c:4856
    //      sets linearVelocity = -18 on the player when shield bounces;
    //      need same on peer's local Link).
    // Edge-trigger the shield-bounce event so vanilla parity holds.
    // Vanilla z_player.c:4813 reads sp64 and applies linearVelocity=-18
    // each frame sp64 is true, BUT vanilla's CollisionCheck only sets
    // shield's AC_BOUNCED on the actual collision frame (one tick).
    // Our gate `acHitForGate && shieldBounced` would fire every tick
    // of the Invader's AT-active window (3-5 frames), shipping that
    // many SHIELD_BOUNCE_PLAYER packets and each one resetting peer's
    // linearVelocity = -18 between Player_Update calls → cumulative
    // pushback far exceeds vanilla. Fix: track false→true edge.
    static std::unordered_map<uint32_t, bool> sLastShieldBouncedHit;
    const bool shieldBouncedNow  = acHitForGate && shieldBounced;
    const bool shieldBouncedEdge = shieldBouncedNow && !sLastShieldBouncedHit[clientId];
    sLastShieldBouncedHit[clientId] = shieldBouncedNow;

    // Per-client-local-AI projectile attackers also skip the wire
    // SHIELD_BOUNCE_PLAYER broadcast — peer's vanilla local collision
    // already produced the shield-bounce particle + sfx on peer's
    // machine via CollisionCheck_HitSolid. A wire packet would duplicate
    // the visual. Same gate as DAMAGE_PLAYER above.
    if (shieldBouncedEdge && authoritative && !attackerIsPerClientProjectile) {
        const u16 blockedAttackerId = (player->cylinder.base.ac != nullptr)
                                      ? player->cylinder.base.ac->id : 0;
        // bumper.hitPos is Vec3s set by CollisionCheck when AC_BOUNCED
        // fires. Convert to Vec3f for the spawn helpers.
        Vec3f hitPos;
        hitPos.x = (f32)player->shieldQuad.info.bumper.hitPos.x;
        hitPos.y = (f32)player->shieldQuad.info.bumper.hitPos.y;
        hitPos.z = (f32)player->shieldQuad.info.bumper.hitPos.z;

        // Effect 1 — host-local particle + sfx at shield hit position.
        // Branch on the DummyPlayer's currentShield (mirrored from peer
        // via PLAYER_UPDATE) so Deku Shield gets the wood-bounce visual,
        // Hylian/Mirror get metal sparks. Mirrors vanilla
        // CollisionCheck_HitSolid logic at z_collision_check.c:1597.
        const bool isWoodShield = (player->currentShield == PLAYER_SHIELD_DEKU);
        if (isWoodShield) {
            EffectSsHitMark_SpawnFixedScale(gPlayState, EFFECT_HITMARK_DUST, &hitPos);
            Audio_PlaySoundGeneral(NA_SE_IT_REFLECTION_WOOD, &player->actor.projectedPos, 4,
                                   &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                                   &gSfxDefaultReverb);
        } else {
            EffectSsHitMark_SpawnFixedScale(gPlayState, EFFECT_HITMARK_METAL, &hitPos);
            CollisionCheck_SpawnShieldParticlesMetalSound(gPlayState, &hitPos, &player->actor.projectedPos);
        }

        // Effects 2 + 3 — notify peer via SHIELD_BOUNCE_PLAYER packet.
        // Peer's own currentShield drives its effect choice (peer is the
        // one shielding, knows its own shield type). No need to ship
        // shield-type info over the wire.
        const f32 hitOffsetY = hitPos.y - player->actor.world.pos.y;  // ~chest height
        Anchor::Instance->SendPacket_ShieldBouncePlayer(client.clientId, blockedAttackerId, hitOffsetY);

        SPDLOG_INFO("[DummyPlayer] shield BLOCKED clientId={} attackerId=0x{:04X} "
                    "hitPos=({:.1f},{:.1f},{:.1f}) shieldType={} ({}) — "
                    "host effect spawned + SHIELD_BOUNCE_PLAYER sent (edge)",
                    clientId, blockedAttackerId, hitPos.x, hitPos.y, hitPos.z,
                    (int)player->currentShield, isWoodShield ? "wood" : "metal");
    }

    // Per-frame send-gate evaluation log removed — gate behavior is
    // observable via the SEND log presence/absence below. Re-enable if
    // a future bug suspects double-sends or stuck localGuard.

    if (gateOpen) {
        // Bug 3 fix (2026-06-05) — pass the attacker's world position
        // so the peer-side knockback yaw is computed from the actual
        // attacker (e.g. Invader actor) rather than from the sender's
        // own player.
        //
        // Bug C fix (2026-06-05) — `.ac` is the attacker on an AC
        // bumper, NOT `.at`. CollisionCheck_SetATvsAC writes
        // `ac->ac = at->actor` (z_collision_check.c:1740). The prior
        // `.at` read was always NULL because DummyPlayer's cylinder
        // is only registered as AC (non-PvP), so attackerPos was
        // never set and the receive side fell through to the
        // legacy "yaw from sender's player" path — which produced
        // the wrong-direction knockback on the receiver in field-
        // test 413.
        const Vec3f* attackerPos = (player->cylinder.base.ac != nullptr)
            ? &player->cylinder.base.ac->world.pos
            : nullptr;
        // Bug 2 fix Option C (2026-06-05) — ship the raw AT damage value
        // from the attacker's collider element, NOT the table-filtered
        // colChkInfo.damage. The DummyPlayer damage table (used by
        // CollisionCheck at z_collision_check.c:3023) collapses every
        // attacker that uses a given damage type bit to ONE damage value
        // — but Goroiwa = 4 HP, Iron Knuckle = 64 HP, Bigokuta = 8 HP all
        // share bit 29 ("Unblockable"). The table cannot represent them
        // distinctly. acHitInfo points at the AT ColliderInfo of the
        // attacker that just landed; toucher.damage is the raw per-
        // collider damage value (in HP units). Shipping that gives
        // vanilla parity for every attacker. See
        // Plans/dummy_player_damage_table_audit.md.
        u8 sendDamage = player->actor.colChkInfo.damage;
        if (player->cylinder.info.acHitInfo != nullptr) {
            sendDamage = player->cylinder.info.acHitInfo->toucher.damage;
        }
        // Path A — look up vanilla knockback params for this attacker.
        // Registered → ship the knockback block; peer's Player_Update
        // reproduces the exact local-hit response. Not registered →
        // kbType=0 sentinel; receiver falls back to legacy
        // func_80837C0C path (loses vanilla animation / iframe parity).
        // See Common/EnemyKnockbackTable.{h,cpp}.
        u32 kbType = 0;
        f32 kbSpeed = 0.0f;
        f32 kbYVel = 0.0f;
        u32 kbDamage = 0;
        if (player->cylinder.base.ac != nullptr) {
            AnchorKnockback::KnockbackParams kbp;
            if (AnchorKnockback::LookupKnockback(player->cylinder.base.ac->id, &kbp)) {
                kbType   = kbp.type;
                kbSpeed  = kbp.speed;
                kbYVel   = kbp.yVelocity;
                kbDamage = kbp.damage;
            } else {
                // Path A bypass instrumentation. Fires once per
                // (attackerId) global so log volume stays bounded —
                // catches new vanilla attackers that should be added
                // to EnemyKnockbackTable when they first hit a peer.
                //
                // Intentional bypass cases (custom actors with their
                // own damage semantics — Invader, future NPCs) won't
                // produce false positives if they're explicitly
                // suppressed below. Add IDs to sExpectedBypassIds as
                // new bypass-by-design senders surface.
                static const std::unordered_set<u16> sExpectedBypassIds = {
                    // Custom actors that intentionally don't use Path A:
                    //   (none today — placeholder. Invader DOES want
                    //    Path A eventually; track separately.)

                    // ─── ACTOR_EN_GOMA (Boss_Goma larva, 0x002B) ──────
                    // Vanilla En_Goma has NO func_8002F6D4/_71C/_758/_7A0
                    // call (verified by grep across ovl_En_Goma + ovl_Boss_
                    // Goma — zero matches). Damage to Link flows through
                    // the AT collider's toucher.damage field (8 HP per
                    // hit, set in D_80A4B7A0 at z_en_goma.c:81) consumed
                    // by Player_Update's auto-knockback handler. There
                    // are no fixed knockback params to extract, so Path A
                    // admission isn't applicable.
                    //
                    // Cross-machine sync still works via the legacy
                    // func_80837C0C path in Packets/Player/DamagePlayer.cpp
                    // (hardcoded speed=4.0 / yVel=5.0 / 20-frame iframes).
                    // Cosmetic deviation from vanilla (which would route
                    // through Player_GetDamageReaction's table lookup),
                    // but damage + iframe behavior is correct.
                    //
                    // Bug 1 (host's local Link false-knockback) is also
                    // N/A: EnGoma_UpdateHit's only GET_PLAYER read at
                    // line 634 is for shield-bounce direction (AC_HIT,
                    // not AT_HIT), so the Pitfall 28 hazard class doesn't
                    // apply. EnGoma_Update's GET_PLAYER at line 737 is
                    // visual-only (eye pitch/yaw).
                    //
                    // Source-of-truth audit: 2026-06-08 (log 448).
                    ACTOR_EN_GOMA,
                };
                static std::unordered_set<u16> sLoggedBypass;
                const u16 attackerId = player->cylinder.base.ac->id;
                if (sExpectedBypassIds.find(attackerId) == sExpectedBypassIds.end() &&
                    sLoggedBypass.find(attackerId) == sLoggedBypass.end()) {
                    sLoggedBypass.insert(attackerId);
                    SPDLOG_WARN("[Path A bypass] attackerId=0x{:04X} hit a DummyPlayer "
                                "but is NOT in EnemyKnockbackTable. Full fix is TWO parts: "
                                "(1) add entry to Common/EnemyKnockbackTable.cpp — "
                                "extract speed/yVel/type/kbDamage from the actor's "
                                "func_8002F6D4/_71C/_758/_7A0 call site (Bug 2 = "
                                "cross-machine damage parity); "
                                "(2) gate that call in the actor's AT_HIT branch on "
                                "Anchor_DistXZToLocalLink(actor, play) < HIT_RADIUS — "
                                "see Pitfall 28 in session_state.md (Bug 1 = host's "
                                "local Link must not be falsely knocked back when only "
                                "a DummyPlayer was hit). Without (2) the host still has "
                                "Bug 1 for this attacker even after (1) ships damage "
                                "correctly. Logged once per attackerId per session.",
                                attackerId);
                }
            }
        }
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  sendDamage, attackerPos,
                                                  kbType, kbSpeed, kbYVel, kbDamage);
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
        // 20 ticks ≈ 1s at 20fps — same window as vanilla post-hit iframes.
        // Slightly longer than the round-trip the peer's PLAYER_UPDATE
        // typically takes to sync its iframes back; once that arrives,
        // either gate is sufficient to keep blocking.
        localGuard = 20;

        // SEND log — single line per successful send. Wire damage / kb
        // params / attacker info on one row for easy field-test parsing.
        const u16 attackerId = (player->cylinder.base.ac != nullptr)
                               ? player->cylinder.base.ac->id : 0;
        SPDLOG_INFO("[DummyPlayer] SEND DAMAGE_PLAYER clientId={} attackerId=0x{:04X} "
                    "damage={} effect={} kbType={} kbSpeed={:.1f}",
                    client.clientId, attackerId, (int)sendDamage,
                    (int)player->actor.colChkInfo.damageEffect,
                    kbType, kbSpeed);
    }

    const bool wouldSetAC =
        !(player->stateFlags2 & PLAYER_STATE2_FROZEN) &&
        !(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
        (player->invincibilityTimer <= 0);
    if (wouldSetAC) {
        CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);
    }

    // [DummyPlayer.Diag] — rate-limited heartbeat (~1Hz at 20fps tick)
    // showing the SetAC gate's inputs. Confirms whether DummyPlayer's
    // AC is being registered each frame and what invincibilityTimer
    // value we see. If wouldSetAC is consistently false, the AC isn't
    // in the collision-check list → AC_HIT can never fire → no damage
    // broadcast can ever happen.
    static int sDummyDiagHeartbeat = 0;
    if (++sDummyDiagHeartbeat >= 20) {
        SPDLOG_INFO("[DummyPlayer.Diag] heartbeat clientId={} invincibilityTimer={} stateFlags1=0x{:X} stateFlags2=0x{:X} wouldSetAC={} pos=({:.0f},{:.0f},{:.0f})",
                    clientId, player->invincibilityTimer,
                    player->stateFlags1, player->stateFlags2, wouldSetAC,
                    player->actor.world.pos.x, player->actor.world.pos.y,
                    player->actor.world.pos.z);
        sDummyDiagHeartbeat = 0;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);

    // PvP gate — controls lock-on enable + OC (physical push-apart
    // between players) + AT (DummyPlayer's own attack collider for
    // PvP friendly-fire) + mass. These behaviours are PvP-specific
    // and intentionally remain gated. The AC block above is the only
    // piece that hostile-NPC PvE needs. Reuses `pvpActive` so the AC
    // type bits stay in lockstep with the gate.
    if (!pvpActive) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer < 0)) {
            CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Title-screen peer branch (Phase 1 — Plans/title_screen_peer_actors.md).
    // Bypasses the gameplay-time scene/online/isSaveLoaded gate below since
    // title-screen peers have client.isSaveLoaded=false by construction and
    // client.sceneNum is irrelevant — we render based on the local Link's
    // scene context, not the peer's. The linkAge gate at line ~1066 is
    // retained: title screen forces adult Link (linkAge=0) on both sides
    // per Opening_SetupTitleScreen, so a matched-age peer renders normally
    // and a mismatched-age peer falls through to nametag-only (same as
    // the cross-timeline gameplay case).
    const bool titleMode = Anchor::Instance->IsDummyPlayerTitleMode(actor);
    if (!titleMode) {
        if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
            return;
        }
    } else {
        if (!client.online) return;
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

    // KB-19 — Pillar G.i companion gate. Pillar G.i lets actors keep
    // updating and drawing while the pause menu is open (so other
    // multiplayer clients see this client moving normally). The pause
    // menu, however, reconfigures gSegments[4]/[6] to point at its own
    // pause-allocated heap buffer for rendering pauseCtx->playerSkelAnime.
    // While those segments are mid-pause, calling Player_Draw on a remote
    // DummyPlayer reads vertex/skeleton data through the wrong segment
    // and either visibly distorts the local Link (vertex bug, KB-19) or
    // SEGVs inside Player_DrawImpl OPEN_DISPS (#171 Deku Tree crash —
    // R1 control test 2026-04-27 reproduced this with both clients in
    // SCENE_DEKU_TREE Room 0, P1 opens pause menu → CVarSetString stack-
    // walker artifact in the dump, real crash site z_player_lib.c:1040).
    // Suppressing the body draw for the few frames the pause menu is up
    // is the cleanest fix: world time still advances, the remote player
    // is briefly invisible, name tag still renders. R1/R2/R3 control
    // tests narrowed the trigger to exactly this condition.
    //
    // #182 Phase 2.5 Option 2: when the live-world pause-menu rendering
    // feature is active, the pause-Link DMA + segment override are
    // skipped (commit 21bee4caa) and the rotating-Link blit is skipped
    // (commit 66dcf51fa), so gSegments[4]/[6] stay pointed at the world's
    // object bank. The KB-19/#171 trigger condition does not apply, and
    // remote DummyPlayers can draw safely. Allow the draw through in
    // that case so peers stay visible behind the live-rendered pause UI.
    if (gPlayState->pauseCtx.state != 0 && !Anchor_PauseLiveWorldRendering()) {
        return;
    }

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

    // Held-actor sync (§3.2 + Phase 2 + log 317 follow-up) — Actor_Kill
    // any local actor we were carrying for this client. Detaching alone
    // would leave the pot in ObjTsubo_LiftedUp state, which checks
    // Actor_HasNoParent every tick and transitions to SetupThrown on
    // parent-cleared, immediately breaking on floor contact. Killing
    // matches the vanilla "held pot vanishes" semantic for the case
    // where the holder is no longer present. See
    // AnchorDummyDetachAndKillHeldActor comment.
    AnchorDummyDetachAndKillHeldActor(actor);

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
