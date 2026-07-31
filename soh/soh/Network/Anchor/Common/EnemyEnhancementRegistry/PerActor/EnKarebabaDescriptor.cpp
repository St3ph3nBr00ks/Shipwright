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

// V6 — charge state machine phase per user spec.
enum class KarebabaChargeState : u8 {
    Charging = 0,  // rolling per-spin, chance = counter*25%
    Ready    = 1,  // waiting for spin+range to fire acid; telegraph visible
    Cooldown = 2,  // 3-spin post-attack lockout, no roll
};

// Per-actor enhancement state. Some fields reset at OnSpinExit;
// charge counters persist per V6 spec (reset on OnDeath +
// OnActorDestroy). Created on demand at OnHostSetupSpin /
// OnPeerReceiveEnhancedSpinFlag / OnUprightTick.
struct KarebabaEnhancedState {
    // Per-spin flags — reset at OnSpinExit.
    bool currentSpinEnhanced   = false;
    bool geyserSpawnedThisSpin = false;
    // Counts ground-splash bursts fired this spin cycle (0..5).
    u8   groundSplashesFired   = 0;

    // V6 charge state machine — persist across spins, reset on death.
    KarebabaChargeState chargeState     = KarebabaChargeState::Charging;
    u8                  chargeCounter   = 0;  // 0..4, chance = counter*25%
    u8                  cooldownSpins   = 0;  // 3..0 during Cooldown

