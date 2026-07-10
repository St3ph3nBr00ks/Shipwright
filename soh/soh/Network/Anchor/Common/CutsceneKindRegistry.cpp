#include "CutsceneKindRegistry.h"
#include "soh/Network/Anchor/Anchor.h"

#include <libultraship/libultraship.h>
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
        .applyForce = &DekuTreeIntro_ApplyForce,
        .applyEnd   = nullptr,  // vanilla self-teardown; no explicit end hook
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
