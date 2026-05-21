/*
 * FollowerNPC.cpp — SoH NPC Follower companion lifecycle (Flotilla).
 *
 * Plan: Plans/npc_follower_plan.md.
 * Branch: feature/npc-follower off development-multiplayer @ af57a20e7.
 *
 * Phase 2 scope (this commit): CVar toggle drives spawn/despawn of the
 * local ACTOR_EN_FOLLOWER instance. No state machine yet (Phase 4), no
 * network sync yet (Phase 3) — purely local-client behaviour. Single
 * NPC per client; per-room caps deferred.
 *
 * Subsequent phases land in this same directory:
 *   Phase 3: Packets/FollowerNPCSpawn.cpp + State.cpp + Despawn.cpp.
 *   Phase 4: state machine handlers (IDLE / FOLLOW with Actor_MoveXZGravity).
 *   Phase 5: STUCK + recovery harness.
 *   Phase 6: CLIMBING scripted-climb driver.
 *   Phase 8: G-guards + debug-draw.
 */

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/NPCFollower/FollowerNPC.h"
#include "soh/Network/Anchor/Common/ActorTrail.h"     // Phase 5: substrate path consumption
#include "soh/Network/Anchor/Common/AILocomotion/NavOrDirect.h"  // Phase 3 (2026-05-18): shared substrate-path helper
#include "soh/Network/Anchor/Common/AILocomotion/ScriptedFollow.h"  // Phase 5 (2026-05-19): shared scripted-FOLLOW step
#include "soh/Network/Anchor/Common/AILocomotion/LocomotionAnim.h"  // Phase 6 (2026-05-19): shared climb-anim decision
#include "soh/Network/Anchor/Common/AILocomotion/AirborneRecovery.h"  // shared airborne-stuck detection
#include "soh/Network/Anchor/Common/AILocomotion/HeadLook.h"          // shared head-look math
#include "soh/Network/Anchor/Common/AILocomotion/StepPhase.h"         // shared step-phase + footstep SFX
#include "soh/Network/Anchor/Common/AILocomotion/StuckEscalation.h"   // shared STUCK escalation tiers
#include "soh/Network/Anchor/Common/AILocomotion/StuckRecovery.h"     // shared TickSTUCK dispatch
#include "soh/Network/Anchor/Common/AINavTest.h"      // Navigation Test Harness — combat-disable + reach
#include "soh/Network/Anchor/Common/DistanceMath.h"   // AnchorDist::DistXZSq
#include "soh/Network/Anchor/Common/AILocomotion/NavStateTransitions.h"  // 3D-aware arrive/pursue/progress predicates
#include "soh/Enhancements/RoomNavData/RoomNavData.h" // Phase 6: ClimbAnchor lookup
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <cmath>
#include <limits>

extern "C" {
#include "variables.h"        // gPlayState, gEnFollowerId, gSaveContext
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "objects/gameplay_keep/gameplay_keep.h"  // gPlayerAnim_link_normal_*
#include "src/overlays/actors/ovl_En_Follower/z_en_follower.h"  // EnFollower struct + state enum
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"        // ARROW_NORMAL etc. for RANGED_ATTACK
extern PlayState* gPlayState;
extern s16        gEnFollowerId;
}

// ----------------------------------------------------------------------------
// Stage 1+2 (npc_follower_health_and_respawn_plan): health constants
// and helpers. Stage 3+ will add ApplyDamage() that calls
// FollowerNpcInvulnerable() before applying.
// ----------------------------------------------------------------------------
// Floor for the per-spawn health cap. The leader-mirroring helper
// returns max(this, gSaveContext.healthCapacity / 16) so the NPC
// always has at least 3 hearts even before Link picks up his
// starting heart container.
constexpr s8 kFollowerNpcMinHealth = 3;
// Hard cap so we never overflow the s8 EnFollower::health field
// (max 127). 20 hearts is OoT's max anyway.
constexpr s8 kFollowerNpcMaxHealth = 20;

bool FollowerNpcInvulnerable() {
    return CVarGetInteger(CVAR_ENHANCEMENT("AI.FollowerNPC.Invulnerable"), 1) != 0;
}

// Stage 3 — public API: is the local NPC follower a valid target
// for enemy AI? True only when the NPC exists, is alive, and the
// invulnerable CVar is OFF. Same predicate also drives the per-tick
// collider register, so targeting + damage application stay in sync.
bool Anchor::IsFollowerNpcTargetable() const {
    if (FollowerNpcInvulnerable()) return false;
    if (mFollowerNpcLocalActor == nullptr) return false;
    if (mFollowerNpcLocalActor->update == nullptr) return false;
    EnFollower* asFollower = (EnFollower*)mFollowerNpcLocalActor;
    if (asFollower->state == EN_FOLLOWER_STATE_DEAD) return false;
    return true;
}

// Stage 2: NPC HP mirrors Link's heart capacity. healthCapacity is
// in OoT's quarter-heart units (16 = 1 heart) so divide by 16 to get
// the integer heart count. Clamped to [kFollowerNpcMinHealth,
// kFollowerNpcMaxHealth] so an NPC spawned mid-game with a high-cap
// Link still fits in s8 and a degenerate empty save still gives us
// something reasonable.
s8 FollowerNpcMaxHealthFromLink() {
    int hearts = (int)gSaveContext.healthCapacity / 16;
    if (hearts < kFollowerNpcMinHealth) hearts = kFollowerNpcMinHealth;
    if (hearts > kFollowerNpcMaxHealth) hearts = kFollowerNpcMaxHealth;
    return (s8)hearts;
}

// Stage 2 — death/respawn timings. All in milliseconds, converted to
// game ticks via Anchor::MsToGameTicks at the comparison site so the
// values stay correct at any framerate (20fps default vs unlocked).
constexpr int kFollowerNpcDeathHoldMs       = 3000;   // anim hold + ground lay
// User-spec is "10 seconds total from death to respawn". We achieve
// that by setting this cooldown so deathHold + cooldown ≈ 10s.
// Log 161 timing: death @ 02:20:58, respawn @ 02:21:12 = 13.9s with
// the prior 10s cooldown — felt long. 7s cooldown gives 10s total.
constexpr int kFollowerNpcRespawnCooldownMs = 7000;
constexpr int kFollowerNpcDrowningMs        = 30000;  // user-spec (Player default)
constexpr float kFollowerNpcVoidThresholdY  = -3000.0f;  // Y below this = void death

// Stage 2 — find the door actor closest to `nearPos` (typically the
// leader's position). Returns the door's world.pos + facing, or
// nullopt-style failure via outFound. Walks ACTORCAT_DOOR; ignores
// dead actors (update == nullptr). If no door is found the caller
// falls back to a sensible default (leader pos).
struct DoorRespawnResult {
    Vec3f pos;
    s16   yaw;
    bool  found;
};

DoorRespawnResult FindClosestDoorToLeader(PlayState* play, const Vec3f& nearPos) {
    DoorRespawnResult result{ nearPos, 0, false };
    if (play == nullptr) return result;
    Actor* it = play->actorCtx.actorLists[ACTORCAT_DOOR].head;
    float bestDistSq = std::numeric_limits<float>::max();
    while (it != nullptr) {
        if (it->update != nullptr) {
            const float dx = it->world.pos.x - nearPos.x;
            const float dy = it->world.pos.y - nearPos.y;
            const float dz = it->world.pos.z - nearPos.z;
            const float distSq = dx*dx + dy*dy + dz*dz;
            if (distSq < bestDistSq) {
                bestDistSq    = distSq;
                result.pos    = it->world.pos;
                result.yaw    = it->shape.rot.y;
                result.found  = true;
            }
        }
        it = it->next;
    }
    return result;
}

// ----------------------------------------------------------------------------
// File-scope Phase 5 nav state. Declared here (above SetFollowerNpcActive)
// so the spawn helper can reset it. Single instance — v1 has one NPC per
// client.
// ----------------------------------------------------------------------------
namespace {
// Animation identity tag — tracked so the per-tick handler only calls
// LinkAnimation_Change on actual transitions (calling it every frame
// restarts the animation and freezes the playhead). Stored on the
// EnFollower as `currentAnim` (s32) so peer replicas track their own
// animation independently.
enum class FollowerNpcAnim {
    kNone        = 0,  // initial state — first EnsureAnimation call always fires
    kWait        = 1,  // wait_free (idle) — actually a waitL/waitR blend each frame
    kWalk        = 2,  // walk_free
    kRun         = 3,  // run_free
    // Climb steps — one-shots, alternated by dispatcher driven by
    // NPC's vertical motion. Mirrors Player's climb at z_player.c:13391-
    // 13412 where each step is Player_AnimPlayOnce(upL/upR) toggled
    // via actionVar2 ^= 1. Holds last frame when stationary (no new
    // anim fired — equivalent to Player's PLAYER_STATE2_STATIONARY_LADDER).
    kClimbUp     = 4,  // [legacy alias for kClimbUpL — same value to keep wire compat]
    kClimbUpL    = 4,  // gPlayerAnim_link_normal_Fclimb_upL (one-shot)
    kStopL       = 5,  // walk_endL_free — one-shot stop anim, left foot forward
    kStopR       = 6,  // walk_endR_free — one-shot stop anim, right foot forward
    // Fidgets — one-shot variants of the standing idle. Cycled by the
    // dispatcher after kFidgetIntervalTicks in kWait. Source = Player's
    // sFidgetAnimations at z_player.c:1014-1056.
    kFidgetLookA = 7,  // gPlayerAnim_link_normal_wait_typeA_20f (look around)
    kFidgetWarmB = 8,  // gPlayerAnim_link_normal_wait_typeB_20f (warm — wipe brow)
    kFidgetStretchD = 9,  // gPlayerAnim_link_wait_typeD_20f (stretch arms)
    kSwim     = 10,  // gPlayerAnim_link_swimer_swim (moving)
    kSwimWait = 11,  // gPlayerAnim_link_swimer_swim_wait (treading water)
    // Ledge-hoist one-shot anims. AnimForState returns one of these
    // during EN_FOLLOWER_STATE_LEDGE_HOIST based on the
    // EnFollower::hoistContext field. Both are ANIMMODE_ONCE; the
    // existing stopAnimPlaying handshake holds the anim until
    // LinkAnimation_Update reports completion.
    kHoistGround = 12,  // gPlayerAnim_link_normal_climb_up (mantle from floor)
    kHoistSwim   = 13,  // gPlayerAnim_link_swimer_swim_15step_up (climb out of water)
    // Auto-jump-off-ledge anims. Player picks between these at
    // z_player.c:5663 based on speed (run-jump when fast + facing
    // forward, regular jump otherwise). One-shot; falls back to
    // walk/run/wait when complete via the stopAnimPlaying handshake.
    kRunJump  = 14,  // gPlayerAnim_link_normal_run_jump
    kJump     = 15,  // gPlayerAnim_link_normal_jump
    kClimbUpR = 16,  // gPlayerAnim_link_normal_Fclimb_upR (alternates with kClimbUpL)
    // Lateral climb anims — for sideways motion on a climbable surface
    // (e.g. shimmying along a vine wall). Player uses these via
    // ageProperties->unk_BC table at z_player.c:13415; alternates L/R
    // similar to vertical climb. One-shot per step.
    kClimbSideL = 17,  // gPlayerAnim_link_normal_Fclimb_sideL
    kClimbSideR = 18,  // gPlayerAnim_link_normal_Fclimb_sideR
    // Stage 2 — death poses. Generic (back-down) for fall/void/combat;
    // drowning-specific (swim KO) when the death cause is drowning.
    // Both one-shot; hold at last frame after completion.
    kDeath      = 19,  // gPlayerAnim_link_normal_back_downA  (generic)
    kDeathDrown = 20,  // gPlayerAnim_link_swimer_swim_dead   (drowning)
    // Stage 4 — basic vertical sword swing. One-shot; AT collider
    // active during the apex frames. NPC inherits Player's
    // modelAnimType so the grip matches whatever Link is holding.
    kSwordSwing = 21,  // gPlayerAnim_link_fighter_normal_kiru
    // Stage 4 — shield-up defensive stance. kBlockWait is the held
    // pose (loop); kBlockHit is the one-shot reaction when an attack
    // is successfully blocked (~10 frames). The dispatcher swaps
    // between them based on a frame counter.
    kBlockWait  = 22,  // gPlayerAnim_link_normal_defense_wait  (loop)
    kBlockHit   = 23,  // gPlayerAnim_link_normal_defense_hit   (one-shot)
    // Stage 4 — bow shoot one-shot. Full draw + release sequence
    // in a single anim. EN_ARROW projectile spawns at the release
    // frame (~5/15) inside TickRANGED_ATTACK.
    kBowShoot   = 24,  // gPlayerAnim_link_bow_bow_shoot
    // Stage 5 — child-Link crawlspace anims. Both one-shots:
    // - kCrawlMove plays once on entry (the get-down-and-crouch
    //   motion); after completion the SkelAnime holds at the end
    //   frame (low crouch pose) while TickCRAWLING translates the
    //   body forward. This mirrors Player exactly — Player calls
    //   Player_AnimPlayOnce at z_player.c:7695 then translates via
    //   linearVelocity each frame without a continuous crawl loop.
    //   OoT does not have a true crawl-stride loop animation.
    // - kCrawlExit plays once on the way out.
    // (Earlier this enum had kCrawlMove as a loop. Looping the
    // entry anim cycled back to frame 0 = upright stand pose,
    // producing a visible stand→crouch→stand→crouch flicker the
    // user perceived as walking.)
    kCrawlMove  = 25,  // gPlayerAnim_link_child_tunnel_start (one-shot, holds end frame)
    kCrawlExit  = 26,  // gPlayerAnim_link_child_tunnel_end   (one-shot)
};

struct LocalNpcNavState {
    // Phase 3 (2026-05-18) — substrate path consumption refactored to
    // use the shared AnchorAI::NavState. Previously had inline path /
    // lastPathRefreshFrame / lastPathTargetPos fields; AnchorAI::ChooseSubgoal
    // operates on the wrapped NavState directly so the consumer (TickFOLLOW
    // + TickENGAGE) calls the helper instead of inlining ComputePathTo.
    // Mirrors NPC Invader Phase 2 commit 79767e605.
    AnchorAI::NavState navState;

    Vec3f    stuckCheckPos        = { 0.0f, 0.0f, 0.0f };
    uint64_t lastStuckCheckFrame  = 0;
    // (currentAnim moved to EnFollower::currentAnim — per-actor tracking
    // so peer replicas don't share state with the local owner.)
    // Phase 6 — anchor cached during a CLIMBING run so handler doesn't
    // re-resolve every frame. Cleared on CLIMBING exit.
    const ::AnchorNavRoom::ClimbAnchor* activeClimbAnchor = nullptr;
    // Phase 8 — G10 leash. Counts consecutive ticks the NPC has been
    // beyond kNpcLeashDistance from leader. Reset when within range.
    uint32_t leashFrames = 0;
    // Phase 8 — G14 close-fail. Counts consecutive ticks the NPC is
    // in the close-fail distance band without progress.
    // closeFailBaseline = distance-to-leader at window entry; reset
    // whenever progress > kNpcCloseFailProgressDelta is observed.
    uint32_t closeFailFrames   = 0;
    float    closeFailBaseline = 0.0f;

    // STUCK escalation state (Common/AILocomotion/StuckEscalation).
    // Counts consecutive FOLLOW→STUCK transitions within the decay
    // window; TickSTUCK reads via GetStuckAction to choose between
    // nudge / cursor advance / teleport.
    AnchorAI::StuckCycleState stuckCycle;

    // Auto-jump-off-ledge diagnostics. Captured at jump trigger,
    // emitted as periodic per-frame logs while airborne, summary
    // log at landing. `airborneState.jumpInProgress` flips false on
    // landing. The persistent jump-fire / peak-tracking / pos-resnap
    // fields live in AnchorAI::AirborneState (shared with NPC Invader
    // via Common/AILocomotion/AirborneRecovery); NPC-specific
    // diagnostic captures (yaw, speed, velocityY, log throttle,
    // floor-edge tracking) stay actor-side.
    AnchorAI::AirborneState airborneState;
    s16      jumpStartYaw           = 0;
    float    jumpStartSpeedXZ       = 0.0f;
    float    jumpStartVelocityY     = 0.0f;
    uint64_t jumpLastDiagFrame      = 0;
    bool     jumpWasOnFloorPrevTick = true;

    // Climb step alternation. Mirrors Player's actionVar2 toggle at
    // z_player.c:13412. Each climb step is a one-shot of upL/upR;
    // we fire the next step when (1) the current one-shot is done
    // AND (2) NPC is making vertical progress. When stationary on
    // wall, no new step fires — anim holds at last frame
    // (Player's PLAYER_STATE2_STATIONARY_LADDER equivalent).
    bool     climbNextIsRight = false;  // toggles each step
    float    climbPrevY       = 0.0f;   // for vertical motion detection
    Vec3f    climbPrevXZ      = { 0.0f, 0.0f, 0.0f };  // for lateral motion (XZ delta on wall)

    // Ledge-hoist position interpolation. Captured at LEDGE_HOIST
    // entry; pos.y is lerp'd from startPos.y → hoistTargetPos.y over
    // the anim duration so the body visibly moves up during the
    // mantle motion (instead of staying at lower pos and snapping
    // at end).
    Vec3f    hoistStartPos   = { 0.0f, 0.0f, 0.0f };

    // Stage 3 — non-fatal fall hurt reaction. Counts down each frame
    // after a hard (non-lethal) landing so the dispatcher's anim
    // resolution holds the back-down anim briefly. Decremented in
    // the dispatcher; reset to 0 (or a fresh value) on each hard
    // landing.
    uint32_t fallHurtFramesRemaining = 0;

    // Edge-detect for leader's climbing state — used in the CLIMBING
    // fast-path to handle the "leader hoisted over the rim while NPC
    // was still mid-climb" case. Without this, the fast-path stops
    // firing the moment leader exits CLIMBING, NPC's path is empty
    // (fast-path bypasses path mgmt), the mantle-out check requires
    // NPC within 60u of top (frequently outside the band when NPC
    // was tracking ~30u below leader), and gravity drops NPC all the
    // way back to the wall base. We detect the true→false edge of
    // leaderClimbing while NPC is still in CLIMBING and inject a
    // LEDGE_HOIST to the active anchor's topPos.
    bool     leaderWasClimbingPrevTick = false;

    // Throttled FOLLOW progress diagnostic. One snapshot every
    // kFollowProgressLogMs while NPC is in FOLLOW — pos/target/path
    // state/distToSubgoal/distToTarget. Tells us what NPC is doing
    // during otherwise-silent windows (e.g. stuck-on-platform symptoms).
    uint64_t lastFollowProgressLogFrame = 0;
};
}  // namespace
static LocalNpcNavState sLocalNav;

// ----------------------------------------------------------------------------
// Color-bug fix — draw-context flag. Mirrors the pause-menu's
// Anchor_PauseLinkDrawBegin/End / Anchor_IsDrawingPauseLink pattern
// (see soh/Network/Anchor/Common/PauseLinkBuffer.{h,cpp}).
//
// Why needed: Player_DrawImpl receives the local player's Player* as
// its `data` arg (so equipment-draw resolves correctly). The
// VB_APPLY_TUNIC_COLOR hook receives that same `data` and matches
// against `myPlayer == actor` — which TRUE-matches the local-player
// branch and applies the local color to the NPC. But more
// importantly: when the NPC is drawn AFTER a remote DummyPlayer
// draw, the GPU env color from the previous draw leaks onto the
// NPC unless the hook applies an override.
//
// The flag lets the hook know "this draw is an NPC; resolve color
// from the NPC's owner, not from `actor` matching".
// ----------------------------------------------------------------------------
static Actor* sCurrentlyDrawingNpc = nullptr;

// Stage 4 Phase B — equipment-visibility swap. While the NPC is being
// drawn, Player's `leftHandType / rightHandType / sheathType / *DLists`
// fields are temporarily replaced with values matching the NPC's own
// combat intent (sword+shield in combat states; bow in RANGED_ATTACK;
// empty hands otherwise). Restored at draw end so the local Player's
// next draw uses the user's actual equipment.
//
// Mechanism: we call Player_SetModels(localPlayer, intendedGroup) to
// overwrite the fields (it knows how to pick the right DLists from
// sPlayerDListGroups internally). On End, we call Player_SetModels
// again with the saved group to restore.
//
// File-scope save slot — single instance because actor draws don't
// nest (each actor's full draw sequence completes before the next
// actor begins). If nesting ever becomes possible, switch to a stack.
static bool      sEquipmentSwapActive       = false;
static s32       sSavedPlayerModelGroup     = 0;
static u8        sSavedPlayerLeftHandType   = 0;
static u8        sSavedPlayerRightHandType  = 0;
static u8        sSavedPlayerSheathType     = 0;
static Gfx**     sSavedPlayerLeftHandDLists  = nullptr;
static Gfx**     sSavedPlayerRightHandDLists = nullptr;
static Gfx**     sSavedPlayerSheathDLists    = nullptr;
static Gfx**     sSavedPlayerWaistDLists     = nullptr;
static s8        sSavedPlayerHeldItemAction  = 0;
static bool      sSavedHeldItemActionActive  = false;

// Last combat type used. 0 = melee (sword+shield); 1 = ranged (bow).
// Set by combat state ENTRY in TryEngageCombat. Used by the
// time-based equipment-retention logic below.
static s32 sLastCombatWeapon = 0;

// Time-based equipment retention. Tracks when the NPC last exited a
// combat state. While the post-combat sheathe delay is active,
// equipment stays visible regardless of current state — mirrors
// Player's vanilla sheathe behavior where Link keeps his sword/bow
// drawn for several seconds after combat actions before
// auto-sheathing.
//
// Without time-based retention, the NPC visibly flickered weapon
// → empty hands → weapon → empty hands every shot (log 162 —
// STANDBY's tight 80u leader-leash dropped NPC to FOLLOW immediately
// after each RANGED_ATTACK exit, FOLLOW's modelGroup is DEFAULT
// (empty hands), then next combat tick re-drew the weapon).
//
// 4000ms picked empirically — long enough to bridge most STANDBY ↔
// FOLLOW ↔ combat cycles without visible flicker, short enough to
// feel responsive (NPC sheathes within a few seconds of true combat
// end). Player's vanilla sheathe is similar order of magnitude.
static constexpr int kSheatheDelayMs = 4000;
static uint64_t sLastCombatExitFrame = 0;

// Helper — model group for the most-recent combat weapon. Used both
// by STANDBY's direct mapping AND the time-based sheathe-delay
// retention for non-combat states.
static s32 ModelGroupForLastWeapon() {
    return (sLastCombatWeapon == 1) ? PLAYER_MODELGROUP_BOW_SLINGSHOT
                                    : PLAYER_MODELGROUP_SWORD_AND_SHIELD;
}

// Map NPC state → intended Player model group.
//
// Combat states return their direct weapon. STANDBY mirrors the
// last-combat weapon. Non-combat states (IDLE / FOLLOW / etc.) return
// DEFAULT (empty hands) UNLESS the post-combat sheathe-delay is still
// active — in which case the last-combat weapon stays visible. This
// time-based retention mirrors Player's vanilla sheathe behavior
// (Link keeps weapon drawn for several seconds after last combat
// action before auto-sheathing) and prevents the
// weapon→empty→weapon→empty visible flicker the user reported in
// log 162.
static s32 NpcStateToModelGroup(s32 state) {
    switch (state) {
        case EN_FOLLOWER_STATE_CRAWLING:
            // Crawlspaces force weapons sheathed regardless of
            // sheathe-delay. Player vanilla also clears all combat
            // state on crawlspace entry. Returning DEFAULT here
            // bypasses the time-based retention in the default
            // branch — NPC visually puts everything away to crawl.
            return PLAYER_MODELGROUP_DEFAULT;
        case EN_FOLLOWER_STATE_RANGED_ATTACK:
            return PLAYER_MODELGROUP_BOW_SLINGSHOT;
        case EN_FOLLOWER_STATE_ATTACK:
        case EN_FOLLOWER_STATE_BLOCK:
        case EN_FOLLOWER_STATE_ENGAGE:
            return PLAYER_MODELGROUP_SWORD_AND_SHIELD;
        case EN_FOLLOWER_STATE_STANDBY:
            return ModelGroupForLastWeapon();
        default:
            // IDLE / FOLLOW / SWIMMING / LEDGE_HOIST / CLIMBING /
            // STUCK / DEAD — sheathe-delay window?
            if (Anchor::Instance != nullptr && sLastCombatExitFrame > 0) {
                const uint64_t curFrame =
                    Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed);
                const uint64_t sheatheTicks =
                    (uint64_t)Anchor::Instance->MsToGameTicks(kSheatheDelayMs);
                if (curFrame < sLastCombatExitFrame + sheatheTicks) {
                    // Still within sheathe-delay — keep weapon visible.
                    return ModelGroupForLastWeapon();
                }
            }
            // Sheathed — default empty-hands model.
            return PLAYER_MODELGROUP_DEFAULT;
    }
}

extern "C" void Anchor_FollowerNpcDrawBegin(Actor* npc) {
    sCurrentlyDrawingNpc = npc;

    // Phase B equipment swap. Save Player's current model state +
    // apply NPC's intended model. End() restores.
    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    EnFollower* asFollower = (EnFollower*)npc;
    const s32 intendedGroup = NpcStateToModelGroup(asFollower->state);

    // No-op if we're already at the intended group (saves a redundant
    // SetModels call when NPC is in IDLE and Player has nothing
    // special equipped — the most common case).
    if (intendedGroup == localPlayer->modelGroup) {
        sEquipmentSwapActive = false;
        return;
    }

    // Save state. We save BOTH the model group (for the canonical
    // restore via Player_SetModels) AND the raw DList/type fields
    // (defensive — in case something between save and restore
    // mutates them, the raw restore brings them back exactly).
    sSavedPlayerModelGroup       = localPlayer->modelGroup;
    sSavedPlayerLeftHandType     = localPlayer->leftHandType;
    sSavedPlayerRightHandType    = localPlayer->rightHandType;
    sSavedPlayerSheathType       = localPlayer->sheathType;
    sSavedPlayerLeftHandDLists   = localPlayer->leftHandDLists;
    sSavedPlayerRightHandDLists  = localPlayer->rightHandDLists;
    sSavedPlayerSheathDLists     = localPlayer->sheathDLists;
    sSavedPlayerWaistDLists      = localPlayer->waistDLists;

    // Bow vs slingshot inventory check. Player_SetModels picks the
    // bow vs slingshot DList variant via Player_HoldsSlingshot(this),
    // which checks Player's heldItemAction (NOT inventory ownership).
    // When NPC is the shooter, Player isn't actively holding either —
    // so the default selection is bow. That makes child Link's NPC
    // visually wield a bow even when only the slingshot is owned
    // (log 163 user complaint). Fix: if intended group is
    // BOW_SLINGSHOT and Player has slingshot but not bow,
    // temporarily set heldItemAction to PLAYER_IA_SLINGSHOT so the
    // SetModels DList pick resolves correctly. Restore at End.
    sSavedHeldItemActionActive = false;
    if (intendedGroup == PLAYER_MODELGROUP_BOW_SLINGSHOT) {
        const u8 bowSlot   = INV_CONTENT(ITEM_BOW);
        const u8 slingSlot = INV_CONTENT(ITEM_SLINGSHOT);
        const bool hasBow       = (bowSlot   != ITEM_NONE);
        const bool hasSlingshot = (slingSlot != ITEM_NONE);
        s8 desired = -1;
        if (hasSlingshot && !hasBow) {
            desired = PLAYER_IA_SLINGSHOT;
        } else if (hasBow && !hasSlingshot) {
            desired = PLAYER_IA_BOW;
        }  // both / neither → no override (let Player's natural state pick)
        if (desired >= 0 && localPlayer->heldItemAction != desired) {
            sSavedPlayerHeldItemAction = localPlayer->heldItemAction;
            sSavedHeldItemActionActive = true;
            localPlayer->heldItemAction = desired;
        }
    }

    // Apply NPC's intended model. Player_SetModels writes
    // leftHandType/DLists, rightHandType/DLists, sheathType/DLists,
    // waistDLists from sPlayerDListGroups[type][linkAge]. Does NOT
    // write modelAnimType (we control that separately via
    // currentAnimType).
    Player_SetModels(localPlayer, intendedGroup);
    localPlayer->modelGroup = intendedGroup;  // SetModels doesn't touch this; SetModelGroup does
    sEquipmentSwapActive = true;
}

extern "C" void Anchor_FollowerNpcDrawEnd(void) {
    sCurrentlyDrawingNpc = nullptr;

    if (!sEquipmentSwapActive) return;
    sEquipmentSwapActive = false;

    if (gPlayState == nullptr) return;
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == nullptr) return;

    // Restore. Both the canonical SetModels call (so any internal
    // bookkeeping is consistent) AND the raw fields (defensive,
    // since the EquipmentAlwaysVisible CVar branches inside
    // Player_SetModels may pick slightly different DLists than the
    // user originally had).
    Player_SetModels(localPlayer, sSavedPlayerModelGroup);
    localPlayer->modelGroup      = sSavedPlayerModelGroup;
    localPlayer->leftHandType    = sSavedPlayerLeftHandType;
    localPlayer->rightHandType   = sSavedPlayerRightHandType;
    localPlayer->sheathType      = sSavedPlayerSheathType;
    localPlayer->leftHandDLists  = sSavedPlayerLeftHandDLists;
    localPlayer->rightHandDLists = sSavedPlayerRightHandDLists;
    localPlayer->sheathDLists    = sSavedPlayerSheathDLists;
    localPlayer->waistDLists     = sSavedPlayerWaistDLists;
    if (sSavedHeldItemActionActive) {
        localPlayer->heldItemAction = sSavedPlayerHeldItemAction;
        sSavedHeldItemActionActive  = false;
    }
}

