#pragma once

// AnchorNavExt — per-navigator nav-substrate state (refactor B.4).
//
// Lifted from EnemyNetId 2026-06-04 per
// Plans/decoupling_gap_audit_2026-05-16.md §3. Despite the historical
// name (EnemyNetId), nav state was already attached to navigators that
// aren't enemies (AI Player Follower / DummyPlayer / future Link-rigged
// navigators). Keeping the fields on EnemyNetId conflated two
// concerns:
//   - network sync (netId / netPos / netHealth / phase / ...)
//   - nav substrate (held-target representation, vertical-teleport
//     mismatch counter, teleport cooldown)
//
// Both extensions key on the same ObjectExtension actor pointer.
// Consumers fetch via `ObjectExtension::GetInstance().Get<AnchorNavExt>(actor)`.
// Registered in Anchor.cpp alongside EnemyNetId.

#include <cstdint>

extern "C" {
#include "z64.h"  // Vec3f
}

struct AnchorNavExt {
    // navHeldKind discriminates the held-target representation:
    //   None     → no target held; AcquireOrHoldTarget will evaluate fresh.
    //   Player   → navTargetClientId is the held target's client ID.
    //   Enemy    → navTargetNetId is the held actor's netId.
    //   FixedPos → navHeldTargetPos is a pinned world position
    //              (HoldPositionTarget).
    enum class HeldTargetKind : uint8_t { None = 0, Player, Enemy, FixedPos };

    HeldTargetKind navHeldKind          = HeldTargetKind::None;
    uint8_t        navTargetClientId    = 0xFF;        // valid when navHeldKind == Player
    uint32_t       navTargetNetId       = 0;           // valid when navHeldKind == Enemy
    Vec3f          navHeldTargetPos     = { 0.0f, 0.0f, 0.0f }; // valid when navHeldKind == FixedPos OR cached for trails
    uint16_t       navTargetTimerFrames = 0;           // counts down from NavTraits.targetStickyFrames
    bool           navTargetIsStale     = false;       // true if returning a held target despite invalidation signals

    // VerticalTeleport (plan §9). Counts up while |Δy(target, navigator)|
    // exceeds NavTraits.verticalTeleportYThreshold; reaches
    // verticalTeleportDelayFrames before the slow-path teleport fires.
    // Cooldown counts down after a slow-path teleport to avoid rapid
    // re-fire.
    uint16_t navVerticalMismatchFrames = 0;
    uint16_t navTeleportCooldownFrames = 0;
};
