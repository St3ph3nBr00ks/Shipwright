/**
 * AcidVisuals — shared green-acid tint palette + splash / bubble /
 * rain / dust particle constants for the "acid attack" family of
 * enemy enhancements.
 *
 * Extracted 2026-07-31 during Dekubaba prep work (second consumer of
 * the palette Karebaba's V4-V7 iterations dialed in). Karebaba's
 * geyser and Dekubaba's acid vomit / seed spawn share the same
 * visual vocabulary — green softsprite particles with matching
 * saturation profile so the two actors read as the same acid family.
 *
 * Design decisions locked from Karebaba V4-V7 field-test iterations:
 *   - Spit/rain/dust tints: original ~pre-V4 brightness (V5 restored
 *     from V4's over-aggressive -25% dim).
 *   - Bubble tint: V4-dimmed values (user confirmed bubbles read
 *     cleanly as translucent accent, not primary color).
 *   - Splash type + scale (kSpitSplashType=2, kSpitSplashScale=250):
 *     proven visible-green in-scene. V7 plan-B replaced the earlier
 *     kGroundSplash config (type 1, scale 400) after white water
 *     texture bled through even at full alpha.
 *
 * Any future acid-family enhancement (Dekubaba, seed-spawn,
 * hypothetical acid puddle, hypothetical Dodongo acid variant)
 * should consume these constants rather than defining its own.
 */

#pragma once

#include "soh/Network/Anchor/Anchor.h"  // Pitfall 40

extern "C" {
#include "z64.h"
#include "functions.h"  // Rand_ZeroOne, EffectSsDtBubble_SpawnCustomColor
}

