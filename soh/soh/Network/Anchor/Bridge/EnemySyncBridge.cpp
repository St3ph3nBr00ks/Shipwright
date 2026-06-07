// Refactor A.8 — EnemySync extern "C" shim bridge.
//
// Four C-linkage senders for actor-side events that need to route
// through host arbitration: projectile hits, dialog start, dialog
// end, enemy-on-peer-Link hits. Moved from HookHandlers.cpp on
// 2026-06-01 per Plans/A.8_design_review.md (DR-5) Stage 3.8.
//
// Domain: EnemySync notify-host senders. No file-statics. All shims
// read EnemyNetId via ObjectExtension and send via
// Anchor::Instance->SendPacket_*.

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemyNetId.h"  // #243.7.2 — explicit (was transitive via Anchor.h)
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/EnemyStateSync/EnemyLifecycle.h"  // #90 — PhaseImpliesPendingNaturalDeath
#include "soh/ObjectExtension/ObjectExtension.h"

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

// Sender wrapper — routes a local nutball-on-hintnut collision to the
// room host. Host applies HitByScrubProjectile1+2 on its own copy of
// the actor and broadcasts the resulting state via ENEMY_STATE.
extern "C" void Anchor_NotifyProjectileHitEnemy(Actor* targetActor, s16 projectileActorId) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;  // unsynced actor — silently drop
    Anchor::Instance->SendPacket_ProjectileHitEnemy(ext->netId, projectileActorId);
}

// Sender wrapper — peer's Run actionFunc calls this when its local
// Link initiates dialog with the hintnut. Host runs the canonical
// SetupTalk on its local actor and broadcasts state=Talk so peer's
// rx-driver applies it (instead of host's stale Run state reverting
// peer back). See Packets/TalkRequest.cpp.
extern "C" void Anchor_NotifyTalkRequest(Actor* targetActor) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_TalkRequest(ext->netId);
}

// Sender wrapper — peer's Talk actionFunc calls this when its local
// Message_GetState returns TEXT_STATE_EVENT (dialog closed on peer).
// Host runs the canonical SetupLeave on its local actor and
// broadcasts state=Leave back via ENEMY_STATE so peer's rx-driver
// applies it (instead of peer's local SetupLeave running and
// spawning hearts every 50ms). See Packets/DialogEnd.cpp.
extern "C" void Anchor_NotifyDialogEnd(Actor* targetActor) {
    if (targetActor == nullptr) return;
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(targetActor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_DialogEnd(ext->netId);
}

// Receiver-side predicate — true when an En_St death cycle was network-
// driven (peer received ENEMY_DEFEATED and is replaying the bounce → die
// sequence). EnSt_Die uses this to suppress the random-drop call so
// host's authoritative ITEM_DROP_SYNC isn't double-applied.
// See z_en_st.c EnSt_Die + Plans/en_st_sync_plan_v2.md §3 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnStDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// Receiver-side predicate — true when an En_Test (Stalfos) death cycle
// was network-driven (peer received ENEMY_DEFEATED and is replaying the
// fall-over → body-break sequence). func_80862E6C and func_808633E8 use
// this to suppress Item_DropCollectibleRandom so host's authoritative
// ITEM_DROP_SYNC isn't double-applied.
// See z_en_test.c + Plans/en_test_sync_plan.md §3 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnTestDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// Receiver-side predicate — true when an En_Wf (Wolfos) death cycle was
// network-driven. Mirror of Anchor_ShouldSuppressEnStDrop. EnWf_Die uses
// this to suppress Item_DropCollectibleRandom so host's authoritative
// ITEM_DROP_SYNC isn't double-applied. See z_en_wf.c EnWf_Die +
// Plans/en_wf_sync_plan.md §3 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnWfDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// Receiver-side predicate — true when an En_Reeba (Leever) death cycle
// was network-driven. func_80AE5C38 (the shrink-and-drop death cycle) uses
// this to suppress Item_DropCollectibleRandom so host's authoritative
// ITEM_DROP_SYNC isn't double-applied. Both small and big variants
// converge on this function — same predicate covers both.
// See z_en_reeba.c + Plans/en_reeba_sync_plan.md §3 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnReebaDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// Receiver-side predicate -- true when an En_Firefly (Keese) death cycle
// was network-driven (peer received ENEMY_DEFEATED and is replaying the
// fall -> die sequence). EnFirefly_Die uses this to suppress the random-
// drop call so host's authoritative ITEM_DROP_SYNC isn't double-applied.
// The `|| ext->networkDriveDying` clause matches the Dekubaba race
// mitigation -- closes a race when peer's vanilla EnFirefly_UpdateDamage
// consumes the same ENEMY_STATE health<=0 read and transitions to
// SetupFall before ENEMY_DEFEATED arrives.
// See z_en_firefly.c EnFirefly_Die + Plans/en_firefly_sync_plan.md section 4 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnFireflyDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase)
        || ext->networkDriveDying;
}

