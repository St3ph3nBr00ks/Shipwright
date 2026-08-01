/**
 * EnKarebabaDescriptor — geyser AoE Spin enhancement.
 *
 * Full architecture / infinite-whys / 7-principles evaluation in
 * `Claude/Plans/en_karebaba_enhanced_plan.md`. This file implements
 * the descriptor virtual interface + the per-actor state map that
 * tracks whether each Karebaba's current Spin cycle is enhanced.
 *
 * MP model:
 *   - Host at SetupSpin: OnHostSetupSpin() rolls RNG. On success,
 *     writes state.currentSpinEnhanced = true. Send-side ENEMY_STATE
 *     bridge reads via IsCurrentSpinEnhanced() and includes the flag
 *     in the outgoing payload.
 *   - Peer at ENEMY_STATE receive: HookHandlers calls
 *     OnPeerReceiveEnhancedSpinFlag() BEFORE EnKarebaba_ApplyNetState.
 *     Sets state.currentSpinEnhanced = incoming flag.
 *   - Both clients per-frame in EnKarebaba_Spin: OnSpinTick() reads
 *     state.currentSpinEnhanced. If true: applies head-scale sinusoid
 *     to actor.scale + on frame 1, spawns EN_KAREBABA_GEYSER at
 *     actor.home.pos. Geyser is deterministic (spawn pos + frame
 *     both fixed) so both clients spawn locally — no per-spawn
 *     broadcast needed.
 *   - At Spin exit (SetupUpright): OnSpinExit() clears flag.
 *
 * Damage flow (Path A):
 *   - Geyser actor holds an AC-typed cylinder collider.
 *   - Host-side collision routes damage via DummyPlayer's normal
 *     ENEMY_HIT_PLAYER path — no new sync needed.
 */

// Pitfall 40 — Anchor.h FIRST.
#include "soh/Network/Anchor/Anchor.h"

#include "EnKarebabaDescriptor.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt
#include "soh/Network/Anchor/Common/SceneAuthority.h" // IsMyCurrentRoomHost

// Shared helpers extracted 2026-07-31 during Dekubaba prep — see the
// files under Common/EnemyEnhancementRegistry/ for the substrate.
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/AcidVisuals.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/EnhancementAudio.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/ChargeStateMachine.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"  // gSfxDefaultFreqAndVolScale, gSfxDefaultReverb
// Custom actor id for the geyser plume. Registered via
// ActorDB::AddBuiltInCustomActors; declared in soh/src/code/z_play.c.
extern s16 gEnKarebabaGeyserId;
}

namespace AnchorEnemyEnhancement {

namespace {

// V6 charge machine numbers (extracted into shared ChargeStateMachine
// 2026-07-31 during Dekubaba prep). Karebaba spec: 25% steps, max
// counter 4, 3-spin post-fire cooldown.
constexpr ChargeStateMachine::Config kAcidChargeConfig = {
    /*stepIncrement*/ 0.25f,
    /*maxCounter*/    4,
    /*cooldownSteps*/ 3,
};

// Per-actor enhancement state. Some fields reset at OnSpinExit;
// charge machine persists per V6 spec (reset on OnDeath +
// OnActorDestroy). Created on demand at OnHostSetupSpin /
// OnPeerReceiveEnhancedSpinFlag / OnUprightTick.
struct KarebabaEnhancedState {
    // Per-spin flags — reset at OnSpinExit.
    bool currentSpinEnhanced   = false;
    bool geyserSpawnedThisSpin = false;
    // Counts ground-splash bursts fired this spin cycle (0..5).
    u8   groundSplashesFired   = 0;

    // V6 charge state machine — persist across spins, reset on death.
    ChargeStateMachine acidCharge{ kAcidChargeConfig };

