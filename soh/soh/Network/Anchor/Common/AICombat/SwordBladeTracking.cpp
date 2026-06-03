#include "SwordBladeTracking.h"

#include <cmath>  // sqrtf

#include <spdlog/spdlog.h>  // SPDLOG_INFO — first-call diagnostic

extern "C" {
#include "functions.h"  // Matrix_MultVec3f
}

// Local-space sword offsets. Source: z_player_lib.c:1664-1678
// (func_80090A28) — Player's tip is at (sword_length, 400, 0) in
// L_HAND limb-local space, where sword_length is read from
// sMeleeWeaponLengths[heldWeapon]. We use the Kokiri Sword length
// (4000) as the default because:
//   - AI actors don't track per-weapon inventory state.
//   - Most AI actors using Link's skel render with child Link's
//     gear (default forced via PLAYER_MODELGROUP_SWORD_AND_SHIELD
//     in Anchor_*DrawBegin).
//   - AT collider geometry only needs to be approximate; the actor
//     scale (typical 0.01) converts 4000 limb-local to 40 world
//     units, which matches the prior fixed-plane reach.
//
// Base is at (0, 400, 0) — the hand pivot (origin of L_HAND limb
// space), with the same Y offset so tip and base form a horizontal
// line in limb-local space. When transformed through the L_HAND
// matrix, both end up at the correct world positions for the
// blade's current swing pose.
static const Vec3f kSwordTipLocal  = { 4000.0f, 400.0f, 0.0f };
static const Vec3f kSwordBaseLocal = {    0.0f, 400.0f, 0.0f };

extern "C" void Anchor_ComputeBladeWorldFromMatrix(Vec3f* outTip, Vec3f* outBase) {
    // Matrix_MultVec3f reads the CURRENT matrix stack (the limb's
    // transform when called from a post-limb callback). Const-cast
    // is required because OoT's Matrix_MultVec3f signature takes a
    // non-const Vec3f* even though it only reads the input.
    Matrix_MultVec3f(const_cast<Vec3f*>(&kSwordTipLocal),  outTip);
    Matrix_MultVec3f(const_cast<Vec3f*>(&kSwordBaseLocal), outBase);

    // [SwordBlade.Diag] — Option D first-call diagnostic.
    // One-shot per process: confirms the helper is actually invoked
    // on first frame and produces non-degenerate world positions.
    // If outTip/outBase both come back at (0,0,0), the matrix stack
    // was identity — likely because the caller wasn't inside a
    // post-limb callback when invoking. After the first log,
    // subsequent calls are silent; per-frame logging happens at the
    // per-actor diagnostic site (Invader.Diag AT registered ...).
    static bool sFirstCallLogged = false;
    if (!sFirstCallLogged) {
        SPDLOG_INFO("[SwordBlade.Diag] First Anchor_ComputeBladeWorldFromMatrix "
                    "call: tip=({:.1f},{:.1f},{:.1f}) base=({:.1f},{:.1f},{:.1f}) "
                    "(non-degenerate if matrix stack was at L_HAND limb)",
                    outTip->x, outTip->y, outTip->z,
                    outBase->x, outBase->y, outBase->z);
        sFirstCallLogged = true;
    }
}

extern "C" void Anchor_BuildSweepAtQuadFromBlade(
    const Vec3f* prevTip, const Vec3f* prevBase,
    const Vec3f* curTip,  const Vec3f* curBase,
    Vec3f* outBottomLeft, Vec3f* outBottomRight,
    Vec3f* outTopLeft, Vec3f* outTopRight) {
    // Sweep quad — four vertices form a parallelogram covering the
    // area between previous frame's blade pose and current frame's
    // blade pose. Mirrors vanilla Player's quad-from-two-frames
    // shape in func_80090480.
    //
    // Layout:
    //   prevBase  ●─────● curBase           (bottomLeft/bottomRight)
    //              │   ╱
    //              │  ╱  ← the blade SWEPT this area through
    //              │ ╱       this frame's animation step
    //              │╱
    //   prevTip   ●─────● curTip            (topLeft/topRight)
    //
    // No perpendicular extent needed (the old single-snapshot
    // approach used cross(bladeDir, worldUp) for lateral width, but
    // the sweep gives natural lateral coverage through the actual
    // motion delta — and matches vanilla's geometry).
    *outBottomLeft  = *prevBase;
    *outBottomRight = *curBase;
    *outTopLeft     = *prevTip;
    *outTopRight    = *curTip;
}
