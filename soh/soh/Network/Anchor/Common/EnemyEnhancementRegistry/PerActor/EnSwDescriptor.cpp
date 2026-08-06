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

int EnSwDescriptor::GetConfiguredMaxHP() const {
    // Reader clamp: 1 = still alive; 127 = fits s8 netHealth cache used
    // for MP sync (see session_state.md "EnemyNetId struct" — netHealth
    // is s8). Widening the cache would violate YAGNI for the values
    // this CVar is intended to expose (spider is meant to be 2-5 HP).
    const int raw = AnchorCVarSync::GetEnforcedInt(MaxHPCVar(), 1);
    if (raw < 1)   return 1;
    if (raw > 127) return 127;
    return raw;
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

    // Walk-anim gate — state machine sets isWalkAnimActive = true only
    // during ticks that ACTUALLY translate the spider (WallPursue
    // motion block, GroundPursue motion block, WalkLunge dash) OR
    // rotate the spider (idle-gaze smooth-step). Otherwise we still
    // write a STATIC leg bend (kBaseBend, no oscillation) — critically,
    // we must not `return false` here because vanilla EnSw_Update
    // advances SkelAnime each frame (line 1085 of z_en_sw.c) and the
    // default animation is a wall-walking leg cycle. Returning false
    // lets vanilla's cycle render, so legs appear to walk even while
    // spider is stationary. Applying our static bend as an OVERRIDE
    // pins the legs to a rest pose over vanilla's animation.
    EnSw* enSw = reinterpret_cast<EnSw*>(actor);
    const bool isMoving =
        AnchorEnemyEnhancement::EnSw_EnhancedStateMachine_IsWalkAnimActive(enSw);

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

    // Walk cycle — alternating tetrapod gait. Two groups of 4 legs
    // alternate stance (planted at kBaseBend) and swing (lifted via
    // half-sine arc). Group B is 180° out of phase from Group A.
    //
    // Group assignment follows the kinematic rules from Hao et al
    // 2019 "Analysis of Spiders' Joint Kinematics and Driving Modes
    // under Different Ground Conditions" (PMC6935789):
    //   - Same-side legs 1 and 3 move together (also 2 and 4)
    //   - Diagonal pairs move together (L1+R2, L2+R1, L3+R4, L4+R3)
    // → Group A: L1, L3, R2, R4  →  legIdx {5, 2, 4, 7}
    // → Group B: L2, L4, R1, R3  →  legIdx {0, 1, 6, 3}
    //
    // Duty cycle: 60% of the cycle is stance (foot planted, holding
    // body weight); 40% is swing (foot lifted, moving forward). The
    // swing lift uses a half-sine arc (0 → peak → 0) so the foot
    // rises smoothly and plants smoothly, which reads more natural
    // than a triangular lerp or a full sine.
    static constexpr uint8_t kLegGroupB[8] = {
        1,  // 0 — L2 — group B
        1,  // 1 — L4 — group B
        0,  // 2 — L3 — group A
        1,  // 3 — R3 — group B
        0,  // 4 — R2 — group A
        0,  // 5 — L1 — group A
        1,  // 6 — R1 — group B
        0,  // 7 — R4 — group A
    };
    static constexpr int16_t kLiftAmplitude = 0x1200;  // ~25° lift — matches
                                                        // kBaseBend so peak-
                                                        // swing leg is at
                                                        // neutral (fully
                                                        // lifted off surface,
                                                        // no overswing).
    static constexpr float   kStepPeriod    = 8.0f;    // frames per full
                                                        // cycle (~0.4 s at
                                                        // 20 fps) — matches
                                                        // vanilla horizontal
                                                        // leg-sweep cadence.
    static constexpr float   kStancePortion = 0.6f;    // 60% planted.

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
        // Normalized progress through the gait cycle, [0.0, 1.0).
        const float rawT = (float)play->gameplayFrames / kStepPeriod;
        float t = rawT - std::floor(rawT);
        // Group B trails Group A by half a cycle.
        if (kLegGroupB[legIndex]) {
            t += 0.5f;
            if (t >= 1.0f) t -= 1.0f;
        }
        // Stance (t < 0.6): planted at kBaseBend. Swing (t >= 0.6):
        // lift via half-sine → foot rises to fully-lifted at mid-
        // swing then returns to plant, over the remaining 40% of
        // the cycle.
        if (t >= kStancePortion) {
            const float swingT = (t - kStancePortion) /
                                  (1.0f - kStancePortion);
            const float lift   = std::sin(swingT * (float)M_PI);
            bend = (int16_t)(kBaseBend - (int)(lift * kLiftAmplitude));
        }
    }
    bend = (int16_t)(bend * kLegBendSign[legIndex]);
    // REPLACE the animation-provided rot.x rather than adding to it.
    // `rotInOut->x` comes in carrying vanilla SkelAnime_Update's pose
    // value for this limb — for En_Sw's default animation that's a
    // walking-cycle rot.x that OSCILLATES per frame regardless of
    // spider state. If we only ADD our bend on top, vanilla's cycle
    // leaks through: legs animate whenever the spider is idle (bend
    // static) OR moving (bend oscillating), because vanilla's
    // underlying cycle is always ticking. Replacing pins the leg
    // rot.x to our value exactly — legs static when isMoving = false,
    // legs animated ONLY by our tetrapod cycle when isMoving = true.
    rotInOut->x = bend;
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
    // Relaxed picker applies to enhanced instances under AggressiveAcquire
    // OR NavConsume. NavConsume drives the spider onto the floor and
    // pursues non-climbing players; the vanilla climbing-required gate
    // would otherwise reject every attack. AggressiveAcquire also implies
    // relaxed gates for its wind-up-flicker fix. IsInstanceEnhanced
    // filters gold-token variants (never relax gates for tokens).
    if (!IsInstanceEnhanced(actor, play)) return false;
    return AnchorCVarSync::GetEnforcedInt(ActiveAggroCVar(), 0) != 0 ||
           AnchorCVarSync::GetEnforcedInt(NavConsumeCVar(), 0) != 0;
}

}  // namespace AnchorEnemyEnhancement
