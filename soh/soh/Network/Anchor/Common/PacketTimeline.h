#pragma once

// Pillar B Phase 1 — timeline field on scene-scoped packets.
//
// Cross-timeline coexistence (Q 4.B.4): players on different
// `gSaveContext.linkAge` values can be in the same scene without their
// scene-scoped state polluting each other. ENEMY_UPDATE for adult Kokiri
// Forest must NOT apply to a child-Kokiri-Forest peer, even though both
// scenes share sceneNum.
//
// Send-side: stamp `payload["timeline"] = gSaveContext.linkAge` on every
// scene-scoped packet (one byte; legacy peers ignore unknown fields).
//
// Receive-side: drop the packet if `payload["timeline"] != local linkAge`.
// Legacy senders (no `timeline` field) bypass the filter — pre-Pillar-B
// behaviour is preserved when one side hasn't upgraded yet.
//
// Sentinel note: LINK_AGE_ADULT == 0 in z64save.h, so we cannot use 0 as
// "missing". `payload.contains("timeline")` is the explicit presence
// check the receive-side helper uses.
//
// Affected packet types (schema bumped in PacketSchemas.h):
//   ENEMY_UPDATE, ENEMY_DEFEATED, ENEMY_SPAWN, ENEMY_RESPAWN,
//   DAMAGE_ENEMY, ENEMY_HIT_PLAYER, SCENE_TRANSITION_HANDOFF.
//
// NOT affected (timeline-agnostic, per design doc Q 4.B.3):
//   PLAYER_UPDATE (already carries linkAge), all save-context packets
//   (GIVE_ITEM, SET_FLAG, UNSET_FLAG, UPDATE_TEAM_STATE, ...).
//
// Plan: Claude/Plans/pillar_b_timeline_implementation.md.

#include <nlohmann/json.hpp>

namespace PacketTimeline {

// Stamp the local timeline (gSaveContext.linkAge) into payload["timeline"].
// Caller must have already verified IsSaveLoaded() — gSaveContext.linkAge
// is uninitialised before file load.
void SetTimelineField(nlohmann::json& payload);

// Returns true when the packet's `timeline` field disagrees with the
// local `gSaveContext.linkAge` (i.e. cross-timeline → drop). Returns
// false when the packet has no `timeline` field (legacy sender) or the
// timelines match.
bool IsCrossTimelinePacket(const nlohmann::json& payload);

}  // namespace PacketTimeline
