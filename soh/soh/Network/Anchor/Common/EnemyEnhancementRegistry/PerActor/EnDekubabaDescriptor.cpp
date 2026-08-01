/**
 * EnDekubabaDescriptor implementation — Feature A (acid vomit).
 *
 * Feature B / C are stubbed; extending them layers additional
 * ChargeStateMachine instances + booleans on this descriptor.
 *
 * MP model (mirror of Karebaba V6 pattern):
 *   Host: OnHostMaybeAcidLunge rolls state machine + range gate;
 *         if fires → sets state.currentAttackIsAcid = true.
 *         Send-side (EnemyState.cpp) reads via IsCurrentAttackAcid()
 *         and IsAcidCharged() and adds the two bools to the outgoing
 *         payload.
 *   Peer: HookHandlers calls OnPeerReceive*Flag BEFORE ApplyNetState.
 *         Then the local ApplyNetState(state 14 = AcidVomit) sees
 *         the flags and runs the acid path.
 *   Both: OnAcidVomitTick runs each frame during the acid state;
 *         spawns EN_DEKUBABA_ACID projectile at fire frame. Spawn
 *         is deterministic (fixed position + fixed frame) — no
 *         per-spawn broadcast needed. Damage via Path A when the
 *         projectile's AC collider hits a DummyPlayer.
 */

#include "soh/Network/Anchor/Anchor.h"  // Pitfall 40

#include "EnDekubabaDescriptor.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/Common/PlayerLookup.h"  // FindNearestPlayerActor
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/AcidVisuals.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementAudio.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/ChargeStateMachine.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GroundFollow.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
// Custom projectile actor ids — registered via ActorDB::AddBuiltIn
// CustomActors, declared in soh/src/code/z_play.c.
extern s16 gEnDekubabaAcidId;
extern s16 gEnDekubabaSeedId;
}

#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // FindNearestFloorNodeXZRadius

