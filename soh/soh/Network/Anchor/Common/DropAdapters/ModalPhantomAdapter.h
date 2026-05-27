#pragma once

// ModalPhantomAdapter — third concrete DropAdapter (Plan B step 5).
//
// Covers vanilla mechanism #5: the modal-completion phantom spawned by
// `func_8083E4C4` (z_player.c). After every `Actor_OfferGetItemNearby`
// modal completes, this function spawns a transient `EN_ITEM00` with
// the `0x8000` flag at the picker's player position. It's a render-
// only artifact of the "got X" animation — not a real drop. Vanilla
// schedules it to despawn in 15 game-ticks via the `unk_15A` countdown
// in EnItem00_Update.
//
// Why we need an adapter, when in vanilla single-player the lifecycle
// just works: in MP the phantom's actionFunc (`func_8001E5C8`,
// z_en_item00.c:745) is offer-driven — it increments `unk_15A` every
// frame the offer is "active" (i.e. `getItemId != GI_NONE`), exactly
// cancelling the per-frame decrement. The phantom only despawns once
// the player ACCEPTS the phantom's second-stage offer, clearing
// `getItemId`. In MP, something (extra GIVE_ITEM cross-credit
// firing Item_Give again, player-state divergence, etc.) blocks the
// second-stage offer from connecting on the picker's client. The
// phantom sits on-screen indefinitely. Reported as Bug B in
// task_checklist_done.md, field-tested 2026-05-26.
//
// Adapter responsibilities:
//   - Detect modal phantoms at OnActorSpawn(EN_ITEM00) via
//     `ogParams & 0x8000` (the flag is captured pre-Init-strip on
//     `EnItem00::ogParams`).
//   - Record each phantom in a LOCAL list (not a SyncedClaimableDrop —
//     phantoms are per-client render artifacts; no broadcast, no
//     cross-client coordination).
//   - Tick once per game frame: if any phantom has been alive longer
//     than `kPhantomBackstopMs`, force `Actor_Kill` to break the
//     unk_15A stall.
//   - Cleanup on OnActorDestroy.
//
// Also tells the OnActorSpawn(EN_ITEM00) host-broadcast path to skip
// modal phantoms entirely (they never broadcast). This replaces the
// "modal-visual phantom filter" that was added in fix 1 (051cd9801)
// and removed in cleanup 2/4 — now it lives in this adapter's
// detection path instead of as a freestanding gate.
//
// Plan: Claude/Plans/synced_claimable_drop_design.md §5.5, §10.3.

#include "DropAdapter.h"
#include <cstdint>
#include <vector>

extern "C" {
struct EnItem00;
}

namespace SyncedClaimableDrop {

class ModalPhantomAdapter : public DropAdapter {
public:
    static ModalPhantomAdapter* GetInstance();

    const char* Name() const override { return "ModalPhantom"; }
    // DismissVisualRep: unused — phantoms are not registered in the
    // SyncedClaimableDrop registry, so the registry never dispatches
    // dismissal through this adapter. Inheritance from DropAdapter is
    // kept for symmetry + DropAdapterRegistry enumeration only.

    // Called from OnActorSpawn(EN_ITEM00) when the spawned actor has
    // `ogParams & 0x8000` set. Adds the phantom to the backstop list
    // so the tick can force-kill if vanilla's unk_15A countdown
    // stalls.
    void OnPhantomSpawn(EnItem00* phantom);

    // Called from OnActorDestroy. Scrubs the phantom from the
    // backstop list (the actor's memory is about to be reused).
    // Idempotent — safe to call for non-phantom actors too.
    void OnActorDestroyed(Actor* actor);

    // Per-frame tick. Walks the backstop list and Actor_Kills any
    // phantom whose age (wall-clock ms since spawn) exceeds
    // `kPhantomBackstopMs`. Called from OnGameFrameUpdate.
    void Tick();

    // Backstop threshold. Vanilla phantom lifetime is ~15 game ticks
    // (~250ms @ 60fps, ~750ms @ 20fps). 1500ms gives vanilla plenty
    // of grace; the backstop fires only when vanilla is clearly
    // stalled.
    static constexpr int64_t kPhantomBackstopMs = 1500;

private:
    struct PhantomEntry {
        Actor*  actor;
        int64_t spawnTimeMs;
        int64_t lastDiagLogMs;  // Throttles per-phantom diagnostic to ~1 Hz.
    };
    std::vector<PhantomEntry> phantoms_;
};

}  // namespace SyncedClaimableDrop