    // V6 — peer-received flag for the Ready telegraph. On peer, drives
    // OnUprightTick's decision to render 1.25× head + mouth spit.
    // Host writes this from its own charge state. Peer's charge
    // machine is stale (only host runs it) so this bool is the actual
    // telegraph source of truth on peer.
    bool                netCharged      = false;
};

// Keyed by Actor* — file-static, single-threaded (game thread only).
// Cleaned up on OnActorDestroy. Persistent scene-teardown cleanup
// isn't strictly required (small map size; entries clear when
// Actor_Kill destroys the Karebaba) but OnActorDestroy is wired
// for correctness.
std::unordered_map<Actor*, KarebabaEnhancedState> sStates;

KarebabaEnhancedState& GetOrCreate(EnKarebaba* actor) {
    return sStates[&actor->actor];
}

// Range gate — Link must be within 2 × cylinder radius (60u) = 120u
// XZ to actually USE the ready acid attack. Range gate does NOT
// affect charge roll (per user #2 clarification: chance ramps
// regardless of Link position).
constexpr float kAcidRangeXZ          = 60.0f * 2.0f;  // = 120u

// Telegraph parameters during Ready state (visible in Upright).
//   - Head scale: 1.25× (matches spin's mid-swell peak).
//   - Mouth spit: 1 GSplash every N frames — subtle continuous
//     visual signal. Same color/scale/type as attack-time spit
//     but at lower spawn rate.
constexpr float kTelegraphHeadScale    = 1.25f;
constexpr int   kTelegraphSpitPeriod   = 8;   // spawn every N Upright frames

// Spin animation duration (vanilla params counts 40 → 0).
constexpr float kSpinTotalFrames  = 40.0f;

// Vanilla Karebaba scale at Init: 0.01f (per z_en_karebaba.c).
// Multiplier applied to actor.scale during enhanced Spin.
constexpr float kVanillaScale = 0.01f;

// Sequenced geyser visual (2026-07-31 refactor v3 per user
// direction). Four phases across the 40-frame Spin cycle, tied to
// `frameF = 40 - actor.params`:
//
//   Phase 1  (f 0-40)  — Head-attached SPIT + supporting bubbles.
//                        Continuous throughout spin. Spit raised
//                        +5u Y so it originates from the mouth
//                        rather than slightly below the head.
//   Phase 2  (f 10-40) — Falling RAIN from Y=home+30 in a modest
//                        column. Starts 0.5s (10 frames at 20fps)
//                        after spit begins — reads as "the vomit
//                        that went up is now coming down".
//   Phase 3  (f 17-21) — 5 GROUND SPLASHES in rapid succession
//                        (1/frame × 5 frames) at random XZ around
//                        the plant. Timed to fire as the first
//                        falling rain drops reach the ground.
//                        Impact frame computed from rain velocity
//                        (-3) + gravity (-0.35) + 30u fall distance
//                        ≈ 7 frames of flight → f10 + f7 = f17.
//   Phase 4  (f 17-40) — Rising DUST from ground (repurposed —
//                        previously fell from above, now rises).
//                        Complements the rain-impact by looking
//                        like a caustic vapor kicked up on impact.

// Karebaba-specific tuning constants (not shared — dial-in for this
// actor's geyser attack). Non-tint constants live locally; the
// green-acid tint palette + splash type/scale + bubble/rain/dust
// particle attrs come from AcidVisuals.h (shared with Dekubaba).
//
// Phase 1 — mouth spit position + spawn density.
constexpr float kSpitYOffset          = 15.0f;
constexpr int   kSpitSplashesPerFrame = 2;
constexpr int   kRisingBubblesPerFrame = 1;

// Phase 2 — rain start frame (10 frames after spin begin) + spawn
// density + spawn column geometry.
constexpr int   kRainStartFrame       = 10;
constexpr int   kRainDropletsPerFrame = 3;
constexpr float kRainSpawnHeightY     = 130.0f;
constexpr float kRainSpawnRadius      = 30.0f;

// Phase 3 — ground splash burst starting at rain-impact frame.
// Impact frame derived from rain physics: 20-frame fall + f10 start
// = f30 impact. Total 5 splashes over 5 consecutive frames.
constexpr int   kGroundImpactStartFrame = 30;
constexpr u8    kGroundSplashTotalCount = 5;
constexpr float kGroundSplashRadius     = 40.0f;

// Phase 4 — rising dust from ground.
constexpr int   kRisingDustStartFrame   = 30;
constexpr int   kRisingDustPerFrame     = 2;
constexpr float kRisingDustSpawnYOffset = 5.0f;
constexpr float kRisingDustRadius       = 60.0f;

// V7 — telegraph render helper. Extracted so both OnUprightTick
// and OnSpinTick can call it (user spec: telegraph persists through
// Spin animation, only clears after acid actually fires). Sets
// head scale + spawns mouth spit every N frames.
inline void RenderTelegraph(EnKarebaba* actor, PlayState* play) {
    // Head at kTelegraphHeadScale (1.25× vanilla).
    Actor_SetScale(&actor->actor, kVanillaScale * kTelegraphHeadScale);

    // Mouth spit every kTelegraphSpitPeriod frames using shared
    // AcidVisuals splash config (type 2, scale 250 — proven visible-
    // green in Karebaba V7 plan-B).
    if ((play->gameplayFrames % kTelegraphSpitPeriod) == 0) {
        Color_RGBA8 primC = AcidVisuals::kSpitPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kSpitEnvColor;
        Vec3f pos = {
            actor->actor.world.pos.x + (Rand_ZeroOne() - 0.5f) * 6.0f,
            actor->actor.world.pos.y + kSpitYOffset +
                (Rand_ZeroOne() - 0.5f) * 4.0f,
            actor->actor.world.pos.z + (Rand_ZeroOne() - 0.5f) * 6.0f,
        };
        EffectSsGSplash_Spawn(play, &pos, &primC, &envC,
                                AcidVisuals::kSpitSplashType,
                                AcidVisuals::kSpitSplashScale);
    }
}

}  // namespace

bool EnKarebabaDescriptor::OnHostSetupSpin(EnKarebaba* actor, PlayState* play) {
    if (actor == nullptr) return false;
    (void)play;

    // Sync-rule 1 — host is sole RNG decider. Skip on peer (peer's
    // flag was set by OnPeerReceiveEnhancedSpinFlag BEFORE the local
    // ApplyNetState → SetupSpin chain fired). SceneAuthority per-
    // room authority: only the client that owns this actor's room
    // rolls RNG. In single-player, IsMyCurrentRoomHost() returns
    // true so behaviour is normal.
    if (!SceneAuthority::IsMyCurrentRoomHost()) {
        return false;
    }

    // Gate on the CVar (host-authoritative via enforced registry —
    // peer's view of this CVar is host's).
    if (AnchorCVarSync::GetEnforcedInt(GeyserSpinCVarName(), 0) == 0) {
        // Fresh Spin cycle without CVar → clear any stale per-spin
        // flags. Charge counters preserved so if CVar re-enables
        // mid-session the ramp continues where it left off.
        auto it = sStates.find(&actor->actor);
        if (it != sStates.end()) {
            it->second.currentSpinEnhanced   = false;
            it->second.geyserSpawnedThisSpin = false;
            it->second.groundSplashesFired   = 0;
        }
        return false;
    }

    // Shared ChargeStateMachine dispatch (extracted 2026-07-31).
    KarebabaEnhancedState& state = GetOrCreate(actor);
    // Always clear per-spin flags at spin entry — this spin decides
    // whether to enhance based on state machine outcome below.
    state.currentSpinEnhanced   = false;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;

    // Attack-complete decrement runs at spin entry (equivalent for
    // Karebaba — no distinct Recover state, spin exit is the natural
    // "attack complete" moment). Decrements Cooldown counter; may
    // transition Cooldown → Charging.
    state.acidCharge.OnAttackComplete();

    // Ready branch: if charged, gate on Link-in-range, fire acid.
    if (state.acidCharge.IsReady()) {
        // Range gate — Link within 2× cylinder radius (120u XZ).
        // vanilla `xzDistToPlayer` is already updated by z_actor.c
        // per-frame (patched by SoH's #153 nearest-player overlay
        // for MP correctness — see session_state Pitfall 28).
        if (actor->actor.xzDistToPlayer <= kAcidRangeXZ) {
            // Fire acid this spin. OnFire transitions Ready → Cooldown.
            state.acidCharge.OnFire();
            state.currentSpinEnhanced = true;
            // SFX — dramatic "eruption" sound at attack start. +50%
            // volume per V7 "erupt too quiet" tuning (shared helper
            // default is kBoostedVolScale = 1.5f).
            EnhancementAudio::PlayBoostedActorSfx(
                &actor->actor, NA_SE_EV_ERUPTION_CLOUD);
            return true;
        }
        // Out of range — Ready state preserved (per user spec "skip
        // AND preserve charge"). No fire, no counter change.
        return false;
    }

    // Charging branch: TryCharge rolls chance = counter*25% and
    // transitions Charging → Ready on success. Returns true only
    // on the transition edge so we can play the SFX exactly once.
    if (state.acidCharge.TryCharge()) {
        // Just entered Ready. Play "sizzle" tell so player notices
        // the telegraph state change.
        EnhancementAudio::PlayBoostedActorSfx(
            &actor->actor, NA_SE_EV_WATER_BUBBLE);
    }
    return false;  // No acid this spin (Ready fires on NEXT spin).
}

void EnKarebabaDescriptor::OnPeerReceiveEnhancedSpinFlag(EnKarebaba* actor,
                                                            bool enhanced) {
    if (actor == nullptr) return;
    KarebabaEnhancedState& state = GetOrCreate(actor);
    state.currentSpinEnhanced   = enhanced;
    // If newly-enabling for this spin, reset per-spin counters so
    // peer's local Spin fires the sequenced phases from scratch.
    if (enhanced) {
        state.geyserSpawnedThisSpin = false;
        state.groundSplashesFired   = 0;
    }
}

void EnKarebabaDescriptor::OnPeerReceiveChargedFlag(EnKarebaba* actor,
                                                     bool charged) {
    if (actor == nullptr) return;
    // V6 — peer stores host's "Ready" state for telegraph rendering
    // in OnUprightTick. Peer's own chargeState is not authoritative
    // (only host runs the state machine); netCharged is the source
    // of truth on peer.
    KarebabaEnhancedState& state = GetOrCreate(actor);
    state.netCharged = charged;
}

void EnKarebabaDescriptor::OnUprightTick(EnKarebaba* actor, PlayState* play) {
    // Ready-phase telegraph. Applied per-frame during vanilla
    // EnKarebaba_Upright when charge state indicates Ready (host)
    // or netCharged (peer). V7 change 7 — telegraph render logic
    // extracted to shared RenderTelegraph() helper so OnSpinTick
    // can render the same telegraph during non-firing spins
    // (Ready + Link out of range).
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    KarebabaEnhancedState& state = it->second;

    const bool telegraphActive =
        state.acidCharge.IsReady() || state.netCharged;
    if (!telegraphActive) return;

    RenderTelegraph(actor, play);
}

void EnKarebabaDescriptor::OnSpinTick(EnKarebaba* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return;

    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) {
        // Not tracked — defensive scale reset on first spin frame in
        // case telegraph scale leaked in from somewhere. Very rare.
        if (actor->actor.params == 39) {
            Actor_SetScale(&actor->actor, kVanillaScale);
        }
        return;
    }
    KarebabaEnhancedState& state = it->second;