namespace AnchorEnemyEnhancement {

namespace {

// Charge state machine — matches Karebaba pattern (25% steps, max 4,
// 3-attack cooldown). Rolls on each DecideLunge → attack transition.
constexpr ChargeStateMachine::Config kAcidChargeConfig = {
    /*stepIncrement*/ 0.25f,
    /*maxCounter*/    4,
    /*cooldownSteps*/ 3,
};

// Range gate — acid is useful when Link is outside melee lunge range
// but within a plausible arc distance. Melee lunge ends at ~80u*size.
// Acid range 120..300u XZ — Link too close makes acid trivially
// dodgeable; too far and the arc math gets silly.
constexpr float kAcidMinRangeXZ = 100.0f;
constexpr float kAcidMaxRangeXZ = 400.0f;

// Vanilla Dekubaba scale at Init: this->size × 0.01 (where size is
// 1.0 for NORMAL, ~2.0 for BIG). Multiplier used during the acid
// telegraph to grow the head 1.5×.
constexpr float kTelegraphHeadScale = 1.5f;
constexpr int   kTelegraphSpitPeriod = 4;  // spawn every N frames

// Acid attack timing (per plan §Feature A Design "~15-20 frames"):
//   Frames 0-15  — telegraph (head grows, mouth spits, stem rears)
//   Frame 15     — projectile spawn (host + peer both spawn locally)
//   Frames 15-25 — snap forward (post-spit follow-through)
//   Frame 25     — transition to PullBack (vanilla-parity recovery)
constexpr int kAcidTelegraphEndFrame = 15;
constexpr int kAcidSpawnFrame        = 15;
constexpr int kAcidTotalFrames       = 25;

// Projectile spawn — velocity computed toward player XZ at spawn
// time (moving-target-not-tracked per plan). Y velocity gives a
// shallow arc; gravity pulls it down. Tuned so the projectile
// reaches ~300u XZ before dropping to ground.
constexpr float kAcidSpitInitialXZSpeed = 8.0f;
constexpr float kAcidSpitInitialYSpeed  = 4.0f;

// ---- Feature B (#309) — detach + pursue tuning ---------------------
//
// UNIFIED PATTERN (per user 2026-07-31): detach uses the same
// ChargeStateMachine config as acid — 25% steps, max 4, 3-attack
// cooldown. Roll site moved to DecideLunge (same as acid, evaluated
// after acid fails). No range gate — fires purely on counter roll.
// One-shot per actor life via sticky isDetached flag; the ChargeState
// cooldown never activates because the actor transitions to
// DetachedSquirm and never runs another DecideLunge.
constexpr ChargeStateMachine::Config kDetachChargeConfig = {
    /*stepIncrement*/ 0.25f,
    /*maxCounter*/    4,
    /*cooldownSteps*/ 3,
};

// Squirm motion (DetachedSquirm state).
//   XZ speed 2.0u/frame — slow crawl. Not fast enough to catch
//   sprinting Link; requires Link to engage.
//   Stem-angle sine amplitude 0x1000 (~5.6°) with 3-segment phase
//   offset 0°/120°/240° for a serpentine wave.
constexpr float kSquirmSpeedXZ            = 2.0f;
constexpr s16   kSquirmStemAmplitude      = 0x1000;
constexpr s16   kSquirmStemBase           = 0x0800;
constexpr float kSquirmPhasePerFrame      = 0.10472f;  // 2π/60 → 60-frame period
constexpr int   kBleedoutIntervalMs       = 5000;      // -1 HP every 5s

// DetachedDying state.
//   ShrinkDie animation duration ~30 frames vanilla; give 40 for a
//   slightly slower squirm-death visual.
constexpr int   kDetachedDyingFrames      = 40;

// ---- Feature C (#318) — seed spawn tuning --------------------------
//
// UNIFIED PATTERN — same ChargeStateMachine config as acid + detach
// (25% steps, max 4, 3-attack cooldown). Consistent with user 2026-
// 07-31 spec: "all three ... same karebaba 25% increase per attack".
constexpr ChargeStateMachine::Config kSeedChargeConfig = {
    /*stepIncrement*/ 0.25f,
    /*maxCounter*/    4,
    /*cooldownSteps*/ 3,
};

// Seed telegraph + fire timing. Matches acid cycle length so both
// exotics slot into the same DecideLunge → attack → PullBack → Recover
// sequence positions.
constexpr int kSeedTelegraphEndFrame = 15;
constexpr int kSeedFireFrame         = 15;   // spawn projectile at this frame
constexpr int kSeedTotalFrames       = 25;

// Landing pick — behind Link, along Link's facing. Distance beyond
// Link puts the child in Link's blind spot forcing reposition.
constexpr float kSeedBehindLinkOffset = 100.0f;

// Max seed flight distance from Dekubaba (safety cap on where the
// projectile can spawn a child). Extended from plan's 300u to 400u
// per unified-pattern discussion to accommodate behind-Link firings
// at medium Dekubaba-Link distances.
constexpr float kSeedMaxFlightDistance = 400.0f;

// Fallback landing radius when behind-Link landing fails nav-validation.
// Random XZ within this radius of the Dekubaba — always fires (never
// skip) per user 2026-07-31 spec.
constexpr float kSeedFallbackRadius   = 300.0f;

// Nav-validation radius when checking landing target for a valid
// floor node.
constexpr float kSeedNavRadius        = 60.0f;

// Seed projectile flight speed (units/frame). Straight-line trajectory
// from head to landing; time-of-flight = distance / speed.
constexpr float kSeedProjectileSpeed  = 12.0f;

// Per-actor state map. Same shape as Karebaba's — created on demand
// at first hook fire, wiped on OnDeath / OnActorDestroy.
struct DekubabaEnhancedState {
    // Per-attack flags (reset at OnAttackComplete).
    bool currentAttackIsAcid   = false;
    bool acidProjectileSpawned = false;
    int  acidAttackFrame       = 0;   // 0..kAcidTotalFrames

    // Charge state machine — persists across attacks, reset on death.
    ChargeStateMachine acidCharge{ kAcidChargeConfig };

    // Peer-received flags. Host writes these from its own charge
    // state; peer's ChargeStateMachine is stale (host is sole roller).
    bool netAcidActive  = false;
    bool netAcidCharged = false;

    // ---- Feature B (#309) — detach state --------------------------
    //
    // UNIFIED PATTERN (2026-07-31): detach uses same ChargeStateMachine
    // shape as acid — rolled at DecideLunge, 25% steps, 3-attack
    // cooldown. On fire, sets isDetached=true and transitions actor
    // out of the attack cycle permanently, so the cooldown never
    // actually decrements (actor never returns to DecideLunge). The
    // ChargeStateMachine is used for pattern uniformity even though
    // it's effectively one-shot.
    ChargeStateMachine detachCharge{ kDetachChargeConfig };

