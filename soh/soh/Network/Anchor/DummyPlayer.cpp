#include "Anchor.h"
#include "EnemyNetId.h"  // #243.7.2 — EnemyNetId no longer transitive via Anchor.h
#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"
#include "soh/Network/Anchor/Common/SceneMultiplayerConfig.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"  // IsMyCurrentRoomHost — Bug B fix gate (2026-06-05)
#include "soh/Network/Anchor/Common/EnemyKnockbackTable.h"  // Path A vanilla knockback lookup (2026-06-05)
#include "soh/Network/Anchor/Common/GameTimeControllerBridge.h"
#include "soh/Network/Anchor/Common/TitlePeerFormation.h"  // title-mode formation math
#include "soh/Enhancements/nametag.h"
#include <unordered_map>
#include <unordered_set>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.h"  // EFFECT_HITMARK_METAL
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"  // mounted-pose reconciliation + ENHORSE_ANIM_*
#include "objects/object_horse/object_horse.h"        // gEpona*Anim (Phase 3 state mirror)
#include "objects/gameplay_keep/gameplay_keep.h"      // gPlayerAnim_link_uma_anim_stand (title-peer freeze pose)
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

namespace {

// Title-mode per-peer Link skelAnime buffers. Decoupled from local
// Link via the "freeze at offset frame" mechanism — LinkAnimation_
// Change loads ONE frame into the buffer, no further ticks happen,
// each peer holds a different static pose of local Link's current
// animation. Avoids the LinkAnimation_Update foot-gallop-flopping
// pitfall (field test log 487) by never running the per-frame
// animation tick. Sized for Link's max 22-limb skel + 2-Vec3s
// padding = 24 slots.
constexpr int kMaxTitlePeerBuffers = 8;
constexpr int kLinkJointTableLen   = 24;
Vec3s sTitlePeerJointTable[kMaxTitlePeerBuffers][kLinkJointTableLen];
Vec3s sTitlePeerMorphTable[kMaxTitlePeerBuffers][kLinkJointTableLen];

// Title-mode Link-delta tracking. Peer horses translate by local
// Link's per-frame position delta instead of being re-derived from
// Link's pos+rot each frame. This eliminates the lateral "slide"
// that the previous full-recompute approach produced when local
// Link rotates — formation offsets used to rotate with Link, so
// peers visibly slid across the ground during turns. With delta
// motion, peers maintain their world-space position and only
// translate when Link translates.
//
// Sampled once per game frame from the formationIdx == 0 peer
// (sentinel — multiple peers tick per game frame, all sampling
// would over-count). Snap-to-target threshold below catches
// large jumps (scene cuts, formation override transitions) by
// forcing peer to the current target slot when distance exceeds
// the threshold.
Vec3f    sTitlePeerLastLinkPos    = {0, 0, 0};
uint64_t sTitlePeerLastDeltaFrame = 0;
Vec3f    sTitlePeerLinkDelta      = {0, 0, 0};
bool     sTitlePeerDeltaPrimed    = false;

}  // namespace

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

