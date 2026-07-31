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
#include "soh/Network/Anchor/Common/EnemyEnhancementRegistry/PerActor/EnSwStateMachine.h"  // GroundPursue query for leg-bend gate
#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt (host-authoritative wrapper)
#include "soh/Network/Anchor/Common/PlayerLookup.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <cmath>
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
    if (rotInOut == nullptr || actor == nullptr || play == nullptr) return false;

    // Gated on NavConsume CVar — leg-bend is cosmetic compensation for
    // the nav-driven ground-walking mode; when Nav is off, actor stays
    // on wall and vanilla wall-tangent leg sweep is correct.
    if (AnchorCVarSync::GetEnforcedInt(NavConsumeCVar(), 0) == 0) return false;

    // Fire in any Wall* or Ground* state where the body is offset from
    // the surface (see EnSwStateMachine kBodySurfaceOffset). Legs bend
    // outward to visually reach the surface across the gap. Moving
    // states (Pursue) additionally oscillate the bend per-leg to
    // produce a walking cycle; idle/transitional states hold a static
    // bend so feet rest against the surface.
    EnSw* enSw = reinterpret_cast<EnSw*>(actor);
    const AnchorEnemyEnhancement::EnSwState smState =
        AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_QueryState(enSw);
    bool isMoving;
    switch (smState) {
        case AnchorEnemyEnhancement::EnSwState::WallPursue:
        case AnchorEnemyEnhancement::EnSwState::GroundPursue:
            isMoving = true;
            break;
        case AnchorEnemyEnhancement::EnSwState::WallIdle:
        case AnchorEnemyEnhancement::EnSwState::GroundToWallReattach:
            isMoving = false;
            break;
        default:
            return false;  // Uninitialized / WallEdgeDrop / LungeYield /
                           // PermanentlyDisabled — vanilla pose intact
    }

    // Leg-limb table — enumerated from vanilla EnSw_OverrideLimbDraw
    // switch (z_en_sw.c:1082-1106). 8 legs at limb indices:
    //   8, 11, 14, 17, 20, 23, 26, 29
    // These are the 8 leg-DL slots the gold-variant branch remaps.
    int legIndex = -1;
    switch (limbIndex) {
        case 8:  legIndex = 0; break;
        case 11: legIndex = 1; break;
        case 14: legIndex = 2; break;
        case 17: legIndex = 3; break;
        case 20: legIndex = 4; break;
        case 23: legIndex = 5; break;
        case 26: legIndex = 6; break;
        case 29: legIndex = 7; break;
        default: return false;  // not a leg — leave vanilla pose alone
    }

    // Base bend positions leg tips against the surface across the
    // ~8u body offset (see kBodySurfaceOffset). Tuned for that offset;
    // if the offset changes, this may need adjustment.
    static constexpr int16_t kBaseBend = 0x1200;  // ~25°

    // Walk-cycle oscillation. Amplitude ±0x0600 (~8°) around the base:
    // during pursuit, legs rise ~8° above and dip ~8° below their
    // resting position, giving a visible step-and-plant motion.
    // Period 20 frames (~1 second at 20fps game tick).
    // Per-leg phase offset (2π/8 = 45° per leg) spreads the 8 legs
    // across one cycle → ripple gait, not synchronized stomp.
    static constexpr int16_t kWalkAmplitude = 0x0600;  // ~8° swing
    static constexpr float   kStepPeriod    = 20.0f;   // frames per cycle
    static constexpr float   kLegPhaseStep  = 2.0f * (float)M_PI / 8.0f;

    // Per-leg sign — diagonal (cross-mirror) pattern derived
    // empirically 2026-07-30. En_Sw model rig pairs diagonally-
    // opposite legs (front-left mirrors back-right, front-right
    // mirrors back-left) rather than the usual left-right mirror,
    // so each diagonal-pair members need matching sign and the two
    // diagonals need opposite signs. Anatomical map (from parent-
    // joint origin analysis via [EnSwLegMap2] diagnostic, log 780):
    //   leg 0 = front-mid-left      leg 4 = front-mid-right
    //   leg 1 = back-most-left      leg 7 = back-most-right
    //   leg 2 = back-mid-left       leg 3 = back-mid-right
    //   leg 5 = front-most-left     leg 6 = front-most-right
    static constexpr int8_t kLegBendSign[8] = {
        +1,  // 0 — front-mid-left
        -1,  // 1 — back-most-left
        -1,  // 2 — back-mid-left
        +1,  // 3 — back-mid-right
        -1,  // 4 — front-mid-right
        +1,  // 5 — front-most-left
        -1,  // 6 — front-most-right
        +1,  // 7 — back-most-right
    };

    int16_t bend = kBaseBend;
    if (isMoving) {
        const float phase =
            (float)play->gameplayFrames * (2.0f * (float)M_PI / kStepPeriod)
            + (float)legIndex * kLegPhaseStep;
        const float wave = std::sin(phase);
        bend = (int16_t)(kBaseBend + (int)(wave * kWalkAmplitude));
    }
    bend = (int16_t)(bend * kLegBendSign[legIndex]);
    rotInOut->x = (int16_t)(rotInOut->x + bend);
    return true;
}

