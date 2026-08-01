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
// Custom projectile actor id — registered via ActorDB::AddBuiltIn
// CustomActors, declared in soh/src/code/z_play.c.
extern s16 gEnDekubabaAcidId;
}

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
// Detach trigger: post-attack, +25% chance per attack where Link was
// out of lunge range (≥240u XZ). One-shot per actor life (Dekubaba
// doesn't regrow after detach death). Simple u8 counter, not
// ChargeStateMachine — no cooldown needed for a one-shot event.
constexpr float kDetachChancePerCounter = 0.25f;
constexpr u8    kDetachChanceMaxCounter = 4;
constexpr float kDetachRangeThresholdXZ = 240.0f;  // Link must be beyond this XZ
                                                    // (matches plan §Feature B "trigger condition")

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
    // isDetached is sticky per actor life. Once host sets true, the
    // Dekubaba is permanently in detach/squirm/dying flow — attack
    // cycle counters become irrelevant. Cleared only on OnDeath (which
    // fires when Actor_Kill is invoked, i.e. after squirm-dying
    // completes — but by then the actor is being destroyed so the
    // state map entry gets erased via OnActorDestroy anyway).
    //
    // Wire-mirror: netDetachActive lives on EnemyNetId::dekubaba and
    // OnPeerReceiveDetachActiveFlag writes it here for local reads.
    bool isDetached          = false;
    // Detach chance counter — u8 0..kDetachChanceMaxCounter (4).
    // Chance = counter * 25%. Advances +1 on each attack that
    // completed with Link out of lunge range. Reset by OnDeath.
    u8   detachChanceCounter = 0;
    // Timer state for the squirm state.
    int  squirmFrameCounter  = 0;  // 0..∞, drives sine phase + bleedout
    int  lastBleedoutFrame   = 0;  // wall-clock game-tick of last -1 HP
    // Timer for DetachedDying (frames since state entry).
    int  dyingFrameCounter   = 0;

    // Peer-received detach flag (mirror of netAcidActive shape).
    bool netDetachActive = false;
};

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

    // Attack-cycle end — decrement Cooldown (may transition back
    // to Charging). Clear per-attack flags for the next cycle.
    state.acidCharge.OnAttackComplete();
    state.currentAttackIsAcid   = false;
    state.acidProjectileSpawned = false;
    state.acidAttackFrame       = 0;

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
    state.isDetached          = false;
    state.detachChanceCounter = 0;
    state.squirmFrameCounter  = 0;
    state.lastBleedoutFrame   = 0;
    state.dyingFrameCounter   = 0;
    state.netDetachActive     = false;
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
    (void)play;

    // Sync-rule 1 — host is sole RNG decider.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return false;

    // CVar gate (host-authoritative via enforced registry).
    if (AnchorCVarSync::GetEnforcedInt(DetachAndPursueCVarName(), 0) == 0) {
        return false;
    }

    // One-shot per life — already detached, don't roll again.
    DekubabaEnhancedState& state = GetOrCreate(actor);
    if (state.isDetached) return false;

    // Trigger condition: Link out of lunge range at attack time (per
    // plan §Feature B). Uses xzDistToPlayer patched to nearest player
    // (Anchor #153 overlay), then scaled by actor->size to match
    // vanilla lunge-range convention (240u * size).
    const float outOfRangeThreshold = kDetachRangeThresholdXZ * actor->size;
    if (actor->actor.xzDistToPlayer < outOfRangeThreshold) {
        // Link is within lunge range — normal attack, don't count
        // toward detach chance ramp.
        return false;
    }

    // Roll chance = counter × 25%.
    const float chance = (float)state.detachChanceCounter *
                          kDetachChancePerCounter;
    if (Rand_ZeroOne() < chance) {
        // Fires — set sticky flag. Caller transitions to DetachedSquirm.
        state.isDetached         = true;
        state.squirmFrameCounter = 0;
        state.lastBleedoutFrame  = (int)play->gameplayFrames;
        return true;
    }

    // Fail — increment counter, clamped.
    if (state.detachChanceCounter < kDetachChanceMaxCounter) {
        state.detachChanceCounter++;
    }
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
    Actor* target = Anchor_GetNearestPlayerActor(&actor->actor, play);
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

}  // namespace AnchorEnemyEnhancement
