// TeleportEffect — implementation.
//
// See header for design intent.

#include "soh/Network/Anchor/Common/TeleportEffect.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "functions.h"
#include "macros.h"
}

namespace TeleportEffect {

void SpawnSparkleBurst(PlayState* play,
                       float centerX, float centerY, float centerZ,
                       uint8_t primR, uint8_t primG, uint8_t primB,
                       uint8_t envR, uint8_t envG, uint8_t envB,
                       int count,
                       int life) {
    if (play == nullptr || count <= 0) return;

    // Color_RGBA8 is a 4-byte packed color used by the vanilla effect
    // system (see z_effect_soft_sprite_old_init.c:227). Alpha=255 for
    // opaque sparkles; the KiraKira effect itself animates alpha over
    // the particle's lifetime, so this is just the starting value.
    Color_RGBA8 primColor = { primR, primG, primB, 255 };
    Color_RGBA8 envColor  = { envR,  envG,  envB,  255 };

    // Cadence tuned to match vanilla Farore's Wind respawn (z_actor.c:
    // 2449-2454): small radial spread + upward Y bias, per-sparkle
    // randomization for a "burst" reading. Scale 1000 = vanilla default.
    constexpr float kRadialSpread = 30.0f;   // XZ per-particle randomness
    constexpr float kUpwardHeight = 60.0f;   // Y bias, so sparkles cluster
                                              // near torso rather than at feet
    constexpr float kInitialSpeedXZ = 2.0f;  // sideways drift
    constexpr float kInitialSpeedY  = 2.0f;  // upward drift base
    constexpr float kSpeedYRandomExtra = 2.0f;
    constexpr float kGravityAccelY = -0.1f;  // gentle settle

    for (int i = 0; i < count; i++) {
        Vec3f pos = { centerX + Rand_CenteredFloat(kRadialSpread),
                      centerY + Rand_ZeroOne() * kUpwardHeight,
                      centerZ + Rand_CenteredFloat(kRadialSpread) };
        Vec3f velocity = { Rand_CenteredFloat(kInitialSpeedXZ),
                           kInitialSpeedY + Rand_ZeroOne() * kSpeedYRandomExtra,
                           Rand_CenteredFloat(kInitialSpeedXZ) };
        Vec3f accel = { 0.0f, kGravityAccelY, 0.0f };
        EffectSsKiraKira_SpawnDispersed(play, &pos, &velocity, &accel,
                                        &primColor, &envColor, 1000, life);
    }
}

}  // namespace TeleportEffect

extern "C" void Anchor_SpawnTeleportSparkles(
    PlayState* play,
    float x, float y, float z,
    uint8_t primR, uint8_t primG, uint8_t primB,
    uint8_t envR, uint8_t envG, uint8_t envB) {
    TeleportEffect::SpawnSparkleBurst(play, x, y, z,
                                       primR, primG, primB,
                                       envR, envG, envB);
}