    // Sticky per-life flag — true after detach fires. Peer's Draw
    // gate + local SetupDetachedSquirm path both read via IsDetached
    // bridge query.
    bool isDetached          = false;

    // Timer state for the squirm/dying states.
    int  squirmFrameCounter  = 0;  // 0..∞, drives sine phase + bleedout
    int  lastBleedoutFrame   = 0;  // wall-clock game-tick of last -1 HP
    int  dyingFrameCounter   = 0;

    // Peer-received detach flag (mirror of netAcidActive shape).
    bool netDetachActive = false;

    // ---- Feature C (#318) — seed spawn state ----------------------
    //
    // Seed charge state machine (25% steps, max 4, 3-attack cooldown).
    ChargeStateMachine seedCharge{ kSeedChargeConfig };

    // Sticky flag — this actor was spawned by a parent Dekubaba's
    // seed. Prevents child from seeding (unless SeedChildrenCanSeed
    // CVar is on). Set at Init when the g_isSpawningDekubabaChild
    // thread-local flag is true.
    bool isSpawnedByEnhancement = false;

    // Own-child tracking (Q2 answer 2026-07-31: parent tracks own
    // child only). Stores the netId of the currently-alive child
    // spawned by THIS parent's seed. 0 = no active child. Cleared
    // when child is destroyed (host walks actor list each seed
    // roll to verify).
    Actor* spawnedChildActor = nullptr;

    // Per-attack seed flags (reset at OnAttackComplete).
    bool currentAttackIsSeed = false;
    bool seedProjectileSpawned = false;
    int  seedAttackFrame       = 0;

    // Seed landing target — computed at fire decision, shipped over
    // wire so peer's projectile arcs to the same coord.
    Vec3f seedLandingPos = { 0.0f, 0.0f, 0.0f };

    // Peer-received seed active flag.
    bool netSeedActive = false;
};

// Thread-local flag set by OnSeedLanded's Actor_Spawn bracket, read
// by OnActorInit on the newly-spawned Dekubaba. Same pattern as
// g_isSpawningNetworkItemDrop (session_state Pillar C2 Phase 4 §3.1).
thread_local bool g_isSpawningDekubabaChild = false;

std::unordered_map<Actor*, DekubabaEnhancedState> sStates;

DekubabaEnhancedState& GetOrCreate(EnDekubaba* actor) {
    return sStates[&actor->actor];
}

// Telegraph render — head grows, mouth spits, matching Karebaba's
// RenderTelegraph pattern but scaled for Dekubaba's larger vanilla
// head + longer telegraph window.
inline void RenderAcidTelegraph(EnDekubaba* actor, PlayState* play) {
    // Head at 1.5× (Dekubaba's baseline scale = this->size × 0.01).
    const f32 baseScale = actor->size * 0.01f;
    Actor_SetScale(&actor->actor, baseScale * kTelegraphHeadScale);

    // Mouth spit every N frames using shared AcidVisuals config.
    if ((play->gameplayFrames % kTelegraphSpitPeriod) == 0) {
        Color_RGBA8 primC = AcidVisuals::kSpitPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kSpitEnvColor;
        // Spawn at head position. Dekubaba's world.pos IS the head
        // tip (per session_state Fix 7 — world.pos is animation-
        // computed, not stem base). Small XZ jitter for organic feel.
        Vec3f pos = {
            actor->actor.world.pos.x + (Rand_ZeroOne() - 0.5f) * 8.0f,
            actor->actor.world.pos.y + (Rand_ZeroOne() - 0.5f) * 4.0f,
            actor->actor.world.pos.z + (Rand_ZeroOne() - 0.5f) * 8.0f,
        };
        EffectSsGSplash_Spawn(play, &pos, &primC, &envC,
                                AcidVisuals::kSpitSplashType,
                                AcidVisuals::kSpitSplashScale);
    }
}

}  // namespace

// ---- Feature A implementation ---------------------------------------

bool EnDekubabaDescriptor::OnHostMaybeAcidLunge(EnDekubaba* actor,
                                                  PlayState* play) {
    if (actor == nullptr) return false;
    (void)play;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate (host-authoritative via enforced registry).
    if (AnchorCVarSync::GetEnforcedInt(AcidVomitCVarName(), 0) == 0) {
        // CVar off — clear any stale per-attack flags so nothing
        // leaks into an unenhanced attack.
        auto it = sStates.find(&actor->actor);
        if (it != sStates.end()) {
            it->second.currentAttackIsAcid   = false;
            it->second.acidProjectileSpawned = false;
            it->second.acidAttackFrame       = 0;
        }
        return false;
    }

    DekubabaEnhancedState& state = GetOrCreate(actor);
    // Clear per-attack flags at each decision point — either acid
    // fires this attack or it doesn't, no residual state.
    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;

    // Ready branch: charged + Link in valid range → fire acid.
    if (state.acidCharge.IsReady()) {
        const f32 dist = actor->actor.xzDistToPlayer;
        if (dist >= kAcidMinRangeXZ && dist <= kAcidMaxRangeXZ) {
            state.acidCharge.OnFire();
            state.currentAttackIsAcid = true;
            // "Sizzle" SFX at attack commit — subtle audio cue.
            EnhancementAudio::PlayBoostedActorSfx(
                &actor->actor, NA_SE_EV_WATER_BUBBLE);
            return true;
        }
        // Out of range — Ready preserved. Vanilla lunge proceeds.
        return false;
    }

    // Charging branch: TryCharge rolls chance = counter*25%. On
    // Charging → Ready transition returns true; play "charging" SFX
    // at that moment.
    if (state.acidCharge.TryCharge()) {
        EnhancementAudio::PlayBoostedActorSfx(
            &actor->actor, NA_SE_EV_WATER_BUBBLE);
    }
    return false;  // Charging state doesn't fire acid this attack.
}

void EnDekubabaDescriptor::OnPeerReceiveAcidActiveFlag(EnDekubaba* actor,
                                                        bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.netAcidActive = active;
    // If newly-enabling for this attack, reset per-attack counters
    // so peer's local AcidVomit fires the sequence from scratch.
    if (active) {
        state.currentAttackIsAcid   = true;
        state.acidProjectileSpawned = false;
        state.acidAttackFrame       = 0;
    }
}

void EnDekubabaDescriptor::OnPeerReceiveAcidChargedFlag(EnDekubaba* actor,
                                                         bool charged) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.netAcidCharged = charged;
}

