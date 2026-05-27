#include "SyncedClaimableDrop.h"

#include <algorithm>

namespace SyncedClaimableDrop {

Registry& Registry::Instance() {
    static Registry instance;
    return instance;
}

Drop* Registry::AllocateDrop(uint32_t dropId, uint32_t authorityClientId,
                              int16_t itemType, Vec3f spawnPos,
                              int16_t sceneNum, int8_t roomNum, uint8_t linkAge,
                              uint32_t killerClientId, int64_t spawnTimeMs) {
    if (dropId == 0) {
        SPDLOG_WARN("[SyncedClaimableDrop] AllocateDrop: dropId=0 reserved; rejecting");
        return nullptr;
    }
    if (drops_.count(dropId) != 0) {
        SPDLOG_DEBUG("[SyncedClaimableDrop] AllocateDrop: dropId={} already exists; "
                     "returning existing entry (caller should Find() first)",
                     dropId);
        return &drops_[dropId];
    }

    Drop& drop = drops_[dropId];
    drop.dropId            = dropId;
    drop.authorityClientId = authorityClientId;
    drop.itemType          = itemType;
    drop.spawnPos          = spawnPos;
    drop.sceneNum          = sceneNum;
    drop.roomNum           = roomNum;
    drop.linkAge           = linkAge;
    drop.state             = DropState::Available;
    drop.claimerClientId   = 0;
    drop.spawnTimeMs       = spawnTimeMs;
    drop.killerClientId    = killerClientId;
    drop.visualReps.clear();

    SPDLOG_INFO("[SyncedClaimableDrop] alloc dropId={} authority={} itemType=0x{:02X} "
                "pos=({:.0f},{:.0f},{:.0f}) scene={} room={} timeline={} killer={}",
                dropId, authorityClientId, itemType,
                spawnPos.x, spawnPos.y, spawnPos.z,
                sceneNum, (int)roomNum, (int)linkAge, killerClientId);
    return &drop;
}

Drop* Registry::Find(uint32_t dropId) {
    if (dropId == 0) return nullptr;
    auto it = drops_.find(dropId);
    return (it == drops_.end()) ? nullptr : &it->second;
}

void Registry::RegisterVisualRep(uint32_t dropId, Actor* actor) {
    if (dropId == 0 || actor == nullptr) return;
    Drop* drop = Find(dropId);
    if (drop == nullptr) {
        SPDLOG_WARN("[SyncedClaimableDrop] RegisterVisualRep: dropId={} not in registry",
                    dropId);
        return;
    }
    // Dedup — duplicate registers are no-ops.
    for (Actor* existing : drop->visualReps) {
        if (existing == actor) return;
    }
    drop->visualReps.push_back(actor);
}

void Registry::UnregisterFromAllDrops(Actor* actor) {
    if (actor == nullptr) return;
    for (auto& [_dropId, drop] : drops_) {
        auto& reps = drop.visualReps;
        reps.erase(std::remove(reps.begin(), reps.end(), actor), reps.end());
    }
}

void Registry::TransitionTo(Drop& drop, DropState newState) {
    drop.state = newState;
}

void Registry::RemoveDrop(uint32_t dropId) {
    drops_.erase(dropId);
}

void Registry::Clear() {
    drops_.clear();
    pendingDismissals_.clear();
}

void Registry::MarkPendingDismissal(uint32_t dropId, uint32_t killerClientId) {
    if (dropId == 0) return;
    pendingDismissals_[dropId] = killerClientId;
    SPDLOG_INFO("[SyncedClaimableDrop] pending-dismissal queued dropId={} killer={} "
                "(awaiting matching modal-offer ITEM_DROP_SYNC)",
                dropId, killerClientId);
}

bool Registry::ConsumePendingDismissal(uint32_t dropId, uint32_t& outKillerClientId) {
    auto it = pendingDismissals_.find(dropId);
    if (it == pendingDismissals_.end()) return false;
    outKillerClientId = it->second;
    pendingDismissals_.erase(it);
    return true;
}

}  // namespace SyncedClaimableDrop
