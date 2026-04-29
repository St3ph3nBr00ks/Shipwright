#pragma once

// Centralised joint/morph table (de)serialization for ENEMY_STATE and any
// future packet that ships a SkelAnime pose. Three goals:
//
//   1. One canonical place for the loop bound. The bound was previously
//      duplicated across 4 sites in EnemyState.cpp and an off-by-one
//      (`i <= limbCount` instead of `i < limbCount`) wrote past the actor
//      struct's table allocation, corrupting adjacent fields. #171 root
//      cause — see commit log for the EnDekubaba boundFloor case.
//
//   2. Defence-in-depth on the receive side. No OoT actor ships more than
//      ~30 limbs; the kHardCap below clamps any incoming pose to a sane
//      maximum regardless of what `limbCount` claims, protecting against
//      malformed packets from a malicious or buggy peer.
//
//   3. The single helper makes adding a third pose-style sync field
//      (e.g., a future actor's secondary skel) a one-line change rather
//      than a four-site copy-paste invitation.

#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

#include "global.h"  // Vec3s

namespace SkelAnimeWire {

// No vanilla OoT actor exceeds this. Loud red flag if anything does.
constexpr uint8_t kHardCap = 32;

// Serialize `count` slots from `table` into a JSON array. Caller passes
// the actor struct's actual allocated slot count (NOT skelAnime->limbCount,
// which can include a +1 root-translation slot for some skeletons but
// never indexes past the struct allocation).
nlohmann::json SerializePoseTable(const Vec3s* table, uint8_t count);

// Deserialize a JSON array into `dest`, writing at most `maxSlots` entries.
// `maxSlots` MUST be the actor struct's allocated slot count. Anything
// beyond is silently dropped; anything claiming more than kHardCap entries
// emits a warning log identifying the netId.
void DeserializePoseTable(Vec3s* dest,
                          uint8_t maxSlots,
                          const nlohmann::json& arr,
                          uint32_t netIdForDiag);

}  // namespace SkelAnimeWire
