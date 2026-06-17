#include "ZTargetSync.h"

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemyNetId.h"

extern "C" {
#include "functions.h"   // Actor_IsTargeted forward-decl
}

// Returns true if either:
//   (a) the local Player has Z-locked onto `actor` (vanilla
//       Actor_IsTargeted condition, unchanged); or
//   (b) any online same-scene peer's PLAYER_UPDATE-broadcast
//       focusActorNetId resolves to `actor`'s EnemyNetId.
//
// Defensive nulls return false (matches single-player semantics when
// the helper is reachable on edge cases).
//
// Cost: O(numPeers) per call. With small N this is negligible. If a
// hot AI loop ever calls this many times per frame for many actors,
// caching could be added — but per the audit (Analysis §3.x) the call
// sites are state-machine transitions firing once per actor per
// frame, so YAGNI.
extern "C" bool Anchor_IsActorTargetedByAnyPlayer(Actor* actor, PlayState* play) {
    if (actor == nullptr) return false;

    // (a) Local Z-target — preserves vanilla behaviour for the host.
    if (play != nullptr && Actor_IsTargeted(play, actor)) {
        return true;
    }

    // (b) Peer Z-target — walk online same-scene peers and check
    //     whether any has us as their focus actor. Resolve via
    //     EnemyNetId; the actor must be in the synced-actor pipeline
    //     (have an EnemyNetId extension) for the comparison to
    //     match a non-zero focusActorNetId.
    if (Anchor::Instance == nullptr) return false;
    const EnemyNetId* selfExt =
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    if (selfExt == nullptr || selfExt->netId == 0) return false;
    const uint32_t selfNetId = selfExt->netId;

    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (!client.online || !client.isSaveLoaded || client.self) continue;
        if (client.focusActorNetId == 0) continue;
        if (client.focusActorNetId == selfNetId) return true;
    }
    return false;
}