extern "C" Actor* Anchor_GetCurrentlyDrawingFollowerNpc(void) {
    return sCurrentlyDrawingNpc;
}

// Defensive scene-transition reset. Called from the OnSceneInit hook in
// HookHandlers.cpp. Clears the equipment-swap active flag without
// running the End-path restore — the saved Player DList pointers
// (sSavedPlayer{LeftHand,RightHand,Sheath,Waist}DLists) reference the
// old scene's allocated resources, which may have been freed during
// the transition. Restoring them would write dangling pointers into
// Link's draw state and crash on the first Player_Draw of the new
// scene. Player_Init re-binds these naturally for the new scene; we
// just need to prevent the next DrawBegin from short-circuiting on a
// stale-active flag, and prevent the matching End-call (if any) from
// writing stale data on top of the new scene's freshly-initialized
// Player state.
extern "C" void Anchor_FollowerNpcDrawStateResetOnSceneTransition(void) {
    sEquipmentSwapActive       = false;
    sSavedHeldItemActionActive = false;
    sCurrentlyDrawingNpc       = nullptr;
}

// ----------------------------------------------------------------------------
// Owner lookup — given an NPC Actor*, return the ownerClientId.
// Local NPC → ownClientId; peer replica → ownerClientId from
// mPeerFollowerNpcs; unknown → 0.
// ----------------------------------------------------------------------------
uint32_t Anchor::FindFollowerNpcOwner(Actor* npc) const {
    if (npc == nullptr) return 0;
    if (npc == mFollowerNpcLocalActor) {
        return ownClientId;
    }
    for (const auto& [cid, replica] : mPeerFollowerNpcs) {
        if (replica == npc) return cid;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Scene-transition pointer cleanup. OoT clears the actor list on
// scene reload; our cached Actor* pointers become dangling. Reading
// dangling-pointer->update is undefined behaviour (the memory may
// have been freed, reused by another actor with a non-null update,
// or contain stale bytes). This explicit clear — called from the
// OnSceneSpawnActors hook in NpcCompanionInit.cpp — is the safe
// alternative.
//
// After the clear, TickFollowerNpcCVar's auto-respawn branch fires
// on the next tick (CVar is on AND mFollowerNpcLocalActor is null
// → SetFollowerNpcActive(true) re-spawns at the new scene's player
// position). For peer replicas: their owners' next FOLLOWER_NPC_SPAWN
// (auto-broadcast by the owner's auto-respawn) will repopulate
// mPeerFollowerNpcs.
// ----------------------------------------------------------------------------
void Anchor::ClearFollowerNpcSceneCache() {
    mFollowerNpcLocalActor = nullptr;
    mPeerFollowerNpcs.clear();
    // Reset the CVar transition baseline too — without this, the
    // next TickFollowerNpcCVar sees CVar=1, last=1, no edge, no
    // auto-respawn (the auto-respawn branch handles this case via
    // the `cur != 0 && mFollowerNpcLocalActor == nullptr` check
    // BEFORE the edge check, so this reset isn't strictly needed
    // — but it's clearer to start the new scene with a clean slate).
    // Don't touch mFollowerNpcCVarLast here; the auto-respawn branch
    // is the one that handles "CVar on but pointer null" correctly.
}

// ----------------------------------------------------------------------------
// CVar transition polling — called once per OnGameFrameUpdate tick.
// ----------------------------------------------------------------------------
void Anchor::TickFollowerNpcCVar() {
    // Stale-pointer cleanup is now handled by the OnSceneSpawnActors
    // hook in NpcCompanionInit.cpp, which calls
    // ClearFollowerNpcSceneCache(). Reading update on a dangling
    // pointer is UB and was unreliable in field test (the dangling
    // read returned non-null often enough that auto-respawn never
    // fired). The explicit clear is the safe alternative.

    const int cur = CVarGetInteger(CVAR_ENHANCEMENT("AI.FollowerNPC.Enabled"), 0);

    // Auto-respawn after scene transition: if the CVar is on but the
    // pointer is null (cleared by OnSceneSpawnActors), spawn now.
    // This is NOT a CVar edge — handle it before the edge check.
    if (cur != 0 && mFollowerNpcLocalActor == nullptr) {
        // Stage 2 — death respawn cooldown. If a death just occurred,
        // suppress the spawn until the cooldown elapses; then resolve
        // the respawn position (door closest to leader, or leader
        // itself as fallback) and arm the SetFollowerNpcActive
        // override so the spawn lands at the chosen door.
        const uint64_t curFrame =
            gameFrameCounter.load(std::memory_order_relaxed);
        if (mFollowerNpcRespawnAtFrame != 0) {
            if (curFrame < mFollowerNpcRespawnAtFrame) {
                // Cooldown still active. Skip respawn; CVar edge
                // bookkeeping still proceeds below.
                if (cur != mFollowerNpcCVarLast) {
                    mFollowerNpcCVarLast = cur;
                }
                return;
            }
            // Cooldown elapsed — pick a respawn pos (door near leader)
            // and arm the spawn override. Cleared by the spawn helper.
            if (gPlayState != nullptr) {
                Player* leader = GET_PLAYER(gPlayState);
                if (leader != nullptr) {
                    DoorRespawnResult dr =
                        FindClosestDoorToLeader(gPlayState,
                                                 leader->actor.world.pos);
                    if (dr.found) {
                        mFollowerNpcSpawnPosOverride    = true;
                        mFollowerNpcSpawnPosOverridePos = dr.pos;
                        mFollowerNpcSpawnPosOverrideYaw = dr.yaw;
                        SPDLOG_INFO("[FollowerNPC] respawn at door "
                                    "({:.0f},{:.0f},{:.0f}) yaw=0x{:X} "
                                    "(distFromLeader squared visible in scan)",
                                    dr.pos.x, dr.pos.y, dr.pos.z,
                                    (uint16_t)dr.yaw);
                    } else {
                        SPDLOG_INFO("[FollowerNPC] respawn — no door found, "
                                    "falling back to leader pos");
                    }
                }
            }
            mFollowerNpcRespawnAtFrame = 0;  // consumed
        }
        SetFollowerNpcActive(true);
        // Fall through to edge-check update; mFollowerNpcCVarLast
        // tracking is independent of the auto-respawn.
    }

    if (cur == mFollowerNpcCVarLast) {
        // No edge — nothing more to do.
        return;
    }
    // Edge detected. The spawn case above already handled it; the
    // despawn case (1→0) still needs to fire here.
    if (cur == 0) {
        // Manual disable cancels any pending death cooldown — the
        // user explicitly turned the feature off.
        mFollowerNpcRespawnAtFrame  = 0;
        mFollowerNpcSpawnPosOverride = false;
        SetFollowerNpcActive(false);
    }
    mFollowerNpcCVarLast = cur;
}

// ----------------------------------------------------------------------------
// Spawn / despawn the local NPC. Idempotent.
// ----------------------------------------------------------------------------
void Anchor::SetFollowerNpcActive(bool active) {
    if (active) {
        // Idempotent: if we already have a live NPC, don't double-spawn.
        if (mFollowerNpcLocalActor != nullptr &&
            mFollowerNpcLocalActor->update != nullptr) {
            return;
        }

        // Need a live play state + player to spawn at.
        if (gPlayState == nullptr) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but gPlayState is null");
            return;
        }
        Player* player = GET_PLAYER(gPlayState);
        if (player == nullptr) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but no local Player");
            return;
        }
        // Need the dynamic actor id (assigned by ActorDB::AddBuiltInCustomActors).
        if (gEnFollowerId == 0) {
            SPDLOG_WARN("[FollowerNPC] SetFollowerNpcActive(true) but gEnFollowerId is 0 "
                        "(actor not registered? — check ActorDB)");
            return;
        }

        // Spawn at the local player's current pos + facing. Y exactly
        // matches Link's so the NPC stands on the same floor; rotation
        // matches so the NPC initially faces the same way (the AI in
        // Phase 4 will reorient toward leader/path).
        //
        // Stage 2 override: when the death-respawn cooldown elapsed
        // and TickFollowerNpcCVar resolved a door near the leader,
        // it set mFollowerNpcSpawnPosOverride. Consume it here so
        // the NPC respawns at the door instead of on top of leader.
        Vec3f p   = player->actor.world.pos;
        s16   yaw = player->actor.shape.rot.y;
        if (mFollowerNpcSpawnPosOverride) {
            p   = mFollowerNpcSpawnPosOverridePos;
            yaw = mFollowerNpcSpawnPosOverrideYaw;
            mFollowerNpcSpawnPosOverride = false;  // single-shot
        }

        Actor* spawned = Actor_Spawn(
            &gPlayState->actorCtx, gPlayState,
            gEnFollowerId,
            p.x, p.y, p.z,
            0 /* rotX */, yaw, 0 /* rotZ */,
            0 /* params */);

        if (spawned == nullptr) {
            SPDLOG_ERROR("[FollowerNPC] Actor_Spawn failed for gEnFollowerId={} at "
                         "({:.0f},{:.0f},{:.0f})", (int)gEnFollowerId, p.x, p.y, p.z);
            return;
        }

        mFollowerNpcLocalActor = spawned;
        // Reset Phase 5 nav state so a fresh path computes on the
        // first FOLLOW tick. stuckCheckPos seeded to spawn pos so the
        // first stuck check (3s in) compares to where we started.
        sLocalNav.navState.path.Reset();
        sLocalNav.navState.lastPathRefreshFrame = 0;
        sLocalNav.navState.lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
        sLocalNav.stuckCheckPos        = p;
        sLocalNav.lastStuckCheckFrame  = gameFrameCounter.load(std::memory_order_relaxed);
        sLocalNav.activeClimbAnchor    = nullptr;
        // Reset jump-tracking state — log 161 showed a respawn at
        // 02:30:56 followed immediately at 02:30:57 by an "airborne
        // for 4432 frames" warning. The previous NPC instance had
        // jumpInProgress=true at the time of disappearance; the new
        // NPC inherited that file-static state and its first tick
        // tripped the airborne-stuck safety net. Reset all fields
        // here so each respawn starts fresh.
        sLocalNav.airborneState = {};  // value-reset (jumpInProgress = false,
                                       // frame = 0, start/peak/prev all zero)
        // Seed start/peak to current pos so first airborne tick after
        // respawn doesn't compute huge deltas from origin.
        sLocalNav.airborneState.jumpStartPos = p;
        sLocalNav.airborneState.jumpPeakPos  = p;
        sLocalNav.jumpLastDiagFrame          = 0;
        sLocalNav.jumpWasOnFloorPrevTick     = true;
        // (Combat-state cleanup intentionally NOT done here.
        // sAttackState and sCombatCooldownEndFrame are defined later
        // in the file; forward-referencing them from this early
        // function is a compile error. They're not load-bearing for
        // the disappearance bug — combat states validate
        // sAttackState.target->update on entry and exit harmlessly
        // when stale. The killer leftover is sLocalNav.airborneState.jumpInProgress
        // which IS reset above. sLastCombatWeapon (used by Phase B
        // STANDBY equipment) is also defined later — same deferral
        // applies; default 0 is fine for first STANDBY entry on a
        // fresh NPC because it'll be set by the next combat
        // engagement before STANDBY's draw fires.)
        // (currentAnim init lives on EnFollower itself — set in
        // EnFollower_Init. The first EnsureAnimation tick will swap
        // from the kWait set up by LinkAnimation_PlayLoop in init to
        // whatever AnimForState chooses based on first-tick state.)
        SPDLOG_INFO("[FollowerNPC] Spawned ACTOR_EN_FOLLOWER(id={}) at "
                    "({:.0f},{:.0f},{:.0f}) yaw={} (CVar 0→1)",
                    (int)gEnFollowerId, p.x, p.y, p.z, (int)yaw);

        // Phase 3: broadcast SPAWN so peers spawn read-only replicas.
        // netId scheme: ownClientId (one NPC per client in v1, so the
        // client id IS the natural unique key).
        Vec3s rotVec3s{ 0, yaw, 0 };
        SendPacket_FollowerNpcSpawn((uint32_t)ownClientId, p, rotVec3s,
                                    (int16_t)gPlayState->sceneNum,
                                    (int8_t)gPlayState->roomCtx.curRoom.num,
                                    (uint8_t)(gSaveContext.linkAge & 0x1));
    } else {
        // Despawn: Actor_Kill the tracked instance if it's still alive.
        if (mFollowerNpcLocalActor == nullptr) {
            return;  // already despawned / never spawned
        }
        if (mFollowerNpcLocalActor->update != nullptr) {
            SPDLOG_INFO("[FollowerNPC] Despawning ACTOR_EN_FOLLOWER (CVar 1→0)");
            Actor_Kill(mFollowerNpcLocalActor);
        }
        // Phase 3: broadcast DESPAWN so peers tear down replicas.
        // reason=0 → cvar_off (the primary v1 trigger). Future phases
        // may pass 1 (died) / 2 (scene_change) / 3 (owner_disconnect).
        SendPacket_FollowerNpcDespawn((uint32_t)ownClientId, /*reason=*/0);
        // Clear our tracking pointer either way — even if update was
        // already NULL (the actor was destroyed by something else, e.g.
        // a scene transition wiping the actor list).
        mFollowerNpcLocalActor = nullptr;
    }
}

// ----------------------------------------------------------------------------
// Phase 4 — state machine + locomotion.
// ----------------------------------------------------------------------------
//
// Distance thresholds for IDLE / FOLLOW transitions. Hysteresis prevents
// flap when the leader stands at the boundary.
//   - dist >= kEnterFollow → IDLE → FOLLOW
//   - dist <= kEnterIdle   → FOLLOW → IDLE
// Same shape as the AI Player Follower's kFollowThreshold pattern
// but with explicit hysteresis since the NPC's locomotion is
// direct-vector, not stick-injection (smoother but no built-in dead-zone).
static constexpr float kEnterFollow = 80.0f;
static constexpr float kEnterIdle   = 50.0f;
// Y-axis hysteresis pair (P0 audit / log 263 fix). Without these gates,
// when the leader is directly above on a ledge (e.g., y=800 above NPC's
// y=360), small XZ distance makes the actor declare itself "arrived"
// and stop trying to climb. Mirrors AI Player Follower's
// kFollowYThreshold pattern (Follower.cpp:223) added 2026-05-12 for
// the same bug class.
//   - Arrival (FOLLOW→IDLE): require |dy| ≤ kEnterIdleY in addition
//     to XZ.
//   - Re-engage (IDLE→FOLLOW): trigger on XZ exceeds OR |dy| exceeds
//     kEnterFollowY (hysteresis upper bound).
static constexpr float kEnterIdleY   = 40.0f;
static constexpr float kEnterFollowY = 60.0f;
// Fix C: grouped form — preferred at predicate call sites; the
// individual float constants stay accessible for log strings + speed
// scaling. Float and band forms are interchangeable via predicate
// overloads (NavStateTransitions.h).
static constexpr AnchorAI::ThresholdPair kEnterIdleBand   = { kEnterIdle,   kEnterIdleY };
static constexpr AnchorAI::ThresholdPair kEnterFollowBand = { kEnterFollow, kEnterFollowY };

// Walk and run speeds in OoT units/frame. Matches Link's vanilla walk
// (~6.0) and run (~12.0). The NPC walks when close to leader and runs
// when leader is far — gives a natural feel without making the NPC
// always sprint.
static constexpr float kRunDistance = 250.0f;  // beyond this, run instead of walk
static constexpr float kWalkSpeed   = 5.04f;
// Speed history:
//   v1: 12.0    (50% faster than Link — visibly outran Link in tests)
//   v2: 8.0     (matches Link R_RUN_SPEED_LIMIT — still felt fast in
//                harness; scripted-position locomotion lacks Link's
//                anim-blending which makes vanilla movement read
//                smoother at the same nominal speed)
//   v3: 6.4     (20% reduction from v2 per user report 2026-05-19)
//   v4: 5.12    (further 20% reduction per user report 2026-05-19 PM —
//                NPC + Invader still visibly outpaced Link in field test)
//   v5: 5.376   (+5% per user report 2026-05-20 — NPC + Invader now
//                slightly slower than AI Player Follower; bump back up)
static constexpr float kRunSpeed    = 5.376f;

// Phase 5 — substrate path consumption + STUCK recovery.
//
// Path refresh policy: re-query ComputePathTo every kPathRefreshMs OR
// when the captured target has moved beyond kPathRetargetDist (leader
// walked far enough that the path is stale). 500ms = 2Hz refresh —
// fast enough to track a moving leader without spamming the planner.
static constexpr int   kPathRefreshMs       = 500;
static constexpr float kPathRetargetDist    = 60.0f;
static constexpr float kAdvanceSubgoalDist  = 30.0f;  // advance cursor when within this XZ
static constexpr int   kStuckCheckMs        = 3000;   // matches player-Follower's tuned 3s
static constexpr int   kFollowProgressLogMs = 5000;   // throttled FOLLOW diagnostic period
static constexpr float kStuckMinProgress    = 20.0f;
static constexpr float kStuckNudgeDist      = 30.0f;  // direct world.pos nudge in STUCK
// STUCK escalation (ported from AI Player Follower's G12 — Follower.cpp:1680).
// Counts consecutive FOLLOW→STUCK transitions within a sliding window:
//   Cycle 1: legacy nudge (toward leader)
//   Cycle 2: edge-triggered navPath cursor advance (skip stuck subgoal)
//   Cycle 3+: teleport to next subgoal OR leader as fallback
// Cycle counter decays after kStuckCycleWindowMs of no new STUCK.
static constexpr int kStuckCycleWindowMs  = 3000;
static constexpr int kStuckCycleEscalation = 3;

// Phase 6 — scripted-climb constants. Tuned to match Link's vanilla
// climb feel; field-test in Inside Deku Tree may refine.
static constexpr float kClimbSpeedY         = 2.0f;   // u/frame upward; halved 2026-05-19 PM per user report (was 4.0 — too fast)
static constexpr float kClimbSubgoalReach3D = 24.0f;  // advance cursor when within 3D
static constexpr float kClimbXzSnap         = 1.0f;   // smooth XZ snap rate to subgoal (per frame fraction)
//
// Body offset from wall surface. Climb cells sit ON the climbable
// polygon; the NPC's world.pos is at its body center, so snapping
// directly to a cell embeds the body half-into the wall. Offset
// along anchor.planeNormal (which points OUT from wall) by this
// amount so the body sits in front of the wall, matching how OoT
// renders Link on ladders/vines.
static constexpr float kClimbBodyOffset     = 12.0f;

// Phase 8 — G-guard recovery teleports. Mirror the
// Follower's G10 / G14 semantics but adapted for direct world.pos
// writes (no stick injection).
//
// G10 leash: NPC distance to leader > kNpcLeashDistance for >
// kNpcLeashTimeoutMs → teleport NPC to leader's pos. Catches "NPC
// stuck behind a closed door / left in another scene / fell into
// untracked geometry."
static constexpr float kNpcLeashDistance      = 1200.0f;  // 3D units
static constexpr int   kNpcLeashTimeoutMs     = 2000;     // 2s, framerate-aware
//
// G14 close-fail: NPC in the 200-1200u band (close enough that G10
// won't fire) but making < kNpcCloseFailProgressDelta progress
// across kNpcCloseFailTimeoutMs → teleport NPC to current substrate
// subgoal. Catches "NPC stuck in tight geometry between rooms /
// path-around-obstacle outside the substrate's understanding."
static constexpr float kNpcCloseFailMinDistance   = 200.0f;
static constexpr float kNpcCloseFailMaxDistance   = 1200.0f;
static constexpr int   kNpcCloseFailTimeoutMs     = 10000;  // 10s
static constexpr float kNpcCloseFailProgressDelta = 30.0f;

