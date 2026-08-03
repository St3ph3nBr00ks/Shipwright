/**
 * GenericSpawnDescriptor — implementation. See header for design.
 *
 * v1 substrate scope: scaffold + floor-spawn picker + eligibility
 * pipeline. Registry is EMPTY at scaffold — consumers append entries
 * via RoomSpawnRegistry() as they land (#316 Werewolf, #323 Goma
 * swarms, per-dungeon ambient spawns per #311 plan §4).
 *
 * Sync: proposals fire on the room host only
 * (SceneAuthority::IsMyCurrentRoomHost via Director::Tick's gate).
 * Actor_Spawn + ENEMY_SPAWN broadcast reuses the existing Director
 * ExecuteSpawn path — no wire additions.
 */

#include "GenericSpawnDescriptor.h"

#include "../../Anchor.h"            // Anchor::Instance->MsToGameTicks
#include "../../Common/SceneAuthority.h"
#include "../Director.h"

#include "soh/cvar_prefixes.h"       // CVAR_ENHANCEMENT
#include "soh/Enhancements/RoomNavData/RoomNavData.h"  // GetForRoom + FindRandomReachableNode

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <cmath>     // sqrtf
#include <cstdio>    // snprintf
#include <cstdlib>   // rand

extern "C" {
#include "z64.h"
#include "variables.h"  // gSaveContext (linkAge, IS_DAY read)
#include "macros.h"     // IS_DAY macro
}

extern "C" PlayState* gPlayState;

namespace AnchorDirector {

std::vector<RoomSpawnConfig>& RoomSpawnRegistry() {
    static std::vector<RoomSpawnConfig> sRegistry;
    return sRegistry;
}

namespace {

// #311 Phase 2 — floor-only spawn-position picker. Ceiling / water /
// air variants deferred until first consumer needs them (YAGNI).
//
// Samples RoomNavData for a walkable floor node beyond
// `minDistFromPlayer` from any online player in this scene+room.
// Returns true on success; outPos set to the picked node's position.
// Returns false when no valid node found (empty nav data, all nodes
// too close to a player, etc.).
bool FindFloorSpawnPosition(int16_t sceneNum, int8_t roomNum,
                            const SessionView& view,
                            float minDistFromPlayer,
                            Vec3f& outPos) {
    const AnchorNavRoom::RoomNavData* nav =
        AnchorNavRoom::GetForRoom(sceneNum, roomNum);
    if (nav == nullptr || nav->nodes.empty()) {
        return false;
    }

    // Pick a seed player-position to search from — nearest online
    // player in this scene+room. Search radius covers the whole room
    // by using a large maxDistance clamp.
    Vec3f seedPos = { 0.0f, 0.0f, 0.0f };
    bool foundSeed = false;
    for (const auto& p : view.players) {
        if (p.sceneNum != sceneNum || p.roomNum != roomNum) continue;
        seedPos = p.worldPos;
        foundSeed = true;
        break;
    }
    if (!foundSeed) {
        // No player in the room — nothing to sample against. Caller
        // shouldn't have reached us in this state, but return false
        // defensively rather than pick a random node that could
        // spawn behind a player who's on their way in.
        return false;
    }

    // Find nearest floor node to seed player as BFS origin.
    int fromIdx = AnchorNavRoom::FindNearestFloorNodeXZRadius(nav, seedPos, 300.0f);
    if (fromIdx < 0) {
        return false;
    }

    // Pick a random reachable node within a generous 1500u radius —
    // Deku Tree main room is ~1000u, most dungeon rooms fit. Nodes
    // closer than minDistFromPlayer to ANY player in the room are
    // rejected by the filter.
    AnchorNavRoom::NavQueryOptions opts{};
    opts.avoidHazardNodes  = true;
    opts.eligibleForSwimming = false;

    // Try up to 8 random picks; take the first that clears the
    // minDistFromPlayer gate against all players in the room. This
    // is cheap (each pick is one BFS) and typically hits on iter 1-2.
    for (int attempt = 0; attempt < 8; ++attempt) {
        int idx = AnchorNavRoom::FindRandomReachableNode(nav, fromIdx,
                                                          1500.0f, opts, nullptr);
        if (idx < 0) break;
        const Vec3f& np = nav->nodes[idx].pos;
        bool tooClose = false;
        for (const auto& p : view.players) {
            if (p.sceneNum != sceneNum || p.roomNum != roomNum) continue;
            const float dx = np.x - p.worldPos.x;
            const float dy = np.y - p.worldPos.y;
            const float dz = np.z - p.worldPos.z;
            const float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < minDistFromPlayer) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) {
            outPos = np;
            return true;
        }
    }
    return false;
}

// Filter a candidate against LinkAge + day/night gates.
// Returns true if the candidate is eligible this tick.
bool CandidateEligible(const SpawnCandidate& c) {
    if (c.requireLinkAge != 0) {
        const int currentAge = (int)gSaveContext.linkAge;  // 0=adult,1=child in decomp
        // Plan spec: 1=child, 2=adult. Convert.
        const int candidateAge = (c.requireLinkAge == 1) ? 1 : 0;
        if (currentAge != candidateAge) return false;
    }
    if (c.requireDay != 0) {
        const bool isDay = IS_DAY;
        const bool candidateWantsDay = (c.requireDay == 1);
        if (isDay != candidateWantsDay) return false;
    }
    return true;
}

// Weighted-random pick among eligible candidates. Returns index into
// the candidates vector, or -1 if none are eligible (empty pool or
// all filtered out).
int PickWeightedCandidate(const std::vector<SpawnCandidate>& candidates) {
    // Sum weights of eligible candidates.
    int totalWeight = 0;
    for (const auto& c : candidates) {
        if (!CandidateEligible(c)) continue;
        totalWeight += (int)c.weight;
    }
    if (totalWeight <= 0) return -1;

    // Roll in [0, totalWeight).
    int roll = std::rand() % totalWeight;
    int accum = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!CandidateEligible(candidates[i])) continue;
        accum += (int)candidates[i].weight;
        if (roll < accum) return (int)i;
    }
    // Fallback (shouldn't reach here — rounding safety).
    return -1;
}

}  // namespace

