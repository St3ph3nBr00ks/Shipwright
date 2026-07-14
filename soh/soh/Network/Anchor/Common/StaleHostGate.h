#ifndef SOH_ANCHOR_COMMON_STALE_HOST_GATE_H
#define SOH_ANCHOR_COMMON_STALE_HOST_GATE_H

#include <cstdint>

struct EnemyNetId;

namespace EnemyStateSync {

// Freshness threshold. When more than this many wall-clock ms have
// elapsed since the last ENEMY_STATE from host for a given netId,
// the host is considered "stale" and per-actor sync sites should
// defer to the peer's local AI instead of clobbering it back to
// the last cached broadcast.
//
// 500 ms tuning rationale:
//   - Host's normal ENEMY_STATE cadence for near enemies is 15+ Hz
//     (throttle band 500u max delay ≈ 66 ms). At 1000u it drops to
//     5 Hz (≈200 ms). 500 ms comfortably exceeds worst-case natural
//     silence for actively-updating actors near a player.
//   - Distance throttling (EnemyState.cpp) can go quiet for further
//     enemies indefinitely — but those are also far from every peer,
//     so the peer's local AI running freely does no harm.
//   - Actor culling (host walks far from actor's room) produces
//     30+ second silences; the gate catches those instantly.
//   - RTT + brief network hitches are well under 500 ms.
//
// Not currently CVar-exposed. If field-test reveals a need to tune
// it (per-actor or globally), add a CVar in a follow-up commit.
constexpr uint64_t kHostStalenessThresholdMs = 500;

// Called from per-actor sync sites in HookHandlers.cpp before the
// force-apply of `ext->netStateIndex`. Returns true when peer should
// let its own local AI drive this actor instead of snapping it back
// to the last cached broadcast. The caller wraps the apply in an
// `&& !ShouldDeferToPeerLocalAI(...)` clause.
//
// Behaviour:
//   - Returns false for boss actors (IsSyncedBossActor) — bosses stay
//     strictly host-authoritative regardless of host silence. Boss
//     fights are host-driven scripted encounters where any peer-side
//     divergence would break the scripted state machine.
//   - Returns false when no broadcast has ever been received
//     (ext->lastStateReceiveMs == 0). No history means no basis to
//     judge staleness; fall through to the standard sync policy.
//   - Returns true when (nowMs - ext->lastStateReceiveMs) exceeds
//     kHostStalenessThresholdMs.
//
// Safe to call on a null `ext` — returns false. Caller-side null
// check is redundant but harmless.
bool ShouldDeferToPeerLocalAI(int16_t actorId,
                              const EnemyNetId* ext,
                              uint64_t nowMs);

// Called from the ENEMY_STATE receive path (HandlePacket_EnemyUpdate)
// when a broadcast for this actor's netId has been accepted. Stamps
// ext->lastStateReceiveMs with the current wall-clock ms so a
// subsequent ShouldDeferToPeerLocalAI call at the sync site can
// measure elapsed silence.
//
// Safe to call on a null `ext` — no-ops.
void RecordStateReceive(EnemyNetId* ext, uint64_t nowMs);

// Convenience — wraps steady_clock::now() into ms. Consumers may
// call this locally instead if they already have a timestamp.
uint64_t NowMs();

}  // namespace EnemyStateSync

#endif  // SOH_ANCHOR_COMMON_STALE_HOST_GATE_H