    // V7 change 6+7 — head-scale + telegraph rendering during Spin.
    // User spec: "head should only return to normal size after the
    // acid attack is used" AND "telegraph mouth spit persists during
    // spin". Two cases:
    //   (a) Ready state + Link out of range → non-enhanced Spin
    //       running. Render telegraph (1.25× head + mouth spit)
    //       through the spin — preserves visual continuity with
    //       Upright telegraph.
    //   (b) Enhanced spin firing → constant 1.25× head throughout
    //       (was sinusoid 1.0→1.25→1.0 in V6, changed to constant
    //       per user "should only return to normal after acid used").
    //       Continues rendering Phase 1 attack visuals below.
    //   (c) Not Ready, not enhanced → normal vanilla spin, scale 1.0.

    const bool telegraphActive =
        state.acidCharge.IsReady() || state.netCharged;

    if (state.currentSpinEnhanced) {
        // Case (b) — head at constant 1.25 through entire enhanced
        // spin (V7 change 6). Vanilla Spin doesn't write scale, so
        // we own it. OnSpinExit will reset to 1.0 after acid fires.
        Actor_SetScale(&actor->actor, kVanillaScale * kTelegraphHeadScale);
    } else if (telegraphActive) {
        // Case (a) — render telegraph for a Ready spin that isn't
        // firing acid (Link out of range). Head 1.25 + mouth spit.
        RenderTelegraph(actor, play);
    } else if (actor->actor.params == 39) {
        // Case (c) — first frame of vanilla spin, reset scale from
        // any prior state (defensive; scale should already be 1.0).
        Actor_SetScale(&actor->actor, kVanillaScale);
    }