bool GenericSpawnDescriptor::IsEnabled() const {
    return CVarGetInteger(GenericSpawnsCVarName(), 0) != 0;
}

std::vector<SpawnProposal> GenericSpawnDescriptor::ProposeSpawn(
    const Director& director, const SessionView& view) {

    std::vector<SpawnProposal> out;

    if (gPlayState == nullptr) return out;

    const auto& registry = RoomSpawnRegistry();
    if (registry.empty()) {
        // Scaffold state — nothing to spawn until a consumer appends
        // registry entries. Silent no-op.
        return out;
    }

    // Find any player in a registered (scene, room) tuple. First
    // matching entry wins — most rooms will have at most one
    // registry entry so priority doesn't matter for scaffold.
    for (const auto& p : view.players) {
        if (!p.isSaveLoaded) continue;

        for (const auto& cfg : registry) {
            if (cfg.sceneNum != p.sceneNum) continue;
            // Wildcard roomNum=-1 not yet supported; requires cfg
            // to match player's room exactly.
            if (cfg.roomNum != p.roomNum) continue;

            // Min-time-in-room gate — plan spec "wait N seconds after
            // player enters room before proposing spawn."
            if (Anchor::Instance != nullptr) {
                const int minTicks = Anchor::Instance->MsToGameTicks(
                    (int)cfg.minTimeInRoomSeconds * 1000);
                if (p.framesInCurrentRoom < minTicks) continue;
            }

            // Cooldown gate — via Director's per-(scene, room, descId) ledger.
            if (!director.MsCooldownElapsed(cfg.sceneNum, cfg.roomNum,
                                            GetDescriptorId(),
                                            (int)cfg.cooldownSeconds * 1000)) {
                continue;
            }

            // Per-room live-cap gate — own map.
            const uint32_t roomKey = MakeRoomKey(cfg.sceneNum, cfg.roomNum);
            const int live = mLiveCountByRoom.count(roomKey) ?
                              mLiveCountByRoom.at(roomKey) : 0;
            if (live >= (int)cfg.maxConcurrentSpawns) continue;

            // Weighted-random candidate pick after LinkAge/day filter.
            const int candIdx = PickWeightedCandidate(cfg.candidates);
            if (candIdx < 0) continue;
            const SpawnCandidate& cand = cfg.candidates[candIdx];

            // Position pick — floor variant only for scaffold.
            // Ceiling/water/air deferred to first consumer that needs
            // them. Rejecting the proposal is OK; another descriptor
            // may still spawn this tick.
            Vec3f pos{};
            if (cand.surface == SpawnSurface::Floor) {
                if (!FindFloorSpawnPosition(cfg.sceneNum, cfg.roomNum,
                                            view, cfg.minDistFromPlayer,
                                            pos)) {
                    continue;
                }
            } else {
                // Non-floor surfaces are v2. Silently skip for now —
                // consumer using ceiling/water/air will find their
                // spawn candidate rejected until surface picker lands.
                continue;
            }

            SpawnProposal prop{};
            prop.source       = this;
            prop.sceneNum     = cfg.sceneNum;
            prop.roomNum      = cfg.roomNum;
            prop.worldPos     = pos;
            prop.yawTowards   = 0;
            prop.actorId      = cand.actorId;
            prop.actorParams  = cand.actorParams;
            prop.priority     = DescriptorPriority::Ambient;

            // Stash for OnSpawnExecuted so we know which room bucket to bump.
            mLastProposedScene = cfg.sceneNum;
            mLastProposedRoom  = cfg.roomNum;

            ++mProposalsOffered;
            out.push_back(prop);
            // One proposal per tick keeps the arbitration loop simple;
            // even if multiple rooms match (e.g. two players in two
            // registered rooms), the earliest match wins.
            return out;
        }
    }

    return out;
}

void GenericSpawnDescriptor::OnSpawnExecuted(uint32_t netId, const Vec3f& worldPos) {
    (void)worldPos;
    if (mLastProposedScene < 0) return;   // defensive
    const uint32_t key = MakeRoomKey(mLastProposedScene, mLastProposedRoom);
    ++mLiveCountByRoom[key];
    mNetIdToRoomKey[netId] = key;
    ++mSpawnsExecuted;
}

void GenericSpawnDescriptor::OnSpawnRemoved(uint32_t netId, DefeatCause cause) {
    (void)cause;
    auto it = mNetIdToRoomKey.find(netId);
    if (it == mNetIdToRoomKey.end()) return;
    const uint32_t key = it->second;
    mNetIdToRoomKey.erase(it);
    auto liveIt = mLiveCountByRoom.find(key);
    if (liveIt != mLiveCountByRoom.end() && liveIt->second > 0) {
        --liveIt->second;
    }
}

std::string GenericSpawnDescriptor::GetDebugSnapshotLine() const {
    char buf[128];
    const size_t registrySize = RoomSpawnRegistry().size();
    std::snprintf(buf, sizeof(buf),
                  "registry=%zu proposals=%d spawns=%d liveRooms=%zu",
                  registrySize, mProposalsOffered, mSpawnsExecuted,
                  mLiveCountByRoom.size());
    return std::string(buf);
}

}  // namespace AnchorDirector
