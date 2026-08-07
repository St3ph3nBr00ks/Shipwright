/**
 * PlayerStunManager — GH #333 web-attack stun state management.
 *
 * Sidecar map on Anchor tracking per-client stun state (keyed by
 * Anchor clientId). Not stored on Player actor itself because the
 * state needs to persist across ownership boundaries (host applies
 * stun to a peer's DummyPlayer; the peer's own client sees the
 * effect via broadcast and applies to their own local Link).
 *
 * v1 scope (Phase 4a — this file at commit time):
 *   - Sidecar map + lifecycle (apply/clear/query).
 *   - Per-tick timer decrements + 10s hard-cap auto-clear (A per
 *     user 2026-08-06).
 *   - Local input suppression via OnPlayerUpdate pre-hook when local
 *     player is stunned. Full action lock per user.
 *   - Discrete escape-event tracking (A press / B press / stick
 *     release-to-deflect / cardinal-direction change) — 10 events
 *     in rolling 3-second window triggers SELF clear (A5).
 *   - Cutscene / scene-transition local clear (A10).
 *
 * Deferred to Phase 4b:
 *   - PLAYER_STUN_APPLIED / PLAYER_STUN_CLEARED packet broadcast.
 *     ApplyStun/ClearStun here update local map only; peer sync
 *     wiring lives in Packets/Player/PlayerStun*.cpp.
 * Deferred to Phase 4c:
 *   - Peer DummyPlayer stun overlay draw.
 *   - Sword + fire rescue detection.
 *
 * Design brief: GH #333 comment 5209793604 (A1-A15 resolved).
 */

#pragma once

#include <cstdint>
#include <deque>

// Forward-decls to avoid pulling z64.h into headers unnecessarily.
struct Actor;
struct PlayState;
typedef struct PlayState PlayState;

namespace AnchorPlayerStun {

// Reason for a stun clear. Wire values are stable — used in Phase 4b
// PLAYER_STUN_CLEARED packet payload. Additions go at the end.
enum class ClearReason : uint8_t {
    Self         = 0,  // stunned player mashed to threshold
    PeerSword    = 1,  // teammate sword AT hit the web
    PeerFire     = 2,  // fire-family source hit the web
    Timeout      = 3,  // 10s hard cap fired
    SceneChange  = 4,  // stunned player's scene changed
    Cutscene     = 5,  // Play_InCsMode became true
    Disconnect   = 6,  // stunned peer disconnected (host reap)
    Reserved     = 7,
};

// Per-client stun state entry. One instance per stunned client;
// absent from map when not stunned.
struct StunEntry {
    uint32_t                 sourceEnSwNetId = 0;  // which spider webbed (informational)
    uint64_t                 appliedAtMs     = 0;  // wall-clock, for 10s hard cap
    // Escape-event tracking — timestamps of the last N discrete
    // events (A press / B press / stick release-to-deflect /
    // cardinal-direction change). Rolling 3s window: older events
    // are purged at the start of each tick. When size hits
    // kEscapeThreshold, SELF clear fires.
    std::deque<uint64_t>     escapePressesMs;
    // Prev-frame input state for discrete-event detection.
    uint8_t                  prevAButton     = 0;
    uint8_t                  prevBButton     = 0;
    int8_t                   prevStickX      = 0;
    int8_t                   prevStickY      = 0;
    // Cardinal direction of prev-frame stick (0=neutral, 1=up, 2=right,
    // 3=down, 4=left). Change to a different cardinal counts as an
    // escape event.
    uint8_t                  prevCardinal    = 0;
};

// Tunables — kept as constants here so the state machine + bridge
// share the same numbers.
constexpr int      kEscapeThreshold      = 10;      // A5: 10 events
constexpr uint64_t kEscapeWindowMs       = 3000;    // A5: within 3s
constexpr uint64_t kStunTimeoutMs        = 10000;   // A cap: 10s hard cap
constexpr int      kStickDeadzone        = 30;      // s8 stick past this = "deflected"

// Query — is this client currently stunned?
bool IsClientStunned(uint32_t clientId);

// Query via actor pointer. Resolves actor → clientId (local Link or
// DummyPlayer) then queries. Returns false if actor isn't a player.
// Used by EnSw state machine's TryEnterWebAttack no-multi-stack gate.
bool IsActorStunned(Actor* playerActor);

// Apply stun to a client. Called from PLAYER_STUN_APPLIED packet
// handler (Phase 4b — via wire) AND directly from
// Anchor_Enhance_EnSwWeb_DetectAndApplyHit on host (local apply
// alongside broadcast). Idempotent: if already stunned, no-op.
void ApplyStun(uint32_t clientId, uint32_t sourceEnSwNetId);

// Clear stun for a client with a reason. Idempotent: no-op if not
// stunned. Callers: SELF clear (from tick handler mash detect),
// PLAYER_STUN_CLEARED packet handler, cutscene/scene-change local
// detect, 10s timeout.
void ClearStun(uint32_t clientId, ClearReason reason);

// Per-frame tick. Called from OnGameFrameUpdate. Handles:
//   - 10s timeout auto-clear per stunned client.
//   - Local player's input suppression (if local is stunned).
//   - Local player's escape-event detection + threshold trigger.
//   - Cutscene/scene-change local clear detect.
void Tick(PlayState* play);

// Scene-init hook — clears the entire stun map on scene transition.
// A10: web auto-clears on scene start (both because Play_InCsMode
// often becomes true briefly during transitions AND because peers
// may be in different scenes post-transition).
void OnSceneInit(void);

// Disconnect cleanup — called when a client disconnects. Reaps their
// entry so a reconnect starts fresh.
void OnClientDisconnect(uint32_t clientId);

}  // namespace AnchorPlayerStun

// C-linkage shims for actor-side / bridge consumers. Match the
// project convention (Pitfall 7).
#ifdef __cplusplus
extern "C" {
#endif

// Query: is the actor (local Player or DummyPlayer) currently stunned?
int Anchor_PlayerStun_IsActorStunned(Actor* playerActor);

// Query: is the given clientId currently stunned?
int Anchor_PlayerStun_IsClientStunned(uint32_t clientId);

// Apply: called from EnSw_Web bridge on host after projectile
// proximity hit + victim clientId resolved. Broadcasts occur inside
// this call in Phase 4b.
void Anchor_PlayerStun_ApplyStun(uint32_t victimClientId,
                                   uint32_t sourceEnSwNetId);

// Clear: called from cutscene detection / self-mash / peer-rescue
// sites. Broadcasts occur inside this call in Phase 4b.
void Anchor_PlayerStun_ClearStun(uint32_t victimClientId, uint8_t reason);

#ifdef __cplusplus
}
#endif