void EnDekubabaDescriptor::OnAcidVomitTick(EnDekubaba* actor,
                                             PlayState* play, int frame) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // Render telegraph particles through the whole cycle for
    // continuous visual (matches Karebaba V7 telegraph-persists
    // pattern).
    if (frame <= kAcidTelegraphEndFrame) {
        RenderAcidTelegraph(actor, play);
    }

    // Fire projectile at kAcidSpawnFrame — both host and peer spawn
    // locally with deterministic position + velocity.
    if (!state.acidProjectileSpawned && frame >= kAcidSpawnFrame) {
        state.acidProjectileSpawned = true;
        if (gEnDekubabaAcidId != 0) {
            // Compute velocity toward player XZ at spawn moment.
            // Moving-target intentionally not tracked per plan
            // "vanilla-lunge-parity".
            const s16 aimYaw = actor->actor.yawTowardsPlayer;
            const f32 vx = Math_SinS(aimYaw) * kAcidSpitInitialXZSpeed;
            const f32 vz = Math_CosS(aimYaw) * kAcidSpitInitialXZSpeed;
            // Encode velocity in the actor params bit-field so the
            // new actor's Init reads it: high 5 bits = signed Y vel
            // (unused here; Y starts fixed), low 11 bits = aim yaw
            // in 16ths of a degree (approx). Actually simpler: the
            // spawned actor reads projectedPos + parent home.pos and
            // computes its own initial velocity. Params carry only
            // the aim yaw quantized to s16 range.
            const s16 params = (s16)(aimYaw / 8);  // ~0.7°/step
            Actor_Spawn(&play->actorCtx, play,
                         gEnDekubabaAcidId,
                         actor->actor.world.pos.x,
                         actor->actor.world.pos.y,
                         actor->actor.world.pos.z,
                         0, aimYaw, 0, params);
            (void)vx; (void)vz;
        }
        // Spawn SFX — "eruption cloud" fires at the moment the
        // projectile launches.
        EnhancementAudio::PlayDoubledActorSfx(
            &actor->actor, NA_SE_EV_ERUPTION_CLOUD);
    }
}

