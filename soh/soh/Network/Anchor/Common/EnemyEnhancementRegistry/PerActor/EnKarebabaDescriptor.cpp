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

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
// Custom actor id for the geyser plume. Registered via
// ActorDB::AddBuiltInCustomActors; declared in soh/src/code/z_play.c.
extern s16 gEnKarebabaGeyserId;
}

namespace AnchorEnemyEnhancement {

namespace {

// Per-actor enhancement state. Reset at OnSpinExit; created on
// demand at OnHostSetupSpin / OnPeerReceiveEnhancedSpinFlag.
struct KarebabaEnhancedState {
    bool currentSpinEnhanced   = false;
    bool geyserSpawnedThisSpin = false;
    // Counts ground-splash bursts fired this spin cycle (0..5).
    // See kGroundSplashTotalCount — user spec: 5 splashes in rapid
    // succession as the falling rain impacts.
    u8   groundSplashesFired   = 0;
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

// 33% chance per spin per plan §"Design". Field-tune if too
// frequent — but Karebaba's Spin only fires when a player is in
// range, so 1-in-3 spins carries the AoE means a player who
// hangs around a Karebaba for several attack cycles WILL see the
// geyser eventually.
constexpr float kEnhancedSpinChance = 0.33f;

// Head-scale sinusoid parameters. Vanilla Spin runs 40 frames
// (params counts 40→0). Multiplier goes 1.0 at frame 0, peaks at
// kHeadScalePeak at frame 20 (mid-spin, matches vanilla's `value`
// peak), back to 1.0 at frame 40. Half-cycle of sine from 0 to π
// gives that shape. Peak reduced 1.5→1.25 per user direction
// 2026-07-31 (subtler swell — less "boss form", more "attack tell").
constexpr float kSpinTotalFrames  = 40.0f;
constexpr float kHeadScaleBase    = 1.0f;
constexpr float kHeadScalePeak    = 1.25f;

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

// Phase 1 constants.
constexpr float kSpitYOffset           = 5.0f;   // raise above head to mouth level
constexpr int   kSpitSplashesPerFrame  = 2;
constexpr s16   kSpitSplashType        = 2;      // silhouette variant 0/1/2
constexpr s16   kSpitSplashScale       = 500;    // ~half of vanilla water surface splash

constexpr int   kRisingBubblesPerFrame = 1;      // supporting accent to the spit
constexpr float kRisingBubbleSpeed     = 6.0f;   // upward Y velocity
constexpr float kRisingBubbleAccelY    = -0.25f; // slight gravity so bubbles arc down

// Phase 2 constants. Rain starts at f10 and continues through spin
// end. XZ radius kept modest so the column reads as directly above
// the plant rather than a scattered downpour.
constexpr int   kRainStartFrame        = 10;
constexpr int   kRainDropletsPerFrame  = 3;
constexpr float kRainSpawnHeightY      = 30.0f;  // Y above home.pos
constexpr float kRainSpawnRadius       = 30.0f;  // XZ jitter around home
constexpr float kRainDropletSpeed      = -3.0f;  // initial downward velocity
constexpr float kRainDropletAccelY     = -0.35f; // gravity
constexpr s16   kRainDropletScale      = 90;     // small drops
constexpr s16   kRainDropletLife       = 20;     // enough to reach ground + fade

// Phase 3 constants. 5 total splashes over 5 consecutive frames.
// Impact frame chosen to match rain flight time — see comment block
// above for the physics derivation.
constexpr int   kGroundImpactStartFrame = 17;
constexpr u8    kGroundSplashTotalCount = 5;
constexpr float kGroundSplashRadius     = 40.0f; // XZ ring around home
constexpr s16   kGroundSplashScale      = 400;   // slightly smaller than spit
constexpr s16   kGroundSplashType       = 1;     // sharper silhouette (types 0/1/2)

// Phase 4 constants. Rising dust from ground upward. Small positive
// Y velocity + slight decay accel → dust rises then slows and
// disperses. Wider XZ radius spreads the mist around the plant.
constexpr int   kRisingDustStartFrame  = 17;
constexpr int   kRisingDustPerFrame    = 2;
constexpr float kRisingDustSpawnYOffset = 5.0f;  // just above ground so we see it rise
constexpr float kRisingDustSpeed       = 1.5f;   // upward velocity
constexpr float kRisingDustAccelY      = -0.05f; // slight decay
constexpr float kRisingDustRadius      = 60.0f;  // XZ spread
constexpr s16   kRisingDustScale       = 300;
constexpr s16   kRisingDustScaleStep   = 8;      // grow → puffy mist
constexpr s16   kRisingDustLife        = 25;

// Acid green tints. Prim = bright fill color; Env = darker outline
// color for the multi-tone gradient the softsprite render uses.
// Alphas below 255 keep particles translucent so they blend into
// each other and don't look like solid blobs.
constexpr Color_RGBA8 kSpitPrimColor   = { 170, 240, 110, 220 };
constexpr Color_RGBA8 kSpitEnvColor    = {  50,  90,  30, 255 };
constexpr Color_RGBA8 kBubblePrimColor = { 150, 220, 100, 200 };
constexpr Color_RGBA8 kBubbleEnvColor  = {  60, 100,  40, 255 };
constexpr Color_RGBA8 kRainPrimColor   = { 150, 220, 100, 220 };
constexpr Color_RGBA8 kRainEnvColor    = {  60, 100,  40, 255 };
constexpr Color_RGBA8 kSplashPrimColor = { 170, 240, 110, 230 };
constexpr Color_RGBA8 kSplashEnvColor  = {  50,  90,  30, 255 };
constexpr Color_RGBA8 kDustPrimColor   = { 180, 230, 130, 150 };
constexpr Color_RGBA8 kDustEnvColor    = {  80, 130,  60, 255 };

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
        // Fresh Spin cycle without CVar → clear any stale enhanced
        // flag from previous spin (defensive; SetupUpright normally
        // clears on exit).
        auto it = sStates.find(&actor->actor);
        if (it != sStates.end()) {
            it->second.currentSpinEnhanced   = false;
            it->second.geyserSpawnedThisSpin = false;
            it->second.groundSplashesFired   = 0;
        }
        return false;
    }

