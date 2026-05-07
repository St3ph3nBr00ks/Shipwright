/**
 * TargetSelection — implementation.
 *
 * Per plan §6. AcquireOrHoldTarget either holds the existing locked
 * target (sticky timer counting down) or evaluates a fresh nearest-of-set
 * pick. Held targets are validated each call (alive, in scene, in
 * timeline, within leash) and re-evaluated when the timer expires —
 * subject to the 0.8× hysteresis factor that prevents rapid wagging
 * across equidistant candidates.
 *
 * Per-navigator state lives on EnemyNetId (Anchor.h) — the canonical
 * "per-actor nav state" container. Lifecycle: state is reset on
 * navigator death / scene change via the same paths that clear netState
 * (existing OnActorKill / OnSceneSpawnActors flow).
 */

#include "TargetSelection.h"

#include "ActorSyncHelpers.h"  // IsSyncableActor / IsSyncedBossActor / kSyncableActorCategories
#include "NavTraits.h"
#include "PlayerLookup.h"     // FindNearestPlayerActor / GetSyncedPlayerActors
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#define CVAR_NAV_ENABLED            CVAR_ENHANCEMENT("Nav.Enabled")
#define CVAR_NAV_TARGET_SELECTION   CVAR_ENHANCEMENT("Nav.TargetSelection")

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Tunables. Locked per plan §6.
// ---------------------------------------------------------------------------

// Hysteresis factor: a candidate must be < 0.8× the held target's
// distance to win re-evaluation. Prevents waggling between two
// near-equidistant candidates as positions drift slightly.
static constexpr float kHysteresisFactor = 0.8f;

// Hard upper bound on leash distance when no per-actor override exists.
// 1500u covers most scene-spanning encounters; per-actor leash overrides
// can be added to NavTraits later.
static constexpr float kDefaultLeashMax = 1500.0f;

// ---------------------------------------------------------------------------
// Per-navigator scratch (Custom candidate set).
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<const Actor*, std::vector<Actor*>>& CustomCandidatesMap() {
    static std::unordered_map<const Actor*, std::vector<Actor*>> map;
    return map;
}