void EnDekubabaDescriptor::OnAttackComplete(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // UNIFIED PATTERN (per user 2026-07-31): all three ChargeState
    // machines advance at attack-complete regardless of which fired.
    // Always-advance prevents starvation between competing exotics.
    state.acidCharge.OnAttackComplete();
    state.detachCharge.OnAttackComplete();
    state.seedCharge.OnAttackComplete();

    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;
    state.currentAttackIsSeed   = false;
    state.seedProjectileSpawned = false;
    state.seedAttackFrame       = 0;

    // Restore scale to vanilla in case telegraph was mid-render at
    // cycle end (defensive; the AcidVomit exit path should also
    // reset).
    Actor_SetScale(&actor->actor, actor->size * 0.01f);
}

bool EnDekubabaDescriptor::IsCurrentAttackAcid(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.currentAttackIsAcid;
}

bool EnDekubabaDescriptor::IsAcidCharged(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.acidCharge.IsReady();
}

void EnDekubabaDescriptor::OnDeath(EnDekubaba* actor) {
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;
    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;
    state.acidCharge.Reset();
    state.netAcidActive         = false;
    state.netAcidCharged        = false;
    // Feature B — detach state reset. Note: for vanilla Dekubaba this
    // reset doesn't take effect because Dekubaba doesn't regrow (dies
    // and stays dead until scene reload). OnActorDestroy erases the
    // state entry entirely. Kept for symmetry + safety in case the
    // reset is ever needed for a regrow variant.
    state.isDetached         = false;
    state.detachCharge.Reset();
    state.squirmFrameCounter = 0;
    state.lastBleedoutFrame  = 0;
    state.dyingFrameCounter  = 0;
    state.netDetachActive    = false;
    // Feature C — seed state reset.
    state.seedCharge.Reset();
    state.currentAttackIsSeed   = false;
    state.seedProjectileSpawned = false;
    state.seedAttackFrame       = 0;
    state.spawnedChildActor     = nullptr;
    state.netSeedActive         = false;
    // Note: isSpawnedByEnhancement is NOT reset — it's a sticky
    // per-life property of the actor identity (was this Dekubaba
    // spawned by a seed?), unaffected by death.
    // Restore vanilla scale.
    Actor_SetScale(&actor->actor, actor->size * 0.01f);
}

void EnDekubabaDescriptor::OnActorDestroy(EnDekubaba* actor) {
    if (actor == nullptr) return;
    sStates.erase(&actor->actor);
}

// ---- Feature B (#309) — detach + pursue --------------------------

bool EnDekubabaDescriptor::OnHostMaybeDetach(EnDekubaba* actor, PlayState* play) {
    if (actor == nullptr) return false;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate (host-authoritative via enforced registry).
    if (AnchorCVarSync::GetEnforcedInt(DetachAndPursueCVarName(), 0) == 0) {
        return false;
    }

    // One-shot per life — already detached, don't roll again.
    DekubabaEnhancedState& state = GetOrCreate(actor);
    if (state.isDetached) return false;

    // UNIFIED PATTERN (per user 2026-07-31): no range gate. Roll
    // purely on charge counter (matches acid + seed shape).
    // Ready branch: fire if state machine says Ready.
    if (state.detachCharge.IsReady()) {
        state.detachCharge.OnFire();
        state.isDetached         = true;
        state.squirmFrameCounter = 0;
        state.lastBleedoutFrame  = (int)play->gameplayFrames;
        return true;
    }

    // Charging branch: TryCharge rolls chance = counter × 25%.
    state.detachCharge.TryCharge();
    return false;
}

void EnDekubabaDescriptor::OnPeerReceiveDetachActiveFlag(EnDekubaba* actor,
                                                          bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.netDetachActive = active;
    // Mirror into isDetached so peer's Draw / OnDetachedSquirmTick
    // read the right value regardless of authority origin.
    if (active && !state.isDetached) {
        state.isDetached         = true;
        state.squirmFrameCounter = 0;
    }
}