    // V6 — peer-received flag for the Ready telegraph. On peer, drives
    // OnUprightTick's decision to render 1.25× head + mouth spit.
    // Host writes this from its own chargeState. Peer's chargeState
    // is stale (only host runs the state machine) so this bool is
    // the actual telegraph source of truth on peer.
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

// V6 charge state machine constants per user 2026-07-31 spec.
//   - Charging: chance per spin = counter × kAcidChargePerCounter.
//     Counter starts at 0 → first spin 0%. Counter++ on fail.
//     Max 4 → max chance 100% at 5th spin.
//   - Ready: fires acid on next spin IF Link in acid-range.
//   - Cooldown: 3 spins guaranteed no-acid post-attack.
constexpr float kAcidChargePerCounter = 0.25f;
constexpr u8    kAcidChargeMaxCounter = 4;   // clamp to keep chance ≤ 1.0
constexpr u8    kAcidCooldownSpins    = 3;

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

// Legacy — no longer used (V6 replaced flat 33% with state machine).
// Kept as reference / removable in a future cleanup.
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

// Phase 1 constants. Spit position lifted from head+5 to head+15
// (V4 tuning) so splash originates cleanly above the mouth. Splash
// scale halved 500 → 250 to reduce visual dominance.
constexpr float kSpitYOffset           = 15.0f;  // above head (mouth clearance)
constexpr int   kSpitSplashesPerFrame  = 2;
constexpr s16   kSpitSplashType        = 2;      // silhouette variant 0/1/2
constexpr s16   kSpitSplashScale       = 250;    // V4: halved from 500

constexpr int   kRisingBubblesPerFrame = 1;      // supporting accent to the spit
constexpr float kRisingBubbleSpeed     = 6.0f;   // upward Y velocity
constexpr float kRisingBubbleAccelY    = -0.25f; // slight gravity so bubbles arc down
// V4 tuning — up-bubble scale halved. Range 120-170 → 60-85.
constexpr int   kRisingBubbleScaleBase = 60;
constexpr int   kRisingBubbleScaleRand = 25;

// Phase 2 constants. Rain starts at f10 and continues through spin
// end. XZ radius kept modest so the column reads as directly above
// the plant rather than a scattered downpour.
//
// V4 tuning — spawn height raised 30u → 130u per user direction.
// Recomputed impact frame: with initial velocity -3 and gravity
// -0.35, distance 130 = 3t + 0.175t² → 0.175t² + 3t - 130 = 0
//   → t = (-3 + sqrt(9 + 91)) / 0.35 = (-3 + 10) / 0.35 = 20 frames
// So rain hits ground at f10 + f20 = f30 (was f17 with 30u fall).
// Rain-drop life bumped 20 → 25 so drops remain visible AT the
// impact frame (would otherwise die exactly on impact and not
// visually connect to the splash burst).
constexpr int   kRainStartFrame        = 10;
constexpr int   kRainDropletsPerFrame  = 3;
constexpr float kRainSpawnHeightY      = 130.0f; // V4: was 30.0
constexpr float kRainSpawnRadius       = 30.0f;  // XZ jitter around home
constexpr float kRainDropletSpeed      = -3.0f;  // initial downward velocity
constexpr float kRainDropletAccelY     = -0.35f; // gravity
constexpr s16   kRainDropletScale      = 90;     // small drops
constexpr s16   kRainDropletLife       = 25;     // V4: bumped 20 → 25 to survive longer fall

// Phase 3 constants. 5 total splashes over 5 consecutive frames.
// Impact frame recomputed from rain physics (see Phase 2 block
// above): 20-frame fall + f10 start = f30 impact.
constexpr int   kGroundImpactStartFrame = 30;    // V4: was 17 (rain fell 30u); now 130u fall
constexpr u8    kGroundSplashTotalCount = 5;
constexpr float kGroundSplashRadius     = 40.0f; // XZ ring around home
constexpr s16   kGroundSplashScale      = 400;   // slightly smaller than spit
constexpr s16   kGroundSplashType       = 1;     // sharper silhouette (types 0/1/2)

// Phase 4 constants. Rising dust from ground upward. Small positive
// Y velocity + slight decay accel → dust rises then slows and
// disperses. Wider XZ radius spreads the mist around the plant.
// Start frame follows Phase 3 impact (V4: was 17, now 30).
constexpr int   kRisingDustStartFrame  = 30;
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
// each other and don't look like solid blobs (bubbles + dust).
//
// V5 tuning (2026-07-31 per user playtest — V4's uniform 25%
// brightness reduction perceived as too aggressive; splashes
// also read as white/blue rather than green):
//   - Spit / Rain / Dust: restored to pre-V4 100% originals.
//   - Bubble: KEPT at V4-dimmed values per user "content with
//     bubbles" feedback (they read as translucent accent to
//     spit, not primary color).
//   - Splash: dedicated saturation-boost pass — R and B reduced
//     to reject the white water-texture bleed-through, G maxed,
//     alpha 230→255 so tint fully overrides texture. Env darkened
//     for stronger outline contrast against the bright water
//     splash texture.
constexpr Color_RGBA8 kSpitPrimColor   = { 170, 240, 110, 220 };  // V5: restored pre-V4
constexpr Color_RGBA8 kSpitEnvColor    = {  50,  90,  30, 255 };
constexpr Color_RGBA8 kBubblePrimColor = { 112, 165,  75, 200 };  // V5: KEEP V4 (user content)
constexpr Color_RGBA8 kBubbleEnvColor  = {  60, 100,  40, 255 };
constexpr Color_RGBA8 kRainPrimColor   = { 150, 220, 100, 220 };  // V5: restored pre-V4
constexpr Color_RGBA8 kRainEnvColor    = {  60, 100,  40, 255 };
constexpr Color_RGBA8 kSplashPrimColor = { 120, 255,  70, 255 };  // V5: saturated green + full α to override white water tex
constexpr Color_RGBA8 kSplashEnvColor  = {  30,  80,  20, 255 };  // V5: darker outline for contrast
constexpr Color_RGBA8 kDustPrimColor   = { 180, 230, 130, 150 };  // V5: restored pre-V4
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

    // V6 — charge state machine dispatch.
    KarebabaEnhancedState& state = GetOrCreate(actor);
    // Always clear per-spin flags at spin entry — this spin decides
    // whether to enhance based on state machine outcome below.
    state.currentSpinEnhanced   = false;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;