    // Roll RNG. Rand_ZeroOne is the vanilla RNG source.
    KarebabaEnhancedState& state = GetOrCreate(actor);
    if (Rand_ZeroOne() >= kEnhancedSpinChance) {
        // Roll failed — mark as non-enhanced for this cycle.
        state.currentSpinEnhanced   = false;
        state.geyserSpawnedThisSpin = false;
        state.groundSplashesFired   = 0;
        return false;
    }

    state.currentSpinEnhanced   = true;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;
    return true;
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

void EnKarebabaDescriptor::OnSpinTick(EnKarebaba* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;  // not tracked
    KarebabaEnhancedState& state = it->second;
    if (!state.currentSpinEnhanced) return;

    // Frame index within the spin cycle. Vanilla params starts at
    // 40 and counts down. Frame 0 = params == 40; frame 39 = params
    // == 1. (params == 0 fires the SetupUpright transition; caller
    // handles that.)
    const int paramsNow = (int)actor->actor.params;
    const float frameF = kSpinTotalFrames - (float)paramsNow;
    // Clamp defensively — spin might tick once more after params
    // hits 0 depending on ordering.
    const float t = (frameF < 0.0f) ? 0.0f
                    : (frameF > kSpinTotalFrames ? kSpinTotalFrames : frameF);
    // Sinusoid: 1.0 at t=0, peak at t=20, 1.0 at t=40.
    // sin(π*t/40) is 0 at endpoints, 1 at t=20.
    const float sinePhase =
        Math_SinS((s16)((t / kSpinTotalFrames) * 0x8000));
    const float scaleMul =
        kHeadScaleBase + (kHeadScalePeak - kHeadScaleBase) * sinePhase;

    // Apply to actor scale — vanilla Spin doesn't write scale, so
    // we own it during enhanced spin.
    Actor_SetScale(&actor->actor, kVanillaScale * scaleMul);

    // Spawn the AC-collider actor once per enhanced spin, on frame 1
    // (params == 39 — first tick after SetupSpin sets params=40 +
    // Update decrements). Guaranteed frame-consistent between host
    // and peer because both process the same params-decrement
    // schedule.
    //
    // Position: actor.home.pos (stem base) — damage cylinder stays
    // stationary at the plant even while the visible head sweeps
    // around it. Encompasses both the head's swing radius (60u) and
    // the falling-dust XZ footprint below.
    if (!state.geyserSpawnedThisSpin && paramsNow <= 39) {
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

    // Phase 1a — Spit-splash at the HEAD (raised +5u to originate
    // from the mouth rather than slightly below the head per user
    // feedback). EffectSsGSplash is a stationary bloom, 8-frame
    // life; spawning per-frame while head sweeps produces a trail.
    {
        Color_RGBA8 primC = kSpitPrimColor;
        Color_RGBA8 envC  = kSpitEnvColor;
        for (int i = 0; i < kSpitSplashesPerFrame; i++) {
            Vec3f jitteredPos = {
                actor->actor.world.pos.x + (Rand_ZeroOne() - 0.5f) * 10.0f,
                actor->actor.world.pos.y + kSpitYOffset +
                    (Rand_ZeroOne() - 0.5f) * 6.0f,
                actor->actor.world.pos.z + (Rand_ZeroOne() - 0.5f) * 10.0f,
            };
            EffectSsGSplash_Spawn(play, &jitteredPos, &primC, &envC,
                                    kSpitSplashType, kSpitSplashScale);
        }
    }

    // Phase 1b — Supporting rising bubble at HEAD (1/frame accent).
    {
        Vec3f bubblePos = actor->actor.world.pos;
        bubblePos.y += kSpitYOffset;  // stays with mouth-height spit
        Vec3f bubbleAccel = { 0.0f, kRisingBubbleAccelY, 0.0f };
        Color_RGBA8 primC = kBubblePrimColor;
        Color_RGBA8 envC  = kBubbleEnvColor;
        for (int i = 0; i < kRisingBubblesPerFrame; i++) {
            Vec3f bubbleVel = {
                (Rand_ZeroOne() - 0.5f) * 2.5f,
                kRisingBubbleSpeed + Rand_ZeroOne() * 2.0f,
                (Rand_ZeroOne() - 0.5f) * 2.5f,
            };
            const s16 scale = (s16)(120 + (int)(Rand_ZeroOne() * 50.0f));
            EffectSsDtBubble_SpawnCustomColor(play, &bubblePos, &bubbleVel,
                                                &bubbleAccel, &primC, &envC,
                                                scale, 25, 8);
        }
    }

    // Phase 2 — Falling rain from Y=home+30 (starts f10). Downward
    // velocity + gravity so drops fall onto the ground around the
    // plant. Read as "vomit that went up now coming down". Reuses
    // EffectSsDtBubble with downward velocity — the same effect
    // renders equally well as rising bubbles or falling droplets.
    if (frameI >= kRainStartFrame) {
        Vec3f rainAccel = { 0.0f, kRainDropletAccelY, 0.0f };
        Color_RGBA8 primC = kRainPrimColor;
        Color_RGBA8 envC  = kRainEnvColor;
        for (int i = 0; i < kRainDropletsPerFrame; i++) {
            Vec3f rainPos = {
                actor->actor.home.pos.x +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRainSpawnRadius,
                actor->actor.home.pos.y + kRainSpawnHeightY,
                actor->actor.home.pos.z +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kRainSpawnRadius,
            };
            Vec3f rainVel = {
                (Rand_ZeroOne() - 0.5f) * 0.3f,  // minimal XZ drift
                kRainDropletSpeed + (Rand_ZeroOne() - 0.5f) * 0.5f,
                (Rand_ZeroOne() - 0.5f) * 0.3f,
            };
            EffectSsDtBubble_SpawnCustomColor(play, &rainPos, &rainVel,
                                                &rainAccel, &primC, &envC,
                                                kRainDropletScale,
                                                kRainDropletLife,
                                                6 /* randXZ jitter */);
        }
    }

    // Phase 3 — Ground-splash burst (5 splashes, 1 per frame, at
    // random XZ around the plant). Fires as rain hits the ground.
    // Impact frame kGroundImpactStartFrame chosen to match rain
    // velocity + gravity + 30u fall distance (~7 frames of flight).
    if (frameI >= kGroundImpactStartFrame &&
        state.groundSplashesFired < kGroundSplashTotalCount) {
        Color_RGBA8 primC = kSplashPrimColor;
        Color_RGBA8 envC  = kSplashEnvColor;
        // Random XZ within a ring around home. Prefer ring-ish
        // distribution (not uniformly filled disk) so splashes
        // don't cluster at center — sqrt(u) biases toward the rim.
        const float u = Rand_ZeroOne();
        const float r = kGroundSplashRadius * std::sqrt(u);
        const float ang = Rand_ZeroOne() * 2.0f * (float)M_PI;
        Vec3f splashPos = {
            actor->actor.home.pos.x + r * std::cos(ang),
            actor->actor.home.pos.y,  // ground level
            actor->actor.home.pos.z + r * std::sin(ang),
        };
        EffectSsGSplash_Spawn(play, &splashPos, &primC, &envC,
                                kGroundSplashType, kGroundSplashScale);
        state.groundSplashesFired++;
    }

    // Phase 4 — Rising dust from ground (fires from
    // kRisingDustStartFrame through spin end). Repurposes what
    // was previously "falling dust from above" — now dust rises
    // out of the ground as if kicked up by the rain impact.
    if (frameI >= kRisingDustStartFrame) {
        Vec3f dustAccel = { 0.0f, kRisingDustAccelY, 0.0f };
        Color_RGBA8 primC = kDustPrimColor;
        Color_RGBA8 envC  = kDustEnvColor;
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
                kRisingDustSpeed + (Rand_ZeroOne() - 0.5f) * 0.4f,
                (Rand_ZeroOne() - 0.5f) * 0.4f,
            };
            EffectSsDust_Spawn(play, 0 /* drawFlags */,
                                &dustPos, &dustVel, &dustAccel,
                                &primC, &envC,
                                kRisingDustScale,
                                kRisingDustScaleStep,
                                kRisingDustLife,
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

void EnKarebabaDescriptor::OnActorDestroy(EnKarebaba* actor) {
    if (actor == nullptr) return;
    sStates.erase(&actor->actor);
}

}  // namespace AnchorEnemyEnhancement
