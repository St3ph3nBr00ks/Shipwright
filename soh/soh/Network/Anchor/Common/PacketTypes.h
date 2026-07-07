#pragma once

// ===========================================================================
// Anchor packet-type wire identifiers (#243.7.1 extraction, 2026-06-06).
//
// The `"type"` field on every wire packet is one of these strings. This
// header is the single source of truth for the catalogue; Anchor.h
// re-exports them as class-scope `Anchor::FOO` aliases for backward
// compat with existing callers.
//
// Adding a new packet type — three steps:
//   1. Add the constant below in the appropriate semantic group.
//   2. Add a matching `inline static const std::string& X = PacketTypes::X;`
//      alias inside the `// Packet types //` block of Anchor.h.
//   3. Register the handler via Anchor's packet-handler registry
//      (Anchor.cpp B.3 refactor).
//
// New code may reference either form (`Anchor::FOO` or `PacketTypes::FOO`)
// — both resolve to the same underlying std::string.
// ===========================================================================

#include <string>

namespace PacketTypes {

// ---------------------------------------------------------------------------
// Session lifecycle + bookkeeping
// ---------------------------------------------------------------------------
inline const std::string ALL_CLIENT_STATE     = "ALL_CLIENT_STATE";
inline const std::string DISABLE_ANCHOR       = "DISABLE_ANCHOR";
inline const std::string HANDSHAKE            = "HANDSHAKE";
inline const std::string SERVER_MESSAGE       = "SERVER_MESSAGE";
inline const std::string UPDATE_CLIENT_STATE  = "UPDATE_CLIENT_STATE";
inline const std::string UPDATE_ROOM_STATE    = "UPDATE_ROOM_STATE";

// TIME_SYNC (issue #63) — lightweight time-of-day broadcast carrying just
// dayTime + nightFlag + a reason tag for diagnostics. Sent on:
//   - Periodic timer (5s default, CVar-configurable 1-60s, gated by
//     CVAR_REMOTE_ANCHOR("TimeSync.Enabled"))
//   - Cutscene-start edge (csCtx.state IDLE -> non-IDLE)
//   - Cutscene-end edge (csCtx.state non-IDLE -> IDLE)
//   - Scene-transition edge (transitionTrigger OFF -> TRANS_TRIGGER_START)
// Edge sends always fire when connected + save loaded + gameMode==NORMAL;
// only the periodic timer respects the .Enabled CVar. Forward-only
// bidirectional reconcile on receive (no host authority). See
// Claude/Plans/time_of_day_periodic_sync_plan_2026-06-16.md.
inline const std::string TIME_SYNC            = "TIME_SYNC";

// HEARTBEAT — every client → all clients. Sent every ~2s from the
// network thread (NOT the game thread) so it survives game-thread
// freezes (textbox stuck, cutscene gate, pause). Carries:
//   - sendTimeMs: monotonic-clock timestamp at network-thread send,
//     used by receivers to detect connection liveness.
//   - gameFrameCounter: monotonic counter incremented from
//     OnGameFrameUpdate, used by receivers to detect game-thread
//     liveness. See #194.
inline const std::string HEARTBEAT            = "HEARTBEAT";

// ---------------------------------------------------------------------------
// Player + team state
// ---------------------------------------------------------------------------
inline const std::string PLAYER_UPDATE        = "PLAYER_UPDATE";
inline const std::string PLAYER_SFX           = "PLAYER_SFX";
inline const std::string OCARINA_SFX          = "OCARINA_SFX";
inline const std::string REQUEST_TELEPORT     = "REQUEST_TELEPORT";
inline const std::string TELEPORT_TO          = "TELEPORT_TO";
inline const std::string REQUEST_TEAM_STATE   = "REQUEST_TEAM_STATE";
inline const std::string UPDATE_TEAM_STATE    = "UPDATE_TEAM_STATE";

// ---------------------------------------------------------------------------
// Combat
// ---------------------------------------------------------------------------
inline const std::string DAMAGE_ENEMY         = "DAMAGE_ENEMY";
inline const std::string DAMAGE_PLAYER        = "DAMAGE_PLAYER";
inline const std::string SHIELD_BOUNCE_PLAYER = "SHIELD_BOUNCE_PLAYER";
inline const std::string ENEMY_HIT_PLAYER     = "ENEMY_HIT_PLAYER";
inline const std::string PROJECTILE_HIT_ENEMY = "PROJECTILE_HIT_ENEMY";

// Pillar C2 Phase 4 — unified enemy lifecycle packet. Consolidates the
// four legacy enemy packet types (ENEMY_UPDATE, ENEMY_DEFEATED,
// ENEMY_SPAWN, ENEMY_RESPAWN) under one wire identifier with phase +
// phaseChanged tags. All emit/receive logic lives in
// EnemyStateSync/Packets/EnemyState.cpp.
inline const std::string ENEMY_STATE          = "ENEMY_STATE";

// ---------------------------------------------------------------------------
// Horse / mount sync (Plans/horse_sync_plan.md)
// ---------------------------------------------------------------------------
// Owner-authoritative bidirectional sync for mounted ACTOR_EN_HORSE
// instances (Epona variant; params 0-9 except 7 cutscene + 8 HBA).
// Wire payloads documented in Packets/World/HorseSpawn.cpp /
// HorseState.cpp / HorseDespawn.cpp.
inline const std::string HORSE_SPAWN          = "HORSE_SPAWN";
inline const std::string HORSE_STATE          = "HORSE_STATE";
inline const std::string HORSE_DESPAWN        = "HORSE_DESPAWN";

// ---------------------------------------------------------------------------
// World state + flags
// ---------------------------------------------------------------------------
inline const std::string ENTRANCE_DISCOVERED  = "ENTRANCE_DISCOVERED";
inline const std::string GAME_COMPLETE        = "GAME_COMPLETE";
inline const std::string SET_FLAG             = "SET_FLAG";
inline const std::string UNSET_FLAG           = "UNSET_FLAG";
inline const std::string SET_CHECK_STATUS     = "SET_CHECK_STATUS";
inline const std::string UPDATE_BEANS_COUNT   = "UPDATE_BEANS_COUNT";
inline const std::string UPDATE_DUNGEON_ITEMS = "UPDATE_DUNGEON_ITEMS";

// Pillar C v1 — globally-replicated world state. Plan in
// Plans/Archived/pillar_c_worldstatesync.md.
inline const std::string WORLD_FLAG_SET       = "WORLD_FLAG_SET";
inline const std::string WORLD_FLAG_UNSET     = "WORLD_FLAG_UNSET";
inline const std::string WORLD_STATE_REQUEST  = "WORLD_STATE_REQUEST";
inline const std::string WORLD_STATE_SNAPSHOT = "WORLD_STATE_SNAPSHOT";

// Test 1.5 exit-gated fix — host broadcasts when a scene becomes
// unoccupied (last player + host all transitioned out). Receivers
// clear scene-scoped buffered state for that scene so the next entrant
// sees fresh actor pool (vanilla respawn parity).
inline const std::string SCENE_DEATHS_CLEARED = "SCENE_DEATHS_CLEARED";

// KB-18 (#177) Option 4 — host-authoritative netId snapshot. Host
// broadcasts the per-static-actor netId table on scene-spawn so non-
// hosts override their locally-computed (potentially divergent) netIds
// with the host's value. Solves the entire class of post-init home.pos
// non-determinism (En_Sw raycast + future actors).
inline const std::string SCENE_ACTOR_NETIDS   = "SCENE_ACTOR_NETIDS";

// Leader→follower scene-transition handoff. Sent by the leader on edge-
// detect of `gPlayState->transitionTrigger` OFF→START. Payload carries
// the source sceneNum, destination nextEntranceIndex, and the world-
// space position (+rotation) of the trigger in the source scene so the
// follower can navigate there and fire its own transition. Supersedes
// the G13 boss-room deactivate path and unblocks cross-scene doors /
// grotto / crawlspace entry. See #169.
inline const std::string SCENE_TRANSITION_HANDOFF = "SCENE_TRANSITION_HANDOFF";

// ---------------------------------------------------------------------------
// Dialog + cutscene
// ---------------------------------------------------------------------------
inline const std::string TALK_REQUEST              = "TALK_REQUEST";
inline const std::string DIALOG_END                = "DIALOG_END";
inline const std::string CUTSCENE_TEXT_ADVANCE     = "CUTSCENE_TEXT_ADVANCE";
inline const std::string CUTSCENE_TEXT_ADVANCED    = "CUTSCENE_TEXT_ADVANCED";
inline const std::string BOSS_GOMA_LOOKED_AT       = "BOSS_GOMA_LOOKED_AT";
inline const std::string MIDO_POST_DEKU_LEAVE      = "MIDO_POST_DEKU_LEAVE";
inline const std::string TALON_CASTLE_STATE        = "TALON_CASTLE_STATE";
inline const std::string HYRULE_CASTLE_GATE_OPEN   = "HYRULE_CASTLE_GATE_OPEN";
inline const std::string CUTSCENE_START            = "CUTSCENE_START";
inline const std::string CUTSCENE_END              = "CUTSCENE_END";
inline const std::string CUTSCENE_TEXT_VOTE_STATE  = "CUTSCENE_TEXT_VOTE_STATE";
// Cutscene late-join (Option B — jump-and-catch-up + skip-and-apply
// fallback). Plan: Claude/Plans/cutscene_late_join_plan.md.
//
// CUTSCENE_FRAME_SYNC — 1Hz periodic broadcast from the leader while
// their local csCtx.state != CS_STATE_IDLE. Carries leader's current
// csCtx.frames + state + monotonic seq (Pitfall 43 pattern). Peers use
// it for ongoing drift correction after initial catch-up.
inline const std::string CUTSCENE_FRAME_SYNC       = "CUTSCENE_FRAME_SYNC";
// CUTSCENE_CATCHUP_REQUEST — targeted (late-joiner → leader) when a
// scene-entry detects a peer mid-cutscene. Asks for the full state
// snapshot (leader frame, delta ledger since frame 0, actor snapshots,
// camera snapshot, music seek offset).
inline const std::string CUTSCENE_CATCHUP_REQUEST  = "CUTSCENE_CATCHUP_REQUEST";
// CUTSCENE_CATCHUP_RESPONSE — targeted (leader → late-joiner) reply
// carrying the delta ledger + snapshots. Applied atomically; then
// csCtx.frames jumps to leader.frame; vanilla playback resumes.
inline const std::string CUTSCENE_CATCHUP_RESPONSE = "CUTSCENE_CATCHUP_RESPONSE";

// BOSS_EXIT_TEAM_WARP — team-routed scene transition for synced boss
// exits. When a team member enters the dungeon-clear blue warp, all
// teammates in the same boss room transition to the same destination
// entrance / next-cutscene, in lockstep. Routed via `targetTeamId`.
inline const std::string BOSS_EXIT_TEAM_WARP = "BOSS_EXIT_TEAM_WARP";

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------
inline const std::string GIVE_ITEM = "GIVE_ITEM";

// ITEM_DROP_SYNC — host → all clients (team-broadcast). Sent on each
// host-side `OnActorSpawn(ACTOR_EN_ITEM00)` for transient drop types
// (rupees, hearts, ammo, magic, sticks, nuts, seeds). Carries the drop's
// authoritative netId, ITEM00_* params, world position, killerClientId
// + spawnTimeMs. Receivers spawn a matching local EnItem00 with an
// `ItemDropNetId` extension stamped from the payload. See #193.
inline const std::string ITEM_DROP_SYNC = "ITEM_DROP_SYNC";

// ITEM_COLLECTED — collector client → all (team-broadcast). Sent when
// a local pickup gate passes and the player's gSaveContext has been
// credited. Receivers walk their actor list for the matching itemNetId
// and Actor_Kill the local copy. See #193.
inline const std::string ITEM_COLLECTED = "ITEM_COLLECTED";

// ITEM_DROP_SNAPSHOT — host → joining peer (targeted). Late-join replay
// of in-flight EN_ITEM00 drops in the joining peer's scene. See #193 Phase 5.
inline const std::string ITEM_DROP_SNAPSHOT = "ITEM_DROP_SNAPSHOT";

// ITEM_PICKUP_REQUEST — peer → room host (targeted). Race A mitigation.
// See #193.
inline const std::string ITEM_PICKUP_REQUEST = "ITEM_PICKUP_REQUEST";

// ENV_ACTOR_DROP — peer → host (targeted). Phase 4 v2. See #193.
inline const std::string ENV_ACTOR_DROP = "ENV_ACTOR_DROP";

// ENV_ACTOR_DESTROY — any client → all (team-broadcast). Sent when a
// synced env actor (grass, pot, crate, beehive, etc.) is destroyed
// locally — including cut events that DON'T call Actor_Kill (e.g.
// ENKUSA_TYPE_1 grass transitions to a cut-stub state without dying).
// Plan: Plans/Archived/env_actor_destroy_sync.md.
inline const std::string ENV_ACTOR_DESTROY = "ENV_ACTOR_DESTROY";

// MODAL_OFFER_CLAIMED — host → all (team-broadcast). #193 Plan B
// follow-up to step 6. Sent when host's modal-offer pickup actually
// resolves. Peers find the matching SyncedClaimableDrop by `offererNetId`.
inline const std::string MODAL_OFFER_CLAIMED = "MODAL_OFFER_CLAIMED";

// ---------------------------------------------------------------------------
// NPC Follower (Plans/Archived/npc_follower_plan.md §2.9)
// Single-owner authority: the client that toggled the CVar owns its
// local NPC; SPAWN announces, STATE periodically broadcasts pos/rot at
// ~10Hz, DESPAWN tears down. Peers spawn/apply/kill read-only replicas
// keyed by ownerClientId.
// ---------------------------------------------------------------------------
inline const std::string FOLLOWER_NPC_SPAWN   = "FOLLOWER_NPC_SPAWN";
inline const std::string FOLLOWER_NPC_STATE   = "FOLLOWER_NPC_STATE";
inline const std::string FOLLOWER_NPC_DESPAWN = "FOLLOWER_NPC_DESPAWN";

// ---------------------------------------------------------------------------
// AI Director
// ---------------------------------------------------------------------------
inline const std::string NAV_TEST_DIRECTIVE = "NAV_TEST_DIRECTIVE";

// AI Director migration snapshot. Broadcast from the global-effective-
// host on every ledger mutation (RecordSpawn, OnEnemyRemoved). Carries
// mLastSpawnFrameByKey + mLiveCountByDescriptor + mNetIdToDescriptor +
// per-descriptor SerializeMigrationState(). Peers cache into their own
// Director instance so a future host-migration target has fresh state.
// See Plans/ai_director_plan.md §2.8.
inline const std::string DIRECTOR_STATE_SYNC = "DIRECTOR_STATE_SYNC";

}  // namespace PacketTypes