    if (!state.currentSpinEnhanced) return;

    // ---- Enhanced spin: Phase 1-4 visual attack ----
    // Frame index within the spin cycle. Vanilla params starts at
    // 40 and counts down. Frame 0 = params == 40; frame 39 = params
    // == 1. (params == 0 fires the SetupUpright transition; caller
    // handles that.)
    const int paramsNow = (int)actor->actor.params;
    const float frameF = kSpinTotalFrames - (float)paramsNow;

    // Spawn the AC-collider actor once per enhanced spin, on frame 20
    // (V6 — was frame 1, delayed per user spec so damage window matches
    // the visual: acid isn't dangerous until 0.5s after rain begins
    // falling. Rain starts at f10 → damage active f20. Geyser actor
    // lifetime = 20 frames = to spin end at f40).
    //
    // Position: actor.home.pos (stem base) — damage cylinder stays
    // stationary at the plant even while the visible head sweeps
    // around it. Encompasses both the head's swing radius (60u) and
    // the falling-dust XZ footprint below.
    // paramsNow starts at 40 (SetupSpin), decrements each Update.
    // frameF = 40 - paramsNow, so paramsNow == 20 means frameF == 20.
    if (!state.geyserSpawnedThisSpin && paramsNow <= 20) {
        state.geyserSpawnedThisSpin = true;
        if (gEnKarebabaGeyserId != 0) {
            Actor_Spawn(&play->actorCtx, play,
                         gEnKarebabaGeyserId,
                         actor->actor.home.pos.x,
                         actor->actor.home.pos.y,
                         actor->actor.home.pos.z,
                         0, 0, 0, 0);
        }
    }

