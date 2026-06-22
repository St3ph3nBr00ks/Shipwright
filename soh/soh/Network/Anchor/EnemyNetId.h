#pragma once

// Attached to actors that participate in cross-client sync — gives them a
// stable network id, caches host-authoritative state for re-application on
// non-host clients, and carries per-actor sync + nav substate.
//
// Extracted from Anchor.h 2026-05-21 per Plans/decoupling_gap_audit_2026-05-16.md
// §3.3 / A.2. Anchor.h re-includes this header so existing consumers continue
// to compile unchanged.

#include <cstdint>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

#include "EnemyStateSync/EnemyLifecycle.h"
#include "EnemySync/PerActor/EnKarebabaState.h"  // refactor A.7

struct EnemyNetId {
    uint32_t netId = 0;
    SkelAnime* skelAnime = nullptr; // nullptr if this enemy type has no supported skeleton
    uint8_t limbCount = 0;          // cached from skelAnime->limbCount at spawn time

    // Pillar C2 Phase 1 — explicit lifecycle phase.
    //
    // Source of truth for the boolean flag soup that grew alongside this
    // struct: `hasLocalDeath`, `defeatPacketSent`, `pendingNaturalDeath`,
    // and `deferredDeadItemDrop` will be removed at the end of Phase 1 in
    // favour of reads off `phase`. During the migration both representations
    // are kept in lock-step by EnemyStateSync::TransitionTo().
    //
    // Default Alive — initialised at OnActorSpawn for new enemies.
    EnemyStateSync::LifecyclePhase phase = EnemyStateSync::LifecyclePhase::Alive;

    // Peer-side drop-suppression signal. Set true the instant
    // `ENEMY_STATE` from host carries `health <= 0`, regardless of
    // whether `ENEMY_DEFEATED` has arrived yet. Read by the per-actor
    // `Anchor_ShouldSuppress*Drop` predicates (Dekubaba / En_St / En_Sw
    // / En_Dekunuts / En_Goma) ORed alongside `PhaseImpliesPendingNaturalDeath`.
    //
    // Closes the race documented by 2026-05-06 field test (Inside Deku
    // Tree, b0ea6f1): peer's vanilla `EnDekubaba_Hit` reads `health == 0`
    // from the same `ENEMY_STATE` and transitions to `ShrinkDie` on the
    // same frame; peer's local `OnEnemyDefeat` fires from there with
    // `phase = DyingByLocal`. The legacy phase-only suppressor returns
    // false on `DyingByLocal` and peer spawns a duplicate local drop.
    // Driving this bool from the same packet that drives the kill itself
    // closes the race entirely — both signals land on the same frame.
    //
    // Cleared by `EnemyStateSync::TransitionTo(Alive | Regrowing)` so
    // Karebaba (and any future regrow-class enemy) doesn't carry the
    // flag across respawn.
    //
    // Host-side: never set, gated on `!IsMyCurrentRoomHost()` at the
    // write site. Host's own kill drives the broadcast normally via
    // `OnActorSpawn(EN_ITEM00)`; a stale flag here would block host's
    // legitimate drop call.
    bool networkDriveDying = false;

    // Last state received from the host via ENEMY_UPDATE (non-host clients only).
    // Re-applied each frame in OnActorUpdate so the enemy update() can run (enabling
    // collision registration) without drifting from the authoritative host position.
    bool hasNetState = false;

    // #265 — set true at the first OnActorUpdate fire for this actor.
    // Actors that conditionally Actor_Kill themselves inside _Init
    // (En_Po_Field's 10→2 candidate pruning at z_en_po_field.c:180-184,
    // and potentially other admitted-but-conditionally-rejected actors
    // in IsSyncedWorldActor) are spawned + reaped within a single frame;
    // OnActorUpdate never fires. Used by OnActorKill to suppress
    // ENEMY_DEFEATED broadcasts for these "never-existed" actors —
    // peers' deterministic Init makes the same decision so they have
    // nothing to clean up. Field log showed ~45 spurious broadcasts per
    // Hyrule Field scene entry (9 × En_Po_Field + 12 × Obj_Mure2
    // candidates × 4 transitions) without this gate.
    bool hasEverUpdated = false;
    Vec3f netPos = { 0.0f, 0.0f, 0.0f };
    Vec3s netRot = { 0, 0, 0 };
    Vec3s netShapeRot = { 0, 0, 0 };
    s8 netHealth = 1;
    Vec3f netScale = { 1.0f, 1.0f, 1.0f }; // actor->scale; synced for enemies that change scale during animation