void EnDekubabaDescriptor::OnDetachedSquirmTick(EnDekubaba* actor,
                                                  PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);

    // Serpentine motion — sine wave on stem angles with 120° phase
    // offset per segment. Feeds vanilla EnDekubaba_UpdateHeadPosition
    // (which the C actionFunc calls after this tick) → head bobs
    // wave-like.
    const float phase = (float)state.squirmFrameCounter * kSquirmPhasePerFrame;
    actor->stemSectionAngle[0] = kSquirmStemBase +
        (s16)(sinf(phase) * (float)kSquirmStemAmplitude);
    actor->stemSectionAngle[1] = kSquirmStemBase +
        (s16)(sinf(phase + (float)M_PI * 2.0f / 3.0f) *
              (float)kSquirmStemAmplitude);
    actor->stemSectionAngle[2] = kSquirmStemBase +
        (s16)(sinf(phase + (float)M_PI * 4.0f / 3.0f) *
              (float)(kSquirmStemAmplitude / 2));  // tail tapered

    // Face nearest player + move forward.
    Actor* target = FindNearestPlayerActor(&actor->actor, play);
    if (target != nullptr) {
        const s16 targetYaw = Math_Vec3f_Yaw(&actor->actor.world.pos,
                                              &target->world.pos);
        Math_ScaledStepToS(&actor->actor.shape.rot.y, targetYaw, 0x400);
        actor->actor.world.rot.y = actor->actor.shape.rot.y;
    }
    actor->actor.speedXZ  = kSquirmSpeedXZ;
    actor->actor.velocity.x = Math_SinS(actor->actor.world.rot.y) *
                               kSquirmSpeedXZ;
    actor->actor.velocity.z = Math_CosS(actor->actor.world.rot.y) *
                               kSquirmSpeedXZ;

    // Ground-follow — snap Y to floor each tick so the squirming form
    // stays on ground even on gentle slopes. Uses shared helper. Body
    // offset 0 (Dekubaba head-base sits at floor level naturally).
    GroundFollow::ProbeAndSnap(&actor->actor, play, /*bodyOffset=*/0.0f);

    // Bleedout — host-only decrement HP every 5 seconds. Peer sees
    // the health change via ENEMY_STATE health field naturally.
    if (SceneAuthority::IsMyCurrentRoomHost() && Anchor::Instance != nullptr) {
        const int nowFrame = (int)play->gameplayFrames;
        const int intervalTicks = Anchor::Instance->MsToGameTicks(
                                    kBleedoutIntervalMs);
        if (intervalTicks > 0 &&
            (nowFrame - state.lastBleedoutFrame) >= intervalTicks) {
            state.lastBleedoutFrame = nowFrame;
            if (actor->actor.colChkInfo.health > 0) {
                actor->actor.colChkInfo.health--;
            }
        }
    }

    state.squirmFrameCounter++;
}

void EnDekubabaDescriptor::OnDetachedDyingTick(EnDekubaba* actor,
                                                 PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    (void)play;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.dyingFrameCounter++;
    // Actor_Kill decision + drop spawning stays in the vanilla C
    // actionFunc — this tick just increments the counter so the C
    // side can time animation length. Vanilla ShrinkDie animation
    // handles the shrink visual; the C side plays it via
    // Animation_Change with reverse playSpeed.
}

bool EnDekubabaDescriptor::IsDetached(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.isDetached;
}

// ---- Feature C (#318) — seed spawn --------------------------------

namespace {

// Compute the "behind Link" landing target per user 2026-07-31 spec:
// Link.pos + Link.forward * kSeedBehindLinkOffset. If that position
// is beyond kSeedMaxFlightDistance from the Dekubaba, clamp along
// Dekubaba-to-target bearing to max distance.
Vec3f ComputeBehindLinkTarget(EnDekubaba* actor, Actor* target) {
    Vec3f behind;
    if (target == nullptr) {
        behind = actor->actor.world.pos;
        return behind;
    }
    // Link's forward direction from world.rot.y.
    const s16 linkYaw = target->world.rot.y;
    behind.x = target->world.pos.x +
               Math_SinS(linkYaw) * kSeedBehindLinkOffset;
    behind.y = target->world.pos.y;
    behind.z = target->world.pos.z +
               Math_CosS(linkYaw) * kSeedBehindLinkOffset;

    // Clamp to max flight distance from Dekubaba.
    const float dx = behind.x - actor->actor.world.pos.x;
    const float dz = behind.z - actor->actor.world.pos.z;
    const float distXZ = sqrtf(dx * dx + dz * dz);
    if (distXZ > kSeedMaxFlightDistance && distXZ > 0.001f) {
        const float ratio = kSeedMaxFlightDistance / distXZ;
        behind.x = actor->actor.world.pos.x + dx * ratio;
        behind.z = actor->actor.world.pos.z + dz * ratio;
    }
    return behind;
}

// Fallback landing: random XZ within kSeedFallbackRadius of Dekubaba.
Vec3f RandomFallbackTarget(EnDekubaba* actor) {
    const float u = Rand_ZeroOne();
    const float r = kSeedFallbackRadius * sqrtf(u);
    const float ang = Rand_ZeroOne() * 2.0f * (float)M_PI;
    Vec3f target = {
        actor->actor.world.pos.x + r * cosf(ang),
        actor->actor.world.pos.y,
        actor->actor.world.pos.z + r * sinf(ang),
    };
    return target;
}

// Check whether parent's spawnedChildActor is still alive. Uses
// actor pointer validity check — Actor_Kill sets actor->update to
// nullptr (session_state Fix 12 pattern).
bool IsChildAlive(Actor* childActor) {
    return childActor != nullptr && childActor->update != nullptr;
}

}  // namespace

