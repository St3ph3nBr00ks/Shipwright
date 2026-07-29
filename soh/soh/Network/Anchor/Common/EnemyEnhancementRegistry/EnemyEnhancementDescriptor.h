/**
 * EnemyEnhancementDescriptor — interface for vanilla enemy types that
 * opt into Flotilla-provided locomotion / behavior enhancements.
 *
 * Pillar 5 of the AI Enhancements umbrella. Each per-actor descriptor
 * declares which capabilities (nav-consume, gravity-aware, active-aggro,
 * draw-time pose compensation) apply to its vanilla actor id, provides
 * parameter blocks (climb mask, gravity constants, ranges), and
 * implements per-tick / per-draw override hooks that the vanilla
 * actor's .c file calls via extern "C" C-linkage helpers.
 *
 * Architecture: modify vanilla actors in place behind CVar toggles
 * rather than shipping duplicate actor types. Same gameplay outcome
 * (e.g. skullwalltula that walks on floors), different pattern —
 * one descriptor per actor id vs one new actor id per variant. See
 * Plans/vanilla_enemy_enhancements_plan.md §0 for the architectural
 * pivot rationale and §3 D1-D13 for locked-in decisions.
 *
 * Boss actors are excluded by policy — Registry::Find rejects any
 * actorId that IsSyncedBossActor() returns true for. Opting a boss
 * in requires explicit user sign-off per
 * `feedback_bosses_excluded_from_ai_extensions.md`.
 *
 * Naming: namespace `AnchorEnemyEnhancement` — flat single-namespace
 * per project convention (Pitfall 11: never nest under Anchor,
 * collides with the `class Anchor` declaration).
 *
 * Tracker: #210 (parent for altered enemies).
 * See Plans/vanilla_enemy_enhancements_plan.md §4 for the full
 * architecture, and §7 Phase 1 for this file's scaffolding-only role.
 */

#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "z64.h"
}

namespace AnchorEnemyEnhancement {

// Capability flags — set to true for each capability an enhanced actor
// opts into. All default false so descriptors override only what they
// need. Registry uses these to gate per-tick / per-draw dispatch so
// disabled capabilities incur zero cost.
struct CapabilityFlags {
    // Enemy consumes RoomNavData walkable and climbable surfaces;
    // pursues nearest player via shared Common/AILocomotion substrate
    // (NavOrDirect, NavStateTransitions, StuckRecovery). Descriptor
    // supplies NavConsumeParams for surface mask / speeds / ranges.
    bool canNavConsume = false;

    // Enemy responds to gravity when off its anchored surface (wall,
    // ceiling, thread, mound). Descriptor supplies GravityAwareParams
    // for gravity constants and stun-on-land timing.
    bool canGravityAware = false;

    // Proximity-detection gaze override. Vanilla En_Sw randomises
    // lunge target; this makes it deterministically track nearest
    // climbing player. See plan §4.11 for the pilot design.
    bool canActiveAggro = false;

    // Combat-replacement capability slot — DEFERRED per §4.9 / D13.
    // Reserved so future per-enemy combat pilots can populate this
    // without changing the base class. Default false; no default
    // behavior. See plan §4.9 for the rationale and the "not in
    // phases 0-5+" note.
    bool canReplaceCombat = false;
};

// Parameter block for canNavConsume — matches the NavTraits + AILocomotion
// primitives already used by NPC Follower / NPC Invader.
struct NavConsumeParams {
    uint16_t climbSurfaceMask = 0;    // NavTraits climb mask (NODE_CLIMB_ANY = spider)
    bool     walksFloor       = true; // consumes floor nav nodes for ground pursuit
    float    walkSpeed        = 4.0f;
    float    runSpeed         = 8.0f;
    float    attackRange      = 50.0f;
    float    detectRange      = 600.0f;
    bool     leavesTrail      = false;
};

// Parameter block for canGravityAware — physics constants for the
// off-anchored-surface fall.
struct GravityAwareParams {
    float gravity      = -1.2f;
    float maxFallSpeed = -20.0f;
    bool  stunOnLand   = false;
    int   stunFrames   = 20;
};

// Base class. Each per-actor descriptor inherits and overrides ActorId,
// Capabilities, and any capability-specific virtual hooks. Registry
// singleton holds descriptors by unique_ptr; Find(actorId) returns
// nullptr when no descriptor is registered.
class EnemyEnhancementDescriptor {
public:
    virtual ~EnemyEnhancementDescriptor() = default;

    // Required: vanilla actor id this descriptor enhances. Must be
    // unique across the registry.
    virtual int16_t ActorId() const = 0;

    // Required: human-readable name for debug UI + logs.
    virtual const char* ActorName() const = 0;

    // Required: capability flag set. Registry uses these to gate
    // per-tick / per-draw dispatch.
    virtual CapabilityFlags Capabilities() const = 0;

    // Optional CVar names for gating each capability at runtime. Names
    // reference the full CVar macro expansion, e.g.
    // "gEnhancements.Skullwalltula.NavConsume". Returning nullptr means
    // the capability is unconditionally on when Capabilities() returns
    // true — v1 sets a name for every capability so authors can flip
    // it off without recompiling.
    virtual const char* NavConsumeCVar()    const { return nullptr; }
    virtual const char* GravityAwareCVar()  const { return nullptr; }
    virtual const char* ActiveAggroCVar()   const { return nullptr; }
    virtual const char* ReplaceCombatCVar() const { return nullptr; }  // DEFERRED

    // Optional parameter blocks. Default constructors return sensible
    // "spider-scale" defaults; descriptors override to tune.
    virtual NavConsumeParams   NavParams()     const { return {}; }
    virtual GravityAwareParams GravityParams() const { return {}; }

    // Optional per-actor extension hooks called from C-side Notify
    // helpers in the vanilla actor's .c file. Default no-ops mean the
    // descriptor doesn't override that specific behavior.
    virtual void OnNavTick(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
    }
    virtual bool ShouldApplyGravity(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
        return false;
    }
    virtual void OnLandedFromFall(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
    }

    // Draw-time pose compensation (override-callback pattern per plan
    // §4.10). Called from the actor's SkelAnime override callback with
    // the current limb index and mutable rotation. Return true to
    // signal the descriptor modified rotInOut; false to signal
    // "leave vanilla pose alone." Default false. Combines with vanilla
    // anim sweep — does not replace it. Pitfall 22 requires a
    // matching draw-state reset on scene transition; descriptors
    // needing state persist across draw calls handle that themselves.
    virtual bool OverrideLimbBend(int32_t limbIndex, Vec3s* rotInOut,
                                   Actor* actor, PlayState* play) {
        (void)limbIndex;
        (void)rotInOut;
        (void)actor;
        (void)play;
        return false;
    }

    // Per-instance carve-out hook. Default: enhancement applies to
    // ALL instances of the actor. Override to read actor->params or
    // per-room state (D12 — per-instance exceptions added on playtest
    // demand, not pre-built).
    virtual bool IsInstanceEnhanced(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
        return true;
    }

    // DEFERRED — combat-replacement hooks. Default no-ops mean vanilla
    // combat actionFunc dispatch runs unmodified. See plan §4.9. When
    // canReplaceCombat lands (its own multi-session pilot), per-enemy
    // descriptors will override these.
    virtual bool ShouldReplaceCombatTick(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
        return false;
    }
    virtual void OnCombatTick(Actor* actor, PlayState* play) {
        (void)actor;
        (void)play;
    }
};

}  // namespace AnchorEnemyEnhancement