    // defeatPacketSent was extracted to EnemyStateSync::HostBookkeeping at
    // end of C2 Phase 2 (consolidated with sentDefeatThisScene into
    // mDefeatBroadcasts — both signals had the same lifetime and purpose).
    // Read sites use HostBookkeeping::Instance().HasDefeatBroadcast(netId);
    // writes use ClaimDefeatBroadcast / ReleaseDefeatBroadcast.

    // hasLocalDeath was deleted at end of C2 Phase 1 (commit landing this
    // change). Read sites now use EnemyStateSync::PhaseImpliesHasLocalDeath(
    // ext->phase), which returns true for DyingByLocal / DyingByNetwork /
    // AwaitingDeadItemDrop / Dead. Write sites moved to TransitionTo()
    // calls during Phase 1 step 2; this commit drops the boolean storage.

    // Shared per-actor-type action-state cache. Receive-side cache for
    // the last `actionState` payload field; consumers map to the actor's
    // native action-function index via `<Actor>_ApplyNetState`. Driven
    // by ACTOR_EN_KAREBABA, ACTOR_EN_DEKUBABA, ACTOR_EN_GOMA,
    // ACTOR_EN_DEKUNUTS, ACTOR_EN_HINTNUTS, ACTOR_EN_ST, ACTOR_EN_SW,
    // ACTOR_BOSS_GOMA, ACTOR_EN_GOROIWA. -1 means no state received yet.
    s16 netStateIndex = -1;

    // Refactor A.7 — Karebaba-specific per-actor state. Other actors
    // leave at default. First per-actor extract under EnemySync/PerActor/;
    // see EnemySync/PerActor/EnKarebabaState.h for the pattern future
    // regrow-class enemies should follow.
    EnemySync::EnKarebabaState karebaba;

    // pendingNaturalDeath was deleted at end of C2 Phase 1 step 5b.
    // Read sites use EnemyStateSync::PhaseImpliesPendingNaturalDeath(
    // ext->phase), which returns true for DyingByNetwork /
    // AwaitingDeadItemDrop. Karebaba item-drop suppression and natural-
    // cycle gating now derive from phase.

    // deferredDeadItemDrop was deleted at end of C2 Phase 1 step 5c.
    // The OnActorInit Karebaba SetupDeadItemDrop gate now reads
    // EnemyStateSync::PhaseImpliesDeferredDeadItemDrop(ext->phase),
    // which returns true iff phase == AwaitingDeadItemDrop. The
    // OnActorInit handler also transitions AwaitingDeadItemDrop ->
    // DyingByNetwork after firing SetupDeadItemDrop, so the second
    // OnActorInit pass on the same actor is a no-op (the predicate
    // returns false).

    // Goroiwa-net state — issue #153.
    // First non-ACTORCAT_ENEMY actor sync (En_Goroiwa is ACTORCAT_PROP). Cached on
    // non-host from ENEMY_UPDATE; re-applied each frame in OnActorUpdate so the local
    // action function can run (collision registration) without drifting from the
    // host-authoritative path-position. -1 means no state received yet.
    s16 goroiwaCurrentWaypoint = -1;
    s16 goroiwaNextWaypoint    = -1;
    s16 goroiwaPathDirection   = 0; // ±1; 0 means uninitialized
    u8  goroiwaFlags           = 0; // ENGOROIWA_* bitmask (PLAYER_IN_THE_WAY etc.)

    // Per-torch lit-state sync (Obj_Syokudai). Host-authoritative.
    // -1 = permanently lit, 0 = unlit, >0 = remaining-burn-frames.
    // Cached at receive time and re-applied every frame in OnActorUpdate
    // so peer's local ObjSyokudai_Update can run normally (the local
    // body's writes to litTimer are overwritten post-update).
    // INT16_MIN sentinel = no host state received yet (fall through to
    // local AI, vanilla single-player parity).
    s16 syokudaiLitTimer = INT16_MIN;