bool EnDekubabaDescriptor::OnHostMaybeSeedFire(EnDekubaba* actor,
                                                 PlayState* play) {
    if (actor == nullptr || play == nullptr) return false;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate.
    if (AnchorCVarSync::GetEnforcedInt(SeedSpawnCVarName(), 0) == 0) {
        return false;
    }

    DekubabaEnhancedState& state = GetOrCreate(actor);

    // Child-not-active gate (per user Q3 2026-07-31: parent tracks
    // own child only). Skip if own child is still alive.
    if (IsChildAlive(state.spawnedChildActor)) {
        // Clear stale reference if child is dead so future rolls
        // work — check every roll rather than depending on child's
        // OnActorDestroy to walk back.
        state.spawnedChildActor = nullptr;
        return false;  // was alive last we checked — skip this attack
    }

    // Suppress on seed-spawned children (per user Q3): children
    // can run A + B but not C. Future toggle CVar
    // gEnhancements.Dekubaba.SeedChildrenCanSeed lets user override.
    if (state.isSpawnedByEnhancement) {
        // TODO: gate on SeedChildrenCanSeed CVar when that lands.
        return false;
    }

    // Ready branch: fire seed if state machine says Ready.
    if (state.seedCharge.IsReady()) {
        // Compute landing target — behind-Link primary, random
        // fallback if nav-invalid.
        Actor* target = FindNearestPlayerActor(&actor->actor, play);
        Vec3f landingTarget = ComputeBehindLinkTarget(actor, target);

        // Nav-validate. If failure, fallback to random XZ within
        // kSeedFallbackRadius of Dekubaba (per user Q3 2026-07-31:
        // never skip fire).
        // Note: nav-validation requires per-scene RoomNavData scan.
        // If room isn't scanned or lookup fails, we still fire —
        // fallback ensures the spawn happens somewhere reasonable.
        // TODO: wire RoomNavData::Instance query when the substrate
        // is confirmed populated. For v1 we skip nav-validation and
        // trust the landing target directly — a follow-up will add
        // proper AnchorNavRoom::FindNearestFloorNodeXZRadius call
        // once the RoomNavData accessor is verified.
        // Random fallback branch (always fire per user Q3):
        // if (nav-invalid) landingTarget = RandomFallbackTarget(actor);
        (void)RandomFallbackTarget;  // reference for future use

        state.seedCharge.OnFire();
        state.currentAttackIsSeed   = true;
        state.seedProjectileSpawned = false;
        state.seedAttackFrame       = 0;
        state.seedLandingPos        = landingTarget;

        // "Sizzle" SFX at commit — subtle audio cue similar to acid
        // charge.
        EnhancementAudio::PlayBoostedActorSfx(
            &actor->actor, NA_SE_EV_WATER_BUBBLE);
        return true;
    }

    // Charging branch: TryCharge advances counter.
    state.seedCharge.TryCharge();
    return false;
}

void EnDekubabaDescriptor::OnPeerReceiveSeedActiveFlag(EnDekubaba* actor,
                                                         bool active) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.netSeedActive = active;
    if (active) {
        state.currentAttackIsSeed   = true;
        state.seedProjectileSpawned = false;
        state.seedAttackFrame       = 0;
    }
}

void EnDekubabaDescriptor::OnPeerReceiveSeedLandingPos(EnDekubaba* actor,
                                                        float x, float y,
                                                        float z) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    state.seedLandingPos = { x, y, z };
}