namespace {

// True iff `npc` is THIS client's local NPC (not a peer replica). Peer
// replicas have their pos driven by FOLLOWER_NPC_STATE packets and
// should not run the AI tick.
bool IsLocalOwnerNPC(Actor* npc) {
    return Anchor::Instance != nullptr &&
           npc == Anchor::Instance->GetFollowerNpcLocalActor();
}

// (LocalNpcNavState + sLocalNav defined at file scope above so the
// spawn helper can reset them.)

// Compute XZ distance squared between two world positions.
inline float Dist2DSq(const Vec3f& a, const Vec3f& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Yaw toward target XZ (s16 binary angle).
inline s16 YawTowardTarget(const Vec3f& from, const Vec3f& to) {
    return Math_Atan2S(to.z - from.z, to.x - from.x);
}

// Forward-decl — defined further down (with TickCLIMBING since it's
// the primary consumer). ComputeEffectiveTarget needs it for the
// leader-climbing redirect. Pitfall 14: single-pass C++ name lookup
// requires the declaration to appear before its first use.
const ::AnchorNavRoom::ClimbAnchor* FindClosestClimbAnchor(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& pos);

// Compute the "effective target" the NPC should pursue. Normally
// this is the leader's world pos, but when the leader is climbing,
// it's the climb anchor's basePos (the floor entry below the climb)
// so the NPC routes toward the wall instead of toward leader's
// unreachable mid-wall pos. Once NPC is near basePos, the leader-
// climbing force-engage check in the dispatcher transitions to
// CLIMBING with a manually-populated path.
//
// Earlier iteration used topPos (assuming pathfinder would route up
// through climb cells), but cross-room nav isn't supported and the
// pathfinder returned empty path → G14 teleport. Targeting basePos
// keeps NPC in the same room as the wall and trips the force-engage.
Vec3f ComputeEffectiveTarget(const Vec3f& leaderPos) {
    if (!Anchor::Instance->IsLocalPlayerClimbing()) return leaderPos;
    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num);
    const ::AnchorNavRoom::ClimbAnchor* leaderAnchor =
        FindClosestClimbAnchor(navData, leaderPos);
    return leaderAnchor ? leaderAnchor->basePos : leaderPos;
}

// IDLE handler — stand still, transition to FOLLOW on distance
// exceed. Body yaw is intentionally NOT auto-rotated to face leader
// — that produced an unnatural "always staring at you" look. The NPC
// preserves whatever facing direction it had when it stopped moving
// (e.g., when it transitioned FOLLOW→IDLE on arriving at the leader).
// This mirrors AI Player Follower, where Link's body keeps
// its last facing while idle. The independent head-look-at-leader
// (TickHeadLookAtLeader, dispatched separately) still tracks the
// player so the NPC visually acknowledges us with eye/head movement
// without rotating the whole body.
void TickIDLE(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Transition check: hysteresis upper bound. Measure against
    // effectiveTarget so IDLE→FOLLOW fires when leader starts
    // climbing — without this the NPC near a wall base sees small
    // XZ distance to climbing-leader's XZ and stays IDLE forever.
    //
    // P0 audit (log 263): re-engage when target moves away in
    // EITHER XZ or Y. Without the Y gate, the NPC stayed in IDLE
    // when the leader was directly above on a ledge (small XZ but
    // 440u Y delta). Mirrors AI Player Follower's xzExceeds ||
    // yExceeds pattern (Follower.cpp:4931 — fixed for the same
    // bug class on log 32). Phase 2 extracted to ShouldPursue3D.
    const Vec3f effectiveTarget = ComputeEffectiveTarget(leaderPos);
    if (AnchorAI::ShouldPursue3D(a->world.pos, effectiveTarget,
                                 kEnterFollowBand)) {
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
    }
}

// FOLLOW handler — walk/run toward the current substrate subgoal,
// transition to IDLE on proximity to leader. Phase 5: substrate path
// consumption replaces direct-vector pursuit so the NPC routes
// around obstacles instead of pressing into walls.
void TickFOLLOW(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // ---- Airborne momentum lock ------------------------------------
    // While in a jump (sLocalNav.airborneState.jumpInProgress), preserve speedXZ at
    // the value captured when the jump fired. Player's airborne
    // handler at z_player.c:7165 uses Math_AsymStepToF with very low
    // rates (0.05/0.1) — effectively constant linearVelocity through
    // the air. Without this, our TickFOLLOW recomputes speedXZ each
    // tick from leader-distance, killing horizontal momentum mid-jump
    // (log 147 jump 1 showed speedXZ decay 4.5 → 0 in ~20 frames).
    // Skip the rest of FOLLOW so speedXZ + yaw stay locked.
    if (sLocalNav.airborneState.jumpInProgress) {
        a->speedXZ = sLocalNav.jumpStartSpeedXZ;
        a->shape.rot.y = sLocalNav.jumpStartYaw;
        a->world.rot.y = sLocalNav.jumpStartYaw;
        return;
    }

    // ---- Effective target -------------------------------------------
    // When leader is climbing, redirect path target to the climb
    // anchor's topPos (a floor node above the climb). Pathfinder
    // routes through climb cells to reach it, and the climb-cell
    // detection below engages FOLLOW→CLIMBING naturally. Without
    // this, the pathfinder's FindNearestNode skips climb nodes and
    // the path targets some random floor node — NPC reaches it and
    // idles instead of climbing up.
    const Vec3f effectiveTarget = ComputeEffectiveTarget(leaderPos);

    // ---- Trail key resolution ---------------------------------------
    // NPC Follower's "leader" is the local Player (owner client). Set
    // each tick so a future per-owner-clientId refactor doesn't break
    // anything (cheap; just an int assignment).
    sLocalNav.navState.trailKey = AnchorNav::TrailKeyForPlayer(
        (uint8_t)Anchor::Instance->ownClientId);

    // ---- Substrate-driven subgoal selection (via shared helper) ----
    // Phase 5 (2026-05-19): consolidated through RunScriptedFollowStep
    // so NPC Follower and NPC Invader share one entry point for the
    // FOLLOW path-decision + climb-cell-flag detection. Each caller
    // does its own state transitions + locomotion drive based on the
    // returned ScriptedFollowResult.
    AnchorAI::FallbackPolicy policy;
    policy.isFriendlyActor = true;
    policy.hasRangedReady  = false;  // ranged engagement is a combat-AI
                                     // concern; locomotion fallback
                                     // doesn't fire RANGED_ATTACK here.
    const AnchorAI::ScriptedFollowResult step =
        AnchorAI::RunScriptedFollowStep(a, effectiveTarget,
                                         sLocalNav.navState, policy, play);
    const AnchorAI::NavOrDirectResult& nav = step.nav;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    // ---- Phase 6: climb-subgoal transition --------------------------
    // The shared helper sets step.shouldEngageClimb when the current
    // path subgoal carries NODE_CLIMB_ANY. Transition to CLIMBING so
    // its handler can snap XZ + drive Y on the climb anchor's grid.
    if (step.shouldEngageClimb) {
        this_->state = EN_FOLLOWER_STATE_CLIMBING;
        sLocalNav.activeClimbAnchor = nullptr;  // resolved fresh on entry
        // Seed climbPrev* with current pos so the first CLIMBING tick's
        // motion-axis decision (LocomotionAnim) compares against the
        // correct baseline. Without seeding, |dy| and |dxz| compute
        // against zeroed init values (or stale post-prior-climb values)
        // and the first tick mis-picks SideL/R vs UpL/R based on
        // world-coordinate magnitude. Matches NPC Invader entry pattern.
        sLocalNav.climbPrevY  = a->world.pos.y;
        sLocalNav.climbPrevXZ = a->world.pos;
        SPDLOG_INFO("[FollowerNPC] FOLLOW→CLIMBING (path entered climb cell at "
                    "({:.0f},{:.0f},{:.0f}); flags=0x{:X})",
                    nav.subgoal.x, nav.subgoal.y, nav.subgoal.z,
                    nav.subgoalFlags);
        return;
    }

    // ---- Drive locomotion -------------------------------------------
    // When usingNavMesh = true, subgoal is the current path waypoint.
    // When false (DirectYaw fallback, target inside 60u or water-gated,
    // or path empty), subgoal is the target position itself.
    const Vec3f& subgoal = nav.subgoal;
    const s16 yaw = YawTowardTarget(a->world.pos, subgoal);
    a->shape.rot.y = yaw;
    a->world.rot.y = yaw;

    // Speed selection — match leader's pace, with a small catch-up
    // bonus that only kicks in when significantly farther than the
    // IDLE→FOLLOW threshold. Prevents the "sprint up → stop → sprint
    // up" oscillation pattern caused by always-sprint catch-up: NPC
    // would close to within kEnterIdle (50u), drop to IDLE, leader
    // walks, NPC re-enters FOLLOW, sprint, repeat.
    //
    // Bands:
    //   - dist > kRunDistance (250u): always sprint at kRunSpeed.
    //     Big gap — catch up fast.
    //   - dist 100-250u: match leader + (dist - 100) * 0.1 catch-up
    //     bonus, capped at kRunSpeed. Slowly closes the gap.
    //   - dist < 100u (down to kEnterIdle): match leader's speed
    //     EXACTLY. Distance is stable; NPC trails at fixed offset.
    //     No oscillation.
    //
    // When leader is stopped (speedXZ=0) and NPC reaches the close
    // band, NPC effectively halts at the current distance. The
    // IDLE re-entry check (distToTargetSq <= kEnterIdle²) handles
    // the IDLE switch when NPC drifts in further.
    const float distToLeaderSq = Dist2DSq(a->world.pos, leaderPos);
    Player*     leader         = GET_PLAYER(play);
    const float leaderSpeed    = (leader != nullptr) ? leader->actor.speedXZ : 0.0f;
    float       speed;
    if (distToLeaderSq > kRunDistance * kRunDistance) {
        speed = kRunSpeed;
    } else {
        const float dist = std::sqrt(distToLeaderSq);
        const float catchupBonus = std::max(0.0f, (dist - 100.0f) * 0.1f);
        speed = std::min(leaderSpeed + catchupBonus, kRunSpeed);
        // No min-speed floor: NPC matches leader's pace exactly when
        // close, including pace-of-zero when leader is stopped. Prior
        // 0.5 floor produced visible sliding (NPC drifting toward
        // leader while idle anim played, since FOLLOW state never
        // transitioned cleanly to IDLE in the hover band).
    }
    a->speedXZ = speed;

    // ---- Stuck check ------------------------------------------------
    const int stuckCheckTicks = Anchor::Instance->MsToGameTicks(kStuckCheckMs);
    if (stuckCheckTicks > 0 &&
        curFrame >= sLocalNav.lastStuckCheckFrame + (uint64_t)stuckCheckTicks) {
        // P0 audit: 3D progress, not XZ. Climbing actors make progress
        // mostly in Y; XZ-only measurement registered them as "stuck"
        // mid-climb (false-positive stuck escalation). Phase 2
        // extracted to RawDisplacement3D.
        const float progress =
            AnchorAI::RawDisplacement3D(sLocalNav.stuckCheckPos, a->world.pos);
        if (progress < kStuckMinProgress) {
            // No real progress in 3s. Enter STUCK; TickSTUCK reads the
            // cycle counter to escalate. Path is NOT reset here (so the
            // cycle-2 advance has a path to operate on); TickSTUCK's
            // cycle-1 branch resets the path after nudging.
            this_->state = EN_FOLLOWER_STATE_STUCK;
            AnchorAI::NoteStuckEntered(sLocalNav.stuckCycle,
                Anchor::Instance->MsToGameTicks(kStuckCycleWindowMs));
            SPDLOG_INFO("[FollowerNPC] FOLLOW→STUCK (no progress {:.1f}u in 3s @ "
                        "({:.0f},{:.0f},{:.0f}); cycle={})",
                        progress, a->world.pos.x, a->world.pos.y, a->world.pos.z,
                        sLocalNav.stuckCycle.count);
        }
        sLocalNav.stuckCheckPos       = a->world.pos;
        sLocalNav.lastStuckCheckFrame = curFrame;
    }

    // ---- Transition: arrived at leader -----------------------------
    // Navigation Test Harness reach reporter — fires when NPC reaches
    // leader within the harness's 3D 60u criterion. One-shot per run.
    if (AINavTest::IsRunActive() &&
        AINavTest::ReachedTarget(a->world.pos, leaderPos)) {
        AINavTest::ReportNpcFollowerReach();
    }

    // Measure against effectiveTarget (which redirects to anchor topPos
    // while leader is climbing). Without this, NPC at the wall base
    // sees small XZ distance to climbing-leader's XZ and enters IDLE
    // before ever engaging CLIMBING.
    //
    // P0 audit (log 263): require BOTH XZ close AND |dy| close. Prior
    // XZ-only check fired false-positive arrival when the leader was
    // directly above on a ledge (XZ=57u, Y=440u). NPC declared
    // itself "arrived" while standing on a lower floor below the
    // leader, then oscillated FOLLOW↔IDLE without ever engaging
    // the next climb segment. Phase 2 extracted to IsArrived3D.
    if (AnchorAI::IsArrived3D(a->world.pos, effectiveTarget,
                              kEnterIdleBand)) {
        this_->state = EN_FOLLOWER_STATE_IDLE;
        a->speedXZ   = 0.0f;
        sLocalNav.navState.path.Reset();  // discard path; IDLE is local-frame
        return;
    }

    // Throttled FOLLOW progress snapshot (log 252 diagnostic). Captures
    // what NPC is doing during otherwise-silent stretches — particularly
    // the "stuck on flat ground at base of next vine wall" symptom
    // where the substrate path isn't engaging CLIMBING and there are
    // no state-transition logs to trace from.
    //
    // Fires every kFollowProgressLogMs (5s). Reports the active path
    // state (size + cursor + flags) so we can see whether NPC is
    // following a path or fell to direct-yaw fallback.
    const int progressLogTicks =
        Anchor::Instance->MsToGameTicks(kFollowProgressLogMs);
    if (progressLogTicks > 0 &&
        curFrame >= sLocalNav.lastFollowProgressLogFrame + (uint64_t)progressLogTicks) {
        const float distToSubgoal = std::sqrt(
            Dist2DSq(a->world.pos, nav.subgoal));
        const float distToLeader = std::sqrt(distToLeaderSq);
        SPDLOG_INFO("[FollowerNPC.follow] pos=({:.0f},{:.0f},{:.0f}) "
                    "target=({:.0f},{:.0f},{:.0f}) path.size={} path.idx={} "
                    "subgoal=({:.0f},{:.0f},{:.0f}) flags=0x{:X} "
                    "distToSubgoal={:.0f}u distToTarget={:.0f}u speedXZ={:.1f} "
                    "usingNavMesh={} fallback={}",
                    a->world.pos.x, a->world.pos.y, a->world.pos.z,
                    effectiveTarget.x, effectiveTarget.y, effectiveTarget.z,
                    (int)sLocalNav.navState.path.waypoints.size(),
                    (int)sLocalNav.navState.path.cursorIdx,
                    nav.subgoal.x, nav.subgoal.y, nav.subgoal.z,
                    nav.subgoalFlags,
                    distToSubgoal, distToLeader, a->speedXZ,
                    nav.usingNavMesh ? "yes" : "no",
                    (int)nav.fallbackEngaged);
        sLocalNav.lastFollowProgressLogFrame = curFrame;
    }
}

// Animation switching helper — only call LinkAnimation_Change on
// real transitions. Calling every frame restarts the playhead.
// `currentAnim` is per-actor (on EnFollower) so peer replicas track
// their own state independently.
//
// Anim variants: use the _free variants (PLAYER_ANIMTYPE_UNARMED
// equivalent — arms down, no shield). The non-_free anims are the
// fighter / sword-and-shield poses, which would look wrong on an
// NPC that has no combat in v1. Player's lookup table at
// z_player.c:580-605 uses _free for unarmed.
//
// playSpeed defaults to 1.0; the dispatcher overrides skelAnime.playSpeed
// per-frame for walk/run anims so motion-cadence stays in sync as
// speedXZ varies within a single anim (z_player.c:8445 pattern).
// Pick anim header for a given anim kind + Player modelAnimType.
// Mirrors Player's D_80853914 2D table (z_player.c:578) but only for
// the kinds our NPC plays. modelAnimType maps:
//   0 = unarmed / free (no shield, no sword in hand)
//   1 = fighter (sword + shield drawn, ready stance)
//   2 = fighter alt — same locomotion anims as 1 in Player's table
//   3 = long sword (two-handed Biggoron Sword)
//   4/5 = "free" variants — same as 0 in Player's table
// CLIMBING anims and fidgets don't have modelAnimType variants in
// Player's table (they're shared across stances).
LinkAnimationHeader* AnimHeaderFor(FollowerNpcAnim kind, s8 modelAnimType) {
    const bool isLong    = (modelAnimType == 3);
    const bool isFighter = (modelAnimType == 1 || modelAnimType == 2);
    switch (kind) {
        case FollowerNpcAnim::kWait:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_wait_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_free;
        case FollowerNpcAnim::kWalk:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_free;
        case FollowerNpcAnim::kRun:
            if (isLong)            return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_run_long;
            if (modelAnimType == 1) return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_run;
            if (modelAnimType == 2) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_free;
        case FollowerNpcAnim::kStopL:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_endL_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endL_free;
        case FollowerNpcAnim::kStopR:
            if (isLong)    return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_walk_endR_long;
            if (isFighter) return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR;
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_walk_endR_free;
        // Shared across stances:
        case FollowerNpcAnim::kClimbUp:  // alias for kClimbUpL
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_upL;
        case FollowerNpcAnim::kClimbUpR:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_upR;
        case FollowerNpcAnim::kClimbSideL:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_sideL;
        case FollowerNpcAnim::kClimbSideR:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_Fclimb_sideR;
        case FollowerNpcAnim::kFidgetLookA:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeA_20f;
        case FollowerNpcAnim::kFidgetWarmB:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_wait_typeB_20f;
        case FollowerNpcAnim::kFidgetStretchD:
            return (LinkAnimationHeader*)&gPlayerAnim_link_wait_typeD_20f;
        // Swimming — Player uses the same swim anims regardless of
        // modelAnimType (sword stays sheathed in water). Anim cycle is
        // looping for both.
        case FollowerNpcAnim::kSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim;
        case FollowerNpcAnim::kSwimWait:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_wait;
        case FollowerNpcAnim::kHoistGround:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_climb_up;
        case FollowerNpcAnim::kHoistSwim:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_15step_up;
        case FollowerNpcAnim::kRunJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_run_jump;
        case FollowerNpcAnim::kJump:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_jump;
        case FollowerNpcAnim::kDeath:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_back_downA;
        case FollowerNpcAnim::kDeathDrown:
            return (LinkAnimationHeader*)&gPlayerAnim_link_swimer_swim_dead;
        case FollowerNpcAnim::kSwordSwing:
            return (LinkAnimationHeader*)&gPlayerAnim_link_fighter_normal_kiru;
        case FollowerNpcAnim::kBlockWait:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_defense_wait;
        case FollowerNpcAnim::kBlockHit:
            return (LinkAnimationHeader*)&gPlayerAnim_link_normal_defense_hit;
        case FollowerNpcAnim::kBowShoot:
            return (LinkAnimationHeader*)&gPlayerAnim_link_bow_bow_shoot;
        case FollowerNpcAnim::kCrawlMove:
            return (LinkAnimationHeader*)&gPlayerAnim_link_child_tunnel_start;
        case FollowerNpcAnim::kCrawlExit:
            return (LinkAnimationHeader*)&gPlayerAnim_link_child_tunnel_end;
        case FollowerNpcAnim::kNone:
        default:
            return nullptr;
    }
}

void EnsureAnimation(EnFollower* this_, PlayState* play, FollowerNpcAnim want) {
    // Pick the right anim header for the (kind, modelAnimType) pair.
    // currentAnimType is updated by the dispatcher BEFORE this call to
    // reflect the local Player's current armed-stance state.
    LinkAnimationHeader* anim = AnimHeaderFor(want, this_->currentAnimType);
    if (anim == nullptr) return;
    // No-op only if BOTH the kind and the resolved anim header match.
    // This way a modelAnimType change (e.g. unarmed → fighter when
    // player draws sword+shield) correctly re-fires the anim with
    // the armed variant, while same-kind+same-header transitions
    // (e.g. type 4 ↔ type 0 — both pick _free) are no-ops.
    if ((FollowerNpcAnim)this_->currentAnim == want &&
        this_->skelAnime.animation == anim) {
        return;
    }
    const bool oneShot =
        want == FollowerNpcAnim::kStopL ||
        want == FollowerNpcAnim::kStopR ||
        want == FollowerNpcAnim::kFidgetLookA ||
        want == FollowerNpcAnim::kFidgetWarmB ||
        want == FollowerNpcAnim::kFidgetStretchD ||
        want == FollowerNpcAnim::kHoistGround ||
        want == FollowerNpcAnim::kHoistSwim ||
        want == FollowerNpcAnim::kRunJump ||
        want == FollowerNpcAnim::kJump ||
        want == FollowerNpcAnim::kClimbUpL ||
        want == FollowerNpcAnim::kClimbUpR ||
        want == FollowerNpcAnim::kClimbSideL ||
        want == FollowerNpcAnim::kClimbSideR ||
        want == FollowerNpcAnim::kDeath ||
        want == FollowerNpcAnim::kDeathDrown ||
        want == FollowerNpcAnim::kSwordSwing ||
        want == FollowerNpcAnim::kBlockHit ||
        want == FollowerNpcAnim::kBowShoot ||
        want == FollowerNpcAnim::kCrawlMove ||  // matches Player (Player_AnimPlayOnce at z_player.c:7695)
        want == FollowerNpcAnim::kCrawlExit;
    LinkAnimation_Change(play, &this_->skelAnime, anim,
                          1.0f /* playSpeed — caller overrides per-frame */,
                          0.0f /* startFrame */,
                          Animation_GetLastFrame((void*)anim),
                          oneShot ? ANIMMODE_ONCE : ANIMMODE_LOOP,
                          -6.0f /* morphFrames */);
    this_->currentAnim     = (s32)want;
    this_->stopAnimPlaying = oneShot ? 1 : 0;
}

// ── Anim polish helpers (audit 2026-05-16, batch 1 / #4 / #5) ────────

// Step phase cycle length — Player uses 29.0f (z_player.c:8109). The
// walk/run anims are tuned to this cycle. Foot-down frames at ~10 and
// ~24 within the 29-unit cycle (Player calls these with
// func_8084021C(stepPhase, advance, 29.0, 10.0/24.0)).
static constexpr float kStepPhaseCycle    = 29.0f;
static constexpr float kStepPhaseFootDownL = 10.0f;
static constexpr float kStepPhaseFootDownR = 24.0f;
// Step phase threshold for stop-anim L vs R selection (Player at
// z_player.c:6397 splits at sp30 < 14). sp30 = unk_868 - 3, so the
// raw threshold is (3 + 14) = 17 in step-phase units; we ignore the
// -3 phase shift and use 14 directly (close enough for visual L/R).
static constexpr float kStopPhaseLRSplit  = 14.0f;

// Step-phase + footstep SFX. Thin wrapper over the shared
// AnchorAI::TickStepPhase helper that supplies NPC's stepPhase
// counter + Link-walk-cycle constants.
void TickStepPhaseAndSfx(EnFollower* this_, PlayState* play) {
    AnchorAI::TickStepPhase(this_->stepPhase, &this_->actor,
                            this_->skelAnime.playSpeed,
                            kStepPhaseCycle,
                            kStepPhaseFootDownL,
                            kStepPhaseFootDownR);
}

// Pick the right animation for the current state. Used by both the
// local-owner path (post-dispatch) and the peer-replica path (state
// arrives via FOLLOWER_NPC_STATE).
//
// FOLLOW chooses walk vs run based on speedXZ. For peers, speedXZ
// comes from the synced field (`syncedSpeedXZ`) populated by the
// state-packet handler; for local owners, it's the value the AI
// just wrote.
// ── Head-look-at-leader ──────────────────────────────────────────────
// Step NPC's `headLimbRot` + `upperLimbRot` toward the relative yaw of
// leader. Pattern mirrors Player's z_player.c:3735-3750 in shape but
// uses simpler step-toward semantics: head turns within a comfortable
// yaw range; if leader is behind, upper body twists too.
//
// Constraints:
//   - Head yaw max ±0x4000 (90°). Beyond that, upper-body twist takes over.
//   - Pitch: small angle (head tilts up/down slightly if leader is above/
//     below). Capped at ±0x2000 (45°).
//   - Step rate: ~0x600 per tick (slower than instant — feels alive).
//
// Drives the NPC's pose values written into Player's headLimbRot /
// upperLimbRot via the EnFollower_Draw save/swap/restore.
void TickHeadLookAtLeader(EnFollower* this_, const Vec3f& leaderPos) {
    AnchorAI::HeadLookInputs in;
    in.actorPos  = this_->actor.world.pos;
    in.actorYaw  = this_->actor.shape.rot.y;
    in.targetPos = leaderPos;
    AnchorAI::StepHeadLookToward(in, &this_->headLimbRot, &this_->upperLimbRot);
}

// ── Idle blend (waitL ↔ waitR) ───────────────────────────────────────
// Continuously blend between waitL_free and waitR_free for a subtle
// breathing-with-side-glance idle. Pattern from Player at z_player.c:8062.
// Free-running phase (sine of frame counter) — no external target driver
// needed for our v1; idle isn't long enough for the slow target-driven
// pattern Player uses.
void TickIdleBlend(EnFollower* this_, PlayState* play) {
    // Phase advances ~1/40 cycle per tick → ~2s per full L↔R↔L cycle.
    this_->idleBlendPhase += (1.0f / 40.0f);
    if (this_->idleBlendPhase > 1.0f) this_->idleBlendPhase -= 1.0f;
    // Blend weight: 0.5 + 0.5 * sin(2π * phase) — oscillates 0..1.
    const float weight = 0.5f + 0.5f * Math_SinS((s16)(this_->idleBlendPhase * 0x10000));

    LinkAnimation_BlendToJoint(
        play, &this_->skelAnime,
        (LinkAnimationHeader*)&gPlayerAnim_link_normal_waitR_free,
        this_->skelAnime.curFrame,
        (LinkAnimationHeader*)&gPlayerAnim_link_normal_waitL_free,
        this_->skelAnime.curFrame,
        weight, this_->blendTable);
}

FollowerNpcAnim AnimForState(s32 state, float speedXZ) {
    switch (state) {
        case EN_FOLLOWER_STATE_FOLLOW: {
            // Truly-stopped speed (≈ 0) → kWait. Anything else plays
            // walk or run based on Player's own walk↔run threshold of
            // speedTarget > 4 (z_player.c:8165). NPC pace-matches the
            // leader, so its speedXZ value range mirrors leader's:
            // walk ~3, run ~7-9, sprint with catch-up bonus up to 12.
            if (speedXZ < 0.1f) return FollowerNpcAnim::kWait;
            return (speedXZ > 4.0f) ? FollowerNpcAnim::kRun : FollowerNpcAnim::kWalk;
        }
        case EN_FOLLOWER_STATE_STUCK:
            // STUCK runs for a single tick before transitioning to
            // FOLLOW; play wait so the brief frame doesn't show the
            // NPC mid-stride at zero motion.
            return FollowerNpcAnim::kWait;
        case EN_FOLLOWER_STATE_CLIMBING:
            return FollowerNpcAnim::kClimbUp;
        case EN_FOLLOWER_STATE_SWIMMING:
            // Treading water at low speed; full swim anim when moving.
            // Threshold matches Player's walk↔run handoff at 4.0.
            return (speedXZ > 0.5f) ? FollowerNpcAnim::kSwim
                                    : FollowerNpcAnim::kSwimWait;
        case EN_FOLLOWER_STATE_LEDGE_HOIST:
            // Picked from hoistContext by the dispatcher's anim
            // resolution path (which has access to `this_`). This
            // case shouldn't normally be hit because the dispatcher
            // overrides localAnim during LEDGE_HOIST before calling
            // EnsureAnimation; return a sensible default in case the
            // dispatcher path is bypassed.
            return FollowerNpcAnim::kHoistGround;
        case EN_FOLLOWER_STATE_DEAD:
            // Stage 2: Player's Game-Over death anim
            // (gPlayerAnim_link_normal_back_downA — fall onto back).
            return FollowerNpcAnim::kDeath;
        case EN_FOLLOWER_STATE_ATTACK:
            // Stage 4 — vertical sword swing.
            return FollowerNpcAnim::kSwordSwing;
        case EN_FOLLOWER_STATE_ENGAGE:
            // Stage 4 — pursuit locomotion (same anims as FOLLOW).
            // Speed threshold matches FOLLOW so handoff IDLE→FOLLOW
            // ↔ ENGAGE looks consistent.
            if (speedXZ > 8.0f) return FollowerNpcAnim::kRun;
            if (speedXZ > 0.5f) return FollowerNpcAnim::kWalk;
            return FollowerNpcAnim::kWait;
        case EN_FOLLOWER_STATE_BLOCK:
            // Stage 4 — shield-up wait. The dispatcher overrides to
            // kBlockHit during the brief window after a successful
            // block; otherwise the looping wait pose holds.
            return FollowerNpcAnim::kBlockWait;
        case EN_FOLLOWER_STATE_RANGED_ATTACK:
            // Stage 4 — bow shoot one-shot. Spans draw + release.
            return FollowerNpcAnim::kBowShoot;
        case EN_FOLLOWER_STATE_STANDBY:
            // Stage 4 — alert idle between combat exchanges. Same kWait
            // anim as IDLE; the visual difference comes from the
            // dispatcher setting currentAnimType=1 (fighter) here, so
            // EnsureAnimation picks gPlayerAnim_link_normal_wait
            // (sword+shield ready stance) instead of the _free variant.
            return FollowerNpcAnim::kWait;
        case EN_FOLLOWER_STATE_CRAWLING:
            // Stage 5 — child crawlspace traversal. Returns kCrawlMove
            // on entry; once it plays through, the dispatcher's
            // stopAnimPlaying hold check keeps localAnim pinned to
            // currentAnim (still kCrawlMove) so EnsureAnimation
            // no-ops and the SkelAnime sits at the end frame (low
            // crouch pose) — body translates via TickCRAWLING.
            // Dispatcher overrides localAnim to kCrawlExit when the
            // exit transition fires.
            return FollowerNpcAnim::kCrawlMove;
        case EN_FOLLOWER_STATE_IDLE:
        default:
            return FollowerNpcAnim::kWait;
    }
}

// Phase 6 — find the climb anchor whose grid is closest to `pos`.
// Used at CLIMBING entry to pick the anchor; cached in
// sLocalNav.activeClimbAnchor for the duration of the climb run.
//
// "Closest" = min XZ distance to anchor.basePos (the anchor's
// floor-level entry point). Suffices because anchors are well-spaced
// in OoT scenes (Inside Deku Tree's 5 spiral-wall anchors are 100u+
// apart in XZ).
const ::AnchorNavRoom::ClimbAnchor* FindClosestClimbAnchor(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& pos)
{
    if (navData == nullptr || navData->climbAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::ClimbAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& anc : navData->climbAnchors) {
        const float dx = pos.x - anc.basePos.x;
        const float dz = pos.z - anc.basePos.z;
        const float dSq = dx*dx + dz*dz;
        if (dSq < bestDistSq) {
            bestDistSq = dSq;
            best = &anc;
        }
    }
    return best;
}

// Force-engage CLIMBING by manually populating the substrate path
// with this anchor's climb cells, sorted bottom-to-top by Y. Used
// when leader is climbing and substrate pathfinding can't bridge
// (cross-room target, leader mid-wall) — we bypass the pathfinder
// and just walk up the anchor's known cells.
//
// `topYBound`: only include cells at or below leaderY + 50u so NPC
// doesn't climb past where leader is. Lets the NPC track leader's
// vertical progress without overshooting.
//
// Returns true on success (path populated with ≥1 cell).
bool PopulateAnchorClimbPath(const ::AnchorNavRoom::RoomNavData* navData,
                             const ::AnchorNavRoom::ClimbAnchor& anchor,
                             const Vec3f& npcPos, const Vec3f& leaderPos,
                             AnchorNav::ActorTrail::NavPath& path)
{
    if (navData == nullptr || anchor.nodeCount == 0) return false;
    path.Reset();

    // Collect (Y, idx) pairs for nodes in the column closest to NPC's
    // XZ. Multi-column walls (wide vines) have many columns; pick the
    // one nearest NPC so we don't zigzag laterally during the climb.
    //
    // Column selection: project NPC and each node onto anchor.planeAxisU
    // (lateral wall axis). Keep nodes whose U-coordinate is within
    // 40u of NPC's U-coordinate (≈ 1 cell spacing of 30u + slack).
    // Use LEADER's U projection as the column-selection reference, NOT
    // NPC's. Leader is on the wall, so leader's U reliably matches a
    // cell column. NPC's pos in water (or on the floor near the wall)
    // can be offset along the wall's lateral axis so NPC's U doesn't
    // match any cell — log 142 showed "anchor found, distance OK
    // (172u), but path population returned empty" because NPC's U
    // missed all 8 cells of the anchor. Tracking leader's column
    // guarantees the path follows where the leader actually climbed.
    const float leaderU =
        (leaderPos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
        (leaderPos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;

    struct Entry { float y; uint16_t idx; };
    std::vector<Entry> column;
    // Column filter — cells within ±15u of leader's U (≈ half a 30u
    // cell pitch). Y-axis cap at leader.y - 20 so NPC stays slightly
    // BELOW leader during co-climb (was leader.y + 50 which let NPC
    // outpace leader by 50-100u — Player's input-driven climb is
    // slower than NPC's fixed 4 u/frame).
    constexpr float kClimbColumnTolerance     = 15.0f;
    constexpr float kClimbColumnFallbackTol   = 30.0f;
    constexpr float kClimbStayBelowLeader     = 20.0f;
    auto collectColumn = [&](float tolerance) {
        column.clear();
        for (uint16_t i = 0; i < anchor.nodeCount; i++) {
            const uint16_t idx = anchor.firstNodeIdx + i;
            if (idx >= navData->nodes.size()) break;
            const auto& n = navData->nodes[idx];
            const float nodeU =
                (n.pos.x - anchor.planeOrigin.x) * anchor.planeAxisU.x +
                (n.pos.z - anchor.planeOrigin.z) * anchor.planeAxisU.z;
            if (std::fabs(nodeU - leaderU) > tolerance) continue;
            if (n.pos.y > leaderPos.y - kClimbStayBelowLeader) continue;
            column.push_back({n.pos.y, idx});
        }
    };
    // Try strict (±15u) first. If empty (leader's U doesn't match any
    // column — happens on curved walls / off-grid leader pos), fall
    // back to wider tolerance (±30u, full cell pitch). Captures
    // adjacent columns; minor lateral hop acceptable vs no climb.
    collectColumn(kClimbColumnTolerance);
    if (column.empty()) {
        collectColumn(kClimbColumnFallbackTol);
    }
    if (column.empty()) return false;

    // Sort by Y ascending. NPC starts at bottom; CLIMBING handler
    // advances cursor as it climbs.
    std::sort(column.begin(), column.end(),
              [](const Entry& a, const Entry& b){ return a.y < b.y; });

    // Skip cells at or below NPC's current Y — strict filter, no
    // downward slack. Without this, RE-ENTRY into CLIMBING (after
    // the previous segment's path exhausted) would build a new path
    // starting up to 30u BELOW NPC's current Y, forcing NPC to climb
    // DOWN before resuming the climb up. User observed this as
    // "oscillates up and down" during sustained leader-climbing.
    // With strict filter, every re-engagement starts strictly ABOVE
    // NPC's current Y → monotonic ascent.
    for (const auto& e : column) {
        if (e.y < npcPos.y) continue;
        const auto& n = navData->nodes[e.idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    if (path.waypoints.empty()) {
        // NPC already above the entire column (unlikely but defensive).
        // Push at least the top cell so CLIMBING has SOMETHING to chase.
        const auto& n = navData->nodes[column.back().idx];
        path.waypoints.push_back(n.pos);
        path.waypointFlags.push_back(n.flags);
    }
    path.sceneNum = gPlayState->sceneNum;
    return true;
}

// Engagement constants for the leader-climbing trigger.
static constexpr float kClimbForceEngageBaseDistSq = 200.0f * 200.0f;

// CLIMBING handler — Phase 6 scripted-climb driver. Runs while the
// substrate path's current subgoal carries a NODE_CLIMB_* flag.
//
// Mechanics (per Plans/npc_follower_plan.md §3):
//   - Snap NPC XZ each frame to the current climb subgoal's XZ.
//   - Drive Y toward the subgoal at kClimbSpeedY.
//   - Face into wall via the active anchor's planeNormal.
//   - Play gPlayerAnim_link_normal_Fclimb_upL (looping).
//   - Advance path cursor on 3D proximity to current subgoal.
//   - Exit CLIMBING when the next subgoal is non-climb (mantle out
//     to FOLLOW) OR when path is exhausted (FOLLOW handles fallback).
//
// v1 simplifications (deferred to later iterations):
//   - No mantle-hold animation (snaps to top on exit).
//   - No lateral-axis splitting (vines only support vertical motion
//     in v1; lateral happens via XZ snap each frame).
//   - No detach-detection (peer-Follower has it; v1 NPC trusts the
//     scripted snap to keep us on the wall).
//   - No climb-down anim (Y always positive in v1; if leader is below,
//     the substrate path probably routes through a drop anchor not
//     a climb-down).
void TickCLIMBING(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // FAST PATH: leader is actively climbing AND NPC is within 30u 3D
    // of leader. Track leader's pos directly (no cell-grid pathfinding).
    // Beyond 30u, fall through to the cell-grid path so NPC can
    // navigate to leader properly (e.g. NPC at bottom of wall, leader
    // at top — needs path to know which cells to climb through).
    //
    // Solves two field-test bugs caused by cell-grid pathfinding when
    // NPC and leader are at near-equal Y:
    //   * 10u Y oscillation during co-climb (path filter empty →
    //     exit → gravity → refire with lower Y → repeat).
    //   * NPC drops to bottom when leader reaches lateral edge (same
    //     empty-path → exit → fall sequence).
    //
    // Direct tracking: NPC.xz = leader.xz, NPC.y lerps toward
    // (leader.y - 10). Smooth, no oscillation.
    {
        const float pdx = leaderPos.x - a->world.pos.x;
        const float pdy = leaderPos.y - a->world.pos.y;
        const float pdz = leaderPos.z - a->world.pos.z;
        const float dist3DSq = pdx*pdx + pdy*pdy + pdz*pdz;
        // Loosened from 30 → 60: the prior 30u limit was a 3D
        // distance, but co-climbing has the NPC ~30u BELOW leader
        // by design — so the |dy| component alone is at the limit
        // and any lateral wall-curvature offset puts the NPC over.
        // Result: fast-path fires only briefly at engagement, then
        // the regular path-based code takes over and produces the
        // ~10u-above-leader visual the user reported. 60u gives the
        // fast-path room to track once the offset settles.
        constexpr float kCoClimbProxLimit = 60.0f;
        const bool nearLeader = dist3DSq <= (kCoClimbProxLimit * kCoClimbProxLimit);
        const bool leaderClimbing = Anchor::Instance->IsLocalPlayerClimbing();

        // Diagnostic — log fast-path eligibility every ~1.5s so we can
        // see if/when it fires and what the resulting Δy is.
        static uint64_t sLastCoClimbDiag = 0;
        const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                      std::memory_order_relaxed);
        if (curFrame > sLastCoClimbDiag + 30) {
            SPDLOG_INFO("[FollowerNPC.coClimb] eligibility: leaderClimbing={} "
                        "nearLeader={} dist3D={:.1f} (limit {:.0f}) "
                        "NPC.y={:.0f} leader.y={:.0f} (Δy={:+.1f})",
                        leaderClimbing, nearLeader, std::sqrt(dist3DSq),
                        kCoClimbProxLimit, a->world.pos.y, leaderPos.y, pdy);
            sLastCoClimbDiag = curFrame;
        }

        if (leaderClimbing && nearLeader) {
            constexpr float kCoClimbYOffset = 30.0f;  // sit ~30u below leader (tuned 10 → 30 per field test)
            a->world.pos.x = leaderPos.x;
            a->world.pos.z = leaderPos.z;
            const float targetY = leaderPos.y - kCoClimbYOffset;
            const float dy = targetY - a->world.pos.y;
            if (std::fabs(dy) < kClimbSpeedY) {
                a->world.pos.y = targetY;
            } else {
                a->world.pos.y += (dy > 0.0f ? kClimbSpeedY : -kClimbSpeedY);
            }
            // Mirror leader's facing — handles wall curvature
            // automatically since Player's rotation tracks the surface.
            Player* leaderPtr = GET_PLAYER(play);
            if (leaderPtr != nullptr) {
                a->shape.rot.y = leaderPtr->actor.shape.rot.y;
                a->world.rot.y = a->shape.rot.y;
            }
            a->speedXZ = 0.0f;
            sLocalNav.leaderWasClimbingPrevTick = true;
            return;  // skip path-based subgoal navigation
        }

        // Leader-just-hoisted-over edge: leader was climbing last tick
        // (fast-path was firing), now isn't. NPC is still in CLIMBING.
        // Without intervention, the regular path-based code below
        // requires NPC within 60u of anchor.topPos.y to mantle out;
        // when fast-path was tracking the user as they climbed past
        // the rim, NPC is typically 30-101u below the rim — outside
        // the mantle band — and falls through to FOLLOW where gravity
        // pulls it back down to the wall base (log 158 line 814+
        // showed NPC dropping from -101 → -721 in 6s).
        //
        // Fix: trigger LEDGE_HOIST to the active anchor's topPos so
        // NPC mantles up to the same ledge leader landed on. Same
        // anim path the GROUND-context hoist uses for ground-to-ledge.
        if (!leaderClimbing && sLocalNav.leaderWasClimbingPrevTick &&
            sLocalNav.activeClimbAnchor != nullptr) {
            const Vec3f topPos = sLocalNav.activeClimbAnchor->topPos;
            SPDLOG_INFO("[FollowerNPC] CLIMBING→LEDGE_HOIST(ground) "
                        "(leader hoisted over rim) anchor.topPos=({:.0f},{:.0f},{:.0f}) "
                        "NPC at ({:.0f},{:.0f},{:.0f})",
                        topPos.x, topPos.y, topPos.z,
                        a->world.pos.x, a->world.pos.y, a->world.pos.z);
            this_->hoistContext   = (s8)HOIST_CONTEXT_GROUND;
            this_->hoistTargetPos = topPos;
            this_->hoistEntryYaw  =
                Math_Atan2S(topPos.z - a->world.pos.z,
                            topPos.x - a->world.pos.x);
            // Snap XZ to ledge top so anim plays in place (matches
            // the FOLLOW→LEDGE_HOIST entry pattern).
            a->world.pos.x = topPos.x;
            a->world.pos.z = topPos.z;
            sLocalNav.hoistStartPos = a->world.pos;
            sLocalNav.navState.path.Reset();
            sLocalNav.activeClimbAnchor = nullptr;
            sLocalNav.leaderWasClimbingPrevTick = false;
            this_->state = EN_FOLLOWER_STATE_LEDGE_HOIST;
            this_->stopAnimPlaying = 0;  // let dispatcher's LEDGE_HOIST anim override flow through
            a->speedXZ = 0.0f;
            return;
        }

        sLocalNav.leaderWasClimbingPrevTick = leaderClimbing;
    }

    // Resolve subgoal.
    if (sLocalNav.navState.path.Empty()) {
        // Path exhausted — if leader is STILL climbing and we have an
        // active anchor, refresh the path with new cells above NPC's
        // current Y and stay in CLIMBING. Without this, NPC exits to
        // FOLLOW between short climb segments, gravity pulls Y down
        // (no floor at the wall side), force-engage refires at a
        // lower Y, NPC climbs again — visible oscillation. Log 143
        // showed NPC bouncing between Y=79 and Y=93 because the
        // anchor had only 2 cells per column above NPC at any time.
        if (Anchor::Instance->IsLocalPlayerClimbing() &&
            sLocalNav.activeClimbAnchor != nullptr) {
            const ::AnchorNavRoom::RoomNavData* navData =
                ::AnchorNavRoom::GetForRoom(
                    gPlayState->sceneNum,
                    (int8_t)gPlayState->roomCtx.curRoom.num);
            if (navData != nullptr &&
                PopulateAnchorClimbPath(navData, *sLocalNav.activeClimbAnchor,
                                        a->world.pos, leaderPos,
                                        sLocalNav.navState.path)) {
                // Path refreshed in place; continue climbing this tick.
                // Fall through to the subgoal-resolution code below.
            }
        }
        // Re-check after refresh attempt.
        if (sLocalNav.navState.path.Empty()) {
            // Mantle-out: if NPC has reached near the top of the wall
            // (within 60u of anchor.topPos.y), snap to topPos and
            // exit. Without this, NPC at the top of climb falls when
            // CLIMBING exits — leader hoisted to ledge, we lost
            // refresh trigger, NPC mid-wall has no floor below →
            // gravity drops NPC. Snap to topPos puts NPC on the
            // ledge floor.
            if (sLocalNav.activeClimbAnchor != nullptr) {
                const float topY = sLocalNav.activeClimbAnchor->topPos.y;
                if (a->world.pos.y >= topY - 60.0f) {
                    a->world.pos  = sLocalNav.activeClimbAnchor->topPos;
                    a->velocity.y = 0.0f;
                    SPDLOG_INFO("[FollowerNPC] CLIMBING→FOLLOW (mantle-out: "
                                "NPC at top, snapped to anchor.topPos "
                                "({:.0f},{:.0f},{:.0f}))",
                                a->world.pos.x, a->world.pos.y, a->world.pos.z);
                }
            }
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
            sLocalNav.activeClimbAnchor = nullptr;
            return;
        }
    }
    const Vec3f& subgoal      = sLocalNav.navState.path.CurrentSubgoal();
    const uint32_t subgoalFlags = sLocalNav.navState.path.CurrentSubgoalFlags();
    const bool subgoalIsClimb = (subgoalFlags & ::AnchorNavRoom::NODE_CLIMB_ANY) != 0;

    // Mantle-out: next subgoal is non-climb → snap to subgoal pos
    // (effectively the top of the climb), exit to FOLLOW.
    if (!subgoalIsClimb) {
        a->world.pos.x = subgoal.x;
        a->world.pos.y = subgoal.y;
        a->world.pos.z = subgoal.z;
        a->speedXZ     = 0.0f;
        sLocalNav.activeClimbAnchor = nullptr;
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        return;
    }

    // Resolve the active anchor (cache on entry; refresh if the
    // navData was rebuilt or cache is stale).
    //
    // Phase 3 follow-up (2026-05-18): on EVERY tick while CLIMBING,
    // try a position-match refresh first. If the subgoal cell
    // belongs to a DIFFERENT anchor than the cached one (spiral wall
    // cross-anchor bridges, L-shape vine wall spanning two anchors),
    // switch the active anchor. Same shape as AI Player Follower's
    // Option A refresh (Follower.cpp:2331+). Without this, NPC would
    // commit to the closest-basePos anchor at engagement and never
    // switch — symptom: stuck/teleport recovery at every cross-anchor
    // climb-cell waypoint.
    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            gPlayState->sceneNum,
            (int8_t)gPlayState->roomCtx.curRoom.num);
    if (navData != nullptr) {
        // Position-match against subgoal first (most accurate — the
        // subgoal IS a climb-node position). Fall back to NPC's
        // current pos if subgoal isn't matched.
        uint16_t refreshedIdx =
            ::AnchorNavRoom::FindAnchorByClimbNodePosition(navData, subgoal);
        if (refreshedIdx == UINT16_MAX) {
            refreshedIdx = ::AnchorNavRoom::FindAnchorByClimbNodePosition(
                navData, a->world.pos);
        }
        if (refreshedIdx != UINT16_MAX) {
            const auto* refreshed = &navData->climbAnchors[refreshedIdx];
            if (refreshed != sLocalNav.activeClimbAnchor) {
                SPDLOG_INFO("[FollowerNPC] CLIMBING anchor refresh: "
                            "{} → {} (subgoal=({:.0f},{:.0f},{:.0f}) "
                            "matched anchor {})",
                            sLocalNav.activeClimbAnchor != nullptr
                                ? "prev" : "(null)",
                            refreshedIdx,
                            subgoal.x, subgoal.y, subgoal.z, refreshedIdx);
                sLocalNav.activeClimbAnchor = refreshed;
            }
        }
    }

    if (sLocalNav.activeClimbAnchor == nullptr) {
        // Position-match found nothing — fall back to closest-by-basePos
        // (legacy behavior; useful when NPC is at floor entry point and
        // pos isn't yet on any climb cell).
        sLocalNav.activeClimbAnchor = FindClosestClimbAnchor(navData, a->world.pos);
        if (sLocalNav.activeClimbAnchor == nullptr) {
            // No anchor data — fall back to FOLLOW.
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
            return;
        }
    }
    const auto& anc = *sLocalNav.activeClimbAnchor;

    // XZ position: when leader is also climbing this anchor, MATCH
    // leader's XZ exactly — leader's pos is on the actual climb
    // surface (vine cell or ladder rung) at the right lateral
    // position. Without this, NPC snaps to nearest CELL.xz which is
    // grid-quantized (~30u pitch) and can be 15u off from leader's
    // actual lateral position.
    //
    // When leader is NOT climbing (NPC alone on the wall), use the
    // cell-based snap with body offset along planeNormal so NPC's
    // body sits in front of the wall surface (not buried).
    if (Anchor::Instance->IsLocalPlayerClimbing()) {
        a->world.pos.x = leaderPos.x;
        a->world.pos.z = leaderPos.z;
    } else {
        a->world.pos.x = subgoal.x + anc.planeNormal.x * kClimbBodyOffset;
        a->world.pos.z = subgoal.z + anc.planeNormal.z * kClimbBodyOffset;
    }

    // Drive Y toward subgoal at kClimbSpeedY. Clamp to subgoal Y on
    // approach so we don't overshoot. (planeNormal.y component
    // intentionally ignored — climbable walls are near-vertical, and
    // applying body-offset to Y would lift NPC above ladder rungs.)
    const float dy = subgoal.y - a->world.pos.y;
    if (std::fabs(dy) < kClimbSpeedY) {
        a->world.pos.y = subgoal.y;
    } else {
        a->world.pos.y += (dy > 0.0f ? kClimbSpeedY : -kClimbSpeedY);
    }

    // Face direction. When leader is also climbing this anchor, mirror
    // leader's facing — leader's rotation reflects the actual wall
    // surface orientation including curvature (Player's collision
    // updates rot.y as it moves between cells on a curved wall).
    // Without this, NPC's rotation locks to the anchor's planeNormal
    // (a single average direction) and doesn't update as NPC moves
    // laterally along a curved vine wall.
    if (Anchor::Instance->IsLocalPlayerClimbing()) {
        Player* leaderPtr = GET_PLAYER(play);
        if (leaderPtr != nullptr) {
            a->shape.rot.y = leaderPtr->actor.shape.rot.y;
        }
    } else {
        a->shape.rot.y = Math_Atan2S(-anc.planeNormal.z, -anc.planeNormal.x);
    }
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ     = 0.0f;  // we're scripting position, not using physics speed

    // Climb anim selection happens in the dispatcher's anim resolution
    // (climb step alternation block). No EnsureAnimation call here —
    // dispatcher fires kClimbUpL / kClimbUpR alternating one-shots
    // driven by Y motion, mirroring Player's actionVar2 toggle.

    // Cursor advance — Y-axis only. NPC's XZ is snapped to subgoal.xz +
    // planeNormal * kClimbBodyOffset every frame (so body sits in front
    // of the wall, not buried), which means XZ distance from subgoal is
    // ~12u baseline regardless of climb progress. A 3D distance check
    // (the earlier kClimbSubgoalReach3D=24u test) would fire on the
    // very first frame after XZ snap because 12² < 24² — chaining
    // multiple advances per tick and zipping through the path.
    //
    // Y is the meaningful progress axis for a vertical climb. Advance
    // when within 12u of the subgoal's Y (≈ 3 frames of climb at
    // kClimbSpeedY=4.0). Produces one cell of progress per ~7 frames
    // at the configured climb speed — smooth and visibly paced.
    const float dyAdv = std::fabs(a->world.pos.y - subgoal.y);
    if (dyAdv < 12.0f) {
        sLocalNav.navState.path.Advance();
    }
}

// Swim constants. Match Player's ageProperties.unk_24 (z_player.c:453,505):
// 36u for adult, 22u for child — that's the submersion depth at which
// Player switches to swimming AND the depth Player maintains while
// swimming (so head sits ~24u above surface for adult, ~28u for child).
//
// kSwimSpeedMax = Link's swim cap (~3-4 u/frame).
// kSwimShoreExitDepth = if floor below NPC is within this much of
//   the water surface, NPC can walk onto the floor → exit SWIMMING.
//   Matches Link's step height (most actors can step up ~30u).
static constexpr float kSwimDepthThresholdAdult = 36.0f;
static constexpr float kSwimDepthThresholdChild = 22.0f;
static constexpr float kSwimSpeedMax            = 4.0f;
static constexpr float kSwimEnterArrive         = 60.0f;
static constexpr float kSwimShoreExitDepth      = 30.0f;

// Pick the per-age swim depth (used both for entry threshold and
// surface-clamp drop — Player maintains depth = unk_24 while swimming).
inline float SwimDepthFor(s8 linkAge) {
    // gSaveContext.linkAge: 0 = adult, 1 = child (matches sAgeProperties
    // indexing at z_player.c:442 / 498).
    return (linkAge == 0) ? kSwimDepthThresholdAdult : kSwimDepthThresholdChild;
}

// SWIMMING handler — clamp Y to water surface at Link's swim depth,
// swim XZ toward leader at swim pace. Exits to FOLLOW when:
//   (a) no water at NPC's XZ (swimming straight off the edge of the
//       waterbox extent), OR
//   (b) shore is shallow enough below NPC to walk on (floor is
//       within kSwimShoreExitDepth of the water surface).
// Dispatcher skips Actor_MoveXZGravity for SWIMMING because gravity
// would fight the Y surface clamp.
void TickSWIMMING(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // Clamp Y to (surface - swim depth). For adult that's surface-36,
    // for child surface-22. Maintains Link's swim pose where head
    // sits above water and most of body submerged.
    f32 surfaceY = a->world.pos.y;
    WaterBox* wb = nullptr;
    if (!WaterBox_GetSurface1(play, &play->colCtx,
                              a->world.pos.x, a->world.pos.z,
                              &surfaceY, &wb)) {
        SPDLOG_INFO("[FollowerNPC] SWIMMING→FOLLOW (no water under NPC at "
                    "({:.0f},{:.0f},{:.0f}))",
                    a->world.pos.x, a->world.pos.y, a->world.pos.z);
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        return;
    }

    // Shore-shallow exit. Raycast floor down from a point slightly
    // above NPC. If the floor is within kSwimShoreExitDepth of the
    // surface, NPC can stand on it — exit swim. Without this, NPC
    // never walks out onto sloped terrain (waterboxes extend right
    // to the shore, so WaterBox_GetSurface1 keeps returning true).
    Vec3f rayStart = { a->world.pos.x, surfaceY + 5.0f, a->world.pos.z };
    CollisionPoly* floorPoly = nullptr;
    const f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &floorPoly, &rayStart);
    if (floorPoly != nullptr && (surfaceY - floorY) < kSwimShoreExitDepth) {
        a->world.pos.y = floorY;
        a->velocity.y  = 0.0f;
        this_->state   = EN_FOLLOWER_STATE_FOLLOW;
        SPDLOG_INFO("[FollowerNPC] SWIMMING→FOLLOW (shore-shallow exit: "
                    "surface={:.0f} floor={:.0f} depth={:.1f}u < {:.0f}u)",
                    surfaceY, floorY, surfaceY - floorY, kSwimShoreExitDepth);
        return;
    }

    // Swim Y clamp. Field test reported NPC sitting 5u too high above
    // water; add a small extra drop on top of the per-age threshold
    // so head sits at a more natural level (slightly less of the body
    // above surface, matching Player's actual swim pose).
    constexpr float kSwimExtraDrop = 5.0f;
    const float swimDepth = SwimDepthFor(this_->linkAge) + kSwimExtraDrop;
    a->world.pos.y  = surfaceY - swimDepth;
    a->velocity.y   = 0.0f;  // reset so a future FOLLOW transition starts fresh

    // Yaw toward leader, move at swim pace. Slower than land speeds —
    // Link's swim caps around 3-4 u/frame. No IDLE substate for
    // SWIMMING — just tread water when close.
    const float dx = leaderPos.x - a->world.pos.x;
    const float dz = leaderPos.z - a->world.pos.z;
    const float distSq = dx*dx + dz*dz;
    if (distSq > kSwimEnterArrive * kSwimEnterArrive) {
        const s16 yaw = Math_Atan2S(dz, dx);
        a->shape.rot.y = yaw;
        a->world.rot.y = yaw;
        a->speedXZ     = kSwimSpeedMax;
        // Manual XZ motion. Math_SinS/CosS convert binary angle → unit
        // vector. Direct write to world.pos.xz mirrors how TickCLIMBING
        // handles XZ outside the standard physics pipeline.
        a->world.pos.x += Math_SinS(yaw) * a->speedXZ;
        a->world.pos.z += Math_CosS(yaw) * a->speedXZ;
    } else {
        a->speedXZ = 0.0f;  // tread water; swim_wait anim selected by AnimForState
    }
}

// ── Ledge-hoist (bundled swim-out + ground-mantle) ───────────────────

// Ledge-anchor lookup constants.
static constexpr float kHoistApproachMatchXZSq   = 60.0f * 60.0f;
static constexpr float kHoistSwimTriggerHeight   = 60.0f;   // leader >60u above water → search for swim-exit
static constexpr float kHoistGroundMaxLift       = 90.0f;   // accept hoists up to this Y delta

// Find the closest LedgeAnchor in this room where approachPos is
// near `nearPos` (XZ) AND topPos is meaningfully above `nearPos.y`.
// `wantHighEnoughForGround` filters out anchors where the lift is
// already-walkable (i.e. step heights handled by gravity, not a
// scripted hoist).
const ::AnchorNavRoom::LedgeAnchor* FindClosestLedgeAnchor(
    const ::AnchorNavRoom::RoomNavData* navData, const Vec3f& nearPos)
{
    if (navData == nullptr || navData->ledgeAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::LedgeAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& anc : navData->ledgeAnchors) {
        const float distSq = Dist2DSq(nearPos, anc.approachPos);
        if (distSq > kHoistApproachMatchXZSq) continue;
        const float lift = anc.topPos.y - nearPos.y;
        if (lift < 20.0f) continue;  // too low; standard step / nothing to hoist
        if (lift > kHoistGroundMaxLift) continue;  // too tall; not a v1 hoist
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = &anc;
        }
    }
    return best;
}

// Raycast fallback — detect a hoistable ledge ahead of NPC even when
// the room doesn't have a LedgeAnchor catalogued for this geometry.
// Probe:
//   1. Cast forward from NPC chest height toward leader direction.
//      A wall hit means there's geometry to hoist over.
//   2. From slightly past the wall hit (along leader direction), cast
//      down from kHoistGroundMaxLift above. The floor that intersects
//      is the ledge top.
//   3. Verify lift (top - NPC.y) is in the hoist range [20, 90].
//
// Returns true and writes outTopPos if a valid hoist target is found.
// chkHeight = how high above NPC.y to start the forward cast (catches
// short walls when small, tall walls when large; tunable).
bool RaycastDetectLedge(PlayState* play, const Vec3f& npcPos,
                        const Vec3f& leaderPos, Vec3f& outTopPos)
{
    const float dx = leaderPos.x - npcPos.x;
    const float dz = leaderPos.z - npcPos.z;
    const float distSq = dx*dx + dz*dz;
    if (distSq < 1.0f) return false;
    const float invDist = 1.0f / std::sqrt(distSq);
    const float dirX = dx * invDist;
    const float dirZ = dz * invDist;

    constexpr float kForwardCastDist = 80.0f;
    constexpr float kChestHeight     = 20.0f;
    constexpr float kPastWallNudge   = 8.0f;
    constexpr float kProbeMaxLift    = 90.0f;

    Vec3f rayA = { npcPos.x, npcPos.y + kChestHeight, npcPos.z };
    Vec3f rayB = { npcPos.x + dirX * kForwardCastDist, rayA.y,
                   npcPos.z + dirZ * kForwardCastDist };
    Vec3f wallHit;
    CollisionPoly* wallPoly = nullptr;
    if (!BgCheck_AnyLineTest1(&play->colCtx, &rayA, &rayB, &wallHit,
                              &wallPoly, 1)) {
        return false;
    }

    Vec3f downStart = {
        wallHit.x + dirX * kPastWallNudge,
        wallHit.y + kProbeMaxLift,
        wallHit.z + dirZ * kPastWallNudge
    };
    CollisionPoly* topPoly = nullptr;
    const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx, &topPoly,
                                                  &downStart);
    if (topPoly == nullptr) return false;
    const float lift = topY - npcPos.y;
    if (lift < 20.0f || lift > kProbeMaxLift) return false;

