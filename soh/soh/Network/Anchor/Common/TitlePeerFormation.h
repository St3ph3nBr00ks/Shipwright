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
// v2 stub: returns all-zero defaults. To enable v3 polish, replace
// the body with hash-derived values. NO OTHER SITE needs changes.
inline TitlePeerFormation MakeTitlePeerFormation(uint32_t clientId,
                                                  uint8_t formationIdx) {
    (void)clientId;
    (void)formationIdx;
    // v3+ implementation sketch — uncomment and tune when ready:
    //   const uint32_t h = std::hash<uint32_t>{}(clientId);
    //   TitlePeerFormation f;
    //   f.posOffset.x       = (((h >>  0) & 0xFF) / 255.0f - 0.5f) * 80.0f;
    //   f.yawOffset         = (int16_t)((((h >>  8) & 0xFF) / 255.0f - 0.5f) * 0x1000);
    //   f.animPhaseOffset   = (int16_t)((h >> 16) & 0x1F);  // 0..31 frames
    //   return f;
    return TitlePeerFormation{};
}

// Compute the base (un-perturbed) formation slot. Single-file column
// behind local Link, 40u spacing per formation index, matching Link's
// ground Y. Pure function of local Link state + index — independent
// of clientId.
//
// Spacing chosen from the title-cutscene camera audit (see plan §
// "Camera audit"): Shot 5 (`-120u` camera distance to Link) is the
// tightest. 40u behind Link keeps each peer within Link's silhouette
// projection across all 9 cutscene shots, so the formation never
// clips the camera regardless of which shot is active.
inline Vec3f ComputeBaseFormationSlot(Vec3f linkPos, int16_t linkRotY,
                                        uint8_t formationIdx) {
    constexpr float kSpacingPerSlot = 40.0f;
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
