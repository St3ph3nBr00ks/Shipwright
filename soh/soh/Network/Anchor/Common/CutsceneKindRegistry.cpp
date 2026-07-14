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

    // Fix K.3 (log 670) — dedup while a catchup is already pending. Actor
    // trigger sites (e.g. Bg_Treemouth come-back branch) may re-check
    // isTargeted many frames per second while the user holds Z-target.
    // Without this dedup, every frame would fire a fresh REQUEST + reset
    // the pending deadline, spamming the network + the leader. Returning
    // 1 (still engaged) tells the caller "catchup is in flight, don't
    // fall through to fresh start" without re-sending.
    if (Anchor::Instance->pendingCatchups.count(kindKey) > 0) {
        return 1;
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
int Savecontext_ApplyForce(uint32_t csKey);

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

    // Savecontext-driven cutscenes (vanilla auto-triggers via
    // Cutscene_HandleConditionalTriggers on scene entry — Lost Woods
    // Saria, Kakariko Nocturne, Desert Requiem, etc.). csKey carries the
    // cutsceneIndex value (typically 0xFFF0). Registered so
    // CUTSCENE_CATCHUP_RESPONSE can dispatch here instead of aborting
    // at "No per-kind setup for csKind='savecontext'". See
    // Claude/Analysis/lost_woods_savecontext_no_handler_2026-07-13.md.
    map["savecontext"] = Handler{
        .applyForce     = &Savecontext_ApplyForce,
        .applyEnd       = nullptr,
        .optInPredicate = nullptr,
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

// Savecontext catchup applyForce.
//
// Called from HandlePacket_CutsceneCatchupResponse's per-kind dispatch
// (CutsceneCatchup.cpp:528+) when peer needs to enter a savecontext-
// driven cutscene locally so fast-forward can advance csCtx.frames.
//
// Contract:
//   Return 1 → catchup delta apply proceeds (fast-forward armed).
//   Return 0 → catchup delta apply aborts (peer stays in current state).
//
// Cases:
//   (a) `gSaveContext.cutsceneIndex != 0`
//       Peer's vanilla `Cutscene_HandleConditionalTriggers` already fired
//       locally when peer entered the scene (fresh save flag / normal MP
//       flow). csCtx.segment is set, state machine is ready to advance.
//       Fast-forward can drive func_800645A0 to reach leader's frame.
//       Return 1.
//
//   (b) `gSaveContext.cutsceneIndex == 0`
//       Two possible reasons:
//         - Peer's save flag is already set (repeat-view scenario) —
//           vanilla's flag check short-circuited the trigger block at
//           z_demo.c:2257-2264 (Saria) or sibling special-case triggers.
//         - Peer is a late-joiner in a scene that never re-fires the
//           auto-trigger without a scene reload.
//       In both cases csCtx.segment is null/stale, so writing
//       cutsceneIndex here would cause vanilla's command dispatch to
//       read from an invalid segment (crash risk).
//       Return 0. Peer stays in gameplay while leader plays the cutscene.
//       Post-cutscene teleport (Fix N, existing) will converge peer with
//       leader's post-cutscene position when leader finishes.
int Savecontext_ApplyForce(uint32_t csKey) {
    if (gPlayState == nullptr) return 0;

    if (gSaveContext.cutsceneIndex != 0) {
        // Case (a) — vanilla triggered locally. Success.
        SPDLOG_INFO("[Savecontext_ApplyForce] cutsceneIndex=0x{:04X} "
                    "already set (vanilla triggered locally); "
                    "fast-forward can proceed. csKey=0x{:04X}",
                    (unsigned)gSaveContext.cutsceneIndex, csKey);
        return 1;
    }

    // Case (b) — cannot force without valid csCtx.segment. Abort
    // gracefully. Field-observed via log 702 P2 Lost Woods Saria
    // (EVENTCHKINF_SPOKE_TO_SARIA_ON_BRIDGE pre-set from prior test).
    SPDLOG_WARN("[Savecontext_ApplyForce] cutsceneIndex=0 — vanilla local "
                "trigger did NOT fire (save flag pre-set OR late-join edge "
                "case). Cannot force without valid csCtx.segment. Catchup "
                "aborted; peer will stay in gameplay. csKey=0x{:04X}",
                csKey);
    return 0;
}

}  // namespace
}  // namespace CutsceneKindRegistry
