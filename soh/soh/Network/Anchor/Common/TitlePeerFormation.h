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

    // Hash-derived perturbation overlaid on top of the slot-index-
    // dependent base stagger in ComputeBaseFormationSlot. The base
    // pattern picks left/right + distance per slot to spread the
    // formation; hash perturbs ±30u laterally and ±40u along the
    // gallop axis to break the strict pattern. Y stays zero —
    // ground snap in DummyPlayer_Update would override any Y
    // perturbation anyway.
    f.posOffset.x = (((float)((h >> 0) & 0xFF) / 255.0f) - 0.5f) * 60.0f;
    f.posOffset.y = 0.0f;
    f.posOffset.z = (((float)((h >> 8) & 0xFF) / 255.0f) - 0.5f) * 80.0f;

    // Yaw divergence — kept at zero. Phase 3 originally introduced
    // ±~5.6° per-peer hash-derived yaw stagger for visual variety,
    // but with peers positioned 250u behind + ±40u laterally from
    // local Link, even a small yaw turn lands the peer's gaze line
    // close to local Link's position. Field test 2026-06-09 — user
    // perceived peers as "turning to look at the local player" with
    // any non-zero yaw stagger. Intent: peers should look at the
    // same waypoint the local Link is galloping toward. Solution:
    // all peers face EXACTLY local Link's facing direction.
    //
    // posOffset is retained — lateral position stagger breaks up the
    // strict single-file column without redirecting any peer's gaze.
    f.yawOffset = 0;

    // Per-peer animation phase as a fraction of loop length [0..1).
    // Consumer sites multiply by the actual animation's loopFrames
    // (read via Animation_GetLastFrame at runtime) to get the
    // start-frame offset. Encoded as a fraction (not a frame count)
    // because different animations have different loop lengths
    // (Epona gallop 23 frames, Epona idle 119 frames, Link mounted-
    // idle 29 frames — see field-test log 487 measurements). A
    // fixed frame-count would overshoot short loops and under-
    // shoot long ones.
    //
    // Slot-deterministic 8-step table: slots 0/1/2 → 25%/50%/75%
    // (the user-requested values for a 3-peer formation), slots
    // 3-7 fill in the remaining 1/8ths so no two formation slots
    // share an identical phase. animPhaseOffset retains its
    // historic name for compatibility with consumer code; the
    // semantic is now "fraction × 256" (s16, range 0..255).
    static constexpr int16_t kPhaseTable[8] = {
        64,   // 0 →  25.0%
        128,  // 1 →  50.0%
        192,  // 2 →  75.0%
        32,   // 3 →  12.5%
        96,   // 4 →  37.5%
        160,  // 5 →  62.5%
        224,  // 6 →  87.5%
        0,    // 7 →   0.0%
    };
    f.animPhaseOffset = kPhaseTable[formationIdx & 7];

    return f;
}

// Compute the base (un-perturbed) formation slot. Staggered V/diamond
// shape behind local Link — alternates left/right with growing
// distance, matching Link's ground Y. Pure function of local Link
// state + index — independent of clientId.
//
// Formation layout (forward = local Link's facing direction):
//
//       Local Link (origin)
//
//          slot 0          slot 1     ← 220u back, ±130u laterally
//       (left, near)    (right, near)
//
//      slot 2                   slot 3   ← 400u back, ±200u laterally
//   (left, mid)              (right, mid)
//
//          slot 4                       ← 600u back, centered
//       (deep center)
//
//   ...further slots single-file at 250u increments behind slot 4.
//
// Per-slot tables tuned against Plans/title_screen_peer_actors.md
// §"Camera audit":
//   - Wide shots (1, 2, 4, 6-9): Link is at small visual scale, so
//     220-600u back + ±200u lateral remains in-frame.
//   - Close shots (3, 5): peers fully behind the camera — lateral
//     spread doesn't matter.
// Collider clearance: EnHorse cylinder colliders are ~60u radius.
// Closest pair (slots 0 + 1) is 260u apart laterally → ~140u
// clearance between cylinders. Closest to local Link is 220u
// straight-back → ~160u clearance.
struct SlotEntry {
    float distance;
    float lateral;
};

inline SlotEntry GetBaseSlotEntry(uint8_t formationIdx) {
    // Hand-tuned layout for the typical 2-5 peer count. Slots 5+
    // fall through to single-file at 250u increments behind slot 4.
    switch (formationIdx) {
        case 0:  return { 220.0f, -130.0f };  // near left
        case 1:  return { 220.0f, +130.0f };  // near right
        case 2:  return { 400.0f, -200.0f };  // mid left
        case 3:  return { 400.0f, +200.0f };  // mid right
        case 4:  return { 600.0f,    0.0f };  // deep center
        default: {
            const float distance =
                600.0f + 250.0f * (float)(formationIdx - 4);
            // Alternating ±100u for slots 5+ so the trailing
            // formation doesn't collapse to a column.
            const float lateral =
                ((formationIdx - 5) % 2 == 0) ? -100.0f : +100.0f;
            return { distance, lateral };
        }
    }
}

// Left-side-only slot table for title cutscene's south-east river
// region (Shots 7-8 per Plans/title_screen_peer_actors.md). Link
// rides along a fence with the river on his right at world +X.
// Default slot table places half the peers on Link's right which
// puts them in the water during these shots. Single-file column
// at lateral=+150u keeps every peer safely on Link's left.
//
// Sign note: lateral is in the "right perpendicular to forward"
// direction (see ComputeBaseFormationSlot). For Link's facing
// during Shot 8 (verified by field test 2026-06-10), positive
// lateral resolves to his LEFT side in world coords. Initial
// implementation used negative which produced the inverse — peers
// ended up on Link's right, still in the river.
inline SlotEntry GetBaseSlotEntryLeftSide(uint8_t formationIdx) {
    constexpr float kLeftLateral  = +150.0f;
    constexpr float kBaseDistance =  220.0f;
    constexpr float kSpacing      =  130.0f;
    return SlotEntry{
        kBaseDistance + kSpacing * (float)formationIdx,
        kLeftLateral,
    };
}

inline Vec3f ComputeBaseFormationSlot(Vec3f linkPos, int16_t linkRotY,
                                        uint8_t formationIdx,
                                        bool useLeftSideFormation = false) {
    const SlotEntry entry = useLeftSideFormation
        ? GetBaseSlotEntryLeftSide(formationIdx)
        : GetBaseSlotEntry(formationIdx);
    const float heading =
        (float)linkRotY / 32768.0f * 3.14159265358979323846f;
    const float fwdX = sinf(heading);
    const float fwdZ = cosf(heading);
    // Right-perpendicular to forward (in screen-relative left-handed
    // coords where +X right, +Z forward): rotate forward -90°.
    const float rightX =  fwdZ;
    const float rightZ = -fwdX;
    return Vec3f{
        linkPos.x - entry.distance * fwdX + entry.lateral * rightX,
        linkPos.y,
        linkPos.z - entry.distance * fwdZ + entry.lateral * rightZ,
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
                                            uint8_t formationIdx,
                                            bool useLeftSideFormation = false) {
    const TitlePeerFormation f =
        MakeTitlePeerFormation(clientId, formationIdx);
    Vec3f basePos =
        ComputeBaseFormationSlot(linkPos, linkRotY, formationIdx,
                                 useLeftSideFormation);
    basePos.x += f.posOffset.x;
    basePos.y += f.posOffset.y;
    basePos.z += f.posOffset.z;
    return TitlePeerSlot{
        basePos,
        (int16_t)(linkRotY + f.yawOffset),
    };
}

}  // namespace AnchorTitlePeer
