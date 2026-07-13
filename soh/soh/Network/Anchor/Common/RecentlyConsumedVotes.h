// RecentlyConsumedVotes — shared straggler-vote drop helper.
//
// Extracted 2026-07-09 for DRY across vote-skip (CutsceneTextAdvance.cpp
// sRecentlyConsumedVotes, landed as commit b9190878d) and choice-vote
// (DialogChoiceVote.cpp, first landed on feature/dialog-choice-vote
// branch).
//
// Purpose: dedup vote packets that arrive twice at the vote-skip host
// because of Fix V (host self-invokes HandlePacket to count its own vote
// without waiting for TCP relay loopback) + the relay's unicast-to-self
// loopback delivering the packet ~50-150 ms later. The first arrival
// often clears state (all-pressed / resolve), so the pressedClientIds /
// votesByClient dedup misses on the second arrival.
//
// Fix: track a small time-bounded list of recently-consumed votes by
// (textId, senderId) with per-entry timestamp. On each vote arrival,
// drop if a matching entry exists within the drop window.
//
// See Analysis/vote_skip_straggler_bug_2026-07-09.md §5 for the
// original pattern rationale and window sizing.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace RecentlyConsumedVotes {

// Per-record slot. Insertion order = vote-order-of-arrival; the entire
// vector is walked linearly on each check. n ≤ ~4 in practice, so
// linear-scan is faster than a hash-map lookup at these sizes.
struct Entry {
    uint16_t textId;
    uint32_t senderId;
    std::chrono::steady_clock::time_point at;
};

// Small buffer with encapsulated purge + check + record semantics.
// Each vote-consumer (vote-skip host, choice-vote host) instantiates
// its own buffer — dedup domains don't cross.
class Buffer {
public:
    // Drop-window default 500 ms (see analysis §6). Purge-window
    // default 2 s (keeps vector bounded — n ≤ ~4).
    Buffer() = default;

    // Purge entries older than kPurgeWindowMs, then check for a
    // duplicate (textId, senderId) within the last kDropWindowMs.
    // Returns true if the incoming vote should be dropped.
    //
    // Purge happens on every call so the vector self-trims without
    // needing a periodic tick. Cheap — O(n) with n ≤ ~4.
    bool ShouldDrop(uint16_t textId, uint32_t senderId,
                     std::chrono::steady_clock::time_point now,
                     int dropWindowMs = kDefaultDropWindowMs,
                     int purgeWindowMs = kDefaultPurgeWindowMs) {
        // Purge stale entries first.
        entries_.erase(
            std::remove_if(
                entries_.begin(), entries_.end(),
                [now, purgeWindowMs](const Entry& e) {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - e.at).count() > purgeWindowMs;
                }),
            entries_.end());

        // Check for duplicate.
        for (const auto& e : entries_) {
            if (e.textId != textId || e.senderId != senderId) continue;
            const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - e.at).count();
            if (ageMs < dropWindowMs) return true;
        }
        return false;
    }

    // Record a vote as consumed. Called AFTER the vote is successfully
    // tallied so that a subsequent duplicate arrival will be dropped
    // by the next ShouldDrop() call.
    void Record(uint16_t textId, uint32_t senderId,
                 std::chrono::steady_clock::time_point now) {
        entries_.push_back(Entry{textId, senderId, now});
    }

    // Public accessor for diagnostic / test purposes.
    size_t size() const { return entries_.size(); }

    // Reset — used on scene load / session teardown for cleanup.
    void Clear() { entries_.clear(); }

private:
    static constexpr int kDefaultDropWindowMs  = 500;
    static constexpr int kDefaultPurgeWindowMs = 2000;

    std::vector<Entry> entries_;
};

}  // namespace RecentlyConsumedVotes