void EnDekubabaDescriptor::OnSeedTelegraphTick(EnDekubaba* actor,
                                                  PlayState* play, int frame) {
    // Telegraph render is identical to acid — head 1.5× + mouth
    // spit. Reuse the same render path.
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    (void)it;
    // Render acid-style telegraph (same visual for both attacks —
    // player learns "big head + green spit = incoming exotic").
    // Delegate to RenderAcidTelegraph helper defined in Feature A
    // section (same file, anonymous namespace).
    if (frame <= kSeedTelegraphEndFrame) {
        RenderAcidTelegraph(actor, play);
    }
}

void EnDekubabaDescriptor::OnSeedFireTick(EnDekubaba* actor,
                                            PlayState* play, int frame) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    if (!state.seedProjectileSpawned && frame >= kSeedFireFrame) {
        state.seedProjectileSpawned = true;
        if (gEnDekubabaSeedId != 0) {
            // Spawn projectile at head; encode landing target in
            // actor spawn coordinates so the projectile can compute
            // its own straight-line velocity. Landing pos is shipped
            // via ENEMY_STATE wire field so peer's projectile arrives
            // at the same coord.
            //
            // Params encode: high 4 bits = discriminator (0 for
            // seed variant), low 12 bits = landing distance / 4u
            // quantized (400u max → 100 max quantized). Actor's Init
            // reads world.pos (spawn source) + delta from home.rot
            // (target vector packed) — simpler approach: pass full
            // Vec3f via new Anchor_SpawnDekubabaSeed helper (extern
            // "C" from actor's C code).
            //
            // For v1 simplicity: spawn actor at head pos, pass
            // landing X/Z via the actor's params + world.rot fields.
            // Actor computes trajectory in its Init.
            const float dx = state.seedLandingPos.x -
                             actor->actor.world.pos.x;
            const float dz = state.seedLandingPos.z -
                             actor->actor.world.pos.z;
            const s16 aimYaw = Math_Atan2S(dz, dx);
            // Note: Math_Atan2S expects (z, x) order for OoT convention.
            const s16 params = (s16)(aimYaw / 8);
            Actor_Spawn(&play->actorCtx, play,
                         gEnDekubabaSeedId,
                         actor->actor.world.pos.x,
                         actor->actor.world.pos.y,
                         actor->actor.world.pos.z,
                         0, aimYaw, 0, params);
        }
        EnhancementAudio::PlayDoubledActorSfx(
            &actor->actor, NA_SE_EV_ERUPTION_CLOUD);
    }
}

void EnDekubabaDescriptor::OnSeedLanded(EnDekubaba* parent, PlayState* play,
                                          float x, float y, float z) {
    if (parent == nullptr || play == nullptr) return;
    auto it = sStates.find(&parent->actor);
    if (it == sStates.end()) return;
    DekubabaEnhancedState& state = it->second;

    // Spawn a fresh EN_DEKUBABA at the landing coord. Bracket the
    // spawn with the g_isSpawningDekubabaChild thread-local so the
    // child's OnActorInit picks up the isSpawnedByEnhancement=true
    // flag.
    g_isSpawningDekubabaChild = true;
    Actor* childActor = Actor_Spawn(&play->actorCtx, play,
                                     ACTOR_EN_DEKUBABA,
                                     x, y, z,
                                     0, 0, 0,
                                     /*params=*/DEKUBABA_NORMAL);
    g_isSpawningDekubabaChild = false;

    // Track own child for the "child-not-active" gate.
    state.spawnedChildActor = childActor;
}

void EnDekubabaDescriptor::OnActorInit(EnDekubaba* actor) {
    if (actor == nullptr) return;
    DekubabaEnhancedState& state = GetOrCreate(actor);
    // Pick up the sticky "spawned by parent's seed" flag from the
    // thread-local. Set true once at Init; never reset.
    if (g_isSpawningDekubabaChild) {
        state.isSpawnedByEnhancement = true;
    }
}

bool EnDekubabaDescriptor::IsSeedActive(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.currentAttackIsSeed;
}

bool EnDekubabaDescriptor::IsSpawnedByEnhancement(EnDekubaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.isSpawnedByEnhancement;
}

float EnDekubabaDescriptor::GetSeedLandingX(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.x;
}
float EnDekubabaDescriptor::GetSeedLandingY(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.y;
}
float EnDekubabaDescriptor::GetSeedLandingZ(EnDekubaba* actor) {
    if (actor == nullptr) return 0.0f;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return 0.0f;
    return it->second.seedLandingPos.z;
}

}  // namespace AnchorEnemyEnhancement
