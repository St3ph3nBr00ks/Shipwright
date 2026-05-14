#ifndef Z_EN_FOLLOWER_H
#define Z_EN_FOLLOWER_H

#include <libultraship/libultra.h>
#include "global.h"
#include "z64player.h"  // PLAYER_LIMB_BUF_COUNT, PLAYER_LIMB_MAX — Link skel sizing

// SoH NPC Follower companion (Flotilla — see Plans/npc_follower_plan.md).
//
// v1 scope:
//   - Registers as a custom actor via ActorDB (dynamic ID at runtime).
//   - Spawned on CVar toggle gEnhancements.AI.FollowerNPC.Enabled (Phase 2).
//   - Pure pathfinding (v1 has no combat; invulnerable).
//   - Uses Link's skel for animation richness (Phase 1.5 rendering work).
//   - Locomotion via Actor_MoveXZGravity + direct world.pos writes
//     (NOT stick injection — this NPC is the Invader's locomotion testbed).
//
// State machine (5 states defined; v1 implements IDLE / FOLLOW / CLIMBING /
// STUCK; DEAD is a reserved enum slot for v2 combat work).
typedef enum {
    EN_FOLLOWER_STATE_IDLE        = 0,
    EN_FOLLOWER_STATE_FOLLOW      = 1,
    EN_FOLLOWER_STATE_CLIMBING    = 2,
    EN_FOLLOWER_STATE_STUCK       = 3,
    EN_FOLLOWER_STATE_DEAD        = 4,
    EN_FOLLOWER_STATE_SWIMMING    = 5,  // tread/swim while submerged
    EN_FOLLOWER_STATE_LEDGE_HOIST = 6,  // one-shot mantle (swim-out / ground-climb)
} EnFollowerAIState;

// LEDGE_HOIST entry context — drives anim selection.
typedef enum {
    HOIST_CONTEXT_GROUND = 0,  // ground-to-ledge mantle (gPlayerAnim_link_normal_climb_up)
    HOIST_CONTEXT_SWIM   = 1,  // swim-out-of-water (gPlayerAnim_link_swimer_swim_15step_up)
} HoistContext;

struct EnFollower;

typedef void (*EnFollowerActionFunc)(struct EnFollower*, PlayState*);