    // #190 — pending DAMAGE_ENEMY queue. When peer's hit packet arrives
    // while the host is in a world-freeze (Item Get cutscene, text box,
    // ocarina playback, scene transition, vanilla pause), the host's
    // actor->update doesn't run, so the synthetic AC_HIT bit and
    // colChkInfo.damage value previously written by HandlePacket_DamageEnemy
    // got cleared by collision-system reset passes that aren't gated on
    // world time — silently dropping the damage. Now the receive path
    // queues the damage onto these fields, and the ShouldActorUpdate
    // hook drains them onto colChkInfo + AC_HIT on the first frame
    // the actor's update is about to run (i.e., when world resumes).
    // pendingSyncDamage accumulates (multi-hit during a single freeze
    // adds up); damageEffect / atHitEffect are last-write-wins.
    u8 pendingSyncDamage        = 0;
    u8 pendingSyncDamageEffect  = 0;
    u8 pendingSyncAtHitEffect   = 0;

    // Race B mitigation (#203) — peer-killing-blow stash. When peer's
    // ShouldActorUpdate hook detects a would-kill damage value, it
    // clamps colChkInfo.damage to leave 1 HP locally AND records the
    // original damage value here so the OnActorUpdate forwarder can
    // broadcast the un-clamped value to host. Without this stash, the
    // forwarder would broadcast the clamped value (e.g. 0 if peer's
    // hit was a one-shot at 1 HP), causing host's enemy to be
    // unkillable from peer hits. Cleared by the forwarder after each
    // broadcast.
    u8 peerKillingBlowOriginalDamage = 0;

    // Candidate B2-D (#288, 2026-06-17) — peer-side timeout fallback
    // timestamp. Stamped via std::chrono::steady_clock ms when the
    // Race-B clamp first arms for this actor (transition from
    // unarmed→armed). Cleared back to 0 when host's authoritative
    // ENEMY_DEFEATED for the netId arrives (happy path) OR when the
    // peer-side timeout fallback fires and broadcasts a peer-attributed
    // ENEMY_DEFEATED (host-incapacitated path).
    //
    // Why this exists: the original Race-B design assumed host always
    // catches up to peer's clamped HP within ~RTT. Field test 576
    // surfaced the failure mode — host on Game Over / cutscene / TCP
    // stall keeps peer's HP pinned at 1 forever (multi-hit guard at
    // EnemyState.cpp blocks upward HP revival once clamped). The
    // 1-second timeout (user-specified, NOT the 3 s suggested in the
    // analysis doc) is tight enough that the visible UX delay is
    // ~1 frame past noticeable, and Commit A's hub-refactor dedup
    // (ClaimDefeatBroadcast on receive) suppresses duplicate
    // broadcasts cleanly if a host-side defeat arrives slightly
    // after the peer-side timeout fires.
    uint64_t peerKillingBlowClampedAtMs = 0;

    // Boss_Goma — sticky "peer is signaling encounter advance" flag (#67).
    // Set true on receipt of any BOSS_GOMA_LOOKED_AT. Stays true until
    // case 3 of BossGoma_Encounter consumes-and-clears it via
    // Anchor_BossGomaConsumePeerSignaled — at which point case 3
    // immediately calls BossGoma_SetupEncounterState4 (eye-roll
    // cinematic) instead of waiting for the local frustum-check
    // 15-frame threshold (vanilla case 3's else-branch resets
    // lookedAtFrames every frame the local check fails, so the
    // earlier per-frame `lookedAtFrames++` approach was getting
    // clobbered). Persists across host's case 1-2 cutscene window
    // (~228 frames) so peer's signal — which arrives ~11s before
    // host enters case 3 — isn't lost.
    bool bossGomaPeerSignaled = false;

    // RoomNavData Phase 2 commit 11 — slope-3 stuck-on-slope diagnostic.
    // Detection-only; intervention deferred to v2 (gEnhancements.RoomNavData
    // .ActiveSlopeRecovery) pending field-test evidence of need.
    //
    // stuckOnSlopeFrames counts up while the predicate is true (actor on
    // slope-3 surface AND velocity.y not consistently descending). Resets
    // to 0 when predicate becomes false. On rising edge above
    // kStuckFrameThreshold, RoomNavData increments stuckOnSlopeEventCount
    // and emits a rate-limited SPDLOG_WARN.
    //
    // Both reset on actor death / scene transition (via the same
    // EnemyNetId reset paths that clear netState fields).
    uint16_t stuckOnSlopeFrames     = 0;
    uint16_t stuckOnSlopeEventCount = 0;

    // Nav-substrate fields were extracted to AnchorNavExt 2026-06-04
    // (refactor B.4). See Common/AnchorNavExt.h. Both extensions key on
    // the same actor pointer; consumers fetch
    // `ObjectExtension::GetInstance().Get<AnchorNavExt>(actor)` for
    // nav state and `Get<EnemyNetId>(actor)` for sync state.
};
