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
    bool currentSpinEnhanced = false;
    bool geyserSpawnedThisSpin = false;
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
// 1.5 at frame 20 (mid-spin, matches vanilla's `value` peak), back
// to 1.0 at frame 40. Half-cycle of sine from 0 to π gives that
// shape.
constexpr float kSpinTotalFrames  = 40.0f;
constexpr float kHeadScaleBase    = 1.0f;
constexpr float kHeadScalePeak    = 1.5f;

// Vanilla Karebaba scale at Init: 0.01f (per z_en_karebaba.c).
// Multiplier applied to actor.scale during enhanced Spin.
constexpr float kVanillaScale = 0.01f;

// Layered geyser visual tuning (2026-07-31 refactor after playtest
// feedback — original hahen effect read as falling leaves, not
// acid). Split into two independent particle layers per user
// direction:
//   - Rising bubbles attached to the HEAD position (which sweeps
//     around the plant during Spin) — reads as head spitting acid
//     as it swings.
//   - Falling dust from 60u ABOVE the plant, random XZ within a
//     modest radius — reads as a caustic vapor ceiling raining
//     down around the plant.
constexpr int   kRisingBubblesPerFrame = 3;
constexpr float kRisingBubbleSpeed     = 6.0f;   // upward Y velocity
constexpr float kRisingBubbleAccelY    = -0.25f; // slight gravity so bubbles arc down
constexpr int   kFallingDustPerFrame   = 2;
constexpr float kFallingSpawnHeight    = 60.0f;  // Y above home.pos
constexpr float kFallingSpawnRadius    = 60.0f;  // XZ jitter around home.pos
constexpr float kFallingDustSpeed      = -1.5f;  // downward Y velocity
constexpr float kFallingDustAccelY     = -0.35f; // gravity

// Acid green tints. Prim = bright fill color; Env = darker outline
// color for the multi-tone gradient the softsprite render uses.
// Alphas below 255 keep particles translucent so they blend into
// each other and don't look like solid blobs.
constexpr Color_RGBA8 kBubblePrimColor = { 150, 220, 100, 200 };
constexpr Color_RGBA8 kBubbleEnvColor  = {  60, 100,  40, 255 };
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
        }
        return false;
    }

    // Roll RNG. Rand_ZeroOne is the vanilla RNG source.
    KarebabaEnhancedState& state = GetOrCreate(actor);
    if (Rand_ZeroOne() >= kEnhancedSpinChance) {
        // Roll failed — mark as non-enhanced for this cycle.
        state.currentSpinEnhanced   = false;
        state.geyserSpawnedThisSpin = false;
        return false;
    }

    state.currentSpinEnhanced   = true;
    state.geyserSpawnedThisSpin = false;
    return true;
}

void EnKarebabaDescriptor::OnPeerReceiveEnhancedSpinFlag(EnKarebaba* actor,
                                                            bool enhanced) {
    if (actor == nullptr) return;
    KarebabaEnhancedState& state = GetOrCreate(actor);
    state.currentSpinEnhanced   = enhanced;
    // If newly-enabling for this spin, reset geyser-spawned flag
    // so the peer's local Spin also spawns the plume once.
    if (enhanced) {
        state.geyserSpawnedThisSpin = false;
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

    // ---- Layered visual particles (2026-07-31 refactor) ----
    // Both layers spawn each Spin frame while enhanced. Copies of the
    // color/vec constants are made because the effect helpers accept
    // non-const pointers.

    // Rising bubble layer — spawn AT THE HEAD (actor.world.pos is
    // updated by vanilla Spin to the swinging head position each
    // frame). Reads as head spitting acid as it swings around.
    {
        Vec3f bubblePos = actor->actor.world.pos;
        Vec3f bubbleAccel = { 0.0f, kRisingBubbleAccelY, 0.0f };
        Color_RGBA8 primC = kBubblePrimColor;
        Color_RGBA8 envC  = kBubbleEnvColor;
        for (int i = 0; i < kRisingBubblesPerFrame; i++) {
            Vec3f bubbleVel = {
                (Rand_ZeroOne() - 0.5f) * 2.5f,          // small XZ jitter
                kRisingBubbleSpeed + Rand_ZeroOne() * 2.0f,  // upward + variance
                (Rand_ZeroOne() - 0.5f) * 2.5f,
            };
            // scale 120-170 gives a moderate bubble; life 25 lets
            // them rise + fade before their arc peaks.
            const s16 scale = (s16)(120 + (int)(Rand_ZeroOne() * 50.0f));
            EffectSsDtBubble_SpawnCustomColor(play, &bubblePos, &bubbleVel,
                                                &bubbleAccel, &primC, &envC,
                                                scale, 25,
                                                8 /* randXZ jitter units */);
        }
    }

    // Falling dust layer — spawn at (home.pos + 60Y) with random XZ
    // jitter inside a 60u radius circle. Downward velocity + slight
    // gravity so they rain onto the ground around the plant. Random
    // pattern per user direction: "do not have to respect the current
    // position of the deku baba head, they can fall in a random
    // pattern around the karebaba."
    {
        Vec3f dustAccel = { 0.0f, kFallingDustAccelY, 0.0f };
        Color_RGBA8 primC = kDustPrimColor;
        Color_RGBA8 envC  = kDustEnvColor;
        for (int i = 0; i < kFallingDustPerFrame; i++) {
            Vec3f dustPos = {
                actor->actor.home.pos.x +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kFallingSpawnRadius,
                actor->actor.home.pos.y + kFallingSpawnHeight,
                actor->actor.home.pos.z +
                    (Rand_ZeroOne() - 0.5f) * 2.0f * kFallingSpawnRadius,
            };
            Vec3f dustVel = {
                (Rand_ZeroOne() - 0.5f) * 0.5f,  // slight XZ drift
                kFallingDustSpeed + (Rand_ZeroOne() - 0.5f) * 0.8f,
                (Rand_ZeroOne() - 0.5f) * 0.5f,
            };
            // scale 300 growing by scaleStep=8 → puffy expanding cloud.
            // life 20 lets it reach the ground and disperse.
            EffectSsDust_Spawn(play, 0 /* drawFlags: default */,
                                &dustPos, &dustVel, &dustAccel,
                                &primC, &envC,
                                300 /* scale */,
                                8   /* scaleStep — grow */,
                                20  /* life */,
                                0   /* updateMode: default */);
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