float DistSq(const Vec3f& a, const Vec3f& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

EnemyNetId* GetMutableNavExt(Actor* actor) {
    if (actor == nullptr) return nullptr;
    return const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
}

const EnemyNetId* GetNavExt(const Actor* actor) {
    if (actor == nullptr) return nullptr;
    return ObjectExtension::GetInstance().Get<EnemyNetId>(const_cast<Actor*>(actor));
}

bool ActorIsAlive(const Actor* actor) {
    return actor != nullptr && actor->update != nullptr;
}

// Iterate every candidate from the requested set, returning the nearest
// member. Returns nullptr if the set is empty.
Actor* NearestInSet(Actor* navigator, TargetSet set, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return nullptr;

    // Players: reuse the canonical helper which already filters cross-
    // timeline + out-of-scene DummyPlayers via the (-9999,-9999,-9999)
    // sentinel.
    if (set == TargetSet::Players) {
        return FindNearestPlayerActor(navigator, play);
    }

    if (set == TargetSet::Custom) {
        auto& map = CustomCandidatesMap();
        auto it = map.find(navigator);
        if (it == map.end()) return nullptr;
        Actor* best = nullptr;
        float bestSq = std::numeric_limits<float>::infinity();
        for (Actor* c : it->second) {
            if (!ActorIsAlive(c)) continue;
            float dSq = DistSq(navigator->world.pos, c->world.pos);
            if (dSq < bestSq) {
                bestSq = dSq;
                best   = c;
            }
        }
        return best;
    }

    // Enemies / EnemiesNoBoss — walk the syncable categories.
    Actor* best = nullptr;
    float bestSq = std::numeric_limits<float>::infinity();
    for (uint8_t cat : kSyncableActorCategories) {
        Actor* candidate = play->actorCtx.actorLists[cat].head;
        while (candidate != nullptr) {
            Actor* next = candidate->next;
            if (candidate != navigator && IsSyncableActor(candidate) &&
                ActorIsAlive(candidate)) {
                if (set == TargetSet::EnemiesNoBoss && IsSyncedBossActor(candidate->id)) {
                    candidate = next;
                    continue;
                }
                float dSq = DistSq(navigator->world.pos, candidate->world.pos);
                if (dSq < bestSq) {
                    bestSq = dSq;
                    best   = candidate;
                }
            }
            candidate = next;
        }
    }
    return best;
}

// Returns the held target Actor*, or nullptr if either no target is
// held, or the held representation is FixedPos / invalid (caller must
// fall through to fresh acquisition).
Actor* LookupHeldActor(const EnemyNetId* ext, PlayState* play) {
    if (ext == nullptr || play == nullptr) return nullptr;
    if (ext->navHeldKind == EnemyNetId::HeldTargetKind::None ||
        ext->navHeldKind == EnemyNetId::HeldTargetKind::FixedPos) {
        return nullptr;
    }

    if (ext->navHeldKind == EnemyNetId::HeldTargetKind::Player) {
        // Walk ACTORCAT_PLAYER and ACTORCAT_NPC (for DummyPlayer) to find
        // the matching client ID. Local player is always slot 0; remote
        // DummyPlayers carry their clientId via Anchor::GetDummyPlayerClientId
        // (the underlying ObjectExtension struct is private to Anchor.cpp).
        const uint8_t want = ext->navTargetClientId;
        if (want == 0) {
            // Local player.
            Player* p = GET_PLAYER(play);
            return (p != nullptr) ? &p->actor : nullptr;
        }
        // Remote DummyPlayer — walk ACTORCAT_NPC for ACTOR_EN_OE2 actors
        // bearing the matching client ID.
        Actor* a = play->actorCtx.actorLists[ACTORCAT_NPC].head;
        Anchor* anchor = Anchor::Instance;
        while (a != nullptr) {
            Actor* next = a->next;
            if (a->id == ACTOR_EN_OE2 && anchor != nullptr) {
                uint32_t cid = anchor->GetDummyPlayerClientId(a);
                if (cid == want) {
                    return a;
                }
            }
            a = next;
        }
        return nullptr;
    }

    // Enemy held — walk syncable categories for the matching netId.
    if (ext->navHeldKind == EnemyNetId::HeldTargetKind::Enemy) {
        const uint32_t want = ext->navTargetNetId;
        if (want == 0) return nullptr;
        for (uint8_t cat : kSyncableActorCategories) {
            Actor* c = play->actorCtx.actorLists[cat].head;
            while (c != nullptr) {
                Actor* next = c->next;
                if (IsSyncableActor(c)) {
                    const EnemyNetId* cExt =
                        ObjectExtension::GetInstance().Get<EnemyNetId>(c);
                    if (cExt != nullptr && cExt->netId == want) {
                        return c;
                    }
                }
                c = next;
            }
        }
    }
    return nullptr;
}

// Bind `target` into the navigator's nav-ext state. Pure write; caller
// has already determined that target is valid.
void BindTarget(EnemyNetId* ext, Actor* target, const NavTraits& traits) {
    if (ext == nullptr || target == nullptr) return;
    if (target->id == ACTOR_PLAYER || target->id == ACTOR_EN_OE2) {
        ext->navHeldKind = EnemyNetId::HeldTargetKind::Player;
        if (target->id == ACTOR_PLAYER) {
            ext->navTargetClientId = 0;
        } else {
            // Remote DummyPlayer — read the client ID via Anchor's accessor
            // (the underlying ObjectExtension struct is file-scope-private
            // to Anchor.cpp).
            // The underlying ObjectExtension struct is file-scope-private to
            // Anchor.cpp; access via the public accessor on the global
            // Anchor::Instance.
            uint32_t cid = (Anchor::Instance != nullptr)
                               ? Anchor::Instance->GetDummyPlayerClientId(target)
                               : 0;
            ext->navTargetClientId = (cid != 0) ? (uint8_t)cid : 0xFF;
        }
        ext->navTargetNetId = 0;
    } else {
        // Treat as enemy.
        ext->navHeldKind = EnemyNetId::HeldTargetKind::Enemy;
        const EnemyNetId* tExt =
            ObjectExtension::GetInstance().Get<EnemyNetId>(target);
        ext->navTargetNetId    = (tExt != nullptr) ? tExt->netId : 0;
        ext->navTargetClientId = 0xFF;
    }
    ext->navHeldTargetPos     = target->world.pos;
    ext->navTargetTimerFrames = traits.targetStickyFrames;
    ext->navTargetIsStale     = false;
}

void ClearHeld(EnemyNetId* ext) {
    if (ext == nullptr) return;
    ext->navHeldKind          = EnemyNetId::HeldTargetKind::None;
    ext->navTargetClientId    = 0xFF;
    ext->navTargetNetId       = 0;
    ext->navTargetTimerFrames = 0;
    ext->navTargetIsStale     = false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool IsTargetSelectionEnabled() {
    return CVarGetInteger(CVAR_NAV_ENABLED, 0) != 0
        && CVarGetInteger(CVAR_NAV_TARGET_SELECTION, 0) != 0;
}

Actor* AcquireOrHoldTarget(Actor* navigator, TargetSet set, PlayState* play) {
    if (navigator == nullptr) return nullptr;

    // Master-off / per-feature-off / per-actor opt-out: pure nearest-of-set
    // (no state mutation).
    if (!IsTargetSelectionEnabled()) return NearestInSet(navigator, set, play);

    const NavTraits& traits = GetTraitsForActor(navigator->id);
    if (!traits.useStickyTargeting || traits.targetStickyFrames == 0) {
        return NearestInSet(navigator, set, play);
    }

    EnemyNetId* ext = GetMutableNavExt(navigator);
    if (ext == nullptr) {
        // No extension → cannot persist held state. Fall back to nearest.
        return NearestInSet(navigator, set, play);
    }

    Actor* held = LookupHeldActor(ext, play);
    bool   heldValid = ActorIsAlive(held);

    // Validate leash and timeline / scene presence.
    if (heldValid && play != nullptr) {
        float dSq = DistSq(navigator->world.pos, held->world.pos);
        if (dSq > kDefaultLeashMax * kDefaultLeashMax) {
            heldValid = false;
        }
    }

    if (!heldValid) {
        // Re-acquire fresh.
        Actor* fresh = NearestInSet(navigator, set, play);
        if (fresh != nullptr) {
            BindTarget(ext, fresh, traits);
        } else {
            ClearHeld(ext);
        }
        return fresh;
    }

    // Held is valid. Consume the timer.
    if (ext->navTargetTimerFrames > 0) {
        ext->navTargetTimerFrames--;
        // Cache the most recent position for downstream consumers.
        ext->navHeldTargetPos = held->world.pos;
        return held;
    }

    // Timer expired — evaluate hysteresis.
    Actor* nearest = NearestInSet(navigator, set, play);
    if (nearest != nullptr && nearest != held) {
        float distHeldSq    = DistSq(navigator->world.pos, held->world.pos);
        float distNearestSq = DistSq(navigator->world.pos, nearest->world.pos);
        // Compare squared distances against squared hysteresis factor.
        float threshold = kHysteresisFactor * kHysteresisFactor * distHeldSq;
        if (distNearestSq < threshold) {
            BindTarget(ext, nearest, traits);
            return nearest;
        }
    }

    // Hysteresis kept the held target — refresh timer + cached pos.
    ext->navTargetTimerFrames = traits.targetStickyFrames;
    ext->navHeldTargetPos     = held->world.pos;
    return held;
}

TargetChoice ChooseTarget(Actor* navigator, TargetSet set, PlayState* play) {
    TargetChoice out;
    Actor* a = AcquireOrHoldTarget(navigator, set, play);
    out.actor = a;
    if (a != nullptr) {
        out.fallbackPos = a->world.pos;
        out.isStale     = false;
    } else {
        // Surface the cached fallback if a held target was recently lost.
        const EnemyNetId* ext = GetNavExt(navigator);
        if (ext != nullptr && ext->navHeldKind != EnemyNetId::HeldTargetKind::None) {
            out.fallbackPos = ext->navHeldTargetPos;
            out.isStale     = true;
        }
    }
    return out;
}

void HoldPositionTarget(Actor* navigator, const Vec3f* targetPos) {
    EnemyNetId* ext = GetMutableNavExt(navigator);
    if (ext == nullptr) return;
    if (targetPos == nullptr) {
        ClearHeld(ext);
        return;
    }
    ext->navHeldKind          = EnemyNetId::HeldTargetKind::FixedPos;
    ext->navHeldTargetPos     = *targetPos;
    ext->navTargetClientId    = 0xFF;
    ext->navTargetNetId       = 0;
    ext->navTargetTimerFrames = 0;
    ext->navTargetIsStale     = false;
}

uint8_t GetHeldTargetClientId(const Actor* navigator) {
    const EnemyNetId* ext = GetNavExt(navigator);
    if (ext == nullptr) return 0xFF;
    if (ext->navHeldKind != EnemyNetId::HeldTargetKind::Player) return 0xFF;
    return ext->navTargetClientId;
}

uint32_t GetHeldTargetNetId(const Actor* navigator) {
    const EnemyNetId* ext = GetNavExt(navigator);
    if (ext == nullptr) return 0;
    if (ext->navHeldKind != EnemyNetId::HeldTargetKind::Enemy) return 0;
    return ext->navTargetNetId;
}

void ResetTargetLock(Actor* navigator) {
    EnemyNetId* ext = GetMutableNavExt(navigator);
    if (ext == nullptr) return;
    ClearHeld(ext);
}

void SetCustomCandidates(Actor* navigator, const std::vector<Actor*>& candidates) {
    if (navigator == nullptr) return;
    CustomCandidatesMap()[navigator] = candidates;
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

namespace {

void TargetSelectionOnExitGameClear(int32_t /*fileNum*/) {
    CustomCandidatesMap().clear();
}

}  // anonymous namespace

void RegisterTargetSelection() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnExitGame>(
        TargetSelectionOnExitGameClear);
}

} // namespace AnchorNav

static RegisterShipInitFunc registerTargetSelection(AnchorNav::RegisterTargetSelection);