// Receiver-side predicate — true when an En_Ik (Iron Knuckle) death cycle
// was network-driven (peer received ENEMY_DEFEATED and is replaying the
// death anim -> 24-tick countdown -> drop sequence). func_80A75A38 uses
// this to suppress Item_DropCollectibleRandom so host's authoritative
// ITEM_DROP_SYNC isn't double-applied.
// See z_en_ik.c func_80A75A38 + Plans/en_ik_sync_plan.md §3 step 3+5.
extern "C" bool Anchor_ShouldSuppressEnIkDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// Receiver-side predicate -- reserved for API consistency. En_Poh's
// death cycle (EnPoh_Death -> soul-talk states) does NOT call
// Item_DropCollectibleRandom; Poe yields ITEM_POE via a per-client
// soul-talk interaction (z_en_poh.c:826 Item_Give path) rather than
// the EN_ITEM00 drop chain. This predicate is currently unused at
// the actor-side call sites but is exposed for symmetry with sibling
// per-enemy sync plans and as a future hook if a drop call is ever
// added to the death cycle.
// See z_en_poh.c + Plans/en_poh_sync_plan.md section 4 step 3+5 / #99.
extern "C" bool Anchor_ShouldSuppressEnPohDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}

// C-callable: non-host tells host that its local Link was just hit by this enemy
// so the host can reverse/update its authoritative copy (En_Goroiwa, issue #153
// Phase 2). No-op when Anchor is disconnected, when this client is the room
// host (it would handle the hit locally), or when the actor lacks an EnemyNetId
// extension (never reached the sync pipeline). Uses Pillar A Phase 2 per-room
// authority so the gate stays correct when the original room owner is offline.
extern "C" void Anchor_NotifyEnemyHitPlayer(Actor* actor) {
    if (!Anchor::Instance || !Anchor::Instance->isConnected) return;
    if (::SceneAuthority::IsMyCurrentRoomHost()) return;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return;
    Anchor::Instance->SendPacket_EnemyHitPlayer(ext->netId);
}

// Receiver-side predicate -- true when an En_Peehat (Peahat) death cycle
// was network-driven. Three actor-side sites use this to suppress
// Item_DropCollectibleRandom so host's authoritative ITEM_DROP_SYNC isn't
// double-applied:
//   - EnPeehat_StateExplode (adult death, 3 x 0x40 drops + 1 bomb spawn;
//     bomb spawn is unsuppressed pending EXPLOSIVE_SPAWN packet family).
//   - EnPeehat_HitWhenGrounded (periodic hit-reward drops every 16 frames
//     while grounded adult takes damage; only suppressed while dying so
//     pre-death hit-reward drops still emit on both clients as today).
//   - EnPeehat_Larva_StateSeekPlayer (larva self-kill on collision,
//     0x20 drop).
// See z_en_peehat.c + Plans/en_peehat_sync_plan.md section 6 step 3+5 / #107.
extern "C" bool Anchor_ShouldSuppressEnPeehatDrop(Actor* actor) {
    if (actor == nullptr) return false;
    const EnemyNetId* ext = ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (ext == nullptr) return false;
    return EnemyStateSync::PhaseImpliesPendingNaturalDeath(ext->phase);
}