    // ---- Sequenced visual particles (refactor v3) ----
    // Frame-phased across the 40-frame Spin. Copies of color/vec
    // constants are made because the effect helpers accept
    // non-const pointers.
    //
    // frameF derived earlier from paramsNow. Integer cast for phase
    // comparisons.
    const int frameI = (int)frameF;

    // Phase 1a — Spit-splash at the HEAD. Uses shared AcidVisuals
    // tint palette + splash type/scale (proven visible-green in
    // Karebaba V7 plan-B).
    {
        Color_RGBA8 primC = AcidVisuals::kSpitPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kSpitEnvColor;
        for (int i = 0; i < kSpitSplashesPerFrame; i++) {
            Vec3f jitteredPos = {
                actor->actor.world.pos.x + (Rand_ZeroOne() - 0.5f) * 10.0f,
                actor->actor.world.pos.y + kSpitYOffset +
                    (Rand_ZeroOne() - 0.5f) * 6.0f,
                actor->actor.world.pos.z + (Rand_ZeroOne() - 0.5f) * 10.0f,
            };
            EffectSsGSplash_Spawn(play, &jitteredPos, &primC, &envC,
                                    AcidVisuals::kSpitSplashType,
                                    AcidVisuals::kSpitSplashScale);
        }
    }

    // Phase 1b — Supporting rising bubble at HEAD (1/frame accent).
    {
        Vec3f bubblePos = actor->actor.world.pos;
        bubblePos.y += kSpitYOffset;
        Vec3f bubbleAccel = { 0.0f, AcidVisuals::kRisingBubbleAccelY, 0.0f };
        Color_RGBA8 primC = AcidVisuals::kBubblePrimColor;
        Color_RGBA8 envC  = AcidVisuals::kBubbleEnvColor;
        for (int i = 0; i < kRisingBubblesPerFrame; i++) {
            Vec3f bubbleVel = {
                (Rand_ZeroOne() - 0.5f) * 2.5f,
                AcidVisuals::kRisingBubbleSpeed + Rand_ZeroOne() * 2.0f,
                (Rand_ZeroOne() - 0.5f) * 2.5f,
            };
            const s16 scale = (s16)(AcidVisuals::kRisingBubbleScaleBase +
                                     (int)(Rand_ZeroOne() *
                                            (float)AcidVisuals::kRisingBubbleScaleRand));
            EffectSsDtBubble_SpawnCustomColor(play, &bubblePos, &bubbleVel,
                                                &bubbleAccel, &primC, &envC,
                                                scale, 25, 8);
        }
    }

    // Phase 2 — Falling rain from Y=home+130 (starts f10). Downward
    // velocity + gravity so drops fall onto the ground. Reuses
    // EffectSsDtBubble with downward velocity + shared AcidVisuals
    // rain constants (speed/accel/scale/life).
    if (frameI >= kRainStartFrame) {
        Vec3f rainAccel = { 0.0f, AcidVisuals::kRainDropletAccelY, 0.0f };
        Color_RGBA8 primC = AcidVisuals::kRainPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kRainEnvColor;
        for (int i = 0; i < kRainDropletsPerFrame; i++) {
            Vec3f rainPos = {
                actor->actor.home.pos.x +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRainSpawnRadius,
                actor->actor.home.pos.y + kRainSpawnHeightY,
                actor->actor.home.pos.z +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRainSpawnRadius,
            };
            Vec3f rainVel = {
                (Rand_ZeroOne() - 0.5f) * 0.3f,
                AcidVisuals::kRainDropletSpeed + (Rand_ZeroOne() - 0.5f) * 0.5f,
                (Rand_ZeroOne() - 0.5f) * 0.3f,
            };
            EffectSsDtBubble_SpawnCustomColor(play, &rainPos, &rainVel,
                                                &rainAccel, &primC, &envC,
                                                AcidVisuals::kRainDropletScale,
                                                AcidVisuals::kRainDropletLife,
                                                6 /* randXZ jitter */);
        }
    }

