#include "ModalPhantomAdapter.h"
#include "DropAdapterRegistry.h"

#include <algorithm>
#include <chrono>
#include <memory>

extern "C" {
#include "functions.h"  // Actor_Kill
#include "z64.h"
}

namespace {

inline int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

namespace SyncedClaimableDrop {

ModalPhantomAdapter* ModalPhantomAdapter::GetInstance() {
    static ModalPhantomAdapter* instance = []() -> ModalPhantomAdapter* {
        auto adapter = std::make_unique<ModalPhantomAdapter>();
        ModalPhantomAdapter* raw = adapter.get();
        DropAdapterRegistry::Instance().Register(std::move(adapter));
        return raw;
    }();
    return instance;
}

void ModalPhantomAdapter::OnPhantomSpawn(EnItem00* phantom) {
    if (phantom == nullptr) return;
    // Dedup — defensive; OnActorSpawn should only fire once per actor,
    // but cheap to guard.
    Actor* actor = &phantom->actor;
    for (const auto& entry : phantoms_) {
        if (entry.actor == actor) return;
    }
    phantoms_.push_back({ actor, NowMs() });
    SPDLOG_INFO("[ModalPhantom] spawn tracked actor={} params=0x{:02X} pos=({:.0f},{:.0f},{:.0f}) "
                "list-size={}",
                fmt::ptr(actor), (uint16_t)actor->params,
                actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
                phantoms_.size());
}

void ModalPhantomAdapter::OnActorDestroyed(Actor* actor) {
    if (actor == nullptr) return;
    auto end = std::remove_if(phantoms_.begin(), phantoms_.end(),
                              [actor](const PhantomEntry& e) { return e.actor == actor; });
    if (end != phantoms_.end()) {
        phantoms_.erase(end, phantoms_.end());
    }
}

void ModalPhantomAdapter::Tick() {
    if (phantoms_.empty()) return;
    const int64_t now = NowMs();
    for (auto& entry : phantoms_) {
        if (entry.actor == nullptr) continue;
        if (entry.actor->update == nullptr) continue;  // Already killed; OnActorDestroy will scrub.
        const int64_t age = now - entry.spawnTimeMs;
        if (age >= kPhantomBackstopMs) {
            SPDLOG_INFO("[ModalPhantom] backstop force-kill actor={} age={}ms "
                        "(vanilla unk_15A countdown stalled, Bug B class)",
                        fmt::ptr(entry.actor), (long long)age);
            Actor_Kill(entry.actor);
            // Don't remove from list here — OnActorDestroy will fire
            // when the actor is dequeued from the list and scrub the
            // entry then. update==nullptr above prevents repeated
            // Actor_Kill calls in the meantime.
        }
    }
}

}  // namespace SyncedClaimableDrop
