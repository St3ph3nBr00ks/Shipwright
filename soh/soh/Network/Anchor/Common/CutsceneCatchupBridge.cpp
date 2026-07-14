#include "soh/Network/Anchor/Common/CutsceneCatchupBridge.h"
#include "soh/Network/Anchor/Anchor.h"

// Implementation. Kept tiny (KISS) — one flag check.
//
// Thread safety: Anchor::Instance->pendingCatchups is written from
// the game thread (HandlePacket_CutsceneCatchupResponse) and read from
// the game thread (vanilla dispatcher gates). Same-thread access; no
// synchronisation needed.
//
// Defensive nullptr check on Anchor::Instance covers the pre-Init
// window during process startup — vanilla decomp can call this from
// Init hooks that fire before Anchor's ShipInit has run.

extern "C" int Anchor_ShouldSuppressLocalCutsceneEntry(void) {
    if (::Anchor::Instance == nullptr) return 0;
    return ::Anchor::Instance->pendingCatchups.empty() ? 0 : 1;
}
