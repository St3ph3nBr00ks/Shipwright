#include "ActorSyncScope.h"

#include "soh/ObjectExtension/ObjectExtension.h"

#include <cstring>

extern "C" {
#include "z64.h"  // Actor
}

namespace AnchorSync {

namespace {

// Per-actor-instance scope storage. Attached via ObjectExtension; the
// Actor* serves as the key. Default-initialized struct represents
// "no explicit scope set" — GetActorSyncScope falls back to Global in
// that case (Phase 0 behaviour-preserving default).
struct ActorSyncScopeData {
    ActorSyncScope scope = ActorSyncScope::Global;
};

// Register the type for ObjectExtension lookup.
ObjectExtension::Register<ActorSyncScopeData> sActorSyncScopeRegister;

}  // namespace

ActorSyncScope GetActorSyncScope(const Actor* actor) {
    if (actor == nullptr) {
        return ActorSyncScope::None;
    }
    auto* data = ObjectExtension::GetInstance().Get<ActorSyncScopeData>(actor);
    if (data == nullptr) {
        // No explicit scope set — Phase 0 default is Global so existing
        // synced-actor behaviour is preserved without per-actor Init
        // hooks. Phase 3 (migration) will flip this to None after every
        // currently-synced actor has an explicit Init-time scope set.
        return ActorSyncScope::Global;
    }
    return data->scope;
}

void SetActorSyncScope(const Actor* actor, ActorSyncScope scope) {
    if (actor == nullptr) {
        return;
    }
    ObjectExtension::GetInstance().Set<ActorSyncScopeData>(actor, ActorSyncScopeData{scope});
}

const char* ActorSyncScopeToString(ActorSyncScope scope) {
    switch (scope) {
        case ActorSyncScope::None:   return "none";
        case ActorSyncScope::Team:   return "team";
        case ActorSyncScope::Global: return "global";
    }
    return "global";  // unreachable; appease compiler
}

ActorSyncScope ActorSyncScopeFromString(const char* s) {
    if (s == nullptr) {
        return ActorSyncScope::Global;
    }
    if (std::strcmp(s, "none") == 0) {
        return ActorSyncScope::None;
    }
    if (std::strcmp(s, "team") == 0) {
        return ActorSyncScope::Team;
    }
    // "global" or unknown (forward-compat) — treat as Global.
    return ActorSyncScope::Global;
}

}  // namespace AnchorSync