    outTopPos.x = downStart.x;
    outTopPos.y = topY;
    outTopPos.z = downStart.z;
    return true;
}

// LEDGE_HOIST handler — one-shot mantle. The anim runs once
// (ANIMMODE_ONCE via the kHoist* one-shot table in EnsureAnimation);
// stopAnimPlaying clears when LinkAnimation_Update reports the anim
// reached endFrame. At that point we snap to hoistTargetPos and
// return to FOLLOW.
void TickLEDGE_HOIST(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ     = 0.0f;
    a->shape.rot.y = this_->hoistEntryYaw;
    a->world.rot.y = this_->hoistEntryYaw;

    // Wait until the hoist anim has been (a) set up by EnsureAnimation
    // AND (b) played to completion. Without check (a), the entry tick
    // would exit immediately because the dispatcher runs TickLEDGE_HOIST
    // BEFORE EnsureAnimation in the per-tick order — stopAnimPlaying
    // is still 0 (stale from prior state) on entry.
    //
    // Field-test log 144 showed LEDGE_HOIST entering and exiting on
    // the same timestamp (no anim visible). The "teleport back to top
    // when leader runs off ledge" was the same bug: NPC walked off
    // snap target, GROUND-MANTLE trigger refired, the new LEDGE_HOIST
    // exited instantly with a snap-back to topPos.
    const bool hoistAnimSetUp =
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kHoistGround ||
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kHoistSwim;
    if (!hoistAnimSetUp || this_->stopAnimPlaying) {
        // Hold position — but if anim has been set up AND is playing,
        // lerp pos over the anim's progress so body moves smoothly
        // from start (lower floor / water surface) up to the ledge.
        // Without this, body sits at start pos for the whole anim
        // duration, then snaps to top — looks like teleport.
        if (hoistAnimSetUp && this_->stopAnimPlaying &&
            this_->skelAnime.endFrame > 0.0f) {
            const float progress =
                std::min(1.0f, this_->skelAnime.curFrame /
                                this_->skelAnime.endFrame);
            // Lerp ALL THREE axes (X, Y, Z) so body translates
            // smoothly from start pos to ledge top during the anim.
            // Without XZ lerp, NPC stays at its start XZ (which may
            // be a few units away from the ledge edge) and only Y
            // moves up — looks like body floating up to the wrong
            // XZ then snapping to ledge top XZ at completion.
            const Vec3f& s = sLocalNav.hoistStartPos;
            const Vec3f& t = this_->hoistTargetPos;
            a->world.pos.x = s.x + (t.x - s.x) * progress;
            a->world.pos.y = s.y + (t.y - s.y) * progress;
            a->world.pos.z = s.z + (t.z - s.z) * progress;
        }
        return;
    }

    // Anim complete — snap to ledge top and exit.
    a->world.pos  = this_->hoistTargetPos;
    a->velocity.y = 0.0f;   // reset so gravity starts fresh from the snap
    sLocalNav.navState.path.Reset();   // any pre-hoist path is now stale (NPC moved)
    sLocalNav.navState.lastPathRefreshFrame = 0;
    sLocalNav.leashFrames     = 0;
    sLocalNav.closeFailFrames = 0;
    this_->state = EN_FOLLOWER_STATE_FOLLOW;
    (void)leaderPos;
    SPDLOG_INFO("[FollowerNPC] LEDGE_HOIST→FOLLOW (snapped to "
                "({:.0f},{:.0f},{:.0f}), context={})",
                this_->hoistTargetPos.x, this_->hoistTargetPos.y,
                this_->hoistTargetPos.z, (int)this_->hoistContext);
}

// ----------------------------------------------------------------------------
// Stage 4 — ATTACK state.
//
// Engagement: when an enemy comes within kAttackEngageDist of the NPC
// AND the NPC is in IDLE or FOLLOW, transition to ATTACK and play the
// vertical sword-swing one-shot. AT collider activates during the apex
// frames (when the sword is mid-arc) and is positioned in front of NPC
// at chest height. On anim completion, transition back to FOLLOW; the
// FOLLOW handler may immediately re-engage if the enemy is still near.
//
// Damage values follow Player's basic-sword convention (1 unit = 1/2
// heart per swing). Enemies' HP / death paths are vanilla — when the
// AT lands on an enemy bumper, OoT's CollisionCheck_Damage decrements
// the enemy's colChkInfo.health like any other player swing.
// ----------------------------------------------------------------------------
static constexpr float kAttackEngageDist     = 80.0f;   // melee acquisition range (XZ)
static constexpr float kAttackBreakDist      = 200.0f;  // bail if enemy fled past this
static constexpr float kAttackQuadForward    = 60.0f;   // sword reach in front of NPC
static constexpr float kAttackQuadHalfWidth  = 25.0f;   // sword arc half-width
static constexpr float kAttackQuadBaseY      = 5.0f;    // bottom of quad above feet
static constexpr float kAttackQuadTopY       = 65.0f;   // top of quad (chest height)

// Forward decl — defined in the STANDBY section below. Used by every
// combat state's exit transition (TickATTACK / TickENGAGE / TickBLOCK
// / TickRANGED_ATTACK) to pick STANDBY (enemy still in detect range,
// keep weapon drawn) vs FOLLOW (no enemies, sheathe and follow leader).
// Must appear before any combat state's TickXxx since C++ name lookup
// for free functions is single-pass top-to-bottom.
s32 ChooseCombatExitState(EnFollower* this_, PlayState* play);

// Combat cooldown — set when any combat state exits. While the
// cooldown is active, TryEngageCombat suppresses re-engagement so the
// NPC has a beat in STANDBY before the next swing/shot. Without this,
// the kBowShoot anim (~6 frames / ~100ms) cycles RANGED_ATTACK ↔
// STANDBY every tick (log 160 wave at 02:03:44-02:03:53), producing
// rapid weapon flicker (Phase B equipment swap toggles each cycle)
// AND blocking FOLLOW since combat states lock speedXZ=0.
//
// 1500ms is long enough to see the result of the swing/shot, short
// enough to feel responsive when an enemy is genuinely in range.
static constexpr int kPostCombatCooldownMs = 1500;
static uint64_t sCombatCooldownEndFrame = 0;

// Friendly state-name lookup for log messages. Replaces hardcoded
// "IDLE" / "FOLLOW" string-literals that predated STANDBY/combat
// states (log 160 showed "FOLLOW→RANGED_ATTACK" when actual source
// was STANDBY — misleading triage).
const char* StateName(s32 s) {
    switch (s) {
        case EN_FOLLOWER_STATE_IDLE:          return "IDLE";
        case EN_FOLLOWER_STATE_FOLLOW:        return "FOLLOW";
        case EN_FOLLOWER_STATE_CLIMBING:      return "CLIMBING";
        case EN_FOLLOWER_STATE_STUCK:         return "STUCK";
        case EN_FOLLOWER_STATE_DEAD:          return "DEAD";
        case EN_FOLLOWER_STATE_SWIMMING:      return "SWIMMING";
        case EN_FOLLOWER_STATE_LEDGE_HOIST:   return "LEDGE_HOIST";
        case EN_FOLLOWER_STATE_ATTACK:        return "ATTACK";
        case EN_FOLLOWER_STATE_ENGAGE:        return "ENGAGE";
        case EN_FOLLOWER_STATE_BLOCK:         return "BLOCK";
        case EN_FOLLOWER_STATE_RANGED_ATTACK: return "RANGED_ATTACK";
        case EN_FOLLOWER_STATE_STANDBY:       return "STANDBY";
        case EN_FOLLOWER_STATE_CRAWLING:      return "CRAWLING";
        default:                              return "UNKNOWN";
    }
}

// Bow / slingshot ownership — RANGED_ATTACK gates on this. Without
// the gate, NPC fires arrows from thin air even in early-game saves
// where Player hasn't picked up a ranged weapon yet (log 160 — NPC
// firing in Kokiri Forest before the slingshot was earned).
//
// gSaveContext.inventory.items[SLOT_BOW] is the inventory slot value
// — ITEM_NONE (0xFF) when not owned, ITEM_BOW (0x03) when owned.
// Slingshot is the child equivalent at SLOT_SLINGSHOT.
bool FollowerNpcHasRangedWeapon() {
    const u8 bowSlot   = INV_CONTENT(ITEM_BOW);
    const u8 slingSlot = INV_CONTENT(ITEM_SLINGSHOT);
    return (bowSlot != ITEM_NONE) || (slingSlot != ITEM_NONE);
}
// Anim active-window — kiru anim is ~22 frames at speed 1.0; the
// blade actually contacts the target around the middle. Activate
// AT collider for frames [kAttackActiveStart .. kAttackActiveEnd]
// of the anim's curFrame.
static constexpr float kAttackActiveStartFrame = 4.0f;
static constexpr float kAttackActiveEndFrame   = 12.0f;

