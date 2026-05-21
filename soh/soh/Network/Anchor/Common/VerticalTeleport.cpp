/**
 * VerticalTeleport — Shape B implementation (commit 6a).
 *
 * Per plan §9. Shape B (non-player teleport) is the v1-self-contained
 * path: when a synced enemy's target is on a different Y level by more
 * than NavTraits.verticalTeleportYThreshold for verticalTeleportDelayFrames
 * AND a climb path exists AND the cooldown is elapsed AND
 * eligibleForVerticalTeleport is true, write a new world.pos at the
 * climb anchor's topPos.
 *
 * Fast path bypasses delay + cooldown when the target is a player
 * with isClimbing=true — strict generalization of the existing G1/G2
 * follower-teleport reactivity. Per-frame re-fire while the climbing
 * flag holds.
 *
 * Shape A wiring (player-derived navigators / AI Player Follower) lands in
 * commit 6b. Hang-state resolution lands in commit 6c. This commit
 * exposes the public predicates `IsTargetClimbing` and
 * `IsPlayerDerivedNavigator` so the Shape A wiring can dispatch
 * cleanly.
 */

#include "VerticalTeleport.h"

#include "ActorSyncHelpers.h"  // IsSyncableActor (avoid teleporting non-syncable actors)
#include "ClimbableSurfaces.h"
#include "NavCVars.h"
#include "NavTraits.h"
#include "TargetSelection.h"   // GetHeldTargetClientId for FixedPos kind read-back
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ShipInit.hpp"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>

#include <cmath>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace AnchorNav {

// ---------------------------------------------------------------------------
// Tunables (file-scope; locked per plan §9).
// ---------------------------------------------------------------------------

