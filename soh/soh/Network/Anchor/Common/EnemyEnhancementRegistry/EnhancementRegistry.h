/**
 * EnhancementRegistry — singleton store of EnemyEnhancementDescriptor
 * instances, keyed by vanilla actor id.
 *
 * Lifecycle:
 *   - Lazy-initialised via Instance() on first Find/Register call.
 *   - Descriptors registered explicitly from a static-init hook or
 *     from Anchor's init path (per plan §7 Phase 1 step 5).
 *   - Never removed once registered — the process holds descriptors
 *     for its lifetime.
 *
 * Boss exclusion guard (§4.3):
 *   Register rejects any descriptor whose ActorId() matches
 *   IsSyncedBossActor(). Explicit-boss opt-in requires bypassing this
 *   guard, which we intentionally do NOT expose in v1 — per project
 *   rule (feedback_bosses_excluded_from_ai_extensions.md), opting a
 *   boss in needs user sign-off.
 *
 * Thread safety: single-threaded during registration (Anchor init or
 * static-init). Read-only lookup thereafter — no lock needed.
 *
 * See Plans/vanilla_enemy_enhancements_plan.md §4.2 for the design
 * rationale and §7 Phase 1 for this file's scaffolding-only role.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "EnemyEnhancementDescriptor.h"

namespace AnchorEnemyEnhancement {

class EnhancementRegistry {
public:
    // Singleton access. Lazy-initialised static local.
    static EnhancementRegistry& Instance();

    // Register a descriptor. Rejected (and returned nullptr) if:
    //   - descriptor is null
    //   - ActorId() is already registered (uniqueness invariant)
    //   - ActorId() matches IsSyncedBossActor (boss exclusion)
    // Returns the raw pointer on success (Registry retains ownership
    // via unique_ptr).
    EnemyEnhancementDescriptor*
        Register(std::unique_ptr<EnemyEnhancementDescriptor> descriptor);

    // Lookup by vanilla actor id. Returns nullptr when no descriptor
    // is registered — vanilla path runs unmodified.
    const EnemyEnhancementDescriptor* Find(int16_t actorId) const;

    // Enumeration for debug UI. Returns descriptor pointers by
    // reference; do not store beyond Registry lifetime.
    const std::vector<const EnemyEnhancementDescriptor*>& All() const {
        return mAllView;
    }

    // Test / diagnostic — number of registered descriptors.
    size_t Count() const { return mDescriptors.size(); }

private:
    EnhancementRegistry() = default;
    ~EnhancementRegistry() = default;
    EnhancementRegistry(const EnhancementRegistry&) = delete;
    EnhancementRegistry& operator=(const EnhancementRegistry&) = delete;

    // Owning storage — descriptor lifetime is process-lifetime.
    std::vector<std::unique_ptr<EnemyEnhancementDescriptor>> mDescriptors;

    // Actor-id → descriptor* index for O(1) Find. Rebuilt whenever
    // Register is called; small size makes rebuild-cost negligible
    // compared to a per-Register unordered_map insert.
    std::unordered_map<int16_t, const EnemyEnhancementDescriptor*> mByActorId;

    // Non-owning parallel vector for All() enumeration. Rebuilt on
    // Register — same rationale as mByActorId.
    std::vector<const EnemyEnhancementDescriptor*> mAllView;

    void RebuildIndex();
};

}  // namespace AnchorEnemyEnhancement