// File-scope ATTACK state — tracks the current target so it stays
// consistent across the swing's duration even if a closer enemy
// appears mid-swing. Reset on every entry.
struct AttackState {
    Actor*  target = nullptr;
    bool    swingFiredAT = false;  // single-shot AT register per swing
};
static AttackState sAttackState;

// Find the nearest live enemy to the NPC within `maxRange` XZ. Returns
// nullptr if no enemy in range. Walks ACTORCAT_ENEMY only (bosses are
// excluded per the project-wide bosses-opt-in rule). Cross-timeline
// not a concern here — enemies are always in the local scene.
Actor* FindNearestEnemyForAttack(EnFollower* this_, PlayState* play, float maxRange,
                                   float maxYDelta = 60.0f) {
    Actor* a = &this_->actor;
    Actor* nearest = nullptr;
    float bestDistSq = maxRange * maxRange;
    Actor* it = play->actorCtx.actorLists[ACTORCAT_ENEMY].head;
    while (it != nullptr) {
        if (it->update != nullptr && it->colChkInfo.health > 0) {
            const float dx = it->world.pos.x - a->world.pos.x;
            const float dz = it->world.pos.z - a->world.pos.z;
            const float dy = it->world.pos.y - a->world.pos.y;
            // Y filter — caller-controlled. Melee scans use the
            // tight default (±60u — Link's body height); ranged scans
            // pass a wider value (~250u) so Skullwalltulas on
            // ceilings, Keese in the air, etc. become valid targets
            // for arrow shots (log 161 — NPC ignored Skullwalltula
            // because it was above the 60u Y threshold).
            if (std::fabs(dy) > maxYDelta) {
                it = it->next;
                continue;
            }
            const float distSq = dx*dx + dz*dz;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                nearest = it;
            }
        }
        it = it->next;
    }
    return nearest;
}

// Position the AT quad as a flat plane in front of the NPC at chest
// height. Vertices: bottom-left, bottom-right, top-right, top-left
// (counter-clockwise from below). Quad faces forward along the NPC's
// rotation. Player's sword AT is more anatomical (sword tip → grip)
// but for v1 a simple forward plane is sufficient.
void PositionAttackQuad(EnFollower* this_) {
    Actor* a = &this_->actor;
    const float yawRad = (float)a->shape.rot.y * (3.14159265f / 32768.0f);
    const float fx = sinf(yawRad);  // forward unit vector
    const float fz = cosf(yawRad);
    const float rx = cosf(yawRad);  // right (perpendicular to forward)
    const float rz = -sinf(yawRad);
    const Vec3f& p = a->world.pos;
    Vec3f bottomLeft  = { p.x + fx * kAttackQuadForward - rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward - rz * kAttackQuadHalfWidth };
    Vec3f bottomRight = { p.x + fx * kAttackQuadForward + rx * kAttackQuadHalfWidth,
                          p.y + kAttackQuadBaseY,
                          p.z + fz * kAttackQuadForward + rz * kAttackQuadHalfWidth };
    Vec3f topRight    = { bottomRight.x, p.y + kAttackQuadTopY, bottomRight.z };
    Vec3f topLeft     = { bottomLeft.x,  p.y + kAttackQuadTopY, bottomLeft.z };
    Collider_SetQuadVertices(&this_->atCollider, &bottomLeft, &bottomRight,
                              &topLeft, &topRight);
}

// ATTACK handler — locks NPC in place, faces target, plays swing
// anim (set up by dispatcher's AnimForState), activates AT during
// apex frames, and transitions back to FOLLOW when the anim ends.
void TickATTACK(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)leaderPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Validate target — if it died or scene-changed (update went null)
    // mid-swing, just complete the swing harmlessly and return to FOLLOW.
    if (sAttackState.target != nullptr &&
        (sAttackState.target->update == nullptr ||
         sAttackState.target->colChkInfo.health <= 0)) {
        sAttackState.target = nullptr;
    }

    // Face the target each frame so the swing tracks a moving enemy.
    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // Active-frame AT registration. Single-shot per swing — once the
    // AT has been registered and a hit landed (or the active window
    // ended), don't re-register on the same swing. swingFiredAT
    // resets on each ATTACK entry.
    const float curFrame = this_->skelAnime.curFrame;
    const bool inActiveWindow = (curFrame >= kAttackActiveStartFrame &&
                                  curFrame <= kAttackActiveEndFrame);
    if (inActiveWindow && !sAttackState.swingFiredAT) {
        PositionAttackQuad(this_);
        CollisionCheck_SetAT(play, &play->colChkCtx, &this_->atCollider.base);
        // Don't set swingFiredAT here — keep registering the AT for
        // every frame in the active window so a passing enemy gets
        // hit. Set after the window closes so the "single-swing"
        // semantics hold even if the swing didn't connect.
    } else if (!inActiveWindow && curFrame > kAttackActiveEndFrame) {
        sAttackState.swingFiredAT = true;
    }

    // Anim complete — exit to STANDBY (if enemy still in detect range,
    // weapon stays drawn for next swing) or FOLLOW (no enemy left,
    // sheathe and resume leader-following).
    if (this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] ATTACK→{} (swing complete)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = (s32)nextState;
        // Don't null sAttackState.target — STANDBY uses it to decide
        // facing direction. ATTACK will re-acquire on next entry
        // anyway via TryEngageCombat.
        sAttackState.swingFiredAT = false;
        // Reset path so FOLLOW recomputes a path that may now route
        // around the (potentially defeated) enemy.
        sLocalNav.navState.path.Reset();
    }
}

// ----------------------------------------------------------------------------
// Stage 4 — ENGAGE state.
//
// Bridges the gap between "wandered into melee range" (ATTACK direct
// from FOLLOW) and "leave NPC alone unless an enemy comes close". An
// enemy spotted within kEngageAcquireDist (wider than the melee
// kAttackEngageDist) triggers ENGAGE: NPC walks/runs toward the enemy
// until close enough to strike, then transitions to ATTACK.
//
// Bail conditions:
//   - target died or scene-changed → FOLLOW
//   - target fled past kEngageBreakDist → FOLLOW
//   - leader got too distant (kEngageLeaderLeash) → FOLLOW (don't
//     stray far from leader chasing one enemy forever)
// ----------------------------------------------------------------------------
static constexpr float kEngageAcquireDist  = 250.0f;  // pursuit acquisition radius
static constexpr float kEngageBreakDist    = 400.0f;  // bail if target fled past this
static constexpr float kEngageBreakDistY   = 250.0f;  // bail if target Y-fled past this (jumped to ledge)
static constexpr float kEngageLeaderLeash  = 600.0f;  // bail if leader >this far away
static constexpr float kEngageLeaderLeashY = 300.0f;  // bail if leader >this far away vertically (climbed ledge)
static constexpr float kEngageStrikeDist   = 70.0f;   // close enough to ATTACK (slight hysteresis vs kAttackEngageDist=80)
static constexpr float kEngageStrikeY      = 60.0f;   // Link body height — ATTACK only when target body in vertical reach
// Fix C: grouped form. Float constants above stay accessible for log
// strings; predicate call sites use the band form.
static constexpr AnchorAI::ThresholdPair kEngageBreakBand        = { kEngageBreakDist,    kEngageBreakDistY };
static constexpr AnchorAI::ThresholdPair kEngageLeaderLeashBand  = { kEngageLeaderLeash,  kEngageLeaderLeashY };
static constexpr AnchorAI::ThresholdPair kEngageStrikeBand       = { kEngageStrikeDist,   kEngageStrikeY };
static constexpr AnchorAI::ThresholdPair kAttackEngageStrikeBand = { kAttackEngageDist,   kEngageStrikeY };  // BLOCK timer recheck
// BLOCK entry threshold — defined here (above TryEngageCombat) so the
// engagement helper can reference it. The remaining BLOCK constants
// stay grouped with the BLOCK section below.
static constexpr float kBlockHpThresholdRatio = 0.5f;  // enter BLOCK when HP <= 50% max
// RANGED_ATTACK acquisition range — defined here (above TryEngageCombat)
// for the same reason. Min dist slightly above kAttackEngageDist gives
// a clean handoff: melee → ATTACK, just-out-of-melee → RANGED.
static constexpr float kRangedMinDist     = 90.0f;   // > kAttackEngageDist (80) for handoff
static constexpr float kRangedAcquireDist = 500.0f;  // max range to enter RANGED_ATTACK

// (forward decl of ChooseCombatExitState moved above TickATTACK so
// every combat state's exit transition can call it.)

void TickENGAGE(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;

    // Validate target.
    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr ||
        sAttackState.target->colChkInfo.health <= 0) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] ENGAGE→{} (target lost/dead)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = nextState;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    const Vec3f& targetPos = sAttackState.target->world.pos;
    const float distXZ = AnchorDist::DistXZ(a->world.pos, targetPos);
    const float dyToTarget = std::fabs(targetPos.y - a->world.pos.y);

    // Leader-leash bail — don't chase enemies forever away from leader.
    // Always FOLLOW (not STANDBY) — yielding to leader is the explicit
    // intent of this exit. Phase 3 P1-F: 3D-aware via ShouldPursue3D so
    // a leader climbing to a ledge above (huge Y, small XZ) also yields
    // combat — the prior XZ-only check kept NPC fighting indefinitely
    // when leader topped a wall during a pursuit.
    if (AnchorAI::ShouldPursue3D(a->world.pos, leaderPos,
                                 kEngageLeaderLeashBand)) {
        const float leaderDistXZ =
            AnchorDist::DistXZ(a->world.pos, leaderPos);
        const float leaderDy = std::fabs(leaderPos.y - a->world.pos.y);
        SPDLOG_INFO("[FollowerNPC] ENGAGE→FOLLOW (leader too far: "
                    "XZ={:.0f}u/{:.0f}u, |dy|={:.0f}u/{:.0f}u)",
                    leaderDistXZ, kEngageLeaderLeash,
                    leaderDy, kEngageLeaderLeashY);
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    // Enemy fled past pursuit range — exit; STANDBY if any other
    // enemy is in detect range, else FOLLOW. Phase 3 P1-G: 3D-aware
    // so a target that jumped/flew to a ledge above (small XZ, huge
    // Y) also counts as "fled" — the XZ-only check kept NPC stuck
    // in ENGAGE chasing a target it could no longer reach.
    if (AnchorAI::ShouldPursue3D(a->world.pos, targetPos,
                                 kEngageBreakBand)) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] ENGAGE→{} (target fled: XZ={:.0f}u/{:.0f}u, "
                    "|dy|={:.0f}u/{:.0f}u)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"),
                    distXZ, kEngageBreakDist,
                    dyToTarget, kEngageBreakDistY);
        this_->state = nextState;
        sAttackState.target = nullptr;
        a->speedXZ = 0.0f;
        return;
    }

    // In strike range — transition to ATTACK. Target carries over
    // (sAttackState.target stays valid; ATTACK uses it directly).
    // Phase 3 P1-G: require BOTH XZ close AND vertical reach (Link's
    // sword swing covers ~60u of vertical body height). Prior
    // XZ-only check fired ATTACK when target was directly above on
    // a ledge — the swing whiffed into empty air every cycle.
    if (AnchorAI::IsInStrikeRange(a->world.pos, targetPos,
                                   kEngageStrikeBand)) {
        SPDLOG_INFO("[FollowerNPC] ENGAGE→ATTACK (strike range, "
                    "XZ={:.0f}u, |dy|={:.0f}u)",
                    distXZ, dyToTarget);
        this_->state = EN_FOLLOWER_STATE_ATTACK;
        this_->stopAnimPlaying = 0;  // let kSwordSwing override flow through
        sAttackState.swingFiredAT = false;
        a->speedXZ = 0.0f;
        return;
    }

    // ── Pursuit — substrate-aware (Phase 3, 2026-05-18) ───────────
    // Was direct-yaw (ignored nav mesh). Same fix shape as NPC Invader
    // Phase 2's TickENGAGE: route through NavOrDirect so pursuit
    // follows the same path planner FOLLOW uses. Combat distance
    // measurements (strike range, target-fled) still measure against
    // the actual target, so handoff to ATTACK / target-lost remains
    // unchanged.
    sLocalNav.navState.trailKey = AnchorNav::TrailKeyForPlayer(
        (uint8_t)Anchor::Instance->ownClientId);

    AnchorAI::FallbackPolicy policy;
    policy.isFriendlyActor = true;
    policy.hasRangedReady  = false;
    const AnchorAI::ScriptedFollowResult step =
        AnchorAI::RunScriptedFollowStep(a, targetPos, sLocalNav.navState,
                                         policy, play);
    const AnchorAI::NavOrDirectResult& nav = step.nav;

    // Climb-cell transition — same shape as TickFOLLOW. If the substrate
    // routes us through a climb cell mid-pursuit, exit ENGAGE so
    // CLIMBING can take over.
    if (step.shouldEngageClimb) {
        this_->state = EN_FOLLOWER_STATE_CLIMBING;
        sLocalNav.activeClimbAnchor = nullptr;
        // Seed climbPrev* (see TickFOLLOW for rationale).
        sLocalNav.climbPrevY  = a->world.pos.y;
        sLocalNav.climbPrevXZ = a->world.pos;
        SPDLOG_INFO("[FollowerNPC] ENGAGE→CLIMBING (path entered climb cell at "
                    "({:.0f},{:.0f},{:.0f}); flags=0x{:X})",
                    nav.subgoal.x, nav.subgoal.y, nav.subgoal.z,
                    nav.subgoalFlags);
        a->speedXZ = 0.0f;
        return;
    }

    // Drive locomotion toward chosen subgoal. Speed band is
    // target-distance-relative (not subgoal-distance) so pursuit pace
    // matches actual distance-to-target.
    a->shape.rot.y = YawTowardTarget(a->world.pos, nav.subgoal);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ = (distXZ > kRunDistance) ? kRunSpeed : kWalkSpeed;
}

// Combined engagement check — replaces the old TryEngageAttack with
// a two-tier scan:
//   1. Enemy within kAttackEngageDist (close, in melee range) → ATTACK
//      directly, skipping ENGAGE pursuit.
//   2. Enemy within kEngageAcquireDist (wider radius) → ENGAGE pursuit
//      first, transitioning to ATTACK once close enough.
//
// Called from the dispatcher pre-state-handler block. Only fires from
// IDLE / FOLLOW (engagement doesn't preempt CLIMBING / SWIMMING / etc.).
// Returns true if engaged.
bool TryEngageCombat(EnFollower* this_, PlayState* play) {
    if (FollowerNpcInvulnerable()) return false;  // gated on same toggle as damage
    if (AINavTest::IsCombatDisabled()) return false;  // Navigation Test Harness
    if (this_->state != EN_FOLLOWER_STATE_IDLE   &&
        this_->state != EN_FOLLOWER_STATE_FOLLOW &&
        this_->state != EN_FOLLOWER_STATE_STANDBY) {
        return false;
    }

    // Post-combat cooldown — after any combat state exits, suppress
    // re-engagement for kPostCombatCooldownMs. Without this, the
    // kBowShoot anim (~6 frames at 60fps) immediately re-fires
    // RANGED_ATTACK from STANDBY, producing visible weapon flicker
    // (Phase B equipment swap toggles each cycle) AND blocking
    // FOLLOW since combat states lock speedXZ=0 (log 160 wave —
    // RANGED_ATTACK ↔ STANDBY every ~133ms for 9 seconds).
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    if (curFrame < sCombatCooldownEndFrame) {
        return false;
    }

    const char* fromName = StateName(this_->state);

    // Tier 1 — enemy already in melee range. Pick BLOCK over ATTACK
    // when HP is low (defensive cycle); otherwise ATTACK.
    Actor* meleeEnemy = FindNearestEnemyForAttack(this_, play, kAttackEngageDist);
    if (meleeEnemy != nullptr) {
        const s8 maxHp = FollowerNpcMaxHealthFromLink();
        const bool lowHp =
            (maxHp > 0) &&
            ((float)this_->health / (float)maxHp <= kBlockHpThresholdRatio);
        if (lowHp) {
            SPDLOG_INFO("[FollowerNPC] {}→BLOCK (low HP {}/{}, target "
                        "enemyId=0x{:X})",
                        fromName, (int)this_->health, (int)maxHp,
                        (uint16_t)meleeEnemy->id);
            this_->state = EN_FOLLOWER_STATE_BLOCK;
            this_->stopAnimPlaying = 0;  // let kBlockWait flow through
            sAttackState.target = meleeEnemy;
            sAttackState.swingFiredAT = false;
            sLastCombatWeapon = 0;  // melee — STANDBY shows sword+shield
            return true;
        }
        SPDLOG_INFO("[FollowerNPC] {}→ATTACK (target enemyId=0x{:X} at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName, (uint16_t)meleeEnemy->id,
                    meleeEnemy->world.pos.x, meleeEnemy->world.pos.y,
                    meleeEnemy->world.pos.z);
        this_->state = EN_FOLLOWER_STATE_ATTACK;
        this_->stopAnimPlaying = 0;
        sAttackState.target = meleeEnemy;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;  // melee — STANDBY keeps sword+shield
        return true;
    }

    // Tier 2 — ENGAGE pursuit if a GROUND-level enemy is in pursuit
    // range. Tight Y filter (default 60u) so ceiling-perched enemies
    // are skipped here; they fall through to Tier 3 (ranged). This
    // makes the NPC prefer melee for ground enemies — log 163 showed
    // NPC firing arrows at Deku Babas at 499u when it could have just
    // walked up and swung. User feedback: "Why is the NPC Follower
    // not using the sword and shield and engaging dekubaba in melee
    // combat?"
    Actor* pursueEnemy = FindNearestEnemyForAttack(this_, play, kEngageAcquireDist);
    if (pursueEnemy != nullptr) {
        SPDLOG_INFO("[FollowerNPC] {}→ENGAGE (target enemyId=0x{:X} at "
                    "({:.0f},{:.0f},{:.0f}))",
                    fromName, (uint16_t)pursueEnemy->id,
                    pursueEnemy->world.pos.x, pursueEnemy->world.pos.y,
                    pursueEnemy->world.pos.z);
        this_->state = EN_FOLLOWER_STATE_ENGAGE;
        sAttackState.target = pursueEnemy;
        sAttackState.swingFiredAT = false;
        sLastCombatWeapon = 0;  // melee pursuit — STANDBY shows sword+shield
        return true;
    }

    // Tier 3 — RANGED_ATTACK as last resort. Fires only when:
    //   (a) Player owns a ranged weapon (bow / slingshot), AND
    //   (b) No ground-level enemy in pursuit range (Tier 2 fell
    //       through — only elevated/flying enemies remain).
    //
    // Wide Y filter (250u vs the 60u default) catches Skullwalltulas
    // on cave ceilings, Keese in flight, etc. — enemies that NPC
    // cannot reach by walking. Ground-level enemies in [pursuit,
    // ranged] range get pursued via FOLLOW (which re-enters
    // TryEngageCombat once close enough for ENGAGE/ATTACK).
    static constexpr float kRangedYFilter = 250.0f;
    if (FollowerNpcHasRangedWeapon()) {
        Actor* rangedEnemy = FindNearestEnemyForAttack(this_, play,
                                                        kRangedAcquireDist,
                                                        kRangedYFilter);
        if (rangedEnemy != nullptr) {
            const float dx = rangedEnemy->world.pos.x - this_->actor.world.pos.x;
            const float dz = rangedEnemy->world.pos.z - this_->actor.world.pos.z;
            const float dy = rangedEnemy->world.pos.y - this_->actor.world.pos.y;
            const float distXZ = std::sqrt(dx*dx + dz*dz);
            // Additional gate: only ranged-attack when the enemy is
            // genuinely out of melee reach. Ground enemies (|dy|<60)
            // were already handled by Tier 2's pursuit; if they
            // somehow got here (e.g., Tier 2 returned null due to a
            // race), we'd rather pursue than shoot them.
            const bool isElevated = std::fabs(dy) > 60.0f;
            if (isElevated && distXZ >= kRangedMinDist) {
                SPDLOG_INFO("[FollowerNPC] {}→RANGED_ATTACK (elevated target "
                            "enemyId=0x{:X} at ({:.0f},{:.0f},{:.0f}) "
                            "dist={:.0f} dy={:+.0f})",
                            fromName, (uint16_t)rangedEnemy->id,
                            rangedEnemy->world.pos.x, rangedEnemy->world.pos.y,
                            rangedEnemy->world.pos.z, distXZ, dy);
                this_->state = EN_FOLLOWER_STATE_RANGED_ATTACK;
                this_->stopAnimPlaying = 0;  // let kBowShoot flow through
                sAttackState.target = rangedEnemy;
                sAttackState.swingFiredAT = false;  // reused as "shot fired" flag
                sLastCombatWeapon = 1;  // ranged — STANDBY keeps bow drawn
                return true;
            }
        }
    }

    return false;
}

// ----------------------------------------------------------------------------
// Stage 4 — BLOCK state.
//
// Defensive stance — NPC raises shield, faces target, absorbs frontal
// damage. Triggered as an alternative to ATTACK when HP is low and an
// enemy is in melee range. Blocks frontal hits (|yaw_to_attacker| <
// 90° from NPC's facing) at zero HP cost; back/side hits take full
// damage. After kBlockDurationMs, NPC re-evaluates: ATTACK if enemy
// still close, FOLLOW otherwise.
//
// Block anim: kBlockWait loops while held. When a hit lands and is
// successfully blocked, kBlockHit one-shot plays for ~10 frames then
// returns to kBlockWait via the dispatcher's frame counter.
// ----------------------------------------------------------------------------
static constexpr int   kBlockDurationMs        = 2000;  // hold block ~2s
// kBlockHpThresholdRatio moved up earlier in the file (above
// TryEngageCombat) to satisfy C++ top-down name lookup. See its
// definition near the engagement-radius constants.
static constexpr u32   kBlockHitAnimFrames     = 12;    // block-hit reaction duration
static constexpr int   kBlockFrontalAngle      = 0x4000; // ±90° = ±0x4000 in s16 yaw units

// File-scope BLOCK state. Reset on every entry.
struct BlockState {
    uint64_t entryFrame      = 0;
    uint32_t hitAnimFrames   = 0;  // > 0 = play kBlockHit instead of kBlockWait
};
static BlockState sBlockState;

// Returns true if the attacker is within ±kBlockFrontalAngle of the
// NPC's facing direction (i.e., NPC has the attacker in its frontal
// cone). Used to decide if a hit is blocked or takes full damage.
bool IsFrontalAttacker(EnFollower* this_, Actor* attacker) {
    if (attacker == nullptr) return false;
    Actor* a = &this_->actor;
    const s16 yawToAttacker = YawTowardTarget(a->world.pos, attacker->world.pos);
    // s16 angle subtraction wraps naturally — the resulting delta is
    // the signed shortest angular distance. Take abs and compare.
    const s16 delta = (s16)(yawToAttacker - a->shape.rot.y);
    const int absDelta = (delta < 0) ? -(int)delta : (int)delta;
    return absDelta < kBlockFrontalAngle;
}

void TickBLOCK(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)leaderPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    // Entry-frame detection.
    if (this_->prevState != EN_FOLLOWER_STATE_BLOCK) {
        sBlockState.entryFrame    = curFrame;
        sBlockState.hitAnimFrames = 0;
        SPDLOG_INFO("[FollowerNPC] BLOCK entry — HP={} target=0x{:X}",
                    (int)this_->health,
                    (sAttackState.target ? (uint16_t)sAttackState.target->id : 0));
    }

    // Validate target (may have died or scene-changed).
    if (sAttackState.target != nullptr &&
        (sAttackState.target->update == nullptr ||
         sAttackState.target->colChkInfo.health <= 0)) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] BLOCK→{} (target lost mid-block)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = nextState;
        sAttackState.target = nullptr;
        return;
    }

    // Face target each frame so the shield always faces the threat.
    if (sAttackState.target != nullptr) {
        a->shape.rot.y = YawTowardTarget(a->world.pos, sAttackState.target->world.pos);
        a->world.rot.y = a->shape.rot.y;
    }

    // Decrement block-hit anim counter (decremented in TickBLOCK so
    // the count advances even when no new hit lands; the override
    // anim plays out then returns to kBlockWait).
    if (sBlockState.hitAnimFrames > 0) {
        sBlockState.hitAnimFrames--;
    }

    // Exit after kBlockDurationMs — re-evaluate. Phase 5 P2-H: 3D-aware
    // recheck via IsArrived3D. Without the Y gate, a target that
    // jumped/climbed to a ledge directly above during the block window
    // would still satisfy the XZ distance and trigger ATTACK — the
    // swing then whiffed into empty air. Reuse kEngageStrikeY (Link
    // body-height reach) added in Phase 3.
    const uint64_t durationTicks =
        (uint64_t)Anchor::Instance->MsToGameTicks(kBlockDurationMs);
    if (curFrame >= sBlockState.entryFrame + durationTicks) {
        // If target still alive and in melee + vertical reach, swap to ATTACK.
        if (sAttackState.target != nullptr &&
            AnchorAI::IsInStrikeRange(a->world.pos,
                                       sAttackState.target->world.pos,
                                       kAttackEngageStrikeBand)) {
            const float distXZ = AnchorDist::DistXZ(a->world.pos,
                                                     sAttackState.target->world.pos);
            const float dy = std::fabs(sAttackState.target->world.pos.y -
                                        a->world.pos.y);
            SPDLOG_INFO("[FollowerNPC] BLOCK→ATTACK (block timer expired, "
                        "target still in range XZ={:.0f}u |dy|={:.0f}u)",
                        distXZ, dy);
            this_->state = EN_FOLLOWER_STATE_ATTACK;
            this_->stopAnimPlaying = 0;
            sAttackState.swingFiredAT = false;
            return;
        }
        // Otherwise drop back to STANDBY (target out of melee but
        // still maybe nearby) or FOLLOW (no enemy detected).
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] BLOCK→{} (block timer expired)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = nextState;
        if (nextState == EN_FOLLOWER_STATE_FOLLOW) {
            sAttackState.target = nullptr;
        }
    }
}

// ----------------------------------------------------------------------------
// Stage 4 — RANGED_ATTACK state.
//
// NPC plays a one-shot bow-shoot anim and spawns an EN_ARROW projectile
// at the release frame, aimed at the current target with both yaw and
// pitch compensation (so arrows fly upward at elevated targets and
// downward at lower ones). Anim completes → exit to FOLLOW; if target
// is still in range, the next dispatcher tick re-engages.
//
// EN_ARROW is the same actor Player spawns when shooting the bow
// (z_player.c references at lines 14396 and 2703). Params=ARROW_NORMAL
// (regular wooden arrow). The arrow handles its own physics + AT
// collider; we just spawn it pointed in the right direction.
// ----------------------------------------------------------------------------
static constexpr float kRangedSpawnFrame   = 5.0f;   // anim frame at which arrow spawns
static constexpr float kRangedBreakDist    = 800.0f; // bail if target fled past this
static constexpr float kRangedSpawnHeightY = 50.0f;  // chest height (arrow leaves the bow)

void TickRANGED_ATTACK(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)leaderPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Validate target.
    if (sAttackState.target == nullptr ||
        sAttackState.target->update == nullptr ||
        sAttackState.target->colChkInfo.health <= 0) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] RANGED_ATTACK→{} (target lost mid-shoot)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = nextState;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    // Range bail — target fled past max range while we were drawing.
    const Vec3f& tp = sAttackState.target->world.pos;
    const float dx = tp.x - a->world.pos.x;
    const float dz = tp.z - a->world.pos.z;
    const float distXZ = std::sqrt(dx*dx + dz*dz);
    if (distXZ > kRangedBreakDist) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] RANGED_ATTACK→{} (target fled: {:.0f}u > {:.0f}u)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"),
                    distXZ, kRangedBreakDist);
        this_->state = nextState;
        sAttackState.target = nullptr;
        sAttackState.swingFiredAT = false;
        return;
    }

    // Face target (yaw only — body alignment for the shoot anim).
    a->shape.rot.y = YawTowardTarget(a->world.pos, tp);
    a->world.rot.y = a->shape.rot.y;

    // Spawn arrow at release frame. swingFiredAT field is reused
    // across combat states as a "single-shot per state entry" flag —
    // here it gates the arrow spawn so we fire one arrow per shoot
    // anim, even though the dispatcher may briefly hold curFrame at
    // the spawn frame across multiple ticks.
    if (!sAttackState.swingFiredAT &&
        this_->skelAnime.curFrame >= kRangedSpawnFrame) {
        // Compute pitch from XZ distance + Y delta. Math_Vec3f_Pitch
        // pattern (z_lib.c:292) uses Math_Atan2S(distXZ, deltaY) for
        // pitch; the arrow's flight code interprets shape.rot.x as
        // elevation, so positive = up. Negate to match the arrow's
        // velocity-vs-ground convention (verified empirically against
        // Player's spawn at z_player.c:14397 which uses 4000 ≈ slight
        // upward for a level shot at ~mid-range).
        const float dy = tp.y - a->world.pos.y;
        const s16 pitch = (s16)(-Math_Atan2S(distXZ, -dy));

        Vec3f spawnPos = {
            a->world.pos.x,
            a->world.pos.y + kRangedSpawnHeightY,
            a->world.pos.z,
        };

        Actor* arrow = Actor_Spawn(
            &play->actorCtx, play, ACTOR_EN_ARROW,
            spawnPos.x, spawnPos.y, spawnPos.z,
            pitch, a->shape.rot.y, 0,
            ARROW_NORMAL);
        sAttackState.swingFiredAT = true;  // single-shot
        if (arrow != nullptr) {
            SPDLOG_INFO("[FollowerNPC] RANGED_ATTACK fire — arrow→target dist={:.0f} "
                        "pitch=0x{:X} yaw=0x{:X}",
                        distXZ, (uint16_t)pitch, (uint16_t)a->shape.rot.y);
        } else {
            SPDLOG_WARN("[FollowerNPC] RANGED_ATTACK fire — Actor_Spawn(EN_ARROW) returned null");
        }
    }

    // Anim complete → exit to STANDBY (target still in detect range,
    // bow stays visible) or FOLLOW (no enemies — sheathe and resume
    // leader-following). STANDBY → RANGED_ATTACK happens automatically
    // via TryEngageCombat next tick.
    if (this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        const s32 nextState = ChooseCombatExitState(this_, play);
        SPDLOG_INFO("[FollowerNPC] RANGED_ATTACK→{} (shoot complete)",
                    (nextState == EN_FOLLOWER_STATE_STANDBY ? "STANDBY" : "FOLLOW"));
        this_->state = nextState;
        if (nextState == EN_FOLLOWER_STATE_FOLLOW) {
            sAttackState.target = nullptr;
        }
        sAttackState.swingFiredAT = false;
    }
}