    switch (state.chargeState) {
        case KarebabaChargeState::Cooldown: {
            // 3-spin post-attack lockout. Decrement per spin.
            // When decremented to 0 → transition to Charging with
            // fresh counter=0 (post-cooldown first spin has 0% chance,
            // matching post-spawn behavior per user spec).
            if (state.cooldownSpins > 0) state.cooldownSpins--;
            if (state.cooldownSpins == 0) {
                state.chargeState   = KarebabaChargeState::Charging;
                state.chargeCounter = 0;
            }
            // No acid, no roll this spin.
            return false;
        }

        case KarebabaChargeState::Ready: {
            // Range gate — Link within 2× cylinder radius (120u XZ).
            // vanilla `xzDistToPlayer` is already updated by
            // z_actor.c per-frame (patched by SoH's #153 nearest-
            // player overlay for MP correctness — see session_state
            // Pitfall 28).
            if (actor->actor.xzDistToPlayer <= kAcidRangeXZ) {
                // Fire acid this spin. Transition to Cooldown.
                state.currentSpinEnhanced = true;
                state.chargeState         = KarebabaChargeState::Cooldown;
                state.cooldownSpins       = kAcidCooldownSpins;
                state.chargeCounter       = 0;
                // SFX — dramatic "eruption" sound at attack start.
                Audio_PlayActorSound2(&actor->actor, NA_SE_EV_ERUPTION_CLOUD);
                return true;
            }
            // Out of range — preserve Ready state per user spec
            // ("skip AND preserve charge"). No acid, no roll.
            return false;
        }

        case KarebabaChargeState::Charging: {
            // Roll chance = counter × 25%. Counter starts at 0 on
            // spawn/cooldown-exit → first spin 0% chance (mandatory
            // vanilla).
            const float chance =
                (float)state.chargeCounter * kAcidChargePerCounter;
            if (Rand_ZeroOne() < chance) {
                // Success — enter Ready phase. Telegraph appears
                // starting this Upright cycle. Acid fires NEXT spin
                // if Link in range. Counter frozen (irrelevant in
                // Ready state).
                state.chargeState = KarebabaChargeState::Ready;
                // SFX — subtle "bubble" tell so player notices the
                // telegraph state change.
                Audio_PlayActorSound2(&actor->actor, NA_SE_EV_WATER_BUBBLE);
            } else {
                // Fail — increment counter, clamped to max (chance
                // never exceeds 100%).
                if (state.chargeCounter < kAcidChargeMaxCounter) {
                    state.chargeCounter++;
                }
            }
            return false;  // No acid this spin regardless of roll.
        }
    }

