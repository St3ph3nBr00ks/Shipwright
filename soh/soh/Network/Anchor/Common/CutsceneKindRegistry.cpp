#include "CutsceneKindRegistry.h"
#include "soh/Network/Anchor/Anchor.h"

#include <libultraship/libultraship.h>
#include <chrono>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
extern PlayState* gPlayState;
}

// ---------------------------------------------------------------------
// Helper D — Anchor_ForceCutsceneOnActor.
// ---------------------------------------------------------------------

extern "C" int Anchor_ForceCutsceneOnActor(
    PlayState* play,
    int16_t actorId,
    uint8_t actorCategory,
    void* segment,
    void (*setupAdapter)(Actor* actor, void* actionFunc),
    void* actionFunc) {
    if (play == nullptr) return 0;
    if (actorCategory >= ACTORCAT_MAX) return 0;
    if (setupAdapter == nullptr) return 0;

    Actor* actor = play->actorCtx.actorLists[actorCategory].head;
    while (actor != nullptr) {
        if (actor->id == actorId) {
            play->csCtx.segment = segment;
            gSaveContext.cutsceneTrigger = 1;
            setupAdapter(actor, actionFunc);
            return 1;
        }
        actor = actor->next;
    }
    return 0;
}

// ---------------------------------------------------------------------
// Opt-in catchup helper — companion to Handler::optInPredicate.
// ---------------------------------------------------------------------

extern "C" int Anchor_TryEngageOptInCatchup(const char* csKind, uint32_t csKey) {
    if (csKind == nullptr) return 0;
    if (Anchor::Instance == nullptr || !Anchor::Instance->isEnabled) return 0;

    const std::string kindKey =
        std::string(csKind) + ":" + std::to_string(csKey);

    // Peer must own this cutscene (it's in cutsceneStartActive but NOT
    // in our local-origin set — meaning a peer broadcast the START).
    if (Anchor::Instance->cutsceneStartActive.count(kindKey) == 0) {
        return 0;
    }
    if (Anchor::Instance->cutsceneStartActiveLocalOrigin.count(kindKey) > 0) {
        return 0;  // We're the originator — no catchup needed
    }

    // Look up the peer originator to route the REQUEST. Fix O tracks this.
    auto originatorIt =
        Anchor::Instance->cutsceneOriginatorByKindKey.find(kindKey);
    if (originatorIt == Anchor::Instance->cutsceneOriginatorByKindKey.end()) {
        return 0;
    }
    const uint32_t leaderClientId = originatorIt->second;
    if (leaderClientId == 0 ||
        leaderClientId == Anchor::Instance->ownClientId) {
        return 0;
    }

    // Fix K.2 (log 669) — register pendingCatchups[kindKey] BEFORE sending
    // the REQUEST. Without this, HandlePacket_CutsceneCatchupResponse
    // hits the "RESPONSE for '...' but not pending; drop" guard at
    // CutsceneCatchup.cpp:1011 and discards our own RESPONSE. Matches the
    // pattern used by DetectAndRequestCutsceneCatchup (CutsceneCatchup.cpp
    // line ~1195) and the FRAME_SYNC direct-request path (line ~869).
    //
    // Timeout of 2000ms mirrors kCatchupTimeoutMs — hardcoded here to
    // avoid pulling the file-static from CutsceneCatchup.cpp. If the
    // shared constant needs to change, update both sites (audit via
    // grep for "kCatchupTimeoutMs").
    Anchor::PendingCatchup p;
    p.deadline = std::chrono::steady_clock::now()
                 + std::chrono::milliseconds(2000);
    p.leaderClientId = leaderClientId;
    Anchor::Instance->pendingCatchups[kindKey] = p;

    // Send the catchup REQUEST. The RESPONSE handler will invoke the
    // registered applyForce (via CutsceneCatchup Setup dispatch), which
    // for BgTreemouth sets csCtx.segment + cutsceneTrigger + actor
    // actionFunc — same code path as if we'd force-applied on the
    // original CUTSCENE_START receive.
    Anchor::Instance->SendPacket_CutsceneCatchupRequest(
        leaderClientId, std::string(csKind), csKey);

    SPDLOG_INFO("[CutsceneStart] Opt-in catchup engaged for '{}' — REQUEST sent to leader={}",
                kindKey, leaderClientId);
    return 1;
}

// ---------------------------------------------------------------------
// Registry — Helper B substrate.
// ---------------------------------------------------------------------

namespace CutsceneKindRegistry {

namespace {

// Forward declarations for per-customer handler bodies. Each customer
// gets a small file-static function; the built-in table below wires them
// into the registry.
int DekuTreeIntro_ApplyForce(uint32_t csKey);

// Populate the registry with built-in customers. Called once via the
// function-static initializer in GetRegistry(). Add new customers here.
std::unordered_map<std::string, Handler> MakeBuiltinRegistry() {
    std::unordered_map<std::string, Handler> map;

    map["deku_tree_intro"] = Handler{
        .applyForce      = &DekuTreeIntro_ApplyForce,
        .applyEnd        = nullptr,  // vanilla self-teardown; no explicit end hook
        // Come-back variant (csKey=1) is opt-in per user design (log 667):
        // player must intentionally Z-target the tree. First-encounter
        // (csKey=0) is proximity-triggered → force-sync unchanged.
        // See Analysis/deku_tree_come_back_sync_design_reversal_2026-07-09.md
        // for the design rationale.
        .optInPredicate  = [](uint32_t csKey) { return csKey == 1; },
    };

    return map;
}

std::unordered_map<std::string, Handler>& GetRegistry() {
    static std::unordered_map<std::string, Handler> registry = MakeBuiltinRegistry();
    return registry;
}

}  // namespace

const Handler* Find(const std::string& csKind) {
    auto& registry = GetRegistry();
    auto it = registry.find(csKind);
    if (it == registry.end()) return nullptr;
    return &it->second;
}

void Register(const std::string& csKind, Handler handler) {
    GetRegistry()[csKind] = std::move(handler);
}

}  // namespace CutsceneKindRegistry

// ---------------------------------------------------------------------
// Per-customer handler bodies.
// ---------------------------------------------------------------------
//
// One block per registered customer. Actor-side details (segment
// pointers, action funcs, per-variant behavior like flag-setting) stay
// here — the actor's .c file owns the local trigger, and its
// SetupActionAdapter + Force* export lets Helper D drive the peer-side
// mirror without needing to know per-actor types.

extern "C" {
// Bg_Treemouth pilot — the actor's Force helper is C-linkage-exported
// from z_bg_treemouth.c. Forward-declared here so the C++ registry
// entry can call it directly. Signature matches the current
// z_bg_treemouth.c definition.
int BgTreemouth_ForceIntroCutscene(PlayState* play, uint32_t csKey);
}

namespace CutsceneKindRegistry {
namespace {

int DekuTreeIntro_ApplyForce(uint32_t csKey) {
    if (gPlayState == nullptr) return 0;
    return BgTreemouth_ForceIntroCutscene(gPlayState, csKey);
}

}  // namespace
}  // namespace CutsceneKindRegistry