// ----------------------------------------------------------------------------
// Stage 4 — STANDBY state.
//
// Alert idle between combat exchanges. NPC keeps the fighter wait pose
// (sword+shield raised) and faces the nearest enemy, ready for the
// next swing. Combat state exits transition here (instead of FOLLOW)
// when an enemy is still in detect range — this is what keeps the
// weapon "drawn" between swings instead of sheathing every time.
//
// Bail conditions:
//   - No enemy in scan range → IDLE (sheathes weapon via animType
//     dropping back to free in the dispatcher's per-tick assign)
//   - Leader > kEnterFollow → FOLLOW (yields to leader-following;
//     re-engages from FOLLOW if enemy still detected via TryEngageCombat)
// ----------------------------------------------------------------------------
static constexpr float kStandbyDetectDist = 600.0f;  // >= max(kRangedAcquireDist=500, kEngageAcquireDist=250)

void TickSTANDBY(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    // Face nearest enemy if known. Falls back to facing leader so the
    // alert pose has a sensible orientation when target is null.
    Actor* faceTarget = sAttackState.target;
    if (faceTarget == nullptr || faceTarget->update == nullptr ||
        faceTarget->colChkInfo.health <= 0) {
        faceTarget = FindNearestEnemyForAttack(this_, play, kStandbyDetectDist);
        if (faceTarget != nullptr) {
            sAttackState.target = faceTarget;  // refresh target tracking
        }
    }
    a->shape.rot.y = (faceTarget != nullptr)
                       ? YawTowardTarget(a->world.pos, faceTarget->world.pos)
                       : YawTowardTarget(a->world.pos, leaderPos);
    a->world.rot.y = a->shape.rot.y;

    // No enemy in detect range — drop back to IDLE / FOLLOW so the
    // dispatcher's animType=0 logic fires next tick (weapon sheathes
    // visually via the free-variant idle anim). Phase 3 P1-E: 3D-aware
    // — if leader is meaningfully above/below (ledge), → FOLLOW so the
    // navigator can engage CLIMBING / hoist / drop subgoals instead of
    // dropping to IDLE under the leader's feet.
    if (faceTarget == nullptr) {
        sAttackState.target = nullptr;
        if (AnchorAI::ShouldPursue3D(a->world.pos, leaderPos,
                                     kEnterFollowBand)) {
            SPDLOG_INFO("[FollowerNPC] STANDBY→FOLLOW (no enemies, leader far)");
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
        } else {
            SPDLOG_INFO("[FollowerNPC] STANDBY→IDLE (no enemies, near leader)");
            this_->state = EN_FOLLOWER_STATE_IDLE;
        }
        return;
    }

    // Leader yields combat — STANDBY won't chase the leader (combat
    // states lock speedXZ=0), but FOLLOW will. Hand off to FOLLOW once
    // leader is moderately far — kEnterFollow=80u was the threshold
    // for IDLE→FOLLOW, and using the same value here gives consistent
    // "follow leader" behaviour. Without this fix (log 160 — leash
    // was 600u), the NPC stayed locked in STANDBY/RANGED_ATTACK
    // cycle even as the leader walked across Hyrule Field. The
    // combat cooldown gives FOLLOW a beat to make progress before
    // TryEngageCombat re-engages. Phase 3 P1-E: 3D-aware so a leader
    // climbing to a ledge above (small XZ, huge Y) also triggers
    // STANDBY→FOLLOW handoff.
    if (AnchorAI::ShouldPursue3D(a->world.pos, leaderPos,
                                 kEnterFollowBand)) {
        const float leaderDistXZ = AnchorDist::DistXZ(a->world.pos, leaderPos);
        const float leaderDy     = std::fabs(leaderPos.y - a->world.pos.y);
        SPDLOG_INFO("[FollowerNPC] STANDBY→FOLLOW (leader beyond hysteresis "
                    "XZ={:.0f}u/{:.0f}u, |dy|={:.0f}u/{:.0f}u)",
                    leaderDistXZ, kEnterFollow,
                    leaderDy, kEnterFollowY);
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
    }

    // Otherwise stay in STANDBY. TryEngageCombat (called pre-dispatch)
    // will swap to ATTACK / BLOCK / ENGAGE / RANGED_ATTACK whenever
    // the enemy is in any combat tier.
}

// Helper used by combat state EXIT transitions — pick STANDBY if any
// enemy is still in detect range (keeps weapon drawn for the next
// engagement), otherwise FOLLOW (sheathe and resume leader-following).
//
// Side effect: arms the post-combat cooldown so TryEngageCombat
// suppresses re-engagement for kPostCombatCooldownMs. Without this,
// short-anim combat states (kBowShoot ~6 frames) re-fire instantly
// from STANDBY, producing weapon flicker + blocking FOLLOW.
s32 ChooseCombatExitState(EnFollower* this_, PlayState* play) {
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    sCombatCooldownEndFrame = curFrame +
        (uint64_t)Anchor::Instance->MsToGameTicks(kPostCombatCooldownMs);
    // Time-based equipment retention — mark this combat exit so the
    // sheathe-delay window starts now. NpcStateToModelGroup uses
    // this to keep the weapon visible across STANDBY ↔ FOLLOW ↔
    // combat re-entry cycles, mirroring Player's vanilla "stay armed
    // for N seconds after combat" behavior.
    sLastCombatExitFrame = curFrame;
    Actor* nearby = FindNearestEnemyForAttack(this_, play, kStandbyDetectDist);
    return (nearby != nullptr) ? EN_FOLLOWER_STATE_STANDBY
                               : EN_FOLLOWER_STATE_FOLLOW;
}

// ----------------------------------------------------------------------------
// Stage 5 — CRAWLING state.
//
// Child-Link-only traversal of crawlspaces (the Player_TryEnteringCrawlspace
// equivalent at z_player.c:7630). RoomNavData catalogues crawlspaces as
// CrawlspaceAnchor entries — entryPos + entryNormal (wall normal pointing
// OUT of the crawlspace toward the entry side). A typical crawlspace
// has TWO anchors, one at each end with opposite normals.
//
// Traversal:
//   1. Detection — if leader is on the far side of an anchor's wall
//      plane AND NPC is on the entry-facing side within entry range,
//      enter CRAWLING.
//   2. Snap NPC to entryPos + face into the wall (-entryNormal).
//   3. Move forward at slow constant speed (matches Player's
//      sControlInput->rel.stick_y * 0.03f scale; ~3.81 max).
//   4. Loop kCrawlMove (gPlayerAnim_link_child_tunnel_start) the
//      whole time.
//   5. Exit when NPC crosses past the wall plane to the leader's side
//      OR after kCrawlMaxDistance traveled (safety cap).
//   6. Play kCrawlExit one-shot, transition to FOLLOW.
//
// AI Player Follower handles this trivially — Player's
// vanilla code detects crawlspace entry on BTN_A, sets
// PLAYER_STATE2_CRAWLING, runs the camera-locked traversal. AI
// Follower just injects stick_y=127 (Follower.cpp:2273). NPC has
// no Player code path; we replicate the traversal here.
//
// linkAge gate — only child Link can crawl (z_player.c:7639's
// !LINK_IS_ADULT). NPC's linkAge field is set at spawn from
// gSaveContext.linkAge; gate on linkAge == LINK_AGE_CHILD (1).
// ----------------------------------------------------------------------------
static constexpr float kCrawlSpeed         = 3.5f;   // matches Player's stick_y*0.03 max ≈ 3.81
static constexpr float kCrawlEntryRadius   = 150.0f; // NPC must be within this XZ of entryPos to enter
static constexpr float kCrawlMinCrossDist  = 20.0f;  // both NPC + leader must be this far from wall plane
// kCrawlExitMargin removed — Approach A's signed-distance exit test
// was replaced by Approach B (wall-collision-based) in TickCRAWLING.
// If switching back to Approach A, restore this constant and use it
// in the per-tick currentSide check.
static constexpr float kCrawlMaxDistance   = 400.0f; // safety cap on traversal length

struct CrawlState {
    const ::AnchorNavRoom::CrawlspaceAnchor* anchor = nullptr;
    Vec3f forwardDir = { 0, 0, 0 };  // -entryNormal direction (into the wall)
    Vec3f entryPos   = { 0, 0, 0 };  // captured at entry for max-distance bail
    bool  exitAnimPlaying = false;   // true once we've switched to kCrawlExit
};
static CrawlState sCrawlState;

// Find the crawlspace anchor that separates NPC from leader. Returns
// nullptr if no eligible anchor exists.
const ::AnchorNavRoom::CrawlspaceAnchor* FindCrawlspaceForCrossing(
    const ::AnchorNavRoom::RoomNavData* navData,
    const Vec3f& npcPos,
    const Vec3f& leaderPos)
{
    if (navData == nullptr || navData->crawlspaceAnchors.empty()) return nullptr;
    const ::AnchorNavRoom::CrawlspaceAnchor* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();
    for (const auto& a : navData->crawlspaceAnchors) {
        // Signed distance to wall plane (positive = entry side, OUT of wall).
        const float npcSide =
            (npcPos.x - a.entryPos.x) * a.entryNormal.x +
            (npcPos.z - a.entryPos.z) * a.entryNormal.z;
        const float leaderSide =
            (leaderPos.x - a.entryPos.x) * a.entryNormal.x +
            (leaderPos.z - a.entryPos.z) * a.entryNormal.z;
        // NPC must be on entry-facing side (positive); leader on far side (negative).
        if (npcSide < kCrawlMinCrossDist || leaderSide > -kCrawlMinCrossDist) {
            continue;
        }
        // Y filter — crawlspaces are roughly at NPC's altitude.
        if (std::fabs(a.entryPos.y - npcPos.y) > 60.0f) continue;
        // NPC must be within entry radius of entryPos XZ.
        const float dx = npcPos.x - a.entryPos.x;
        const float dz = npcPos.z - a.entryPos.z;
        const float distSq = dx*dx + dz*dz;
        if (distSq > kCrawlEntryRadius * kCrawlEntryRadius) continue;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = &a;
        }
    }
    return best;
}

void TickCRAWLING(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)leaderPos;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;  // we drive position directly, not via Actor_MoveXZGravity

    if (sCrawlState.anchor == nullptr) {
        // Defensive — anchor pointer lost (scene change?). Bail.
        SPDLOG_WARN("[FollowerNPC] CRAWLING tick with null anchor — exiting to FOLLOW");
        this_->state = EN_FOLLOWER_STATE_FOLLOW;
        return;
    }

    // Once exit anim is playing, hold position and wait for anim
    // completion — then transition to FOLLOW. Drop Y by kCrawlExitYDrop
    // for the duration of the exit anim: gPlayerAnim_link_child_tunnel_end
    // is authored so the body's pivot starts at standing height while the
    // model renders crouched — without the drop, the NPC visually floats
    // above the crawlspace floor for the length of the exit anim.
    // (Tuned 20u → 25u per field test — slightly more drop needed.)
    if (sCrawlState.exitAnimPlaying) {
        constexpr float kCrawlExitYDrop = 25.0f;
        a->world.pos.y = sCrawlState.entryPos.y - kCrawlExitYDrop;
        if (this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
            SPDLOG_INFO("[FollowerNPC] CRAWLING→FOLLOW (exit anim complete at "
                        "({:.0f},{:.0f},{:.0f}))",
                        a->world.pos.x, a->world.pos.y, a->world.pos.z);
            this_->state = EN_FOLLOWER_STATE_FOLLOW;
            sCrawlState.anchor = nullptr;
            sCrawlState.exitAnimPlaying = false;
        }
        return;
    }

    // Traversal — move forward at constant speed.
    a->world.pos.x += sCrawlState.forwardDir.x * kCrawlSpeed;
    a->world.pos.z += sCrawlState.forwardDir.z * kCrawlSpeed;
    // Y stays at entryPos.y (crawlspaces are flat tunnels — vanilla
    // Player's crawlspace code doesn't apply gravity either).
    a->world.pos.y = sCrawlState.entryPos.y;

    const float dx = a->world.pos.x - sCrawlState.entryPos.x;
    const float dz = a->world.pos.z - sCrawlState.entryPos.z;
    const float traveled = std::sqrt(dx*dx + dz*dz);

    // ------------------------------------------------------------------
    // Approach B — wall-collision-based exit detection.
    //
    // Mirrors Player's Player_TryLeavingCrawlspace at z_player.c:7763.
    // OoT crawlspaces have FOUR walls with the special flag: two
    // ENTRANCE walls (outside-facing, one at each end) and two EXIT
    // walls (inside-facing, deeper into the tunnel than the entrance
    // walls). Player exits when its forward motion bumps it into one
    // of the interior exit walls (z_player.c:7766 — bgCheckFlags & 8
    // AND touched-wall-flags & 0x30). We replicate the same check.
    //
    // We DO need to wait before arming the exit check, because we
    // snap to entryPos at CRAWLING entry — entryPos sits AT or near
    // the entry wall, so the first BG check would touch the entry
    // wall (which also has flag 0x30) and trigger exit immediately.
    // Player avoids this implicitly because Player_TryEnteringCrawlspace
    // (z_player.c:7690-7691) snaps Player past the wall and the entry
    // anim moves them away before their own crawl action func starts
    // testing for exit. Our snap is simpler — manual buffer is needed.
    //
    // ------------------------------------------------------------------
    // Alternative — Approach A (paired-anchor detection, deferred):
    //
    // An OoT crawlspace has TWO CrawlspaceAnchor entries (one at each
    // end with opposite-facing entryNormals). At CRAWLING entry, scan
    // all crawlspaceAnchors for the pair: dot product of normals < -0.7
    // (roughly antiparallel) AND the candidate lies along this
    // anchor's -entryNormal direction. Cache the paired anchor's
    // entryPos as sCrawlState.exitPos. In TickCRAWLING, exit when
    // NPC's XZ distance to exitPos < 30u.
    //
    // Pros: pure math, no collision queries, no entry-wall confusion.
    // Cons: relies on substrate detection cataloguing BOTH ends of
    // every crawlspace; for crawlspaces with only one detected
    // anchor, only the safety cap below would fire.
    //
    // To switch from B → A: (1) at TryEnterCrawling, scan
    // navData->crawlspaceAnchors for the pair and cache
    // sCrawlState.exitPos. (2) Replace the BG-check block below
    // with a distance-to-exitPos check. (3) Remove the
    // Actor_UpdateBgCheckInfo call.
    // ------------------------------------------------------------------

    // Distance buffer — don't run the wall-flag check until we've
    // moved past the entry wall (else we'd trigger exit on the entry
    // wall, which also has the 0x30 flag). 70u ≈ enough clearance
    // for typical crawlspace wall thicknesses + safety margin.
    constexpr float kCrawlExitArmDistance = 70.0f;

    // Safety cap always fires regardless of BG check.
    if (traveled > kCrawlMaxDistance) {
        SPDLOG_INFO("[FollowerNPC] CRAWLING — exit triggered (safety cap, "
                    "traveled {:.0f}u > {:.0f}u)",
                    traveled, kCrawlMaxDistance);
        sCrawlState.exitAnimPlaying = true;
        this_->stopAnimPlaying = 0;
        return;
    }

    if (traveled < kCrawlExitArmDistance) {
        return;  // too close to entry wall — skip exit detection
    }

    // Run BG check at crawl-body height: small wall-radius (NPC is in
    // a narrow tunnel), low wallCheckHeight (~20u — NPC is crouched).
    //
    // CRITICAL: Actor_UpdateBgCheckInfo flag bits (z_actor.c:1692-1723):
    //   bit 0 (1) = wall check          ← WE NEED THIS
    //   bit 1 (2) = ceiling check
    //   bit 2 (4) = floor + water check
    // The standard NPC dispatcher uses flags=4 (floor only). For
    // crawlspace exit detection we need bit 0 — without it, wall_check
    // never runs and bgCheckFlags & 8 stays clear forever (log 194 —
    // 11s of CRAWLING ended via the 400u safety cap, never via a
    // wall hit). Pass flags=1 here for wall-only check.
    //
    // Side effect: when wall check detects a wall, line 1700 copies
    // the wall-slide-corrected pos into world.pos. That would push
    // our NPC back from the wall and prevent forward progress. We
    // snapshot pos before the call and restore after — we own
    // position via direct write, BG check is read-only for us.
    const Vec3f preBgPos = a->world.pos;
    Actor_UpdateBgCheckInfo(play, a, 20.0f /* wallCheckHeight */,
                            15.0f /* wallCheckRadius */,
                            30.0f /* ceilingCheckHeight */,
                            1 /* flags = wall check only */);
    const bool wallTouched = (a->bgCheckFlags & 8) != 0 && a->wallPoly != nullptr;
    CollisionPoly* touchedPoly = a->wallPoly;
    s32 touchedBgId = a->wallBgId;
    a->world.pos = preBgPos;  // restore — keep our forward motion uncorrupted

    if (wallTouched) {
        // func_80041DB8 returns the surface-type wall-flags bitmask
        // for the touched polygon. 0x30 = crawlspace bits (matches
        // Player's interactWallFlags & 0x30 check at z_player.c:7639
        // for entry and z_player.c:7766 for exit).
        const s32 wallFlags = func_80041DB8(&play->colCtx, touchedPoly, touchedBgId);
        if ((wallFlags & 0x30) != 0) {
            SPDLOG_INFO("[FollowerNPC] CRAWLING — exit wall hit "
                        "(flags=0x{:X}, traveled {:.0f}u) — switching to "
                        "kCrawlExit anim",
                        (unsigned)wallFlags, traveled);
            sCrawlState.exitAnimPlaying = true;
            this_->stopAnimPlaying = 0;  // let kCrawlExit override flow through
        }
    }
}

// Try to enter CRAWLING from IDLE / FOLLOW. Returns true if entered.
// Called from the dispatcher pre-state-handler block.
bool TryEnterCrawling(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    if (this_->state != EN_FOLLOWER_STATE_IDLE &&
        this_->state != EN_FOLLOWER_STATE_FOLLOW) {
        return false;
    }
    // Child-Link only — Player's Player_TryEnteringCrawlspace gates
    // on !LINK_IS_ADULT (z_player.c:7639). Adult Link is too tall.
    if (this_->linkAge != LINK_AGE_CHILD) return false;

    const ::AnchorNavRoom::RoomNavData* navData =
        ::AnchorNavRoom::GetForRoom(
            play->sceneNum,
            (int8_t)play->roomCtx.curRoom.num);
    const auto* anchor = FindCrawlspaceForCrossing(navData,
                                                     this_->actor.world.pos,
                                                     leaderPos);
    if (anchor == nullptr) return false;

    // Enter — snap to entryPos + face -entryNormal (into the wall).
    Actor* a = &this_->actor;
    a->world.pos.x = anchor->entryPos.x;
    a->world.pos.z = anchor->entryPos.z;
    a->world.pos.y = anchor->entryPos.y;
    a->shape.rot.y = Math_Atan2S(-anchor->entryNormal.z, -anchor->entryNormal.x);
    a->world.rot.y = a->shape.rot.y;
    a->speedXZ     = 0.0f;
    a->velocity.y  = 0.0f;

    sCrawlState.anchor    = anchor;
    sCrawlState.entryPos  = a->world.pos;
    sCrawlState.forwardDir = {
        -anchor->entryNormal.x, 0.0f, -anchor->entryNormal.z
    };
    sCrawlState.exitAnimPlaying = false;
    this_->state = EN_FOLLOWER_STATE_CRAWLING;
    this_->stopAnimPlaying = 0;  // let kCrawlMove flow through dispatcher hold

    // Diagnostic — confirms the anim setup will fire on the next
    // dispatcher anim-resolution pass. If the user reports walk
    // anim during CRAWLING, this log establishes whether the
    // transition was attempted (vs. some earlier override blocking
    // it).
    SPDLOG_INFO("[FollowerNPC.crawl] anim setup — currentAnim={} → "
                "want=kCrawlMove (one-shot); currentAnimType={} stopAnimPlaying={}",
                (int)this_->currentAnim, (int)this_->currentAnimType,
                (int)this_->stopAnimPlaying);

    SPDLOG_INFO("[FollowerNPC] {}→CRAWLING (entry=({:.0f},{:.0f},{:.0f}) "
                "normal=({:.2f},{:.2f}))",
                StateName(this_->prevState),
                anchor->entryPos.x, anchor->entryPos.y, anchor->entryPos.z,
                anchor->entryNormal.x, anchor->entryNormal.z);
    return true;
}

// DEAD handler — Stage 2.
// Locks XZ motion, lets gravity settle the body, holds the death anim
// (kDeath = gPlayerAnim_link_normal_back_downA) until kFollowerNpcDeathHoldMs
// elapses, then despawns the local actor + arms the respawn cooldown.
// Per user spec: NPC death is purely cosmetic — no system-wide effects,
// no notifications. Respawn happens via TickFollowerNpcCVar's auto-
// respawn branch once the cooldown elapses.
//
// File-scope sDeathEntryFrame tracks when the current DEAD state was
// entered. Reset on any non-DEAD state via dispatcher prevState edge
// (handled inline below).
static uint64_t sDeathEntryFrame = 0;

void TickDEAD(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    (void)leaderPos;
    (void)play;
    Actor* a = &this_->actor;
    a->speedXZ = 0.0f;

    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);

    // Entry-frame detection via prevState edge. The dispatcher updates
    // prevState AFTER state handlers run, so on the entry frame
    // prevState != DEAD while state == DEAD.
    if (this_->prevState != EN_FOLLOWER_STATE_DEAD) {
        sDeathEntryFrame = curFrame;
        SPDLOG_INFO("[FollowerNPC] DEAD entry — anim=kDeath at "
                    "({:.0f},{:.0f},{:.0f}) deathHoldMs={}",
                    a->world.pos.x, a->world.pos.y, a->world.pos.z,
                    kFollowerNpcDeathHoldMs);
    }

    // Let gravity / floor collision settle the body so it lies on the
    // ground naturally instead of floating. No XZ motion (speedXZ=0
    // above; Actor_MoveXZGravity is called by the dispatcher's outer
    // tick after state dispatch).

    // Despawn after the hold elapses.
    const uint64_t holdTicks = (uint64_t)Anchor::Instance->MsToGameTicks(
                                   kFollowerNpcDeathHoldMs);
    if (curFrame >= sDeathEntryFrame + holdTicks) {
        SPDLOG_INFO("[FollowerNPC] DEAD hold complete — despawning + arming "
                    "respawn cooldown ({} ms)", kFollowerNpcRespawnCooldownMs);
        Anchor::Instance->mFollowerNpcRespawnAtFrame =
            curFrame + (uint64_t)Anchor::Instance->MsToGameTicks(
                            kFollowerNpcRespawnCooldownMs);
        Anchor::Instance->SetFollowerNpcActive(false);
    }
}

// Stage 2 — death triggers (drowning, void). Called from the dispatcher
// each tick BEFORE state-handler dispatch. If a hazard is detected
// AND health > 0 AND not invulnerable, sets state=DEAD + health=0 +
// deathFlag=1 so the next tick TickDEAD takes over. Returns true if
// death was just triggered (caller may choose to skip remaining work
// this tick).
bool CheckEnvironmentalDeath(EnFollower* this_, PlayState* play) {
    Actor* a = &this_->actor;

    // Already dead? Nothing to do.
    if (this_->state == EN_FOLLOWER_STATE_DEAD) return false;
    if (FollowerNpcInvulnerable())            return false;

    // Void death — Y below kFollowerNpcVoidThresholdY. Instant kill;
    // no anim hold needed because the body is past the camera anyway.
    if (a->world.pos.y < kFollowerNpcVoidThresholdY) {
        SPDLOG_INFO("[FollowerNPC] death trigger: void (y={:.0f} < {:.0f})",
                    a->world.pos.y, kFollowerNpcVoidThresholdY);
        this_->state           = EN_FOLLOWER_STATE_DEAD;
        this_->health          = 0;
        this_->deathFlag       = 1;
        this_->stopAnimPlaying = 0;  // let kDeath flow through dispatcher hold
        return true;
    }

    // Drowning — NPC has been in SWIMMING state for kFollowerNpcDrowningMs.
    // Tracked on the actor via `idleTicks` which is repurposed here as
    // a swim-entry-frame counter. Reset on any non-SWIMMING state, set
    // on first SWIMMING tick. We only count actual SWIMMING (not
    // shallow/treading-water FOLLOW) — yDistToWater gating already
    // handled the entry transition.
    static uint64_t sSwimStartFrame = 0;
    static bool     sWasSwimming    = false;
    const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                  std::memory_order_relaxed);
    if (this_->state == EN_FOLLOWER_STATE_SWIMMING) {
        if (!sWasSwimming) {
            sSwimStartFrame = curFrame;
            sWasSwimming    = true;
        } else {
            const uint64_t drownTicks =
                (uint64_t)Anchor::Instance->MsToGameTicks(kFollowerNpcDrowningMs);
            if (curFrame >= sSwimStartFrame + drownTicks) {
                SPDLOG_INFO("[FollowerNPC] death trigger: drowning "
                            "(swim duration {}ms exceeded)",
                            kFollowerNpcDrowningMs);
                this_->state           = EN_FOLLOWER_STATE_DEAD;
                this_->health          = 0;
                this_->deathFlag       = 1;
                this_->deathCause      = 1;  // drowning → kDeathDrown
                this_->stopAnimPlaying = 0;  // let kDeath flow through dispatcher hold
                sWasSwimming           = false;  // consumed
                (void)play;
                return true;
            }
        }
    } else {
        sWasSwimming = false;
    }

    return false;
}

// Phase 8 — G10/G14 recovery teleport. Direct world.pos write to
// `dest`, reset path + stuck baselines + G-guard counters, force
// FOLLOW state, snap floor altitude. Caller logs the trigger.
void TeleportNpcTo(EnFollower* this_, PlayState* play, const Vec3f& dest) {
    Actor* a = &this_->actor;
    a->world.pos = dest;
    a->speedXZ   = 0.0f;
    // Reset all nav-state baselines so we don't immediately re-fire a
    // G-guard or STUCK detection at the new position.
    sLocalNav.navState.path.Reset();
    sLocalNav.navState.lastPathRefreshFrame = 0;
    sLocalNav.navState.lastPathTargetPos    = { 0.0f, 0.0f, 0.0f };
    sLocalNav.stuckCheckPos        = dest;
    sLocalNav.lastStuckCheckFrame  = Anchor::Instance->gameFrameCounter.load(
                                          std::memory_order_relaxed);
    sLocalNav.leashFrames          = 0;
    sLocalNav.closeFailFrames      = 0;
    sLocalNav.closeFailBaseline    = 0.0f;
    sLocalNav.activeClimbAnchor    = nullptr;
    this_->state                   = EN_FOLLOWER_STATE_FOLLOW;
    // Snap Y to floor at the new position so we don't sink / float.
    Actor_UpdateBgCheckInfo(play, a, 26.0f, 10.0f, 50.0f, 4);
}

// Phase 8 — G10 leash safety net. NPC > kNpcLeashDistance from leader
// for > kNpcLeashTimeoutMs of consecutive ticks → teleport to leader
// pos. Catches "NPC stuck behind closed door / left in another room /
// fell into untracked geometry where the substrate can't recover."
//
// 3D distance (not XZ) — vertical separation also counts; an NPC
// stuck at the bottom of a pit beneath the leader should leash.
//
// Returns true if a teleport fired (caller should skip the rest of
// the tick — the state machine is in fresh-FOLLOW with no baselines).
bool TryFireG10(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    const float dx = a->world.pos.x - leaderPos.x;
    const float dy = a->world.pos.y - leaderPos.y;
    const float dz = a->world.pos.z - leaderPos.z;
    const float dist3D = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist3D <= kNpcLeashDistance) {
        sLocalNav.leashFrames = 0;
        return false;
    }
    sLocalNav.leashFrames++;
    const int timeoutTicks = Anchor::Instance->MsToGameTicks(kNpcLeashTimeoutMs);
    if (timeoutTicks <= 0 || (int)sLocalNav.leashFrames < timeoutTicks) {
        return false;
    }
    SPDLOG_INFO("[FollowerNPC] G10 leash teleport — dist3D={:.0f}u for {} frames "
                "(>{}u for >{}ms) → snap to leader",
                dist3D, sLocalNav.leashFrames,
                (int)kNpcLeashDistance, kNpcLeashTimeoutMs);
    TeleportNpcTo(this_, play, leaderPos);
    return true;
}