// Horse-sync mounted-pose reconciliation — single source of truth.
// Used by BOTH gameplay-time DummyPlayer_Update (below) AND the
// title-mode branch's horse integration (Plans/title_screen_peer_
// actors.md Phase 2). Per-frame state-driven check (not edge-trigger)
// so late-join races, scene transitions, reconnect/disconnect all
// self-heal.
//
// Mounted (client.mountedHorseNetId != 0): find the horse via
//   Anchor::mPeerHorses fast-path, falling back to FindActorByNetId.
//   Snap DummyPlayer.world.pos to peerHorse.world.pos + horse.riderPos
//   — replicating vanilla parent-child mount linkage on every receiver.
//   Mark PLAYER_STATE1_ON_HORSE so collision skip + animation overlays
//   behave correctly mid-ride.
// Dismounted (mountedHorseNetId == 0): clear PLAYER_STATE1_ON_HORSE so
//   the next frame's standard ground-position handling kicks in.
//
// Extraction history: lifted from inline block in DummyPlayer_Update
// 2026-06-09 as part of Phase 2 horse integration (clean-extraction
// option per user's "fix not workaround" guidance).
static void ApplyMountedPoseReconciliation(Actor* actor,
                                             const AnchorClient& client) {
    Player* player = (Player*)actor;

    if (client.mountedHorseNetId == 0) {
        player->stateFlags1 &= ~PLAYER_STATE1_ON_HORSE;
        return;
    }

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
    if (peerHorse == nullptr || peerHorse->id != ACTOR_EN_HORSE) {
        // Horse not yet spawned or wrong-type — leave stateFlags1 alone
        // (don't clear; rider will land at standard pos this tick and
        // self-heal once the horse replica arrives).
        return;
    }

    EnHorse* horseAsHorse = (EnHorse*)peerHorse;

    // Field-test 2026-06-11 (log 507): peer Link Y oscillated between
    // "correctly seated" and "floating ~27u above" the saddle. Two root
    // causes (title-screen agent at DummyPlayer.cpp:685-740 had already
    // solved both):
    //
    // 1. Missing -27.0f stirrup offset — vanilla z_player.c:13840-13842
    //    applies riderPos.y - 27.0f when positioning the mounted Player.
    //    riderPos.y points to the SADDLE BONE (top of saddle), Player's
    //    world.pos.y represents his FEET in the stirrups (27u below).
    //
    // 2. riderPos lifecycle race — EnHorse_Init seeds riderPos with
    //    ABSOLUTE world position (actor.world.pos + Vec3f{0, 70, 0}).
    //    Vanilla Skin_GetLimbPos call inside EnHorse_Draw converts it to
    //    a relative offset (~30u back, ~50u up, ~30u laterally). On the
    //    first frame after spawn, before any Draw has run, riderPos is
    //    still in absolute form → reading it as an offset flings the
    //    DummyPlayer to crazy world coords.
    //
    // Mitigation (mirror of title-screen agent's pattern): magnitude
    // check distinguishes relative offset (small absolute values) from
    // absolute world position (large values). Fall back to a static
    // approximation while the absolute seed is in play; switch to the
    // vanilla riderPos once it normalises.
    constexpr f32 kSaddleHeightAboveHorse        = 30.0f;
    constexpr f32 kSaddleBackwardFromHorseOrigin = 40.0f;

    const f32 rx = horseAsHorse->riderPos.x;
    const f32 ry = horseAsHorse->riderPos.y;
    const f32 rz = horseAsHorse->riderPos.z;
    const bool useVanillaRiderPos =
        (fabsf(rx) < 80.0f) && (fabsf(ry) < 150.0f) && (fabsf(rz) < 80.0f);

    if (useVanillaRiderPos) {
        actor->world.pos.x = peerHorse->world.pos.x + rx;
        actor->world.pos.y = (peerHorse->world.pos.y + ry) - 27.0f;  // stirrup offset
        actor->world.pos.z = peerHorse->world.pos.z + rz;
    } else {
        // First-frame fallback. Hardcoded approximation until
        // EnHorse_Draw normalises riderPos to a relative offset.
        const f32 yawRad = (f32)peerHorse->shape.rot.y / 32768.0f * (f32)M_PI;
        const f32 forwardX = sinf(yawRad);
        const f32 forwardZ = cosf(yawRad);
        actor->world.pos.x = peerHorse->world.pos.x -
            kSaddleBackwardFromHorseOrigin * forwardX;
        actor->world.pos.y = peerHorse->world.pos.y + kSaddleHeightAboveHorse;
        actor->world.pos.z = peerHorse->world.pos.z -
            kSaddleBackwardFromHorseOrigin * forwardZ;
    }
    actor->shape.rot.y = peerHorse->shape.rot.y;
    player->stateFlags1 |= PLAYER_STATE1_ON_HORSE;
}

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

    // #304 — Neuter DummyPlayer's sword AT at Init for lifecycle symmetry
    // with the per-frame clear in DummyPlayer_Update. Prevents any first-
    // frame edge case where Draw could fire before Update (e.g. freezeTimer
    // suppressing Update). See DummyPlayer_Update for full rationale.
    player->meleeWeaponQuads[0].base.atFlags &= ~AT_ON;
    player->meleeWeaponQuads[1].base.atFlags &= ~AT_ON;

    gSaveContext.linkAge = originalAge;
    if (DebugLogSwapWindows()) {
        SPDLOG_INFO("[KB19][SwapExit:Init] clientId={} restoredAge={}", clientId, originalAge);
    }

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom) {
        // Color the peer's name in their Anchor color; alpha=255 marks
        // the override active so the nametag system also derives a
        // brightness-adaptive background (nametag.cpp DrawNameTag).
        NameTagOptions opts = {};
        opts.textColor = { client.color.r, client.color.g, client.color.b, 255 };
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), opts);
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
                // Formation derivation lives in Common/TitlePeerFormation.h
                // — single source of truth shared with SpawnTitlePeerLink's
                // initial-spawn path. To tune the formation in v3+
                // (stagger / yaw divergence / animation phase), modify only
                // MakeTitlePeerFormation in that header; this call site
                // needs no changes.
                // Rotation source — prefer the local cutscene Epona's
                // shape.rot.y over local Link's world.rot.y. The title
                // cutscene's actor cue can rotate Link to face the
                // camera during close-up shots (e.g., shot transitions
                // where Link's body turns to be visible in the frame),
                // but the horse's facing direction is always the
                // gallop heading toward the next waypoint. Using the
                // horse's rotation makes peers face the actual gallop
                // direction at all times, not whatever momentary pose
                // local Link is in. Falls back to local Link's
                // world.rot.y when no cutscene Epona is present
                // (e.g., CVar gate off → foot peers, or non-Hyrule-
                // Field title shots).
                //
                // Local cutscene Epona identification matches the
                // animation-mirror walk below: ACTOR_EN_HORSE with
                // params=7 in ACTORCAT_BG (z_horse.c:190 — the title-
                // cutscene spawn variant).
                int16_t lookYaw = localLink->actor.world.rot.y;
                {
                    Actor* a =
                        gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
                    while (a != nullptr) {
                        if (a->id == ACTOR_EN_HORSE && a->params == 7) {
                            lookYaw = a->shape.rot.y;
                            break;
                        }
                        a = a->next;
                    }
                }
                // Per-shot left-side formation magnitude.
                // Shot 5 (csFrame 1080-1104): idle by river, default
                //   formation (peers absent during close-up anyway).
                // Shot 6 (csFrame 1105-1204): low dramatic camera —
                //   river hazard, full 150u left-side offset.
                // Shots 7-9 (csFrame 1205-1605): riding alongside the
                //   river, reduced 75u left-side offset (field-test
                //   tuning — the wider shots make 150u look cramped).
                // All other shots: default V/diamond formation.
                float leftLateral = 0.0f;
                if (gPlayState->csCtx.state != CS_STATE_IDLE) {
                    const int32_t cf = gPlayState->csCtx.frames;
                    if      (cf >= 1105 && cf <= 1204) leftLateral = 150.0f;
                    else if (cf >= 1205 && cf <= 1605) leftLateral =  75.0f;
                }

                AnchorTitlePeer::TitlePeerSlot slot =
                    AnchorTitlePeer::ComputeTitlePeerSlot(
                        localLink->actor.world.pos,
                        lookYaw,
                        clientId, formationIdx,
                        leftLateral);

                // Path A — ground snap via raycast from above the slot
                // position. Hyrule Field's terrain dips and rolls under
                // the title-cutscene camera path, so a fixed Y from
                // local Link's pos puts horses floating above (or below)
                // the ground on slopes. We raycast from slot.y + 200u
                // (well above any reasonable terrain) downward; whatever
                // floor poly we hit is the ground level for this slot.
                // BgCheck functions are available at title screen — the
                // colCtx is initialised by Play_Init regardless of
                // gameMode. Returns BGCHECK_Y_MIN (-32000) if no floor;
                // we treat that as "no snap" and keep slot.y as-is.
                {
                    Vec3f probe = { slot.pos.x, slot.pos.y + 200.0f, slot.pos.z };
                    CollisionPoly* outPoly = nullptr;
                    f32 groundY = BgCheck_EntityRaycastFloor1(
                        &gPlayState->colCtx, &outPoly, &probe);
                    if (groundY > BGCHECK_Y_MIN) {
                        slot.pos.y = groundY;
                    }
                }

                // Path A horse position write — horse stands on the
                // ground at the formation slot, rider sits above the
                // horse at saddle height. No reconciliation, no
                // riderPos lookup. We own position, vanilla EnHorse
                // can't drift the horse because its update function
                // is NULL'd at spawn time (Anchor.cpp SpawnTitlePeerLink).
                //
                // Lookup via FindTitlePeerHorse (mTitlePeerHorses,
                // clientId-keyed) instead of mPeerHorses (netId-keyed)
                // because horse-sync v1's own OnSceneSpawnActors hook
                // clears mPeerHorses on every scene init. If horse-sync's
                // hook fires AFTER ours on the same scene-init event
                // (registration order — we don't control it), our just-
                // written mPeerHorses entry gets wiped and the horse
                // becomes orphaned (still alive at spawn position, no
                // longer position-driven). mTitlePeerHorses is owned by
                // the title-screen path, never touched by horse-sync.
                // See log 467 — horse "Spawned ... Path A" lines fire
                // but lookups failed every tick.
                //
                // Local cutscene horse's current animationIdx, captured
                // during the horse-anim-mirror walk below and read by
                // the peer Link pose block. -1 = no local horse found
                // (e.g. CVar off → no peer horse spawned, or pre-gallop
                // setup frames). Used to choose alias vs freeze mode
                // for peer Link.
                int32_t localHorseAnimIdx = -1;

                // Spawned horse may not exist (CVar gate off; spawn
                // failure). When absent, the rider stands on the ground
                // at the slot — Phase 1 behaviour preserved.
                Actor* peerHorse = (Anchor::Instance != nullptr)
                    ? Anchor::Instance->FindTitlePeerHorse(clientId)
                    : nullptr;
                if (peerHorse != nullptr && peerHorse->id == ACTOR_EN_HORSE) {
                    // Sample local Link's per-frame position delta
                    // once per game frame (formationIdx == 0 peer is
                    // the sentinel — multiple peers run per game
                    // frame). Used below to translate peer horse
                    // position instead of re-deriving it from Link's
                    // current pos+rot. Eliminates the lateral slide
                    // when Link rotates.
                    if (formationIdx == 0 && Anchor::Instance != nullptr) {
                        const uint64_t curFrame =
                            Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
                        if (curFrame != sTitlePeerLastDeltaFrame) {
                            if (sTitlePeerDeltaPrimed) {
                                const float ddx =
                                    localLink->actor.world.pos.x - sTitlePeerLastLinkPos.x;
                                const float ddy =
                                    localLink->actor.world.pos.y - sTitlePeerLastLinkPos.y;
                                const float ddz =
                                    localLink->actor.world.pos.z - sTitlePeerLastLinkPos.z;
                                // Detect scene-cut teleport — cutscene
                                // jumps Link hundreds of units between
                                // shots / scenes. Translating peers by
                                // that delta would fling them. Treat
                                // large jumps as a "no advance" frame
                                // and rely on the snap-on-far path to
                                // realign peers via slot.pos.
                                constexpr float kSceneCutThresholdSq =
                                    1000.0f * 1000.0f;
                                if (ddx * ddx + ddy * ddy + ddz * ddz >
                                    kSceneCutThresholdSq) {
                                    sTitlePeerLinkDelta = {0.0f, 0.0f, 0.0f};
                                } else {
                                    sTitlePeerLinkDelta.x = ddx;
                                    sTitlePeerLinkDelta.y = ddy;
                                    sTitlePeerLinkDelta.z = ddz;
                                }
                            } else {
                                sTitlePeerLinkDelta = {0.0f, 0.0f, 0.0f};
                                sTitlePeerDeltaPrimed = true;
                            }
                            sTitlePeerLastLinkPos    = localLink->actor.world.pos;
                            sTitlePeerLastDeltaFrame = curFrame;
                        }
                    }

                    // Snap-or-delta motion. When peer's current
                    // position is close to the target formation slot
                    // (within kSnapThreshold), advance by Link's
                    // delta — preserves world-space position so
                    // peers don't slide when Link rotates. When far
                    // (scene cuts, formation-mode transitions like
                    // entering the river-shot left-side override),
                    // snap to the target slot so peers stay roughly
                    // in formation. 250u threshold catches the
                    // river-override step (150u lateral magnitude)
                    // while tolerating normal cutscene shot turns
                    // (~120u drift from a 90° formation rotation).
                    constexpr float kSnapThresholdSq = 250.0f * 250.0f;
                    const float dx = slot.pos.x - peerHorse->world.pos.x;
                    const float dz = slot.pos.z - peerHorse->world.pos.z;
                    const float distSq = dx * dx + dz * dz;
                    if (distSq > kSnapThresholdSq) {
                        peerHorse->world.pos = slot.pos;
                    } else {
                        peerHorse->world.pos.x += sTitlePeerLinkDelta.x;
                        peerHorse->world.pos.y += sTitlePeerLinkDelta.y;
                        peerHorse->world.pos.z += sTitlePeerLinkDelta.z;
                        // Re-snap Y to ground so peer doesn't drift
                        // above/below terrain when local Link's Y
                        // delta crosses terrain elevation changes.
                        Vec3f probe = { peerHorse->world.pos.x,
                                        peerHorse->world.pos.y + 200.0f,
                                        peerHorse->world.pos.z };
                        CollisionPoly* outPoly = nullptr;
                        f32 groundY = BgCheck_EntityRaycastFloor1(
                            &gPlayState->colCtx, &outPoly, &probe);
                        if (groundY > BGCHECK_Y_MIN) {
                            peerHorse->world.pos.y = groundY;
                        }
                    }
                    peerHorse->shape.rot.y = slot.rotY;

                    // Phase 3 horse animation state mirror — match local
                    // cutscene Epona's animation. When the cutscene
                    // transitions local Link's horse between gallop /
                    // idle / walk / rear / etc., peer horses follow.
                    // We only re-call Animation_PlayLoop on state CHANGE
                    // (not every frame) — once an animation is loaded,
                    // vanilla EnHorse_Idle's SkelAnime_Update keeps
                    // ticking it.
                    //
                    // Local cutscene Epona is identified by params=7 in
                    // ACTORCAT_BG (z_horse.c:190 — the title-cutscene
                    // spawn variant). Peer title horses spawn with
                    // params=0 so they're distinguishable. Walk is O(n)
                    // with n = BG actor count in scene (small).
                    EnHorse* localHorse = nullptr;
                    {
                        Actor* a =
                            gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
                        while (a != nullptr) {
                            if (a->id == ACTOR_EN_HORSE && a->params == 7) {
                                localHorse = (EnHorse*)a;
                                break;
                            }
                            a = a->next;
                        }
                    }
                    if (localHorse != nullptr) {
                        localHorseAnimIdx = localHorse->animationIdx;
                        EnHorse* peer = (EnHorse*)peerHorse;
                        if (peer->animationIdx != localHorse->animationIdx) {
                            AnimationHeader* anim = nullptr;
                            switch (localHorse->animationIdx) {
                                case ENHORSE_ANIM_IDLE:
                                    anim = (AnimationHeader*)&gEponaIdleAnim; break;
                                case ENHORSE_ANIM_WHINNEY:
                                    anim = (AnimationHeader*)&gEponaWhinnyAnim; break;
                                case ENHORSE_ANIM_STOPPING:
                                    // Asset name is "Refuse" but the runtime
                                    // enum index 2 is "STOPPING" (per
                                    // z_en_horse.h).
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
                            Animation_PlayLoop(&peer->skin.skelAnime, anim);
                            // Per-peer phase as fraction of loop length.
                            // TitlePeerFormation stores fraction × 256
                            // (slot-deterministic table — slots 0/1/2 →
                            // 25%/50%/75%, slots 3-7 fill in the other
                            // 1/8ths). Multiplying by the real loop
                            // length gives the right start frame for
                            // any animation (gallop 23, idle 119,
                            // rearing 32 etc. — measured in log 487).
                            const AnchorTitlePeer::TitlePeerFormation
                                phaseFormation =
                                AnchorTitlePeer::MakeTitlePeerFormation(
                                    clientId, formationIdx);
                            const f32 horseLoopFrames =
                                (f32)Animation_GetLastFrame((void*)anim);
                            const f32 horsePhaseFrac =
                                (f32)phaseFormation.animPhaseOffset / 256.0f;
                            const f32 horseStart =
                                horseLoopFrames * horsePhaseFrac;
                            peer->skin.skelAnime.curFrame = horseStart;
                            peer->animationIdx = localHorse->animationIdx;
                        }
                        // Mirror playSpeed every frame, not just on state
                        // change. Vanilla horse code modulates playSpeed
                        // from speedXZ within the WALK / TROT / GALLOP /
                        // STOPPING / JUMP action bodies (z_en_horse.c
                        // :1267, :1314, :1382, :1567, :1633, :1709), so
                        // the value drifts continuously during locomotion
                        // — not just at anim-change time. IDLE / WHINNEY
                        // / REARING / REFUSE never touch playSpeed so the
                        // local stays at 1.0f and the peer matches for
                        // free. At cutscene gallop speedXZ ~13-15 the
                        // local plays at ~4×; default 1.0 made the peer
                        // crawl.
                        peer->skin.skelAnime.playSpeed =
                            localHorse->skin.skelAnime.playSpeed;
                    }
                }

                // Rider position — vanilla-parity via horse->riderPos.
                //
                // EnHorse_Draw computes riderPos by reading the saddle
                // bone's world position via Skin_GetLimbPos(skin, 30, ...)
                // then subtracting horse.world.pos to leave a relative
                // offset (z_en_horse.c:3699-3702). Vanilla mounted
                // Player_Update then does:
                //   player->world.pos = horse->world.pos + horse->riderPos
                // which tracks the saddle bone EXACTLY, including the
                // animation cycle's saddle bob during gallop.
                //
                // The field has a lifecycle wrinkle: EnHorse_Init seeds
                // riderPos as an ABSOLUTE world position (line 756 +
                // 759: riderPos = actor.world.pos with +70 Y), and it
                // only becomes a usable offset after EnHorse_Draw has
                // run once. So on Frame 0 (spawn frame), the value is
                // garbage for our purposes. We detect "this looks like
                // an offset" via a magnitude check — true offsets have
                // small components (saddle ~30u back, ~50u up, ~30u to
                // either side), while absolute values include
                // horse.world.pos.z which is typically large (Hyrule
                // Field's title spawn is z=1867). When the heuristic
                // fails, we fall back to hardcoded approximations
                // (the same constants from the prior Path A approach).
                constexpr f32 kSaddleHeightAboveHorse        = 30.0f;
                constexpr f32 kSaddleBackwardFromHorseOrigin = 40.0f;

                bool useVanillaRiderPos = false;
                Vec3f riderOffset = { 0.0f, 0.0f, 0.0f };
                if (peerHorse != nullptr) {
                    EnHorse* horseAsHorse = (EnHorse*)peerHorse;
                    const f32 rx = horseAsHorse->riderPos.x;
                    const f32 ry = horseAsHorse->riderPos.y;
                    const f32 rz = horseAsHorse->riderPos.z;
                    if (fabsf(rx) < 80.0f &&
                        fabsf(ry) < 150.0f &&
                        fabsf(rz) < 80.0f) {
                        useVanillaRiderPos = true;
                        riderOffset.x = rx;
                        riderOffset.y = ry;
                        riderOffset.z = rz;
                    }
                }

                if (useVanillaRiderPos) {
                    // Vanilla path — precise saddle bone tracking,
                    // follows gallop animation cycle.
                    //
                    // Exact mirror of vanilla mounted Player_Action
                    // position write at z_player.c:13840-13842. The
                    // -27.0f Y offset is vanilla's stirrup-level
                    // adjustment: riderPos points to the saddle BONE's
                    // world position (top of the saddle), but Player's
                    // world.pos represents his FEET, which in vanilla
                    // are seated 27u below the saddle bone in the
                    // stirrups. Without this offset, the rider stands
                    // ~27u above the saddle (field test 2026-06-09,
                    // log 472 — user reported ~60u too high, of which
                    // 27u is this missing offset; the remainder is
                    // visual estimation imprecision).
                    actor->world.pos.x = peerHorse->world.pos.x + riderOffset.x;
                    actor->world.pos.y =
                        (peerHorse->world.pos.y + riderOffset.y) - 27.0f;
                    actor->world.pos.z = peerHorse->world.pos.z + riderOffset.z;
                } else if (peerHorse != nullptr) {
                    // Frame 0 fallback — riderPos hasn't been
                    // normalised yet. Hardcoded constants approximate
                    // the saddle position until EnHorse_Draw runs and
                    // converts riderPos to an offset.
                    const f32 yawRad =
                        (f32)slot.rotY / 32768.0f * (f32)M_PI;
                    const f32 forwardX = sinf(yawRad);
                    const f32 forwardZ = cosf(yawRad);
                    actor->world.pos.x = slot.pos.x -
                        kSaddleBackwardFromHorseOrigin * forwardX;
                    actor->world.pos.y = slot.pos.y + kSaddleHeightAboveHorse;
                    actor->world.pos.z = slot.pos.z -
                        kSaddleBackwardFromHorseOrigin * forwardZ;
                } else {
                    // Unmounted — peer stands on the formation slot
                    // ground (Phase 1 behaviour, no horse spawned).
                    actor->world.pos = slot.pos;
                }
                actor->world.rot.y = slot.rotY;
                actor->shape.rot.y = slot.rotY;
                actor->shape.shadowAlpha = 255;

                // Mirror the horse's pitch + roll onto the rider. Vanilla
                // EnHorse_Update writes horse->shape.rot.x/z based on the
                // floor poly normal (slope adaptation). The skelAnime
                // alias's joint table carries gallop's forward body lean
                // relative to the actor's shape.rot — without this mirror,
                // peer's lean is relative to a level pitch while the horse
                // tilts with the terrain, so peer's head intersects the
                // horse's neck on uphill slopes (field test 2026-06-09).
                // Copying horse pitch + roll to the rider keeps the gallop
                // lean relative to the horse's tilted body, matching
                // vanilla parent-child mount behaviour.
                if (peerHorse != nullptr) {
                    actor->shape.rot.x = peerHorse->shape.rot.x;
                    actor->shape.rot.z = peerHorse->shape.rot.z;
                } else {
                    actor->shape.rot.x = 0;
                    actor->shape.rot.z = 0;
                }

                // Mark mounted state so any rendering code path that
                // checks PLAYER_STATE1_ON_HORSE (animation overlays,
                // collision skip) sees the peer as mounted. Cleared
                // when no horse spawned (CVar off → foot peer).
                if (peerHorse != nullptr) {
                    player->stateFlags1 |= PLAYER_STATE1_ON_HORSE;
                } else {
                    player->stateFlags1 &= ~PLAYER_STATE1_ON_HORSE;
                }

                // Peer Link pose: alias when local Epona is in
                // gallop, freeze on uma_stand frame 0 otherwise.
                //
                //   - GALLOP mode (alias): local Link is actively
                //     riding/swaying with the gallop cutscene cue;
                //     peer mirrors local exactly. This was the
                //     pre-decouple state that worked well during
                //     the gallop segment (log 487 — peer Link
                //     looked correct mounted on a galloping Epona
                //     when its jointTable pointer aliased local's).
                //
                //   - NON-GALLOP mode (freeze): river idle, scene
                //     stops, rearing, etc. Lock peer to frame 0 of
                //     gPlayerAnim_link_uma_anim_stand (the "sitting
                //     idle on horse" pose). This avoids the freeze-
                //     on-wrong-anim distortion seen during the
                //     river idle in log 488 (peer froze on a foot-
                //     locomotion pose) and the alias-during-static
                //     issue (peer mirrors local's cue-driven static
                //     pose, which is also OK but the explicit
                //     idle-anim pose is the user-preferred fallback).
                //
                // Switch is keyed on the local cutscene horse's
                // animationIdx (captured in the horse-anim-mirror
                // block above). When no local horse is found
                // (localHorseAnimIdx == -1, CVar off → no
                // mounted-peer setup), fall back to freeze mode.
                const bool aliasMode =
                    (localHorseAnimIdx == ENHORSE_ANIM_GALLOP);

                const uint8_t bufferIdx = (formationIdx < kMaxTitlePeerBuffers)
                    ? formationIdx : (kMaxTitlePeerBuffers - 1);

                if (aliasMode) {
                    // Alias local Link's jointTable + morphTable
                    // pointers. Peer mirrors local's gallop pose.
                    // Reset peer's own skelAnime.animation so the
                    // next freeze-mode entry's pointer-mismatch
                    // check re-fires the LinkAnimation_Change.
                    player->skelAnime.jointTable = localLink->skelAnime.jointTable;
                    player->skelAnime.morphTable = localLink->skelAnime.morphTable;
                    player->skelAnime.animation  = nullptr;
                    Math_Vec3s_Copy(&player->skelAnime.prevTransl,
                                    &localLink->skelAnime.prevTransl);
                } else {
                    // Freeze mode: per-peer buffer, locked to
                    // uma_stand frame 0. Single LinkAnimation_Change
                    // when peer's anim ptr drifts off target; no
                    // LinkAnimation_Update calls.
                    player->skelAnime.jointTable = sTitlePeerJointTable[bufferIdx];
                    player->skelAnime.morphTable = sTitlePeerMorphTable[bufferIdx];

                    LinkAnimationHeader* targetAnim =
                        (LinkAnimationHeader*)&gPlayerAnim_link_uma_anim_stand;
                    if (targetAnim != (LinkAnimationHeader*)player->skelAnime.animation) {
                        const f32 targetEndFrame =
                            (f32)Animation_GetLastFrame((void*)targetAnim);
                        LinkAnimation_Change(
                            gPlayState, &player->skelAnime, targetAnim,
                            0.0f, 0.0f, targetEndFrame,
                            ANIMMODE_LOOP, 0.0f);
                    }
                }
                player->skelAnime.movementFlags = 0;
                // Intentionally NOT copying upperLimbRot from local Link.
                // At the title cutscene, vanilla code can apply yaw to
                // local Link's upper-body rotation (e.g., to face the
                // camera) — copying that to peer twisted the peer's torso
                // out of alignment with the horse it sits on. Field test
                // 2026-06-09: peer rider's body was rotated independently
                // from the horse's facing direction. With this copy
                // dropped, peer's upper body stays aligned with its
                // shape.rot.y (= slot.rotY = same as the horse).
                player->upperLimbRot.x = 0;
                player->upperLimbRot.y = 0;
                player->upperLimbRot.z = 0;
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

    // Horse-sync mounted-pose reconciliation. Body extracted to file-scope
    // helper ApplyMountedPoseReconciliation (declared near the top of this
    // file) so the title-screen peer path (DummyPlayer_Update's title-mode
    // branch above) can share the exact same math — single source of truth.
    // See Plans/title_screen_peer_actors.md §"Phase 2 horse integration".
    ApplyMountedPoseReconciliation(actor, client);
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
    // Pattern 4 / #277 — peer's melee swing state. Mirroring these into
    // the underlying Player struct of the DummyPlayer makes the existing
    // `(Player*)Anchor_GetNearestPlayerActor(...)->meleeWeaponState`
    // reads in vanilla enemy AI transparently return peer state when
    // peer is the nearest player. No per-enemy refactor needed for
    // field reads — same pattern as the unk_860 sync.
    // See Plans/peer_player_state_sync_2026-06-16.md +
    // Analysis/peer_player_state_deep_analysis_2026-06-16.md §8 Option A.
    player->meleeWeaponState     = client.meleeWeaponState;
    player->meleeWeaponAnimation = client.meleeWeaponAnimation;
    player->unk_845              = client.unk_845;

    // #304 — Neuter the DummyPlayer's sword AT collider. Vanilla Player_Init
    // (which ran when this actor was briefly ACTOR_PLAYER before re-parenting
    // to ACTOR_EN_OE2) permanently set AT_ON on meleeWeaponQuads[0/1] via
    // Collider_InitQuad. Then DummyPlayer_Draw calls vanilla Player_Draw,
    // which reaches func_800906D4 (z_player_lib.c:1573-1577) whenever
    // meleeWeaponState > 0 (synced above). That function calls
    // CollisionCheck_SetAT on the sword quads with the remote's sword
    // dmgFlags — so the remote's swing lands vanilla AT/AC hits on any
    // enemy AC in range on THIS client's replica. Peer's OnActorUpdate
    // forwarder then broadcasts DAMAGE_ENEMY back to host, and host's
    // DrainPendingSyncDamage stacks it (+=) on top of host's own local
    // vanilla hit → double damage.
    //
    // CollisionCheck_AT's filter at z_collision_check.c:2650 skips any
    // collider whose base.atFlags lacks AT_ON, so clearing this bit
    // silently disables the phantom hit path. The visible sword trail
    // (rendered via meleeWeaponInfo[0] with a NULL collider in
    // func_800906D4) is unaffected — that path doesn't register AT.
    // Pattern 4 / #277 enemy-AI reads of meleeWeaponState still work
    // because we only touch the quads' collision flag, not the state field.
    // PvP damage flow uses DummyPlayer's cylinder AT (line ~1687 gate on
    // pvpActive), not the sword — also unaffected.
    //
    // Applied every frame as defense-in-depth: even if some future vanilla
    // path re-sets AT_ON, our next tick clears it before Player_Draw
    // re-registers the quad.
    player->meleeWeaponQuads[0].base.atFlags &= ~AT_ON;
    player->meleeWeaponQuads[1].base.atFlags &= ~AT_ON;

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

        // #261 — mounted-state gate. ApplyMountedPoseReconciliation
        // (called earlier in DummyPlayer_Update via the mountedHorseNetId
        // branch) writes actor.world.pos to horse.world.pos + horse.
        // riderPos - (0,27,0), and sets PLAYER_STATE1_ON_HORSE. If we
        // also apply the animation Y-delta here, the saddle-bob
        // component of the owner's mounted-pose Player anim
        // (movementFlags & 2 is set on gPlayerAnim_link_ride_* family)
        // perturbs the reconciled position every frame → user-visible
        // Y oscillation in the saddle.
        //
        // Title-screen path solved the same problem by explicitly
        // clearing movementFlags = 0 at DummyPlayer.cpp:888 for
        // mounted title peers. Gameplay path uses the same principle
        // via state-flag check (cleaner — preserves movementFlags for
        // any future consumer that reads it, suppresses only the Y
        // write).
        //
        // XZ translation block above (movementFlags & 1) is intentionally
        // left ungated — XZ deltas in mounted poses are negligible and
        // ApplyMountedPoseReconciliation overwrites XZ anyway when it
        // fires on the next frame.
        if (!(player->stateFlags1 & PLAYER_STATE1_ON_HORSE)) {
            player->actor.world.pos.y += diff.y * player->actor.scale.y;
        }
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
    //
    // Title-mode exempt: title screen forces adult Link locally
    // (Opening_SetupTitleScreen), so a peer's gameplay-time linkAge from
    // their last save (could be child) would otherwise dim them out
    // here. The title-cutscene shots all show adult Link riding Epona,
    // so we want to render the peer as adult regardless of their save
    // file. linkAge is swapped to client.linkAge later in this function
    // anyway, so the Player_Draw call still renders the correct model
    // for the cosmetic-sync pack.
    if (!titleMode && client.linkAge != gSaveContext.linkAge) {
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

    // V1 — hide peer body during cutscenes when peer is also mid-cutscene.
    //
    // Vanilla cutscene scripts were authored for a single Link and position
    // GET_PLAYER(play) via csCtx-driven commands. When both local and peer
    // are running the same vanilla cutscene locally, both Link models end
    // up at the same scripted positions → visible stacking on top of each
    // other (log 705 followup discussion, Saria bridge cutscene).
    //
    // Rule (per user 2026-07-14): only hide the peer body when BOTH:
    //   (a) The LOCAL player is currently in a cutscene (Play_InCsMode
    //       captures both csCtx.state != IDLE and Player_InCsMode via
    //       linkAction / stateFlags), AND
    //   (b) THIS PEER is also in a cutscene (client.csCtxState != IDLE,
    //       broadcast via PLAYER_UPDATE at ~5 Hz).
    //
    // Cases NOT hidden (both correct per rule):
    //   - Local in gameplay, peer in cutscene: peer body renders at
    //     their cutscene-driven position. Local can watch them.
    //   - Local in cutscene, peer NOT in cutscene: peer body renders
    //     normally at their gameplay position. Local sees them
    //     standing around during the local cutscene.
    //   - Normal NPC dialogue (textbox): msgCtx-based, not csCtx-based.
    //     Play_InCsMode returns false — peer renders normally.
    //   - Item-get without cutscene (Player_InCsMode via GETTING_ITEM
    //     but peer in gameplay): peer renders normally.
    //
    // Peer's nametag is suppressed alongside the body via the
    // `Anchor_ShouldSuppressPeerNameTag` extern "C" gate consumed by
    // `nametag.cpp DrawNameTag`. The vote-skip HUD dots already signal
    // which peers are in the cutscene, so the floating name label would
    // be redundant clutter. The gate lives on the same conditions as
    // this body-hide early-return, so both hide together.
    //
    // Title-mode exempt (kept above with linkAge check): title cutscene
    // has its own hand-tuned formation (Plans/title_screen_peer_actors.md).
    //
    // Staleness caveat: client.csCtxState broadcast lags by up to
    // ~200 ms. Edge transitions (both entering / both exiting cutscene)
    // may show ~200 ms of peer visibility before the hide engages, or
    // ~200 ms of hide before peer reappears. Not a functional bug.
    if (!titleMode &&
        Play_InCsMode(gPlayState) &&
        client.csCtxState != CS_STATE_IDLE) {
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

    // Hack to account for usage of gSaveContext in Player_Draw.
    // Title-mode peers always render as adult Link — the title
    // cutscene shows adult Epona-mounted Link, and peer's broadcast
    // linkAge may be stale child from their last gameplay session.
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = titleMode ? LINK_AGE_ADULT : client.linkAge;
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

    // Title-mode root-motion snap, gated on freeze mode. Peer's
    // jointTable is either:
    //   - per-peer buffer (freeze mode): root carries the frozen
    //     uma_stand frame's root translation, which must be clamped
    //     to baseTransl to keep the rendered model on the saddle.
    //   - aliased to local Link's jointTable (alias / gallop mode):
    //     local Link's vanilla SkelAnime path already handles root.
    //     Snapping here would write into local Link's own buffer,
    //     disturbing local's draw too. Skip the snap.
    if (titleMode && player->skelAnime.jointTable != nullptr) {
        Player* localLinkForGate = GET_PLAYER(gPlayState);
        const bool aliasMode =
            (localLinkForGate != nullptr &&
             player->skelAnime.jointTable == localLinkForGate->skelAnime.jointTable);
        if (!aliasMode) {
            player->skelAnime.jointTable[0].x =
                player->skelAnime.baseTransl.x;
            player->skelAnime.jointTable[0].y =
                player->skelAnime.baseTransl.y;
            player->skelAnime.jointTable[0].z =
                player->skelAnime.baseTransl.z;
        }
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

// Companion gate to the V1 hide-peer-in-cutscene body-draw early-return
// in DummyPlayer_Draw. Answers "should nametag.cpp DrawNameTag skip this
// actor's tag right now?" for peer DummyPlayers whose body was suppressed
// because both local and peer are in co-active cutscenes.
//
// Called from nametag.cpp DrawNameTag. Returns 0 for any non-DummyPlayer
// actor (so real NPC / boss / follower / etc. nametags are unaffected).
// Title-mode DummyPlayers are also exempted (they don't broadcast the
// gameplay-side csCtxState anyway; title has its own formation).
//
// Rule matches the DummyPlayer_Draw gate exactly:
//   (a) Local player is currently in a cutscene (Play_InCsMode true),
//   (b) The peer this actor represents is also in a cutscene
//       (client.csCtxState != CS_STATE_IDLE, broadcast via PLAYER_UPDATE).
extern "C" int Anchor_ShouldSuppressPeerNameTag(Actor* actor) {
    if (actor == nullptr || gPlayState == nullptr) {
        return 0;
    }
    if (Anchor::Instance == nullptr) {
        return 0;
    }
    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);
    if (clientId == 0) {
        return 0;
    }
    if (Anchor::Instance->IsDummyPlayerTitleMode(actor)) {
        return 0;
    }
    auto it = Anchor::Instance->clients.find(clientId);
    if (it == Anchor::Instance->clients.end()) {
        return 0;
    }
    if (!Play_InCsMode(gPlayState)) {
        return 0;
    }
    if (it->second.csCtxState == CS_STATE_IDLE) {
        return 0;
    }
    return 1;
}
