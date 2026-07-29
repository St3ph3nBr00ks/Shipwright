/**
 * EnSwDescriptor — Phase 2 pilot implementation.
 *
 * Phase 2 partial: params + IsInstanceEnhanced + hook stubs that
 * delegate to NavConsumer / GravityAdapter helpers. NavConsumer and
 * GravityAdapter TickXxx bodies are still Phase 1 no-ops; this file
 * is complete on the descriptor side and waits for those helpers.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §7 Phase 2.
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before any extern "C" block opens (this TU's
// registry headers wrap z64.h in extern "C", which fails otherwise).
#include "soh/Network/Anchor/Anchor.h"

#include "EnSwDescriptor.h"

#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/NavConsumer.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_map>

namespace AnchorEnemyEnhancement {

// Per-actor NavConsumer state map lives here (not in the bridge) so
// the descriptor's OnNavTick override can find its own state without
// crossing the C-linkage boundary. GravityAdapter state is owned by
// the bridge because ShouldApplyGravity is checked BEFORE TickGravity
// and the bridge does the gate + lookup + call.
static std::unordered_map<Actor*, NavConsumerState> sNavStates;

NavConsumeParams EnSwDescriptor::NavParams() const {
    NavConsumeParams p;
    // Spider walks any climb-surface (ladder / vine / designated /
    // generic wall) plus floor. NODE_CLIMB_ANY covers all four bits
    // per RoomNavData.h; the wider "any wall" via GENERIC_WALL still
    // requires GenerateGenericWallGrids console CVar to actually
    // produce nodes for arbitrary walls (see plan open Q1).
    p.climbSurfaceMask = AnchorNavRoom::NODE_CLIMB_ANY;
    p.walksFloor       = true;
    p.walkSpeed        = 4.0f;
    p.runSpeed         = 8.0f;
    p.attackRange      = 50.0f;
    p.detectRange      = 600.0f;
    p.leavesTrail      = false;  // small patrol pattern; no downstream trail consumer
    return p;
}

GravityAwareParams EnSwDescriptor::GravityParams() const {
    GravityAwareParams p;
    p.gravity      = -1.2f;
    p.maxFallSpeed = -20.0f;
    p.stunOnLand   = true;
    p.stunFrames   = 20;
    return p;
}

bool EnSwDescriptor::IsInstanceEnhanced(Actor* actor, PlayState* play) {
    (void)play;
    if (actor == nullptr) return false;
    // Gold-token variants (params & 0xE000 non-zero) are the collectible
    // static skulltulas — leave them fully vanilla.
    const uint16_t variant = ((uint16_t)actor->params & 0xE000) >> 13;
    return variant == 0;
}

void EnSwDescriptor::OnNavTick(Actor* actor, PlayState* play) {
    if (!IsInstanceEnhanced(actor, play)) return;

    // Phase 2 partial: delegate to NavConsumer helper. The helper's
    // Phase 1 body returns false (no-op), so this call currently has
    // no runtime effect. Phase 2 Step 4 fills in the real body.
    NavConsumerState& state = sNavStates[actor];
    (void)TickNavMovement(*this, state, actor, play);
}

bool EnSwDescriptor::ShouldApplyGravity(Actor* actor, PlayState* play) {
    (void)play;
    if (!IsInstanceEnhanced(actor, play)) return false;

    // Off-wall AND not-on-floor semantics.
    // bgCheckFlags bit 0x1 = on ground; bit 0x200 = on wall (via
    // vanilla wall-crawl attachment). If neither is set, actor is
    // airborne → gravity applies.
    //
    // The wall-attachment bit is fragile pre-Phase 2; log-observed
    // behavior on knock-off shows both bits clearing before we
    // observe. Phase 2 Step 5 refines this against real field data.
    const uint32_t bgFlags = actor->bgCheckFlags;
    const bool onFloor = (bgFlags & 0x1) != 0;
    const bool onWall  = (bgFlags & 0x200) != 0;
    return !onFloor && !onWall;
}

void EnSwDescriptor::OnLandedFromFall(Actor* actor, PlayState* play) {
    (void)actor;
    (void)play;
    // Phase 2 partial: no-op. Phase 2 Step 5 wires the stun timer
    // (writes the descriptor-provided GravityParams.stunFrames into a
    // field the actor's Tick body reads) so the spider pauses briefly
    // before resuming pursuit on the ground.
}

bool EnSwDescriptor::OverrideLimbBend(int32_t limbIndex, Vec3s* rotInOut,
                                       Actor* actor, PlayState* play) {
    (void)play;
    if (rotInOut == nullptr || actor == nullptr) return false;

    // Gated on NavConsume CVar — leg-bend is cosmetic compensation for
    // the nav-driven ground-walking mode; when Nav is off, actor stays
    // on wall and vanilla wall-tangent leg sweep is correct.
    if (CVarGetInteger(NavConsumeCVar(), 0) == 0) return false;

    // Only bend when actor is on the floor. bgCheckFlags bit 0x1 =
    // grounded via BgCheck floor. Wall-crawling En_Sw doesn't set
    // this bit, so the bend fires only during nav-driven ground pursuit.
    if ((actor->bgCheckFlags & 0x1) == 0) return false;

    // Leg-limb table — enumerated from vanilla EnSw_OverrideLimbDraw
    // switch (z_en_sw.c:1082-1106). 8 legs at limb indices:
    //   8, 11, 14, 17, 20, 23, 26, 29
    // These are the 8 leg-DL slots the gold-variant branch remaps.
    switch (limbIndex) {
        case 8:  case 11: case 14: case 17:
        case 20: case 23: case 26: case 29:
            break;
        default:
            return false;  // not a leg — leave vanilla pose alone
    }

    // Additive downward bend. s16 angle units — 0x1000 = ~22.5°.
    // Applied to X (pitch) — visually tips leg segments downward from
    // their wall-tangent rest pose so the spider appears to step on
    // the floor rather than slide across it.
    //
    // Direction / magnitude are first-pass estimates — field-test may
    // want to switch to Z (roll) or reduce magnitude if legs clip into
    // the body. Small enough to be visibly correct without breaking
    // silhouette; large enough to be visible at typical camera range.
    static constexpr int16_t kLegBendPitch = 0x0C00;  // ~16.9°
    rotInOut->x = (int16_t)(rotInOut->x + kLegBendPitch);
    return true;
}

}  // namespace AnchorEnemyEnhancement