// Phase 8 — G14 close-fail safety net. NPC in the close-fail distance
// band (200-1200u, i.e. close enough that G10 won't fire) but making
// < kNpcCloseFailProgressDelta progress across kNpcCloseFailTimeoutMs
// → teleport to current substrate subgoal (or leader if no path).
// Catches "NPC stuck in tight geometry between rooms / pressed into
// wall the substrate can't see past."
//
// Progress metric: baseline = distance-to-leader at window entry;
// progress = baseline - current_distance. Reset counter whenever
// progress > kNpcCloseFailProgressDelta (NPC made real headway).
bool TryFireG14(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    const float dx = a->world.pos.x - leaderPos.x;
    const float dy = a->world.pos.y - leaderPos.y;
    const float dz = a->world.pos.z - leaderPos.z;
    const float dist3D = std::sqrt(dx*dx + dy*dy + dz*dz);

    // Outside the band — reset counter, defer to G10 (or normal AI).
    if (dist3D < kNpcCloseFailMinDistance || dist3D > kNpcCloseFailMaxDistance) {
        sLocalNav.closeFailFrames   = 0;
        sLocalNav.closeFailBaseline = 0.0f;
        return false;
    }

    // First tick inside the band → seed baseline.
    if (sLocalNav.closeFailFrames == 0) {
        sLocalNav.closeFailBaseline = dist3D;
        sLocalNav.closeFailFrames   = 1;
        return false;
    }

    // Made meaningful headway → reset window (baseline is updated to
    // current dist so we measure progress from here forward).
    const float progress = sLocalNav.closeFailBaseline - dist3D;
    if (progress > kNpcCloseFailProgressDelta) {
        sLocalNav.closeFailBaseline = dist3D;
        sLocalNav.closeFailFrames   = 1;
        return false;
    }

    sLocalNav.closeFailFrames++;
    const int timeoutTicks = Anchor::Instance->MsToGameTicks(kNpcCloseFailTimeoutMs);
    if (timeoutTicks <= 0 || (int)sLocalNav.closeFailFrames < timeoutTicks) {
        return false;
    }

    // Fire — teleport target is current substrate subgoal if we have
    // one (preserves substrate intent), else leader pos (G10-style
    // fallback).
    Vec3f dest = leaderPos;
    if (!sLocalNav.navState.path.Empty()) {
        dest = sLocalNav.navState.path.CurrentSubgoal();
    }

    // Y-delta gate (2026-05-20, log 69 fix). If the destination is the
    // leader-pos fallback (no path) AND the leader is significantly
    // higher than the NPC, suppress the teleport. NPC at Y=360 on a
    // mid-wall platform with leader at Y=800 atop the next wall above
    // would otherwise jump straight up the wall, bypassing the
    // legitimate CLIMBING engagement. Reset the close-fail window
    // so the next FOLLOW tick re-plans via substrate (which should
    // include climb waypoints from NPC's current floor up to leader's).
    //
    // Mirrors the AI Player Follower TeleportToLeader same-room Y gate
    // (Follower.cpp:1099). Threshold 100u matches.
    //
    // Substrate-subgoal case is NOT gated because the path planner
    // already routed through whatever waypoints are needed; if the
    // current subgoal is a CLIMB cell at higher Y, teleport-to-cell +
    // CLIMBING engagement is the intended outcome (same as Player AI
    // Follower TeleportToNextSubgoal's CLIMB branch).
    constexpr f32 kG14LeaderYAbove = 100.0f;
    const bool destIsLeaderPos = sLocalNav.navState.path.Empty();
    const f32  actorY          = a->world.pos.y;
    if (destIsLeaderPos && dest.y > actorY + kG14LeaderYAbove) {
        SPDLOG_INFO("[FollowerNPC] G14 close-fail SUPPRESSED — dest is leader pos "
                    "at Y={:.0f} but actor Y={:.0f} (delta={:.0f} > {:.0f}); "
                    "next FOLLOW tick re-plans via substrate",
                    dest.y, actorY, dest.y - actorY, kG14LeaderYAbove);
        sLocalNav.closeFailBaseline = 0.0f;
        sLocalNav.closeFailFrames   = 0;
        return false;
    }

    SPDLOG_INFO("[FollowerNPC] G14 close-fail teleport — dist3D={:.0f}u, "
                "progress={:.1f}u over {} frames (<{}u in {}ms) → snap to "
                "({:.0f},{:.0f},{:.0f}) [{}]",
                dist3D, progress, sLocalNav.closeFailFrames,
                (int)kNpcCloseFailProgressDelta, kNpcCloseFailTimeoutMs,
                dest.x, dest.y, dest.z,
                sLocalNav.navState.path.Empty() ? "leader pos (no path)" : "substrate subgoal");
    TeleportNpcTo(this_, play, dest);
    return true;
}

// STUCK handler — single-tick world.pos nudge toward leader, then
// return to FOLLOW. The substrate path was just reset by the FOLLOW
// caller; the next FOLLOW tick will recompute. Combined effect:
// "stuck → nudge forward 30u → recompute path → continue."
//
// STUCK escalation tiers (ported from AI Player Follower's G12):
//   Cycle 1: legacy nudge toward leader + path reset
//   Cycle 2: edge-triggered cursor advance (skip stuck subgoal) +
//            legacy nudge; path NOT reset so cursor advance persists.
//   Cycle 3+: teleport to next subgoal (TeleportNpcTo to
//            navState.path.CurrentSubgoal()) OR leader as fallback.
//
// State lives on sLocalNav.stuckCycle (AnchorAI::StuckCycleState).
// FOLLOW's stuck-detect calls NoteStuckEntered to increment + arm
// the reset window. The dispatcher's TickStuckCycleWindow decays it
// so unrelated stuck episodes don't accumulate into escalation.
// Teleport callback for the shared StuckRecovery dispatch — invoked
// for cycle 3+ (or vertical-dominant cycle 2+) escalation. Wraps
// TeleportNpcTo which handles cross-scene work.
static void StuckTeleportCallback(void* user, Actor* actor, PlayState* play,
                                   const Vec3f& dest, const char* reason) {
    (void)reason;
    (void)actor;
    EnFollower* this_ = static_cast<EnFollower*>(user);
    TeleportNpcTo(this_, play, dest);
}

void TickSTUCK(EnFollower* this_, PlayState* play, const Vec3f& leaderPos) {
    Actor* a = &this_->actor;
    AnchorAI::StuckRecoveryConfig cfg = {
        sLocalNav.stuckCycle,
        sLocalNav.navState,
        sLocalNav.stuckCheckPos,
        sLocalNav.lastStuckCheckFrame,
        Anchor::Instance->gameFrameCounter.load(std::memory_order_relaxed),
        kStuckCycleEscalation,
        kStuckNudgeDist,
        "FollowerNPC",
        &StuckTeleportCallback,
        static_cast<void*>(this_),
    };
    const bool teleported =
        AnchorAI::RunStuckRecoveryStep(a, leaderPos, play, cfg);
    if (teleported) return;
    this_->state = EN_FOLLOWER_STATE_FOLLOW;
}

}  // namespace

