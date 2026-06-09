#pragma once

// Title-screen peer formation perturbation.
//
// Architectural seam introduced in preparation for Phase 2+ horse
// integration and the v3+ "natural-looking gallop" polish pass.
// See Claude/Plans/title_screen_peer_actors.md §"Open design
// questions" Q5 (animation phase offset) and Phase 2 plan
// Claude/Plans/title_screen_horse_phase2_handoff.md §"Forward-
// looking — formation polish (v2+)".
//
// Two call sites consume this:
//   1. Anchor::SpawnTitlePeerLink — initial spawn position
//   2. DummyPlayer_Update title-mode branch — per-tick position
//
// In Phase 1 (current) both call sites pass the resulting formation
// struct through ComputeBaseFormationSlot + zero-offset fields, so
// behaviour matches the straight-line, lock-step gallop you see
// today.
//
// To enable polish in v3+, modify ONLY the body of
// MakeTitlePeerFormation below — populate the three fields with
// hash-derived values keyed on clientId. The per-tick code at the
// two call sites does not need to change.

#include <libultraship/libultraship.h>
#include <cmath>
#include <cstdint>

extern "C" {
#include "z64math.h"  // Vec3f, Vec3s
}

namespace AnchorTitlePeer {

// Per-peer formation perturbation applied on top of the base formation
// slot. v2 ships with all-zero defaults — produces the straight-line
// formation that Phase 1 already validated visually. v3+ populates
// these to break the parade-ground look.
struct TitlePeerFormation {
    // Position offset added to the base formation slot.
    //   v2: zero.
    //   v3+: small lateral stagger (recommended ±20-40u in X), hash-
    //        derived from clientId so the same peer gets the same
    //        offset across rejoins.
    Vec3f posOffset = { 0.0f, 0.0f, 0.0f };

    // Heading offset added to local Link's rot.y. Q1.15 fixed-point
    // s16 (32768 = pi radians, so 0x0400 ≈ 2.8°).
    //   v2: zero (rider's yaw matches local Link exactly).
    //   v3+: ±0x0400 to ±0x0800 (~2-4°) hash-derived so riders aren't
    //        perfectly parallel.
    int16_t yawOffset = 0;

    // Animation phase offset, in frames, on the gallop loop. Applied
    // to the peer's skelAnime curFrame at spawn time so peer hooves
    // don't strike in unison with the local Link's gallop animation.
    //   v2: zero (peer animates in lockstep with local Link).
    //   v3+: 0-N frames hash-derived from clientId.
    int16_t animPhaseOffset = 0;
};

// Compute the per-peer perturbation. Stable function of clientId +
// formationIdx — same inputs always produce the same outputs, so
// peers retain their formation slot across rejoins.
//
// v3 (Phase 3) — hash-derived stagger + yaw divergence populated.
// animPhaseOffset stays zero until the horse-animation work lands
// (the consumer site — local Link skelAnime alias — doesn't admit
// per-peer phase yet).
//
// Hash derivation: bit-mixing of clientId via xor-shift. clientIds
// are typically incremental small numbers (117381, 119901, ...) —
// direct values give correlated offsets; the mix diffuses them.
inline TitlePeerFormation MakeTitlePeerFormation(uint32_t clientId,
                                                  uint8_t formationIdx) {
    uint32_t h = clientId ^ ((uint32_t)formationIdx * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;

    TitlePeerFormation f;

    // Lateral stagger ±40u in X (relative to peer's facing direction;
    // for our v3 spawn site, the formation is rotated to face Link's
    // heading, so X is left/right relative to the gallop). Z (forward/
    // back of slot) intentionally zero — staggering in Z would shift
    // peers into each other's formation slots.
    f.posOffset.x = (((float)((h >> 0) & 0xFF) / 255.0f) - 0.5f) * 80.0f;
    f.posOffset.y = 0.0f;
    f.posOffset.z = 0.0f;

    // Yaw divergence ±~4° in Q1.15 s16 fixed-point. 0x1000 ≈ 11.25°,
    // so ±0x0800 ≈ ±5.6°. Pulled from a byte, normalised to ±0.5,
    // scaled to ±0x0800.
    f.yawOffset = (int16_t)(
        (((int32_t)((h >> 8) & 0xFF) - 128) * 0x1000) / 256);

    // animPhaseOffset: deferred. Local-Link skelAnime alias gives all
    // peers the same animation frame as local Link — phase offset
    // requires switching to manual animation playback (task #28).
    f.animPhaseOffset = 0;

    return f;
}

// Compute the base (un-perturbed) formation slot. Single-file column
// behind local Link, 250u spacing per formation index, matching Link's
// ground Y. Pure function of local Link state + index — independent
// of clientId.
//
// Spacing chosen to keep peer Eponas clear of local Link's vanilla
// Epona — EnHorse cylinder colliders are ~60u radius each, and the
// per-shot camera path can sweep across the peer formation. 250u
// gives ~125u clearance between horse cylinders even at the closest
// formation slot (formationIdx=0). Phase 1 used 40u which was fine
// for on-foot peers but caused collision-and-slide bugs once Phase 2
// added peer horses (field test 2026-06-09).
//
// Title-cutscene camera audit (see Plans/title_screen_peer_actors.md
// §"Camera audit"): wide shots (1, 2, 4, 6-9) have Link at small visual
// scale so 250u-back peers remain in-frame. Close shots (3, 5) have
// peers fully behind the camera so they don't crowd the rider — minor
// trade-off vs. the closer 40u formation.
inline Vec3f ComputeBaseFormationSlot(Vec3f linkPos, int16_t linkRotY,
                                        uint8_t formationIdx) {
    constexpr float kSpacingPerSlot = 250.0f;
    const float distance =
        kSpacingPerSlot * (float)(formationIdx + 1);
    const float heading =
        (float)linkRotY / 32768.0f * 3.14159265358979323846f;
    return Vec3f{
        linkPos.x - distance * sinf(heading),
        linkPos.y,
        linkPos.z - distance * cosf(heading),
    };
}

// Convenience: combine base slot + perturbation into the final
// rider-target position + yaw. Both consumer call sites use this.
struct TitlePeerSlot {
    Vec3f   pos;
    int16_t rotY;
};

inline TitlePeerSlot ComputeTitlePeerSlot(Vec3f linkPos, int16_t linkRotY,
                                            uint32_t clientId,
                                            uint8_t formationIdx) {
    const TitlePeerFormation f =
        MakeTitlePeerFormation(clientId, formationIdx);
    Vec3f basePos =
        ComputeBaseFormationSlot(linkPos, linkRotY, formationIdx);
    basePos.x += f.posOffset.x;
    basePos.y += f.posOffset.y;
    basePos.z += f.posOffset.z;
    return TitlePeerSlot{
        basePos,
        (int16_t)(linkRotY + f.yawOffset),
    };
}

}  // namespace AnchorTitlePeer