    // Phase 3 — Ground-splash burst (V7 plan-B): reuse head-spit
    // config (visible-green) rather than the earlier kSplash* config
    // that let the white water texture bleed through.
    if (frameI >= kGroundImpactStartFrame &&
        state.groundSplashesFired < kGroundSplashTotalCount) {
        Color_RGBA8 primC = AcidVisuals::kSpitPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kSpitEnvColor;
        const float u = Rand_ZeroOne();
        const float r = kGroundSplashRadius * std::sqrt(u);
        const float ang = Rand_ZeroOne() * 2.0f * (float)M_PI;
        Vec3f splashPos = {
            actor->actor.home.pos.x + r * std::cos(ang),
            actor->actor.home.pos.y,
            actor->actor.home.pos.z + r * std::sin(ang),
        };
        EffectSsGSplash_Spawn(play, &splashPos, &primC, &envC,
                                AcidVisuals::kSpitSplashType,
                                AcidVisuals::kSpitSplashScale);
        state.groundSplashesFired++;
    }

    // Phase 4 — Rising dust from ground (fires from
    // kRisingDustStartFrame through spin end).
    if (frameI >= kRisingDustStartFrame) {
        Vec3f dustAccel = { 0.0f, AcidVisuals::kRisingDustAccelY, 0.0f };
        Color_RGBA8 primC = AcidVisuals::kDustPrimColor;
        Color_RGBA8 envC  = AcidVisuals::kDustEnvColor;
        for (int i = 0; i < kRisingDustPerFrame; i++) {
            Vec3f dustPos = {
                actor->actor.home.pos.x +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRisingDustRadius,
                actor->actor.home.pos.y + kRisingDustSpawnYOffset,
                actor->actor.home.pos.z +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRisingDustRadius,
            };
            Vec3f dustVel = {
                (Rand_ZeroOne() - 0.5f) * 0.4f,
                AcidVisuals::kRisingDustSpeed + (Rand_ZeroOne() - 0.5f) * 0.4f,
                (Rand_ZeroOne() - 0.5f) * 0.4f,
            };
            EffectSsDust_Spawn(play, 0 /* drawFlags */,
                                &dustPos, &dustVel, &dustAccel,
                                &primC, &envC,
                                AcidVisuals::kRisingDustScale,
                                AcidVisuals::kRisingDustScaleStep,
                                AcidVisuals::kRisingDustLife,
                                0 /* updateMode */);
        }
    }
}

void EnKarebabaDescriptor::OnSpinExit(EnKarebaba* actor) {
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    KarebabaEnhancedState& state = it->second;
    if (!state.currentSpinEnhanced) return;

    // Reset actor.scale to vanilla — enhanced Spin was scaling it.
    Actor_SetScale(&actor->actor, kVanillaScale);

    state.currentSpinEnhanced   = false;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;
}

bool EnKarebabaDescriptor::IsCurrentSpinEnhanced(EnKarebaba* actor) {
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.currentSpinEnhanced;
}

bool EnKarebabaDescriptor::IsCharged(EnKarebaba* actor) {
    // V6 — for ENEMY_STATE wire send-side. Returns whether host's
    // charge state is Ready (telegraph should show). Peer's query
    // is irrelevant (peer never sends this field back to host).
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.acidCharge.IsReady();
}

void EnKarebabaDescriptor::OnDeath(EnKarebaba* actor) {
    // V6 — per user spec: "all counter reset on death". Wipes both
    // charge state machine AND per-spin flags. Karebaba's Dying →
    // Regrow → Idle cycle preserves the Actor* pointer so the state
    // map entry stays, but its contents get zeroed. Next spin after
    // respawn behaves like fresh spawn (Charging, counter=0 → first
    // spin 0%).
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    KarebabaEnhancedState& state = it->second;
    state.currentSpinEnhanced   = false;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;
    state.acidCharge.Reset();
    state.netCharged            = false;
    // Reset actor.scale in case telegraph was active at death moment.
    Actor_SetScale(&actor->actor, kVanillaScale);
}

void EnKarebabaDescriptor::OnActorDestroy(EnKarebaba* actor) {
    if (actor == nullptr) return;
    sStates.erase(&actor->actor);
}

}  // namespace AnchorEnemyEnhancement