// ----------------------------------------------------------------------------
// extern "C" tick called from EnFollower_Update (z_en_follower.c).
// ----------------------------------------------------------------------------
extern "C" void Anchor_TickFollowerNpcActor(Actor* npc, PlayState* play) {
    if (npc == nullptr || play == nullptr) return;
    EnFollower* this_ = (EnFollower*)npc;

    // Peer replicas have their pos applied each frame from
    // FOLLOWER_NPC_STATE packets; they don't run AI. Skip the state
    // machine entirely — but DO drive animation from the synced state
    // field so peer NPCs visibly walk/run/climb instead of sliding
    // around in the wait pose.
    if (!IsLocalOwnerNPC(npc)) {
        FollowerNpcAnim peerAnim = AnimForState(this_->state, this_->syncedSpeedXZ);
        // Override for LEDGE_HOIST — mirror the dispatcher's
        // local-owner path so peer picks kHoistSwim/kHoistGround
        // from the synced hoistContext field.
        if (this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
            peerAnim = (this_->hoistContext == HOIST_CONTEXT_SWIM)
                         ? FollowerNpcAnim::kHoistSwim
                         : FollowerNpcAnim::kHoistGround;
        }
        // Same override for drowning death — peer plays the swim KO
        // anim instead of the back-down generic.
        if (this_->state == EN_FOLLOWER_STATE_DEAD &&
            this_->deathCause == 1) {
            peerAnim = FollowerNpcAnim::kDeathDrown;
        }
        EnsureAnimation(this_, play, peerAnim);
        // Per-frame playSpeed for walk/run (mirrors local-owner path —
        // fixed 1.0 matching Player_AnimChangeLoopMorph). See local
        // path for explanation of why the earlier velocity-scaled
        // formula was wrong.
        if (peerAnim == FollowerNpcAnim::kWalk || peerAnim == FollowerNpcAnim::kRun) {
            this_->skelAnime.playSpeed = 1.0f;
            // Footstep SFX so peer NPC isn't silent. speedXZ is the
            // synced value (broadcast by owner).
            this_->actor.speedXZ = this_->syncedSpeedXZ;  // for SFX pitch
            TickStepPhaseAndSfx(this_, play);
        }
        // Idle blend DISABLED 2026-05-16 (see local-owner path below).
        // constexpr bool kIdleBlendEnabled = false;
        // if (kIdleBlendEnabled && peerAnim == FollowerNpcAnim::kWait) {
        //     TickIdleBlend(this_, play);
        // }
        // Skip physics — STATE packet pos is authoritative and arrives
        // every ~100ms. If we ran Actor_MoveXZGravity here, gravity
        // (-2.0/frame) would accumulate velocity.y between packets,
        // dropping the peer below packet pos before each snap (visible
        // bobbing — worse at higher framerates where more frames pass
        // between packets). Owner clamps Y to floor before broadcasting,
        // so peer's pos is always on a valid floor.
        return;
    }

    // Need a leader. v1: leader is always the local player.
    Player* player = GET_PLAYER(play);
    if (player == nullptr) {
        npc->speedXZ = 0.0f;
        Actor_MoveXZGravity(npc);
        return;
    }
    const Vec3f& leaderPos = player->actor.world.pos;

    // Persistent room handoff — set NPC's room to leader's room each
    // tick. Without this, after a within-scene room transition the
    // NPC's actor.room field still points to the old room and the
    // engine renders it there (or culls it). Tracking leader's room
    // makes the actor "tag along" — same instance, no despawn/respawn,
    // visible in whichever room the leader currently occupies.
    //
    // Reusable for NPC Invader cross-room pursuit: same per-tick room
    // sync. For cross-SCENE pursuit, the scene transition still kills
    // the actor (engine-level), but the system-level state (CVar /
    // Invader-active flag) drives respawn via OnSceneSpawnActors.
    npc->room = player->actor.room;

    // G18 — cutscene suspension. When a cutscene is running, freeze
    // the NPC entirely (no AI tick, no animation update, no
    // locomotion). Same shape as the AI Player Follower's G18
    // gate. Without this, the NPC can wander into cutscene framing
    // or trigger collision with cutscene-locked actors.
    //
    // csCtx.state values: CS_STATE_IDLE = 0; anything non-zero means
    // a cutscene is in some flavour of running / preparing / ending.
    // The "all non-zero = freeze" rule matches G18 in HookHandlers.cpp.
    if (gPlayState->csCtx.state != CS_STATE_IDLE) {
        npc->speedXZ = 0.0f;
        // Don't call Actor_MoveXZGravity either — gravity during a
        // cutscene can drop the NPC into a void if the cutscene
        // teleported the world out from under us.
        return;
    }

    // STUCK-cycle reset-frame decay (Common/AILocomotion/StuckEscalation).
    // When the window expires, the cycle counter + advance latch reset
    // so unrelated stuck episodes don't accumulate into escalation.
    AnchorAI::TickStuckCycleWindow(sLocalNav.stuckCycle);

    // Leader-climbing force-engage. Fires BEFORE the G-guards and
    // dispatch so a successful engagement skips both. The substrate
    // pathfinder can't reliably route to a leader who is mid-climb
    // (FindNearestNode skips climb cells; cross-room nav not supported),
    // so we directly populate the path with the closest anchor's cells
    // when:
    //   - leader is in PLAYER_STATE1_CLIMBING_LADDER (vine / ladder),
    //   - NPC is not already CLIMBING,
    //   - NPC is within engagement distance of the anchor's basePos.
    //
    // Pattern adapted from AI Player Follower's autonomous-climb engagement at
    // Follower.cpp:1880-1940 (uses FindClimbAnchorAbove to detect leader's
    // anchor; we use FindClosestClimbAnchor as a fallback since
    // FindClimbAnchorAbove requires the leader's projection to be inside
    // the anchor's [0,cellsU)×[0,cellsV) grid bounds which may fail on
    // partial-grid anchors).
    if (Anchor::Instance->IsLocalPlayerClimbing() &&
        this_->state != EN_FOLLOWER_STATE_CLIMBING) {
        // Diagnostic throttle — log when force-engage WOULD-fire but
        // doesn't, so we can see which gate is preventing the
        // transition (e.g. swim→climb when leader grabs a vine wall
        // reachable from water). One log per ~2s to avoid spam.
        static uint64_t sLastDiagFrame = 0;
        const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                      std::memory_order_relaxed);
        const bool diagOK = (curFrame > sLastDiagFrame + 40);  // ~2s @ 20fps

        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        const ::AnchorNavRoom::ClimbAnchor* anchor =
            FindClosestClimbAnchor(navData, leaderPos);
        // Anchor-overhead sanity gate (ported from NPC Invader, 2026-05-19,
        // log 253 fix). If the anchor's top extends meaningfully above
        // the leader's Y, the cell column passes THROUGH or above the
        // platform the leader is standing on. Riding the column to the
        // top would climb PAST the leader — visible as "climbed through
        // the platform" in Invader's log 253. Reject and fall through
        // to substrate path (which routes around if possible).
        constexpr float kAnchorOverheadMax = 50.0f;
        if (anchor != nullptr &&
            anchor->topPos.y > leaderPos.y + kAnchorOverheadMax) {
            anchor = nullptr;
        }
        if (anchor != nullptr) {
            const float distBaseSq = Dist2DSq(npc->world.pos, anchor->basePos);
            if (distBaseSq < kClimbForceEngageBaseDistSq) {
                if (PopulateAnchorClimbPath(navData, *anchor,
                                            npc->world.pos, leaderPos,
                                            sLocalNav.navState.path)) {
                    sLocalNav.activeClimbAnchor = anchor;
                    this_->state                = EN_FOLLOWER_STATE_CLIMBING;
                    // Seed climbPrev* (see TickFOLLOW for rationale).
                    sLocalNav.climbPrevY  = npc->world.pos.y;
                    sLocalNav.climbPrevXZ = npc->world.pos;
                    SPDLOG_INFO("[FollowerNPC] Leader-climbing force-engage — anchor "
                                "base=({:.0f},{:.0f},{:.0f}) top=({:.0f},{:.0f},{:.0f}) "
                                "NPC at ({:.0f},{:.0f},{:.0f}) distBase={:.0f}u — "
                                "populated {} climb waypoints (from state={})",
                                anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                                anchor->topPos.x, anchor->topPos.y, anchor->topPos.z,
                                npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                                std::sqrt(distBaseSq),
                                (int)sLocalNav.navState.path.waypoints.size(),
                                (int)this_->prevState);
                    sLocalNav.leashFrames     = 0;
                    sLocalNav.closeFailFrames = 0;
                } else if (diagOK) {
                    SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — anchor "
                                "found, distance OK ({:.0f}u), but path population "
                                "returned empty. NPC at ({:.0f},{:.0f},{:.0f}) state={} "
                                "anchor U-axis=({:.2f},{:.2f},{:.2f}) cells={}",
                                std::sqrt(distBaseSq),
                                npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                                (int)this_->state,
                                anchor->planeAxisU.x, anchor->planeAxisU.y, anchor->planeAxisU.z,
                                (int)anchor->nodeCount);
                    sLastDiagFrame = curFrame;
                }
            } else if (diagOK) {
                SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — anchor "
                            "found but NPC too far from base ({:.0f}u > {:.0f}u "
                            "threshold). NPC at ({:.0f},{:.0f},{:.0f}) "
                            "anchor.base=({:.0f},{:.0f},{:.0f}) state={}",
                            std::sqrt(distBaseSq),
                            std::sqrt(kClimbForceEngageBaseDistSq),
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            anchor->basePos.x, anchor->basePos.y, anchor->basePos.z,
                            (int)this_->state);
                sLastDiagFrame = curFrame;
            }
        } else if (diagOK) {
            SPDLOG_INFO("[FollowerNPC] Leader-climbing engage SKIPPED — no "
                        "climb anchor in current room (sceneNum={} roomNum={}). "
                        "NPC at ({:.0f},{:.0f},{:.0f}) state={}",
                        gPlayState->sceneNum,
                        (int)gPlayState->roomCtx.curRoom.num,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        (int)this_->state);
            sLastDiagFrame = curFrame;
        }
    }

    // Phase 8 — G-guard safety nets. Run BEFORE state dispatch so a
    // teleport fully resets the state machine for the same tick (the
    // teleport drops us into FOLLOW with cleared path / baselines).
    // CLIMBING, SWIMMING, and LEDGE_HOIST are exempt — each is its
    // own scripted traversal and shouldn't be aborted by a
    // distance-based leash. Free-fall is also exempt — NPC walking
    // off a ledge after leader needs to land before G14 decides "no
    // progress" and teleports it back to the top. Detect free-fall
    // via downward velocity + not-on-floor (bgCheckFlags bit 1).
    const bool isFreeFalling = (npc->velocity.y < -5.0f) &&
                                !(npc->bgCheckFlags & 1);
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST &&
        !isFreeFalling) {
        if (TryFireG10(this_, play, leaderPos)) {
            // Teleport fired — skip rest of tick. NPC is now at leader
            // in fresh FOLLOW; next tick picks up normally.
            return;
        }
        if (TryFireG14(this_, play, leaderPos)) {
            return;
        }
    }

    // Water-entry detection — if NPC's submersion depth exceeds Link's
    // swim threshold (adult ageProperties.unk_24 = 36.0 at z_player.c:453),
    // transition to SWIMMING. CLIMBING / LEDGE_HOIST exempt — those
    // are their own scripted traversals.
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST) {
        const float swimEntryDepth = SwimDepthFor(this_->linkAge);
        if (npc->yDistToWater > swimEntryDepth) {
            this_->state = EN_FOLLOWER_STATE_SWIMMING;
            sLocalNav.navState.path.Reset();  // discard land path; swim handler
                                     // navigates direct-to-leader.
            // Clear jumpInProgress when entering water from a jump.
            // Without this, the airborne anim hold logic keeps the
            // jump anim active because jumpInProgress only clears on
            // bgCheckFlags & 1 (floor landing) — which doesn't fire
            // for water entry. NPC plays kRunJump for several seconds
            // until eventually touching the underwater floor.
            if (sLocalNav.airborneState.jumpInProgress) {
                npc->gravity = -2.0f;  // restore default
                AnchorAI::EndAirborne(sLocalNav.airborneState);
            }
            SPDLOG_INFO("[FollowerNPC] FOLLOW/IDLE→SWIMMING "
                        "(yDistToWater={:.1f}u > {:.0f}u threshold for "
                        "linkAge={})",
                        npc->yDistToWater, swimEntryDepth,
                        (int)this_->linkAge);
        }
    }

    // Ledge-hoist entry triggers. Two contexts:
    //   SWIM_EXIT  — from SWIMMING when leader is meaningfully above
    //                NPC's water surface AND a LedgeAnchor's approachPos
    //                is near NPC's swim pos. NPC plays swim-step-up
    //                anim then snaps to topPos.
    //   GROUND     — from FOLLOW when a LedgeAnchor's approachPos is
    //                near NPC's land pos AND topPos lifts in the
    //                hoist range. NPC plays mantle anim then snaps.
    //
    // Both are autonomous (no need to mirror leader's exact hoist
    // moment) — fire whenever geometry supports the lift and the
    // NPC's current state would otherwise leave it stranded below
    // the leader.
    // Helper to enter LEDGE_HOIST with a given target pos.
    auto enterLedgeHoist = [&](HoistContext ctx, const Vec3f& topPos,
                                const char* source) {
        this_->hoistContext   = (s8)ctx;
        this_->hoistTargetPos = topPos;
        this_->hoistEntryYaw  =
            Math_Atan2S(topPos.z - npc->world.pos.z,
                        topPos.x - npc->world.pos.x);
        // Swim-exit hoist: raise NPC ~60u so the swim-step-up anim
        // plays at the ledge level instead of underwater. User
        // reported NPC sinking below water during the anim. The
        // raise puts NPC's feet near the ledge top, anim visualizes
        // the climb-out motion correctly. End-of-anim snap moves NPC
        // to the exact ledge top.
        // Snap XZ to ledge top XZ at entry so the anim plays in
        // place (only Y will lerp over the anim duration). Without
        // this, an 80u XZ lerp during a 1-second mantle anim looks
        // like the body is "walking sideways while hunched" — the
        // climb_up anim assumes a stationary body pulling up. With
        // the snap, body translates straight up, matching the anim's
        // vertical-only mantle motion. Y stays at NPC's current
        // floor for the moment; TickLEDGE_HOIST lerps Y up.
        npc->world.pos.x = topPos.x;
        npc->world.pos.z = topPos.z;
        // Capture start position for the per-tick lerp during the anim.
        // hoistTargetPos is the END pos; we lerp from start → target
        // over the anim duration so body moves smoothly up.
        sLocalNav.hoistStartPos = npc->world.pos;
        if (ctx == HOIST_CONTEXT_SWIM) {
            constexpr float kSwimHoistRaise = 43.0f;  // tuned 60u → 50u → 45u → 43u over field tests
            npc->world.pos.y += kSwimHoistRaise;
            npc->velocity.y = 0.0f;
            sLocalNav.hoistStartPos = npc->world.pos;  // re-capture after raise
        }
        // For GROUND context, no pre-raise — pos stays at lower
        // floor and TickLEDGE_HOIST lerps Y up over the anim
        // duration. Body visibly mantles from floor to ledge.
        this_->state = EN_FOLLOWER_STATE_LEDGE_HOIST;
        // Clear any in-flight stop/fidget anim hold so the dispatcher's
        // LEDGE_HOIST anim override survives the stopAnimPlaying check
        // (otherwise that hold reverts localAnim to currentAnim — kWalk —
        // and EnsureAnimation never transitions to kHoistGround/kHoistSwim).
        this_->stopAnimPlaying = 0;
        SPDLOG_INFO("[FollowerNPC] {}→LEDGE_HOIST({}) top=({:.0f},{:.0f},{:.0f}) "
                    "NPC at ({:.0f},{:.0f},{:.0f}) via {}",
                    (ctx == HOIST_CONTEXT_SWIM ? "SWIMMING" : "FOLLOW"),
                    (ctx == HOIST_CONTEXT_SWIM ? "swim_exit" : "ground"),
                    topPos.x, topPos.y, topPos.z,
                    npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                    source);
    };

    if (this_->state == EN_FOLLOWER_STATE_SWIMMING) {
        // Autonomous swim-out detection — independent of leader's pos
        // (per user feedback: "Do not rely on the position of the
        // leader to determine if the NPC is in water"). Raycast from
        // 5u above NPC's head straight down. If a floor poly is hit
        // ABOVE NPC's current Y within hoist range, NPC is positioned
        // under a walkable ledge / dock / shore — trigger hoist.
        //
        // BgCheck_EntityRaycastFloor1 only returns floor-normal polys
        // (upward-facing), so cave ceilings / bridge undersides don't
        // false-trigger. Low bridges where NPC's head fits underneath
        // could trigger a premature hoist — edge case accepted in v1.
        //
        // NPC head is roughly at world.pos.y + 50 (Link body height).
        // Probe start: head + 5 = pos.y + 55.
        constexpr float kSwimHoistProbeStartY  = 55.0f;
        constexpr float kSwimHoistMinLift      = 20.0f;
        constexpr float kSwimHoistMaxLift      = 90.0f;
        Vec3f probeStart = { npc->world.pos.x,
                             npc->world.pos.y + kSwimHoistProbeStartY,
                             npc->world.pos.z };
        CollisionPoly* topPoly = nullptr;
        const f32 topY = BgCheck_EntityRaycastFloor1(&play->colCtx,
                                                      &topPoly, &probeStart);
        if (topPoly != nullptr) {
            const float lift = topY - npc->world.pos.y;
            if (lift > kSwimHoistMinLift && lift < kSwimHoistMaxLift) {
                Vec3f topPos = { probeStart.x, topY, probeStart.z };
                enterLedgeHoist(HOIST_CONTEXT_SWIM, topPos, "head-up-probe");
            }
        }
    } else if (this_->state == EN_FOLLOWER_STATE_FOLLOW &&
               leaderPos.y > npc->world.pos.y + 30.0f) {
        // Ground hoist — leader is on a ledge above NPC. LedgeAnchor
        // first, raycast fallback second (covers walls not catalogued).
        // Fires only when state=FOLLOW because IDLE/STUCK shouldn't
        // auto-hoist (NPC isn't actively pursuing).
        const ::AnchorNavRoom::RoomNavData* navData =
            ::AnchorNavRoom::GetForRoom(
                gPlayState->sceneNum,
                (int8_t)gPlayState->roomCtx.curRoom.num);
        const auto* ledge = FindClosestLedgeAnchor(navData, npc->world.pos);
        Vec3f targetTop;
        if (ledge != nullptr) {
            enterLedgeHoist(HOIST_CONTEXT_GROUND, ledge->topPos, "LedgeAnchor");
        } else if (RaycastDetectLedge(play, npc->world.pos, leaderPos, targetTop)) {
            enterLedgeHoist(HOIST_CONTEXT_GROUND, targetTop, "raycast");
        } else {
            // Diagnostic — leader is high enough to warrant hoist but
            // neither LedgeAnchor nor raycast found a ledge geometry.
            // NPC will fall through to auto-jump or just walk into the
            // wall. Log throttled to ~2s to track field-test cases
            // where hoist should fire but doesn't.
            static uint64_t sLastHoistMissDiag = 0;
            const uint64_t curFrame = Anchor::Instance->gameFrameCounter.load(
                                          std::memory_order_relaxed);
            if (curFrame > sLastHoistMissDiag + 40) {
                SPDLOG_INFO("[FollowerNPC.hoist] GROUND trigger conditions met "
                            "but no ledge found — leader.y={:.0f}, NPC.y={:.0f} "
                            "(diff={:.0f}), navData={}, ledgeAnchorCount={}",
                            leaderPos.y, npc->world.pos.y,
                            leaderPos.y - npc->world.pos.y,
                            (navData ? "OK" : "NULL"),
                            (navData ? (int)navData->ledgeAnchors.size() : 0));
                sLastHoistMissDiag = curFrame;
            }
        }
    }

    // Stage 2 — environmental death triggers (drowning, void). Runs
    // BEFORE state dispatch so a death detected this tick gets the
    // first TickDEAD call (anim entry + start-frame capture) instead
    // of waiting a frame.
    CheckEnvironmentalDeath(this_, play);

    // Stage 4 — combat engagement check. Tier 1 (close): direct ATTACK.
    // Tier 2 (medium): ENGAGE pursuit. Fires from IDLE / FOLLOW only.
    // Runs BEFORE state dispatch so the new state's handler gets the
    // first tick (start-frame capture for the active-window check, or
    // initial pursuit yaw computation).
    TryEngageCombat(this_, play);

    // Stage 5 — crawlspace entry check. Fires when leader is on the
    // far side of a CrawlspaceAnchor wall plane and NPC is on the
    // entry side within range. Child Link only.
    TryEnterCrawling(this_, play, leaderPos);

    // Dispatch.
    switch (this_->state) {
        default:
        case EN_FOLLOWER_STATE_IDLE:        TickIDLE(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_FOLLOW:      TickFOLLOW(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_STUCK:       TickSTUCK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_CLIMBING:    TickCLIMBING(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_SWIMMING:    TickSWIMMING(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_LEDGE_HOIST: TickLEDGE_HOIST(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_DEAD:        TickDEAD(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_ATTACK:      TickATTACK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_ENGAGE:      TickENGAGE(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_BLOCK:       TickBLOCK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_RANGED_ATTACK: TickRANGED_ATTACK(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_STANDBY:     TickSTANDBY(this_, play, leaderPos); break;
        case EN_FOLLOWER_STATE_CRAWLING:    TickCRAWLING(this_, play, leaderPos); break;
    }

    // Clear stop-anim latch when the ONCE anim has reached endFrame.
    // LinkAnimation_Once clamps curFrame to endFrame on completion
    // (z_skelanime.c:1237-1238), so once curFrame == endFrame the
    // stop anim is done and we can swap back to wait.
    if (this_->stopAnimPlaying &&
        this_->skelAnime.curFrame >= this_->skelAnime.endFrame) {
        this_->stopAnimPlaying = 0;
    }

    // Head-look-at-leader. Hard-disabled (zeroed, not settled) during
    // CLIMBING + LEDGE_HOIST. The earlier gradual settle (0x600/tick
    // → ~0.65s to neutralize a fully-deflected head turn) left the
    // first ~0.65s of climb entry showing the prior leader-tracking
    // pose on top of the climb anim — user-reported as "torso and
    // head bending at unnatural angles" 2026-05-19 PM. Snapping to
    // zero gives the climb anim a clean default-pose base.
    if (this_->state == EN_FOLLOWER_STATE_CLIMBING ||
        this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
        AnchorAI::ResetHeadLookToNeutral(&this_->headLimbRot,
                                          &this_->upperLimbRot);
    } else {
        TickHeadLookAtLeader(this_, leaderPos);
    }

    // Animation. Run AFTER dispatch so any state transitions made by
    // the handler are reflected this same tick (e.g. FOLLOW → STUCK
    // immediately switches to wait anim, not next-tick).
    //
    // FOLLOW→IDLE transition gets a one-shot stop anim (walk_endL/R)
    // chosen by current step phase — eliminates the "freeze mid-stride"
    // pop when NPC arrives at leader. Mirrors Player's func_8083BF50
    // (z_player.c:6388).
    // currentAnimType — drives armed (fighter) vs unarmed (_free)
    // anim variants. Per the user-spec design (2026-05-15): NPC
    // body language reflects its OWN combat intent, NOT Player's
    // weapon state. The NPC has the fighter pose only when in a
    // combat state (STANDBY / ATTACK / BLOCK / ENGAGE / RANGED_ATTACK);
    // otherwise it uses the relaxed unarmed pose regardless of what
    // Player has drawn.
    //
    // Equipment visibility (sword/shield/bow models) is still
    // inherited from Player via the Player_DrawImpl override
    // callback — that's a future Phase B follow-up requiring
    // hand-type swap in the draw path.
    auto isCombatState = [](s32 s) {
        return s == EN_FOLLOWER_STATE_STANDBY ||
               s == EN_FOLLOWER_STATE_ATTACK  ||
               s == EN_FOLLOWER_STATE_BLOCK   ||
               s == EN_FOLLOWER_STATE_ENGAGE  ||
               s == EN_FOLLOWER_STATE_RANGED_ATTACK;
    };
    this_->currentAnimType = isCombatState(this_->state) ? 1 /*fighter*/ : 0 /*free*/;
    (void)player;  // no longer used for animType inheritance

    FollowerNpcAnim localAnim = AnimForState(this_->state, npc->speedXZ);
    // Ledge-hoist anim variant — AnimForState defaults to kHoistGround,
    // override here based on hoistContext (the dispatcher has access
    // to this_, AnimForState doesn't).
    if (this_->state == EN_FOLLOWER_STATE_LEDGE_HOIST) {
        localAnim = (this_->hoistContext == HOIST_CONTEXT_SWIM)
                      ? FollowerNpcAnim::kHoistSwim
                      : FollowerNpcAnim::kHoistGround;
    }
    // Death-pose variant — AnimForState returns kDeath (back-down)
    // for any DEAD state; override to kDeathDrown when the cause
    // was drowning (NPC is in water; back-down anim would look
    // strange floating).
    if (this_->state == EN_FOLLOWER_STATE_DEAD &&
        this_->deathCause == 1) {
        localAnim = FollowerNpcAnim::kDeathDrown;
    }
    // Crawlspace exit anim — AnimForState returns kCrawlMove for
    // EN_FOLLOWER_STATE_CRAWLING; override to kCrawlExit when the
    // exit transition has been triggered (TickCRAWLING set the
    // exitAnimPlaying flag).
    if (this_->state == EN_FOLLOWER_STATE_CRAWLING &&
        sCrawlState.exitAnimPlaying) {
        localAnim = FollowerNpcAnim::kCrawlExit;
    }
    // Stage 3 — non-fatal fall hurt reaction. Plays the back-down anim
    // briefly without entering DEAD state (NPC sits up automatically
    // once the counter hits zero and AnimForState resumes normal
    // selection). Same anim asset as kDeath; the difference is
    // duration + lack of DEAD state transition.
    if (sLocalNav.fallHurtFramesRemaining > 0 &&
        this_->state != EN_FOLLOWER_STATE_DEAD) {
        localAnim = FollowerNpcAnim::kDeath;
        sLocalNav.fallHurtFramesRemaining--;
    }
    // Stage 4 — BLOCK hit-reaction override. While the block-hit
    // counter is non-zero AND state is still BLOCK, override
    // kBlockWait with the one-shot kBlockHit. Counter decrements
    // in TickBLOCK so this fires for kBlockHitAnimFrames ticks
    // after a successful block.
    if (this_->state == EN_FOLLOWER_STATE_BLOCK &&
        sBlockState.hitAnimFrames > 0) {
        localAnim = FollowerNpcAnim::kBlockHit;
    }

    // Auto-jump-off-ledge — mimic Player's z_player.c:5818-5836.
    // bgCheckFlags & 4 is set by Actor_UpdateBgCheckInfo's
    // func_8002E234 (z_actor.c:1604) when actor was on floor last
    // frame but the floor dropped >11u (walked off a ledge with
    // a meaningful drop). Player triggers an auto-jump when this
    // hits AND linearVelocity > 3 AND facing forward.
    //
    // For our NPC: same gate plus state must be FOLLOW (don't jump
    // during CLIMBING / SWIMMING / LEDGE_HOIST etc.). Pick run_jump
    // anim when fast, regular jump when slow (mirrors func_8083A4A8).
    // Set velocity.y = positive boost so NPC arcs up (gravity then
    // pulls down — natural jump arc).
    {
        const bool isOnFloor      = (npc->bgCheckFlags & 1) != 0;
        const bool walkedOffEdge  = (npc->bgCheckFlags & 4) != 0;
        const uint64_t curFrame   = Anchor::Instance->gameFrameCounter.load(
                                        std::memory_order_relaxed);

        // Diagnostic — log every gate evaluation at low rate so we can
        // see WHY the jump didn't fire. Throttled to once every 30
        // frames (~1.5s at 20fps) to limit spam, only when in FOLLOW
        // and yDistToFloor is meaningful (suggesting the NPC is near
        // an edge or in the air).
        static uint64_t sLastGateDiag = 0;
        if (this_->state == EN_FOLLOWER_STATE_FOLLOW &&
            curFrame > sLastGateDiag + 30 &&
            (walkedOffEdge || npc->yDistToWater > 1.0f ||
             std::fabs(npc->velocity.y) > 1.0f)) {
            SPDLOG_INFO("[FollowerNPC.jump] gate eval: state=FOLLOW "
                        "bgCheckFlags=0x{:X} (onFloor={} walkedOffEdge={}) "
                        "speedXZ={:.2f} velocity.y={:.2f} pos=({:.0f},{:.0f},{:.0f}) "
                        "shape.rot.y=0x{:X} world.rot.y=0x{:X}",
                        npc->bgCheckFlags, isOnFloor, walkedOffEdge,
                        npc->speedXZ, npc->velocity.y,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        (uint16_t)npc->shape.rot.y, (uint16_t)npc->world.rot.y);
            sLastGateDiag = curFrame;
        }

        // Trigger auto-jump.
        if (walkedOffEdge && npc->speedXZ > 3.0f &&
            this_->state == EN_FOLLOWER_STATE_FOLLOW) {
            // Boost matches Player's typical auto-jump value (run_jump
            // peak ≈ 10 from func_8083A4A8 with default IREG(67)).
            // Earlier value of 6.0 produced only +9u peak rise against
            // our -2.0 gravity; bump to 10.0 + lower gravity to match
            // Player's airborne behavior at z_player.c:9670.
            constexpr float kJumpBoostVy = 8.0f;   // tuned 6 → 10 → 8 over field tests
            constexpr float kJumpGravity = -1.2f;  // Player_Action_8084411C
            npc->velocity.y = kJumpBoostVy;
            npc->gravity    = kJumpGravity;        // restored on landing
            npc->bgCheckFlags &= ~4;
            // Same hazard as LEDGE_HOIST: clear any in-flight stop/fidget
            // hold so the localAnim assignment below isn't reverted by the
            // stopAnimPlaying check — otherwise EnsureAnimation never
            // transitions to kRunJump/kJump and the run anim plays airborne.
            this_->stopAnimPlaying = 0;
            localAnim = (npc->speedXZ > 4.0f) ? FollowerNpcAnim::kRunJump
                                              : FollowerNpcAnim::kJump;
            // Capture jump diagnostics
            AnchorAI::StartAirborne(sLocalNav.airborneState,
                                    npc->world.pos, curFrame);
            sLocalNav.jumpStartYaw       = npc->shape.rot.y;
            sLocalNav.jumpStartSpeedXZ   = npc->speedXZ;
            sLocalNav.jumpStartVelocityY = kJumpBoostVy;
            sLocalNav.jumpLastDiagFrame  = curFrame;
            SPDLOG_INFO("[FollowerNPC.jump] FIRE — anim={} speedXZ={:.2f} "
                        "yaw=0x{:X} startPos=({:.0f},{:.0f},{:.0f}) "
                        "boostVel.y={:.2f} gravity={:.2f}",
                        (localAnim == FollowerNpcAnim::kRunJump ? "run_jump" : "jump"),
                        npc->speedXZ, (uint16_t)npc->shape.rot.y,
                        npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                        npc->velocity.y, npc->gravity);
        }

        // Per-frame airborne tracking — log every ~6 frames (~0.3s)
        // while jumpInProgress so we can see velocity / pos / peak
        // evolving. Track peak Y for the summary.
        if (sLocalNav.airborneState.jumpInProgress) {
            // Stuck-detection via shared helper (Common/AILocomotion/
            // AirborneRecovery). Two branches OR'd:
            //   (a) zeroVel: airborne ≥5s with |velocity.y| < 1.0
            //   (b) posStuck: airborne ≥5s with NO position change
            //       in the last 5s (log 161 — frozen at terminal
            //       velocity but pos static).
            AnchorAI::AirborneRecoveryInput in;
            in.currentPos = npc->world.pos;
            in.velocityY  = npc->velocity.y;
            in.isOnFloor  = isOnFloor;
            in.curFrame   = curFrame;
            const AnchorAI::AirborneRecoveryResult rec =
                AnchorAI::UpdateAirborneRecovery(sLocalNav.airborneState, in);

            if (rec.shouldForceTeleport) {
                SPDLOG_WARN("[FollowerNPC.jump] STUCK in air for {} frames "
                            "(velocity.y={:.2f}, pos=({:.0f},{:.0f},{:.0f}), "
                            "posStuck={} zeroVel={}) — force-teleport to leader",
                            (int)rec.airborneFrames, npc->velocity.y,
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            rec.posStuck, rec.zeroVel);
                TeleportNpcTo(this_, play, leaderPos);
                npc->gravity = -2.0f;  // restore default
                AnchorAI::EndAirborne(sLocalNav.airborneState);
                return;
            }
            // Helper already wrote jumpPeakPos via in.peakUpdated path —
            // no need to maintain a separate peak check.
            if (curFrame > sLocalNav.jumpLastDiagFrame + 6) {
                const float dxFromStart = npc->world.pos.x - sLocalNav.airborneState.jumpStartPos.x;
                const float dyFromStart = npc->world.pos.y - sLocalNav.airborneState.jumpStartPos.y;
                const float dzFromStart = npc->world.pos.z - sLocalNav.airborneState.jumpStartPos.z;
                const float distXZ = std::sqrt(dxFromStart*dxFromStart +
                                                dzFromStart*dzFromStart);
                SPDLOG_INFO("[FollowerNPC.jump] airborne tick {}: "
                            "pos=({:.0f},{:.0f},{:.0f}) velocity.y={:.2f} "
                            "speedXZ={:.2f} dY={:+.1f} distXZ={:.1f} "
                            "peakY={:.0f} (peakΔY={:+.1f}) onFloor={}",
                            (int)(curFrame - sLocalNav.airborneState.jumpStartFrame),
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            npc->velocity.y, npc->speedXZ,
                            dyFromStart, distXZ,
                            sLocalNav.airborneState.jumpPeakPos.y,
                            sLocalNav.airborneState.jumpPeakPos.y - sLocalNav.airborneState.jumpStartPos.y,
                            isOnFloor);
                sLocalNav.jumpLastDiagFrame = curFrame;
            }
            // Landing detection — bgCheckFlags & 1 set means NPC
            // touched a floor. Emit summary log + close jump tracking.
            if (isOnFloor && !sLocalNav.jumpWasOnFloorPrevTick) {
                const float dxFromStart = npc->world.pos.x - sLocalNav.airborneState.jumpStartPos.x;
                const float dyFromStart = npc->world.pos.y - sLocalNav.airborneState.jumpStartPos.y;
                const float dzFromStart = npc->world.pos.z - sLocalNav.airborneState.jumpStartPos.z;
                const float distXZ = std::sqrt(dxFromStart*dxFromStart +
                                                dzFromStart*dzFromStart);
                const float peakRise = sLocalNav.airborneState.jumpPeakPos.y -
                                        sLocalNav.airborneState.jumpStartPos.y;
                // Fall distance for damage = peak Y minus landing Y.
                // Using peak (not start) handles "jumped up then fell
                // into a deep pit" — the actual descent measures from
                // the high point of the trajectory, not the takeoff.
                const float fallDistance = sLocalNav.airborneState.jumpPeakPos.y -
                                            npc->world.pos.y;
                SPDLOG_INFO("[FollowerNPC.jump] LAND — totalFrames={} "
                            "startPos=({:.0f},{:.0f},{:.0f}) "
                            "landPos=({:.0f},{:.0f},{:.0f}) "
                            "peakY={:.0f} (rise={:+.1f}) drop={:+.1f}u "
                            "fallDistance={:.0f} horizontalDist={:.1f}u "
                            "final velocity.y={:.2f}",
                            (int)(curFrame - sLocalNav.airborneState.jumpStartFrame),
                            sLocalNav.airborneState.jumpStartPos.x, sLocalNav.airborneState.jumpStartPos.y,
                            sLocalNav.airborneState.jumpStartPos.z,
                            npc->world.pos.x, npc->world.pos.y, npc->world.pos.z,
                            sLocalNav.airborneState.jumpPeakPos.y, peakRise,
                            dyFromStart, fallDistance, distXZ, npc->velocity.y);
                npc->gravity = -2.0f;  // restore default ground gravity
                AnchorAI::EndAirborne(sLocalNav.airborneState);

                // Stage 3 — fall damage. Gated on Invulnerable CVar
                // and IsLocalOwnerNPC (peers don't own damage state).
                // Thresholds chosen to roughly match Player's 320u
                // soft-landing threshold + scale up with fall depth.
                //   < 400u : safe
                //   400-799 : 1 HP
                //   800-1199: 2 HP
                //   1200+  : 3 HP (or more, scaled per 400u increment)
                if (!FollowerNpcInvulnerable() &&
                    IsLocalOwnerNPC(npc) &&
                    this_->state != EN_FOLLOWER_STATE_DEAD) {
                    constexpr float kFallSafeThreshold = 400.0f;
                    constexpr float kFallHpStepUnits   = 400.0f;
                    if (fallDistance >= kFallSafeThreshold) {
                        const int hpLoss =
                            (int)((fallDistance - kFallSafeThreshold) /
                                  kFallHpStepUnits) + 1;
                        const int newHealth =
                            std::max<int>(0, (int)this_->health - hpLoss);
                        SPDLOG_INFO("[FollowerNPC] fall damage: "
                                    "fallDistance={:.0f} → {} HP loss "
                                    "(health {}→{})",
                                    fallDistance, hpLoss,
                                    (int)this_->health, newHealth);
                        this_->health = (s8)newHealth;
                        if (newHealth <= 0) {
                            this_->state           = EN_FOLLOWER_STATE_DEAD;
                            this_->deathFlag       = 1;
                            this_->deathCause      = 0;  // generic (back-down)
                            this_->stopAnimPlaying = 0;
                            SPDLOG_INFO("[FollowerNPC] death by fall");
                        } else {
                            // Non-fatal hard landing — fire the back-
                            // down anim as a brief hurt reaction.
                            // Same one-shot anim as kDeath; flips
                            // back to kWait/kWalk via the
                            // stopAnimPlaying handshake when finished.
                            this_->stopAnimPlaying = 0;
                            // We don't enter DEAD state — only the
                            // anim is overridden via a transient
                            // localAnim assignment further down. Set
                            // a flag so the per-tick anim resolution
                            // picks kDeath (back-down) for this hit.
                            sLocalNav.fallHurtFramesRemaining = 30;  // ~1.5s @ 20fps
                        }
                    }
                }
            }
        }
        sLocalNav.jumpWasOnFloorPrevTick = isOnFloor;
    }
    const bool justStoppedMoving =
        (this_->prevState == EN_FOLLOWER_STATE_FOLLOW &&
         this_->state    == EN_FOLLOWER_STATE_IDLE);
    if (justStoppedMoving) {
        localAnim = (this_->stepPhase < kStopPhaseLRSplit)
            ? FollowerNpcAnim::kStopL
            : FollowerNpcAnim::kStopR;
    }

    // Idle fidget rotation. After sustained kWait, swap to a fidget
    // anim (look-around / warm / stretch) cycling through three
    // variants. Adds the look-around variety Link has in his idle.
    // Counter only advances while actually playing kWait (not while
    // stop-anim or another fidget is in progress).
    static constexpr u32 kFidgetIntervalTicks = 120;  // ~6s at 20fps
    if (localAnim == FollowerNpcAnim::kWait && !this_->stopAnimPlaying &&
        (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kWait) {
        this_->idleTicks++;
        if (this_->idleTicks >= kFidgetIntervalTicks) {
            this_->idleTicks = 0;
            switch (this_->nextFidgetIdx % 3) {
                case 0: localAnim = FollowerNpcAnim::kFidgetLookA;    break;
                case 1: localAnim = FollowerNpcAnim::kFidgetWarmB;    break;
                case 2: localAnim = FollowerNpcAnim::kFidgetStretchD; break;
            }
            this_->nextFidgetIdx++;
        }
    } else if (localAnim != FollowerNpcAnim::kWait) {
        // Out of idle — reset counter so next idle session starts fresh.
        this_->idleTicks = 0;
    }

    // If NPC has started moving while a fidget/stop-anim was holding,
    // cancel the hold so walk/run takes over immediately. Without this
    // the body slides along the ground while the idle/fidget plays
    // out (user-reported). Movement signal: in FOLLOW state with
    // non-trivial XZ speed.
    if (this_->stopAnimPlaying &&
        this_->state == EN_FOLLOWER_STATE_FOLLOW &&
        npc->speedXZ > 0.5f) {
        this_->stopAnimPlaying = 0;
    }

    // Keep playing the stop anim / fidget until it finishes
    // (LinkAnimation_Update returns true). EnsureAnimation guard on
    // `stopAnimPlaying` prevents mid-anim overrides; once stopAnimPlaying
    // clears, fall through to the normal anim choice (which is kWait
    // for fidgets, so AnimForState swaps us back to the breathing
    // idle automatically).
    if (this_->stopAnimPlaying) {
        localAnim = (FollowerNpcAnim)this_->currentAnim;  // hold current
    }

    // Climb step alternation — when in CLIMBING state, drive
    // L/R one-shot alternation from NPC's vertical motion.
    // Mirrors Player at z_player.c:13391-13412 where each climb
    // step is Player_AnimPlayOnce(upL/upR), toggled via
    // actionVar2 ^= 1 when the prior one-shot completes. When
    // NPC is stationary on the wall, no new step fires and the
    // anim holds at last frame (Player's STATIONARY_LADDER).
    if (this_->state == EN_FOLLOWER_STATE_CLIMBING) {
        // Build the abstract decision context; the shared helper maps
        // motion-axis → ClimbAnimStep and we map back to FollowerNpcAnim
        // below. See Common/AILocomotion/LocomotionAnim.h. Mirror of the
        // identical NPC Invader call site (Phase 6, 2026-05-19).
        const auto upL   = FollowerNpcAnim::kClimbUpL;
        const auto upR   = FollowerNpcAnim::kClimbUpR;
        const auto sideL = FollowerNpcAnim::kClimbSideL;
        const auto sideR = FollowerNpcAnim::kClimbSideR;
        const bool currentIsAClimb =
            this_->currentAnim == (s32)upL || this_->currentAnim == (s32)upR ||
            this_->currentAnim == (s32)sideL || this_->currentAnim == (s32)sideR;

        AnchorAI::ClimbAnimContext animCtx;
        animCtx.currentPos          = npc->world.pos;
        animCtx.prevPos             = { sLocalNav.climbPrevXZ.x,
                                        sLocalNav.climbPrevY,
                                        sLocalNav.climbPrevXZ.z };
        animCtx.climbNextIsRight    = sLocalNav.climbNextIsRight;
        animCtx.currentAnimIsClimb  = currentIsAClimb;
        animCtx.prevStepDone        = !this_->stopAnimPlaying || !currentIsAClimb;
        sLocalNav.climbPrevY  = npc->world.pos.y;
        sLocalNav.climbPrevXZ = npc->world.pos;

        const AnchorAI::ClimbAnimResult anim =
            AnchorAI::PickClimbAnimStep(animCtx);

        switch (anim.step) {
            case AnchorAI::ClimbAnimStep::kFireUpL:   localAnim = upL;   break;
            case AnchorAI::ClimbAnimStep::kFireUpR:   localAnim = upR;   break;
            case AnchorAI::ClimbAnimStep::kFireSideL: localAnim = sideL; break;
            case AnchorAI::ClimbAnimStep::kFireSideR: localAnim = sideR; break;
            case AnchorAI::ClimbAnimStep::kHoldCurrent:
                localAnim = (FollowerNpcAnim)this_->currentAnim;
                break;
        }
        if (anim.advanceLR) {
            sLocalNav.climbNextIsRight = !sLocalNav.climbNextIsRight;
        }
    }

    // Airborne anim hold — while jumpInProgress AND a jump anim is
    // already running, keep the jump anim as the active selection
    // regardless of whether the one-shot has ended. Mirrors
    // Player_Action_8084411C (z_player.c:9663): Link stays in the
    // jump pose until landing. Without this, our NPC's kRunJump
    // one-shot ends mid-fall, AnimForState returns kWalk/kRun/kWait
    // based on speedXZ, and NPC visibly switches to a walking pose
    // while still in the air.
    //
    // The `currentAnim is a jump` gate is critical: on the trigger
    // frame, `localAnim` was just set to kRunJump/kJump but
    // `currentAnim` is still the prior anim (kRun/kWalk) because
    // EnsureAnimation hasn't run yet. Without the gate, the hold
    // would revert localAnim to kRun and EnsureAnimation would
    // never transition to the jump anim. With the gate, the hold
    // only fires from frame N+1 onward (after currentAnim is
    // updated to kRunJump/kJump), letting the trigger-frame choice
    // through unmolested.
    if (sLocalNav.airborneState.jumpInProgress &&
        ((FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kRunJump ||
         (FollowerNpcAnim)this_->currentAnim == FollowerNpcAnim::kJump)) {
        localAnim = (FollowerNpcAnim)this_->currentAnim;
    }
    EnsureAnimation(this_, play, localAnim);

    // Per-frame playSpeed for walk/run. Player's run anim is set via
    // Player_AnimChangeLoopMorph at z_player.c:6634 with playSpeed=1.0
    // — fixed cadence regardless of motion speed. Earlier iteration
    // here used `speedXZ * 0.3 + 1.0` which is actually Player's
    // STEP-COUNTER formula (z_player.c:8170, advances unk_868), NOT
    // the anim playSpeed. That produced ~3× too-fast anim cadence
    // (user observed "twice as fast" — close enough).
    //
    // Match Player: playSpeed = 1.0 for both walk and run. Anim
    // cycle frames per game tick = 1.0 * R_UPDATE_RATE * 0.5 = 1.5,
    // a natural cadence that doesn't drift with motion. Step counter
    // still scales with velocity (handled by TickStepPhaseAndSfx
    // using the actual step-counter formula).
    if (localAnim == FollowerNpcAnim::kWalk || localAnim == FollowerNpcAnim::kRun) {
        this_->skelAnime.playSpeed = 1.0f;
        // Tick step phase + emit footstep SFX on foot-down crossings.
        TickStepPhaseAndSfx(this_, play);
    }
    // Climb anims at 2.0× — field-test reported the natural 1.0
    // cadence looked half-speed. The 2× scale matches the user's
    // perceived "normal" speed for the L/R alternation pace.
    else if (localAnim == FollowerNpcAnim::kClimbUpL ||
             localAnim == FollowerNpcAnim::kClimbUpR ||
             localAnim == FollowerNpcAnim::kClimbSideL ||
             localAnim == FollowerNpcAnim::kClimbSideR) {
        this_->skelAnime.playSpeed = 2.0f;
    }

    // Idle blend — DISABLED 2026-05-16 (user reported model collapse +
    // distortion + position jumping while in idle). Suspected causes
    // (any combination):
    //   - LinkAnimation_BlendToJoint queues entries that race with
    //     LinkAnimation_Update's queue entries for the same jointTable,
    //     producing torn / partial joint data when the queue processes.
    //   - Blend table alignment overflow: LinkAnimation_BlendToJoint
    //     ALIGN16's the buffer pointer (up to +15 bytes skew), and
    //     PLAYER_LIMB_BUF_COUNT (24 entries = 144 bytes) provides only
    //     12 bytes of headroom over the 132 bytes of data written —
    //     a 2-byte short in worst-case alignment, stomping adjacent
    //     struct members (headLimbRot / upperLimbRot follow blendTable
    //     in EnFollower).
    //   - waitL/waitR anim lengths may differ from wait_free; passing
    //     skelAnime.curFrame (driven by wait_free's animLength) could
    //     read beyond waitL/R bounds.
    //
    // Re-enabling: flip kIdleBlendEnabled to true AND fix the underlying
    // issue. Likely path: skip LinkAnimation_Update for wait state and
    // let BlendToJoint be the only joint-table writer (Player's
    // pattern — Player doesn't call LinkAnimation_Update when using
    // BlendToJoint for idle, see z_player.c:8061-8064). NPC will fall
    // back to wait_free + LinkAnimation_Update (the prior working
    // path) while this is disabled.
    constexpr bool kIdleBlendEnabled = false;
    if (kIdleBlendEnabled &&
        localAnim == FollowerNpcAnim::kWait && !this_->stopAnimPlaying) {
        TickIdleBlend(this_, play);
    }

    this_->prevState = this_->state;

    // Sync speed for peers — they read syncedSpeedXZ to choose walk
    // vs run anim. (For peers, this is overwritten by the STATE
    // packet handler; for local owner, this is the source of truth.)
    this_->syncedSpeedXZ = npc->speedXZ;

    // Apply locomotion. Standard OoT NPC pattern — speedXZ + world.rot.y
    // give a per-frame velocity vector; gravity pulls Y to floor.
    // CLIMBING bypasses this (climb handler writes world.pos directly,
    // gravity would yank NPC off the wall). SWIMMING also bypasses
    // (swim handler clamps Y to water surface; gravity would drag
    // NPC underwater each frame, fighting the surface clamp).
    // LEDGE_HOIST bypasses too — handler locks pos during the anim
    // and snaps to topPos on completion; gravity would drop NPC
    // below the ledge during the lock-pose phase.
    if (this_->state != EN_FOLLOWER_STATE_CLIMBING &&
        this_->state != EN_FOLLOWER_STATE_SWIMMING &&
        this_->state != EN_FOLLOWER_STATE_LEDGE_HOIST &&
        this_->state != EN_FOLLOWER_STATE_CRAWLING) {
        Actor_MoveXZGravity(npc);

        // Update collision-with-ground / floor altitude. Without this
        // the NPC's Y can drift away from the floor on slopes / steps.
        // Flags=4 matches the standard NPC pattern (e.g. z_en_md.c:889).
        Actor_UpdateBgCheckInfo(play, npc, 26.0f /* wallCheckHeight */,
                                10.0f /* wallCheckRadius */,
                                50.0f /* ceilingCheckHeight */,
                                4 /* flags */);
    }

    // Stage 3 — body collider tick. Update cylinder pos from actor
    // pos + rotation, drain any AC hit landed this frame, register
    // AC for next frame. Skipped while DEAD (the body is despawning
    // and we don't want late hits to re-decrement health). Skipped
    // when Invulnerable (the user-facing kill-switch for the entire
    // damage flow — both detection and application).
    if (this_->state != EN_FOLLOWER_STATE_DEAD &&
        !FollowerNpcInvulnerable() &&
        IsLocalOwnerNPC(npc)) {
        Collider_UpdateCylinder(npc, &this_->collider);

        // Drain AC hit, if any. CollisionCheck_Damage (engine
        // pre-update pass) has already written damage value to
        // npc->colChkInfo.damage and set acFlags & AC_HIT on the
        // collider; clear both, decrement health.
        if ((this_->collider.base.acFlags & AC_HIT) != 0) {
            this_->collider.base.acFlags &= ~AC_HIT;
            const int dmgUnits = (int)npc->colChkInfo.damage;

            // Stage 4 — BLOCK absorption. While in BLOCK state, frontal
            // hits (attacker within ±90° of NPC's facing) are absorbed
            // entirely — kBlockHit reaction plays, no HP loss. Back/
            // side hits take full damage. Approximated attacker = the
            // NPC's current target (sAttackState.target). If no target
            // is tracked, treat all blocked hits as frontal so the
            // BLOCK state has a meaningful effect even when target
            // tracking is fuzzy.
            bool blocked = false;
            if (this_->state == EN_FOLLOWER_STATE_BLOCK && dmgUnits > 0) {
                if (sAttackState.target == nullptr ||
                    IsFrontalAttacker(this_, sAttackState.target)) {
                    blocked = true;
                    sBlockState.hitAnimFrames = kBlockHitAnimFrames;
                    this_->stopAnimPlaying = 0;  // let kBlockHit override flow through
                    SPDLOG_INFO("[FollowerNPC] BLOCK absorbed dmgUnits={} (frontal)",
                                dmgUnits);
                }
            }

            // OoT damage values are in 1/16-heart units (16 = 1 heart).
            // NPC HP is in full hearts; convert via /16 (ceil so a
            // 1u hit still deals 1 HP). A 4-damage enemy attack
            // (1 full heart) → 1 NPC HP; an 8-damage attack (2 hearts)
            // → 1 NPC HP (clamps to 1); etc. Could refine to track
            // partial hearts later.
            const int hpLoss = (!blocked && dmgUnits > 0) ? 1 : 0;
            if (hpLoss > 0) {
                this_->health = (s8)std::max<int>(0, (int)this_->health - hpLoss);
                SPDLOG_INFO("[FollowerNPC] AC_HIT dmgUnits={} → health {}→{}",
                            dmgUnits, (int)this_->health + hpLoss,
                            (int)this_->health);
                if (this_->health <= 0) {
                    this_->state           = EN_FOLLOWER_STATE_DEAD;
                    this_->deathFlag       = 1;
                    this_->deathCause      = 0;  // generic
                    this_->stopAnimPlaying = 0;
                    SPDLOG_INFO("[FollowerNPC] death by combat damage");
                }
            }
            npc->colChkInfo.damage = 0;
        }
        // Mirror local health to colChkInfo so future damage-routing
        // code that reads it (Player_InflictDamage-style helpers)
        // sees a non-zero value.
        npc->colChkInfo.health = this_->health;

        // Register AC for the next collision frame.
        CollisionCheck_SetAC(play, &play->colChkCtx, &this_->collider.base);
        // Register OC (blocking) so the NPC's body has physical
        // presence — enemies can't walk through it.
        CollisionCheck_SetOC(play, &play->colChkCtx, &this_->collider.base);
    }
}
