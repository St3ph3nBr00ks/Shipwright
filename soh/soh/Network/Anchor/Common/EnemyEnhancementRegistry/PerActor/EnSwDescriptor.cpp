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

#include "EnSwDescriptor.h"

#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/NavConsumer.h"
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/GravityAdapter.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"

#include <unordered_map>

namespace AnchorEnemyEnhancement {

// Per-actor helper state maps. Descriptors don't need to own state
// directly — NavConsumer / GravityAdapter carry their own bookkeeping
// per Actor*. Static maps live here so the descriptor's Tick hooks
// have a stable per-instance handle to pass into the helpers.
//
// Lifetime: entries live from OnNavTick's first fire on a given actor
// until the actor's Destroy runs (via a small OnActorDestroy sweep in
// EnhancementBridge — Phase 2 Step 6). Phase 1 stub bodies mean the
// maps stay empty; populating starts when NavConsumer / GravityAdapter
// bodies land.
static std::unordered_map<Actor*, NavConsumerState>    sNavStates;
static std::unordered_map<Actor*, GravityAdapterState> sGravityStates;

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
    (void)limbIndex;
    (void)rotInOut;
    (void)actor;
    (void)play;
    // Phase 2 partial: no-op. Phase 2 Step 8 identifies En_Sw leg-limb
    // indices from the skel header and adds the ~15° downward bend
    // when the descriptor's state machine reads ground-walking.
    return false;
}

}  // namespace AnchorEnemyEnhancement
