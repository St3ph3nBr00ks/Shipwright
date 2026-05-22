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

    // Per-actor-type state machine sync fields.
    // Currently used by ACTOR_EN_KAREBABA (Withered Deku Baba) to keep its
    // Idle/Awaken/Upright/Spin/Retract state in sync with the host.
    // -1 means no state received yet (initial value).
    s16 netStateIndex = -1;
    s16 netActorParams = 0;

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

    // ── Nav system per-navigator state (Plans/nav_system_implementation_plan.md §6 + §9)
    //
    // Despite the struct's historical name (EnemyNetId), this extension is
    // the canonical "per-actor nav state" — attached to any navigator that
    // participates, not only enemies. AI Player Follower (ACTOR_EN_OE2) and any
    // future Link-rigged navigator share it.
    //
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