namespace AnchorEnemyEnhancement {
namespace AcidVisuals {

// --- Color palette --------------------------------------------------

// Spit (mouth-attached splash burst). Primary "acid green" — brightest
// element in the composition. Alpha 220 preserves translucency.
inline constexpr Color_RGBA8 kSpitPrimColor   = { 170, 240, 110, 220 };
inline constexpr Color_RGBA8 kSpitEnvColor    = {  50,  90,  30, 255 };

// Bubble (rising/falling small accent particles). V4-dimmed (60% of
// spit brightness) so bubbles read as translucent accent rather than
// competing with the primary spit.
inline constexpr Color_RGBA8 kBubblePrimColor = { 112, 165,  75, 200 };
inline constexpr Color_RGBA8 kBubbleEnvColor  = {  60, 100,  40, 255 };

// Rain (falling droplet particles). Slightly less saturated than spit;
// implies dilution / wet mist.
inline constexpr Color_RGBA8 kRainPrimColor   = { 150, 220, 100, 220 };
inline constexpr Color_RGBA8 kRainEnvColor    = {  60, 100,  40, 255 };

// Dust (rising mist from ground impact). Alpha 150 — most translucent
// of the palette, reads as vapor rather than droplet.
inline constexpr Color_RGBA8 kDustPrimColor   = { 180, 230, 130, 150 };
inline constexpr Color_RGBA8 kDustEnvColor    = {  80, 130,  60, 255 };

// --- Splash particle config (EffectSsGSplash) ----------------------

// Type 2 silhouette is the visible-green variant proven in Karebaba
// V7 plan-B. Type 1 lets underlying white water texture bleed through
// (see V7 commit body for the tinting analysis).
inline constexpr s16 kSpitSplashType  = 2;
inline constexpr s16 kSpitSplashScale = 250;

// --- Bubble particle config (EffectSsDtBubble_SpawnCustomColor) ----

inline constexpr f32 kRisingBubbleSpeed     = 6.0f;   // upward Y velocity
inline constexpr f32 kRisingBubbleAccelY    = -0.25f; // slight gravity → arc
inline constexpr int kRisingBubbleScaleBase = 60;
inline constexpr int kRisingBubbleScaleRand = 25;    // range = base..base+rand

// --- Rain droplet config (EffectSsDtBubble reused with downward vel)

inline constexpr f32 kRainDropletSpeed  = -3.0f;
inline constexpr f32 kRainDropletAccelY = -0.35f;
inline constexpr s16 kRainDropletScale  = 90;
inline constexpr s16 kRainDropletLife   = 25;

// --- Rising dust config (EffectSsDust_Spawn) -----------------------

inline constexpr f32 kRisingDustSpeed     = 1.5f;
inline constexpr f32 kRisingDustAccelY    = -0.05f;
inline constexpr s16 kRisingDustScale     = 300;
inline constexpr s16 kRisingDustScaleStep = 8;
inline constexpr s16 kRisingDustLife      = 25;

// --- Ready-state bubble accent (added 2026-08-02) ------------------
//
// Small vertical green bubble used to indicate that a charge machine
// has reached Ready state (i.e., next attack decision will fire the
// enhanced attack). User request 2026-08-02: "add a small vertical
// green bubble effect to the ready state to help with visibility.
// Use the bubbles in the karebaba acid geyser attack as a reference.
// the 'ready' state bubbles should be a smaller effect, smaller
// radius, shorter vertical height, approx. 30 units."
//
// Sizing derivation (peak height = vy² / (2 × |accel|)):
//   vy = 4.0, accel = -0.25 → peak ≈ 4² / 0.5 = 32u ✓ (~30u target)
//   scale 30 + rand(15) = 30-45 (half of geyser's 60-85)
//   life 25 frames (unchanged from geyser)
//   Per-frame call rate throttled by caller (see SpawnReadyBubbles).
inline constexpr f32 kReadyBubbleSpeed        = 4.0f;
inline constexpr f32 kReadyBubbleSpeedRand    = 1.0f;
inline constexpr f32 kReadyBubbleAccelY       = -0.25f;
inline constexpr int kReadyBubbleScaleBase    = 30;
inline constexpr int kReadyBubbleScaleRand    = 15;
inline constexpr f32 kReadyBubbleXZJitter     = 1.25f;   // half of geyser 2.5
inline constexpr s16 kReadyBubbleLife         = 25;
inline constexpr int kReadyBubbleSpawnPeriod  = 3;       // 1 per 3 frames

// Spawns one ready-state bubble at `spawnPos` if this frame is a
// spawn tick (throttled by kReadyBubbleSpawnPeriod). Callers pass
// the head position of the actor + PlayState for frame counter +
// particle system.
//
// Extract-at-2 pattern: single free function reused by Karebaba's
// RenderTelegraph + Dekubaba's OnEveryFrameTick. Rendering physics
// (vy/accel/scale/life) locked here so both actors read identical.
inline void SpawnReadyBubbles(PlayState* play, const Vec3f& spawnPos) {
    if (play == nullptr) return;
    if ((play->gameplayFrames % kReadyBubbleSpawnPeriod) != 0) return;

    Vec3f pos = spawnPos;
    Vec3f vel = {
        (Rand_ZeroOne() - 0.5f) * kReadyBubbleXZJitter,
        kReadyBubbleSpeed + Rand_ZeroOne() * kReadyBubbleSpeedRand,
        (Rand_ZeroOne() - 0.5f) * kReadyBubbleXZJitter,
    };
    Vec3f accel = { 0.0f, kReadyBubbleAccelY, 0.0f };
    Color_RGBA8 primC = kBubblePrimColor;
    Color_RGBA8 envC  = kBubbleEnvColor;
    const s16 scale = (s16)(kReadyBubbleScaleBase +
                             (int)(Rand_ZeroOne() *
                                    (float)kReadyBubbleScaleRand));
    EffectSsDtBubble_SpawnCustomColor(play, &pos, &vel, &accel,
                                        &primC, &envC,
                                        scale, kReadyBubbleLife, 4);
}

}  // namespace AcidVisuals
}  // namespace AnchorEnemyEnhancement