namespace {

// Slow-path cooldown after a teleport fires. ~4s @ 60fps — long enough
// to avoid rapid re-fire while the navigator's AI processes the new
// position.
constexpr uint16_t kSlowPathCooldownFrames = 240;

// Search radius for FindNearestClimbable. Climb anchors farther than
// this are considered unreachable and the slow path is suppressed.
constexpr float kClimbSearchRadius = 600.0f;

// Lateral offset perpendicular to climb yaw, applied after a teleport
// to put the navigator at the rim of the climb anchor (not embedded
// in the wall).
constexpr float kRimOffsetUnits = 12.0f;

// Cross-room slow-path teleport via RespawnData is reserved — v1
// limits Shape B to same-room teleports. The same-room invariant is
// derived from `target->room == navigator->room`.

EnemyNetId* GetMutableNavExt(Actor* actor) {
    if (actor == nullptr) return nullptr;
    return const_cast<EnemyNetId*>(
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor));
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public predicates.
// ---------------------------------------------------------------------------

bool IsVerticalTeleportEnabled() {
    return AnchorNavCVars::IsFeatureEnabled(AnchorNavCVars::kVerticalTeleport);
}

bool IsPlayerDerivedNavigator(const Actor* actor) {
    if (actor == nullptr) return false;
    // ACTOR_PLAYER is always Link-rigged. ACTOR_EN_OE2 is the
    // DummyPlayer / AI Player Follower actor — same Player_Update path,
    // same input rig.
    return actor->id == ACTOR_PLAYER || actor->id == ACTOR_EN_OE2;
}

bool IsShapeAEligible(const Actor* actor) {
    // Shape A is the player-derived path. It opts in only when the
    // master Nav CVar + per-feature CVar are both on AND the actor is
    // Link-rigged AND its NavTraits row has eligibleForVerticalTeleport
    // true. Bosses fall through via kBossDefaults; the legacy follower
    // pipeline still runs when the nav system is off (the existing
    // gRemoteAnchor.FollowerEnabled CVar gate is the canonical
    // master switch for follower behaviour itself).
    if (actor == nullptr) return false;
    if (!IsVerticalTeleportEnabled()) return false;
    if (!IsPlayerDerivedNavigator(actor)) return false;
    const NavTraits& traits = GetTraitsForActor(actor->id);
    return traits.eligibleForVerticalTeleport;
}

bool IsTargetClimbing(const Actor* target) {
    if (target == nullptr) return false;
    if (target->id == ACTOR_PLAYER) {
        Anchor* anchor = Anchor::Instance;
        if (anchor != nullptr) {
            return anchor->IsLocalPlayerClimbing();
        }
        return false;
    }
    if (target->id == ACTOR_EN_OE2) {
        // Read the broadcast-isClimbing field from the matching
        // AnchorClient. The clientId is carried on the DummyPlayer
        // via Anchor::GetDummyPlayerClientId.
        Anchor* anchor = Anchor::Instance;
        if (anchor == nullptr) return false;
        uint32_t cid = anchor->GetDummyPlayerClientId(target);
        if (cid == 0) return false;
        return anchor->GetClientIsClimbing(cid);
    }
    // Non-player target — no climbing-state signal exists for
    // enemies. Slow path is the only signal available for them.
    return false;
}

// ---------------------------------------------------------------------------
// Shape B — non-player teleport.
// ---------------------------------------------------------------------------

bool TryVerticalTeleportEnemy(Actor* navigator, Actor* target, PlayState* play) {
    if (navigator == nullptr || play == nullptr) return false;
    if (!IsVerticalTeleportEnabled()) return false;

    // Shape A handles player-derived navigators in HookHandlers.cpp;
    // this Shape B function never teleports them.
    if (IsPlayerDerivedNavigator(navigator)) return false;

    // Boss exclusion + per-actor opt-out via NavTraits.
    const NavTraits& traits = GetTraitsForActor(navigator->id);
    if (!traits.eligibleForVerticalTeleport) return false;

    EnemyNetId* ext = GetMutableNavExt(navigator);
    if (ext == nullptr) return false;

    // Resolve target position. If `target` is null, fall back to the
    // navigator's held FixedPos (HoldPositionTarget API). Both kinds
    // surface a Vec3f the algorithm can compare against.
    Vec3f targetPos;
    bool  haveTarget = false;
    if (target != nullptr && target->update != nullptr) {
        targetPos  = target->world.pos;
        haveTarget = true;
    } else if (ext->navHeldKind == EnemyNetId::HeldTargetKind::FixedPos) {
        targetPos  = ext->navHeldTargetPos;
        haveTarget = true;
    }
    if (!haveTarget) {
        // No target → reset transient state, no teleport.
        ext->navVerticalMismatchFrames = 0;
        if (ext->navTeleportCooldownFrames > 0) ext->navTeleportCooldownFrames--;
        return false;
    }

    // ── FAST PATH ─────────────────────────────────────────────────
    // Per-frame re-fire while target is a player with isClimbing=true.
    // Bypasses delay + cooldown — matches the existing G1/G2 reactivity.
    if (target != nullptr && IsPlayerDerivedNavigator(target) && IsTargetClimbing(target)) {
        std::optional<ClimbAnchor> hit =
            FindNearestClimbable(navigator->world.pos, kClimbSearchRadius, play);
        if (hit.has_value()) {
            navigator->world.pos = hit->topPos;
        } else {
            // Direct attach when no climb path — preserves existing
            // ride-along behaviour for enemies without a usable anchor.
            navigator->world.pos = targetPos;
        }
        ext->navVerticalMismatchFrames = 0;
        // Do NOT set cooldown — fast path is per-frame attach.
        return true;
    }

    // ── SLOW PATH ─────────────────────────────────────────────────
    if (ext->navTeleportCooldownFrames > 0) {
        ext->navTeleportCooldownFrames--;
        return false;
    }

    const float dy = std::fabs(targetPos.y - navigator->world.pos.y);
    if (dy < (float)traits.verticalTeleportYThreshold) {
        ext->navVerticalMismatchFrames = 0;
        return false;
    }

    if (ext->navVerticalMismatchFrames < traits.verticalTeleportDelayFrames) {
        ext->navVerticalMismatchFrames++;
        return false;
    }

    // Mismatch persisted long enough — find a climb path.
    std::optional<ClimbAnchor> hit =
        FindNearestClimbable(navigator->world.pos, kClimbSearchRadius, play);
    if (!hit.has_value()) {
        // No path → don't reset the counter (we'll keep watching for
        // an anchor to load), but also don't teleport.
        return false;
    }

    // Teleport to topPos with a small lateral offset so the navigator
    // sits at the rim, not embedded in the climbable's geometry.
    Vec3f rim = hit->topPos;
    rim.x += Math_SinS(hit->approachYaw) * kRimOffsetUnits;
    rim.z += Math_CosS(hit->approachYaw) * kRimOffsetUnits;
    navigator->world.pos = rim;

    ext->navVerticalMismatchFrames = 0;
    ext->navTeleportCooldownFrames = kSlowPathCooldownFrames;
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void RegisterVerticalTeleport() {
    // No hooks at this layer — Shape A wiring (commit 6b) registers
    // its hang-state resolution / input bridge separately. Stub kept
    // for parity with sibling modules.
}

} // namespace AnchorNav

static RegisterShipInitFunc registerVerticalTeleport(AnchorNav::RegisterVerticalTeleport);