typedef struct EnFollower {
    Actor actor;

    // Link skel runtime state. Matches the Player struct's table sizing
    // (PLAYER_LIMB_BUF_COUNT = align16(22 * sizeof(Vec3s)) / sizeof(Vec3s)
    // ≈ 24 entries) so SkelAnime_InitLink can use them without
    // resizing. Initialized in Init from gPlayerSkelHeaders.
    SkelAnime skelAnime;
    Vec3s     jointTable[PLAYER_LIMB_BUF_COUNT];
    Vec3s     morphTable[PLAYER_LIMB_BUF_COUNT];

    // Owner-client bookkeeping. Set by Anchor at spawn time.
    u32 ownerClientId;
    u32 netId;

    // State machine.
    s32                  state;
    EnFollowerActionFunc actionFunc;

    // Linkage age — matches gSaveContext.linkAge at spawn time. Drives skel
    // variant selection.
    s8 linkAge;

    // Cosmetic state to feed Player_DrawImpl. v1 reads the local player's
    // values via GET_PLAYER(play) at draw time and stores them here for
    // diagnostics; future phases may track them independently per NPC.
    s8 currentTunic;
    s8 currentBoots;
    s8 currentFace;

    // v2 reserved (combat redesign): health, downed timer, etc. Sized to
    // ensure forward-compat with the FOLLOWER_NPC_STATE packet schema's
    // `health` + `deathFlag` fields without struct migration later.
    // Health (Stage 1 of npc_follower_health_and_respawn_plan).
    // Range [0, kFollowerNpcMaxHealth] = 0..4. Starts at max on
    // spawn. Damage application gated by
    // `gEnhancements.AI.FollowerNPC.Invulnerable` CVar (default ON
    // — preserves prior behavior). Death triggers and combat damage
    // come in Stage 2+.
    s8 health;
    // Death-state flag — set to 1 when NPC has died (Stage 2). Sent
    // in FOLLOWER_NPC_STATE so peers play matching anim. Reserved
    // for now; Stage 1 only writes 0.
    u8 deathFlag;

    // Last animation kind set on this actor (FollowerNpcAnim enum value
    // stored as int; matches the C++ enum class FollowerNpcAnim). Per-
    // actor (NOT a file-scope global) so peer replicas track their own
    // animation independently of the local owner. Init = 0 = kNone =
    // "not yet ensured" — first EnsureAnimation call will fire.
    s32 currentAnim;
    // Last modelAnimType used when picking the current anim. Mirrors
    // Player.modelAnimType — 0=unarmed/free, 1/2=fighter (sword+
    // shield), 3=long sword (two-handed). EnsureAnimation re-fires
    // when this changes so swapping sword in/out re-picks the right
    // variant (e.g. walk_free → walk when sword+shield drawn).
    s8 currentAnimType;

    // Owner-broadcast speed signal (for peers). Local NPC writes its
    // own speedXZ; peer NPC reads the value broadcast in
    // FOLLOWER_NPC_STATE so it can pick walk vs run animation. Reset
    // to 0 on init.
    f32 syncedSpeedXZ;

    // ── Animation polish (audit 2026-05-16) ──────────────────────────
    // Step phase counter — Player calls this unk_868 (z_player.c:8105).
    // Tracks position in the 29-unit walk/run step cycle so we know
    // which foot is forward. Used by:
    //   - walk_endL/R stop anim selection (phase < 14 → endL, else endR).
    //   - Footstep SFX trigger (phase crosses 10 or 24 = foot-down frame).
    // Ticked by `playSpeed * R_UPDATE_RATE * 0.5` per frame, wrapped at 29.
    f32 stepPhase;
    // Set true while a one-shot stop anim (walk_endL/R) is playing in
    // IDLE state. Blocks EnsureAnimation from switching to kWait until
    // the stop anim completes (LinkAnimation_Update returns true).
    u8  stopAnimPlaying;
    // Previous tick's state. Used to detect FOLLOW→IDLE transition so
    // the stop anim fires only on the transition frame, not every
    // frame thereafter.
    s32 prevState;

    // Idle fidget timing. Counts ticks while the NPC has been
    // playing kWait. After kFidgetIntervalTicks, the dispatcher
    // swaps to a one-shot fidget anim (look-around / warm / stretch),
    // resets to 0, and cycles `nextFidgetIdx`. Adds the look-around
    // / weight-shift variety Link himself has in long idle.
    u32 idleTicks;
    u8  nextFidgetIdx;

    // Idle blend phase. For waitL ↔ waitR blending in the idle anim.
    // Player's pattern: blend weight `unk_870` steps toward target
    // `unk_874` (0 or 1) at 0.3/frame (z_player.c:8058). NPC simplifies
    // to a free-running sine so the breathing motion oscillates
    // continuously without needing an external target driver.
    f32 idleBlendPhase;
    // 32-aligned blend table for LinkAnimation_BlendToJoint. Sized to
    // PLAYER_LIMB_BUF_COUNT — same as jointTable. +1 to allow for the
    // ALIGN16 manipulation inside LinkAnimation_BlendToJoint (which
    // takes an unaligned pointer and rounds up).
    Vec3s blendTable[PLAYER_LIMB_BUF_COUNT];

    // Independent head-look-at-leader. We pass localPlayer as the
    // override-callback thisx (so equipment-draw resolves), which
    // means the NPC's head normally mirrors the local player's
    // headLimbRot. To make the NPC's head turn independently toward
    // its OWN leader, the NPC's draw path saves/swaps/restores
    // localPlayer's headLimbRot/upperLimbRot around the Player_DrawImpl
    // call (same save/swap/restore pattern as face textures and
    // tunic color). These fields hold the NPC's own pose, computed
    // each tick toward leader's relative direction.
    Vec3s headLimbRot;
    Vec3s upperLimbRot;

    // Ledge-hoist state. Captured at entry; consumed at anim
    // completion. hoistContext picks the anim variant
    // (ground-climb vs swim-step-up). hoistTargetPos is the ledge
    // top — the snap target when the one-shot anim finishes.
    // hoistEntryYaw locks NPC facing throughout the hoist.
    s8    hoistContext;
    Vec3f hoistTargetPos;
    s16   hoistEntryYaw;
} EnFollower;

#ifdef __cplusplus
extern "C" {
#endif

// Dynamic actor id allocated by ActorDB::AddBuiltInCustomActors. Defined in
// soh/src/code/z_play.c alongside gEnPartnerId.
extern s16 gEnFollowerId;

void EnFollower_Init(Actor* thisx, PlayState* play);
void EnFollower_Destroy(Actor* thisx, PlayState* play);
void EnFollower_Update(Actor* thisx, PlayState* play);
void EnFollower_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
