/**
 * EnhancementRegistry — implementation.
 *
 * Pillar 5 Phase 1 scaffolding. No descriptors registered here — the
 * constructor is empty. Descriptors will be registered from their
 * own per-actor .cpp files in Phase 2+, one Register() call each.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.2 + §7 Phase 1.
 */

#include "EnhancementRegistry.h"

#include <libultraship/libultraship.h>

#include "soh/Network/Anchor/Common/ActorSyncHelpers.h"

namespace AnchorEnemyEnhancement {

EnhancementRegistry& EnhancementRegistry::Instance() {
    static EnhancementRegistry sInstance;
    return sInstance;
}

EnemyEnhancementDescriptor*
EnhancementRegistry::Register(std::unique_ptr<EnemyEnhancementDescriptor> descriptor) {
    if (!descriptor) {
        SPDLOG_WARN("[EnhancementRegistry] Register called with null descriptor — ignored");
        return nullptr;
    }

    const int16_t actorId = descriptor->ActorId();

    // Uniqueness invariant. Rejecting duplicates prevents silent
    // last-writer-wins semantics that would surface as intermittent
    // bugs when someone forgets to remove a stub.
    if (mByActorId.count(actorId) > 0) {
        SPDLOG_ERROR("[EnhancementRegistry] Duplicate descriptor for actorId=0x{:04X} "
                     "'{}' — first registration wins, this one ignored",
                     (unsigned)actorId, descriptor->ActorName());
        return nullptr;
    }

    // Boss exclusion. Per project rule, opting a boss into the
    // enhancement registry requires explicit user sign-off. Silently
    // rejecting here would hide the mistake; log at ERROR so any
    // accidental boss registration surfaces immediately.
    if (::IsSyncedBossActor(actorId)) {
        SPDLOG_ERROR("[EnhancementRegistry] Boss actor 0x{:04X} '{}' rejected — "
                     "boss enhancement requires explicit user sign-off per "
                     "feedback_bosses_excluded_from_ai_extensions.md",
                     (unsigned)actorId, descriptor->ActorName());
        return nullptr;
    }

    EnemyEnhancementDescriptor* raw = descriptor.get();
    mDescriptors.push_back(std::move(descriptor));
    RebuildIndex();

    SPDLOG_INFO("[EnhancementRegistry] Registered actorId=0x{:04X} '{}' "
                "capabilities=(nav={} gravity={} activeAggro={} combat={})",
                (unsigned)actorId, raw->ActorName(),
                raw->Capabilities().canNavConsume,
                raw->Capabilities().canGravityAware,
                raw->Capabilities().canActiveAggro,
                raw->Capabilities().canReplaceCombat);
    return raw;
}

const EnemyEnhancementDescriptor*
EnhancementRegistry::Find(int16_t actorId) const {
    auto it = mByActorId.find(actorId);
    if (it == mByActorId.end()) return nullptr;
    return it->second;
}

void EnhancementRegistry::RebuildIndex() {
    mByActorId.clear();
    mAllView.clear();
    mAllView.reserve(mDescriptors.size());
    for (const auto& d : mDescriptors) {
        if (!d) continue;
        mByActorId[d->ActorId()] = d.get();
        mAllView.push_back(d.get());
    }
}

}  // namespace AnchorEnemyEnhancement
