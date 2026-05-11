/**
 * ClimbableSurfaces — implementation.
 *
 * Per plan §8 Path A. Walks ACTORCAT_BG / ACTORCAT_PROP for known
 * climbable scene actor IDs and returns the nearest within range.
 * The allowlist mirrors RoomNavData's `kClimbableActors[]` (per
 * `RoomNavData.cpp:362` at branch `feature/room-nav-data` HEAD); the
 * two are kept independent so each module can grow its allowlist
 * without cross-module coupling.
 */

#include "ClimbableSurfaces.h"
#include "DistanceMath.h"
#include "NavCVars.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <limits>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Climbable scene actor allowlist (Path A). Each entry's actorId
// triggers a ClimbAnchor record at the actor's world.pos with topPos
// at +estimatedHeight along Y. Field-extend as new climbable actor
// types surface.
//
// Mirrors RoomNavData::kClimbableActors but kept independent — one
// module shouldn't depend on the other's allowlist evolution.
// ---------------------------------------------------------------------------

namespace {

struct ClimbableActorEntry {
    int16_t actorId;
    float   estimatedHeight; // climb-top Y delta from actor.world.pos.y
};

constexpr ClimbableActorEntry kClimbableActorIds[] = {
    { ACTOR_BG_SPOT18_OBJ,     150.0f }, // Goron City interior ladder
    { ACTOR_BG_DDAN_KD,        200.0f }, // Dodongo's Cavern ladder
    { ACTOR_BG_SPOT06_OBJECTS, 120.0f }, // Lake Hylia (some climbables)
};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool IsClimbableSurfacesEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kClimbableSurfaces);
}

std::optional<ClimbAnchor> FindNearestClimbable(const Vec3f& from, float maxRadius, PlayState* play) {
    if (play == nullptr) return std::nullopt;
    if (maxRadius <= 0.0f) return std::nullopt;

    const float maxRadiusSq = maxRadius * maxRadius;

    // Walk both ACTORCAT_BG and ACTORCAT_PROP — climbable scene actors
    // may live in either category depending on the actor's setup.
    constexpr ActorCategory kCategoriesToScan[] = { ACTORCAT_BG, ACTORCAT_PROP };

    ClimbAnchor best{};
    float bestDistSq = std::numeric_limits<float>::infinity();
    bool  found      = false;

    for (ActorCategory cat : kCategoriesToScan) {
        Actor* actor = play->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            Actor* next = actor->next;
            for (const ClimbableActorEntry& entry : kClimbableActorIds) {
                if (actor->id != entry.actorId) continue;
                const float dSq = AnchorDist::Dist3DSq(from, actor->world.pos);
                if (dSq > maxRadiusSq) break;
                if (dSq < bestDistSq) {
                    ClimbAnchor anchor{};
                    anchor.basePos = actor->world.pos;
                    anchor.topPos  = { actor->world.pos.x,
                                       actor->world.pos.y + entry.estimatedHeight,
                                       actor->world.pos.z };
                    anchor.actorId = actor->id;
                    // Approach yaw is from `from` toward the actor's
                    // base XZ — the bearing the navigator should face
                    // when walking up to the ladder.
                    anchor.approachYaw = Math_Atan2S(
                        actor->world.pos.z - from.z,
                        actor->world.pos.x - from.x);
                    anchor.type = ClimbType::Ladder;
                    best       = anchor;
                    bestDistSq = dSq;
                    found      = true;
                }
                break;  // matched an entry; stop scanning the inner table
            }
            actor = next;
        }
    }

    if (!found) return std::nullopt;
    return best;
}

bool IsAnchorStillValid(const ClimbAnchor& anchor, PlayState* play) {
    if (play == nullptr) return false;
    if (anchor.type == ClimbType::None) return false;

    // Path A — verify the source actor is still loaded.
    if (anchor.actorId != 0) {
        constexpr ActorCategory kCategoriesToScan[] = { ACTORCAT_BG, ACTORCAT_PROP };
        for (ActorCategory cat : kCategoriesToScan) {
            Actor* a = play->actorCtx.actorLists[cat].head;
            while (a != nullptr) {
                Actor* next = a->next;
                if (a->id == anchor.actorId) {
                    // Approximate position match (within 1u — actors
                    // can be re-emplaced at the same coords across
                    // scene reloads but conceptually be "the same").
                    if (AnchorDist::Dist3DSq(a->world.pos, anchor.basePos) < 1.0f) {
                        return true;
                    }
                }
                a = next;
            }
        }
        return false;
    }

    // Path B / Path C anchors — return true unconditionally for v1.
    // Re-validation is reserved for future commits (vine surface-flag
    // re-scan, static-geometry re-test).
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle. No-op for now — pure helper module.
// ---------------------------------------------------------------------------

void RegisterClimbableSurfaces() {
    // No hooks needed. Stub kept so future tick / scene hooks can
    // land without re-touching the wiring.
}

} // namespace AnchorNav

static RegisterShipInitFunc registerClimbableSurfaces(AnchorNav::RegisterClimbableSurfaces);