bool EnSwDescriptor::PickGazeTarget(Actor* actor, PlayState* play,
                                     Vec3f* outTargetPos) {
    if (actor == nullptr || play == nullptr || outTargetPos == nullptr) {
        return false;
    }
    if (!IsInstanceEnhanced(actor, play)) return false;
    if (AnchorCVarSync::GetEnforcedInt(ActiveAggroCVar(), 0) == 0) return false;

    // Iterate synced players; return first that passes climbing +
    // distance + LoS gates. Skip STATIONARY_LADDER + arc — those are
    // the vanilla-purity gates that this capability is designed to
    // relax (plan §4.11 properties list).
    Actor* candidates[8];
    int n = GetSyncedPlayerActors(play, candidates, 8);
    if (n <= 0) return false;

    CollisionPoly* losPoly = nullptr;
    int32_t losId = 0;
    Vec3f losPos = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < n; i++) {
        Player* p = (Player*)candidates[i];
        if (p == nullptr) continue;
        // Out-of-scene DummyPlayer sentinel — skip.
        if (p->actor.world.pos.y <= -9000.0f) continue;
        // Vanilla climbing requirement — only pursue climbers. This IS
        // kept (not relaxed) because attacking non-climbers would change
        // vanilla gameplay too much; the plan's intent is to fix
        // flicker-abort mid-lunge, not turn En_Sw into an omni-attacker.
        if (!(p->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER)) continue;
        // Distance gate — vanilla 130u 3D radius (kept — target must
        // actually be in range).
        const float dx = p->actor.world.pos.x - actor->world.pos.x;
        const float dy = p->actor.world.pos.y - actor->world.pos.y;
        const float dz = p->actor.world.pos.z - actor->world.pos.z;
        if ((dx*dx + dy*dy + dz*dz) >= (130.0f * 130.0f)) continue;
        // LoS gate — vanilla, line clear from En_Sw to player (kept).
        if (BgCheck_EntityLineTest1(&play->colCtx, &actor->world.pos,
                                     &p->actor.world.pos, &losPos,
                                     &losPoly, true, false, false, true,
                                     &losId)) {
            continue;  // line blocked → next candidate
        }
        *outTargetPos = p->actor.world.pos;
        return true;
    }
    return false;
}

bool EnSwDescriptor::ShouldRelaxLungeGates(Actor* actor, PlayState* play) {
    // Any instance where AggressiveAcquire is active gets the relaxed
    // gate set applied to its state-7 wind-up picker. IsInstanceEnhanced
    // filters gold-token variants (never relax gates for tokens).
    if (!IsInstanceEnhanced(actor, play)) return false;
    return AnchorCVarSync::GetEnforcedInt(ActiveAggroCVar(), 0) != 0;
}

}  // namespace AnchorEnemyEnhancement
