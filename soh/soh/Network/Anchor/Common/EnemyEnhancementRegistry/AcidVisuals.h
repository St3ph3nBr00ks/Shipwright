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

}  // namespace AcidVisuals
}  // namespace AnchorEnemyEnhancement
