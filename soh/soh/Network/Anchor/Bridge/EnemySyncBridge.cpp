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