    return false;  // unreachable; defensive
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
    // V6 — Ready-phase telegraph. Applied per-frame during vanilla
    // EnKarebaba_Upright. Two visual channels:
    //   - Head at kTelegraphHeadScale (1.25× vanilla).
    //   - Subtle mouth spit every kTelegraphSpitPeriod frames.
    // Rendered on both host (chargeState==Ready) and peer
    // (netCharged==true). Peer's chargeState is stale — the
    // telegraph decision uses whichever source is authoritative
    // for the local client.
    if (actor == nullptr || play == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    KarebabaEnhancedState& state = it->second;

    // Determine telegraph active — union of host-side authoritative
    // Ready state OR peer-side received flag. On host both agree;
    // on peer only netCharged is truthful.
    const bool telegraphActive =
        (state.chargeState == KarebabaChargeState::Ready) ||
        state.netCharged;
    if (!telegraphActive) return;

    // Apply enlarged head.
    Actor_SetScale(&actor->actor, kVanillaScale * kTelegraphHeadScale);

    // Subtle mouth spit every N frames. Uses game frame counter for
    // deterministic timing (both clients render synced particles
    // because gameplayFrames is host-synced via TIME_SYNC family).
    if ((play->gameplayFrames % kTelegraphSpitPeriod) == 0) {
        Color_RGBA8 primC = kSpitPrimColor;
        Color_RGBA8 envC  = kSpitEnvColor;
        Vec3f pos = {
            actor->actor.world.pos.x + (Rand_ZeroOne() - 0.5f) * 6.0f,
            actor->actor.world.pos.y + kSpitYOffset +
                (Rand_ZeroOne() - 0.5f) * 4.0f,
            actor->actor.world.pos.z + (Rand_ZeroOne() - 0.5f) * 6.0f,
        };
        // Half-scale of attack-time spit — subtle continuous signal
        // rather than the dramatic bursts of the actual attack.
        EffectSsGSplash_Spawn(play, &pos, &primC, &envC,
                                kSpitSplashType, kSpitSplashScale / 2);
    }
}

void EnKarebabaDescriptor::OnSpinTick(EnKarebaba* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return;

    // V6 — clear any leftover telegraph scale from prior Upright on
    // FIRST spin frame (params==39 = just decremented from 40). Runs
    // unconditionally on host + peer so telegraph → spin transition
    // is clean regardless of Ready/enhanced state. If this spin IS
    // enhanced, OnSpinTick's sinusoid below re-scales starting at
    // 1.0 anyway; if not enhanced, scale stays vanilla for the
    // vanilla Spin cycle. Zero net cost when scale already at vanilla.
    if (actor->actor.params == 39) {
        Actor_SetScale(&actor->actor, kVanillaScale);
    }

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
            // V4 tuning — up-bubble scale halved (was 120 + rand*50).
            const s16 scale = (s16)(kRisingBubbleScaleBase +
                                     (int)(Rand_ZeroOne() * (float)kRisingBubbleScaleRand));
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
        // V6 SFX — waterdrop tick on FIRST splash only (avoid
        // 5-splash SFX spam over 5 frames — one is enough for the
        // "acid hits ground" tell).
        if (state.groundSplashesFired == 0) {
            Audio_PlayActorSound2(&actor->actor, NA_SE_EV_WATERDROP);
        }
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

bool EnKarebabaDescriptor::IsCharged(EnKarebaba* actor) {
    // V6 — for ENEMY_STATE wire send-side. Returns whether host's
    // charge state is Ready (telegraph should show). Peer's query
    // is irrelevant (peer never sends this field back to host).
    if (actor == nullptr) return false;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return false;
    return it->second.chargeState == KarebabaChargeState::Ready;
}

void EnKarebabaDescriptor::OnDeath(EnKarebaba* actor) {
    // V6 — per user spec: "all counter reset on death". Wipes both
    // charge state machine (chargeState/chargeCounter/cooldownSpins)
    // AND per-spin flags. Karebaba's Dying → Regrow → Idle cycle
    // preserves the Actor* pointer so the state map entry stays,
    // but its contents get zeroed. Next spin after respawn behaves
    // like fresh spawn (Charging, counter=0 → first spin 0%).
    if (actor == nullptr) return;
    auto it = sStates.find(&actor->actor);
    if (it == sStates.end()) return;
    KarebabaEnhancedState& state = it->second;
    state.currentSpinEnhanced   = false;
    state.geyserSpawnedThisSpin = false;
    state.groundSplashesFired   = 0;
    state.chargeState           = KarebabaChargeState::Charging;
    state.chargeCounter         = 0;
    state.cooldownSpins         = 0;
    state.netCharged            = false;
    // Reset actor.scale in case telegraph was active at death moment.
    Actor_SetScale(&actor->actor, kVanillaScale);
}

void EnKarebabaDescriptor::OnActorDestroy(EnKarebaba* actor) {
    if (actor == nullptr) return;
    sStates.erase(&actor->actor);
}

}  // namespace AnchorEnemyEnhancement
