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
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

#include "global.h"  // Vec3s, ACTOR_* enum

namespace SkelAnimeWire {

// Wire-layer pose-table size ceiling. Bumped 32 -> 64 (2026-06-16, #274)
// after source-verifying that Stalfos's struct declares
// jointTable[STALFOS_LIMB_MAX = 61] (z_en_test.h:73,80). Prior cap of 32
// was silently truncating Stalfos pose updates by 29 limbs. All three
// callers (SerializePoseTable, DeserializePoseTable, EnemyState.cpp:453)
// use std::min for clamping; no fixed-size buffers sized to kHardCap.
// Bumping to 64 also leaves headroom for Lizalfos (49) and Octorok (38)
// once they're admitted via GetEnemySkelAnime's per-actor ceiling allow-
// list. The Layer 3 limbCount registry below will WARN if any actor's
// observed limbCount exceeds its registered value.
constexpr uint8_t kHardCap = 64;

// Per-actor expected SkelAnime->limbCount registry.
//
// Defensive layering:
//   - Layer 1 (DeserializePoseTable): clamps incoming pose tables
//     to kHardCap=32 regardless of advertised limbCount.
//   - Layer 2 (GetEnemySkelAnime generic-case heuristic): rejects
//     skelAnime probes whose limbCount == 0 || > 30 ||
//     jointTable == nullptr.
//   - Layer 3 (THIS REGISTRY): soft SPDLOG_WARN at OnActorSpawn
//     when local actor's skelAnime->limbCount disagrees with the
//     per-id expected value. Surfaces ROM variants, tampered
//     packs, or new sync admissions that landed without a
//     corresponding skel-exception entry.
//
// Entries deliberately omitted (verify before adding):
//   - EnBa  / EnBubble / EnBx / EnClearTag / EnRr / EnSb /
//     EnTorch2 / EnTp / EnYukabyun — header lacks
//     Vec3s jointTable[N] declaration (programmatic body or
//     non-SkelAnime).
//   - EnTuboTrap — explicitly rejected by GetEnemySkelAnime
//     switch (no SkelAnime substruct).
//   - EnBom / EnBombf — explosive category, not ACTORCAT_ENEMY.
//
// Entries with high limb counts (handled via #274 fix, this commit):
//   - EnTest (Stalfos)  : 61 limbs — skel-exception path; fits under
//     bumped kHardCap=64.
//   - EnZf   (Lizalfos) : 49 limbs — generic-cast path; admitted via
//     per-actor ceiling allowlist in GetEnemySkelAnime (raises >30
//     guard to >64 for this actor only).
//   - EnOkuta (Octorok) : 38 limbs — same per-actor allowlist as EnZf.
//
// Audit lineage: Plans/skelanime_expected_limbcount_registry_2026-06-15.md
inline const std::unordered_map<s16, uint8_t> kExpectedLimbCount = {
    // ----- Skel-exception group (explicit cast in GetEnemySkelAnime) -----
    { ACTOR_EN_DEKUBABA,  8  },  // z_en_dekubaba.h:25
    { ACTOR_EN_TEST,      61 },  // z_en_test.h:73,80 (STALFOS_LIMB_MAX) — within kHardCap=64
    { ACTOR_EN_RD,        26 },  // z_en_rd.h:9
    { ACTOR_EN_WF,        22 },  // z_en_wf.h:34,58 (WOLFOS_LIMB_MAX)
    { ACTOR_EN_MB,        28 },  // z_en_mb.h:14

    // ----- Karebaba -----
    { ACTOR_EN_KAREBABA,  8  },  // z_en_karebaba.h:15

    // ----- Generic-case ENEMY-category enemies -----
    { ACTOR_EN_AM,         14 },  // z_en_am.h:15
    { ACTOR_EN_ANUBICE,    16 },  // ANUBICE_LIMB_MAX
    { ACTOR_EN_ATTACK_NIW, 16 },  // z_en_attack_niw.h:14
    { ACTOR_EN_BB,         16 },  // z_en_bb.h:14
    { ACTOR_EN_BIGOKUTA,   20 },  // z_en_bigokuta.h:20
    { ACTOR_EN_BILI,       5  },  // EN_BILI_LIMB_MAX
    { ACTOR_EN_BROB,       10 },  // z_en_brob.h:17
    { ACTOR_EN_BW,         12 },  // z_en_bw.h:13
    { ACTOR_EN_CROW,       9  },  // z_en_crow.h:19
    { ACTOR_EN_DEKUNUTS,   25 },  // z_en_dekunuts.h:20
    { ACTOR_EN_DH,         16 },  // z_en_dh.h:14
    { ACTOR_EN_DHA,        4  },  // z_en_dha.h:14
    { ACTOR_EN_DODOJR,     15 },  // z_en_dodojr.h:25
    { ACTOR_EN_DODONGO,    31 },  // z_en_dodongo.h:14 — NEAR kHardCap=32
    { ACTOR_EN_EIYER,      19 },  // z_en_eiyer.h:17
    { ACTOR_EN_FD,         27 },  // z_en_fd.h:52
    { ACTOR_EN_FIREFLY,    28 },  // z_en_firefly.h:20
    { ACTOR_EN_FLOORMAS,   25 },  // z_en_floormas.h:19
    { ACTOR_EN_GELDB,      24 },  // GELDB_LIMB_MAX
    { ACTOR_EN_GOMA,       24 },  // z_en_goma.h:49
    { ACTOR_EN_HINTNUTS,   10 },  // z_en_hintnuts.h:18
    { ACTOR_EN_IK,         30 },  // z_en_ik.h:14 — AT kHardCap-2
    { ACTOR_EN_OKUTA,      38 },  // z_en_okuta.h:17 — admitted via per-actor ceiling allowlist
    { ACTOR_EN_PEEHAT,     24 },  // z_en_peehat.h:20
    { ACTOR_EN_PO_SISTERS, 12 },  // z_en_po_sisters.h:23
    { ACTOR_EN_POH,        21 },  // z_en_poh.h:49
    { ACTOR_EN_PO_FIELD,   10 },  // z_en_po_field.h:33
    { ACTOR_EN_REEBA,      18 },  // z_en_reeba.h:14
    { ACTOR_EN_SKB,        20 },  // z_en_skb.h:14
    { ACTOR_EN_SKJ,        19 },  // z_en_skj.h:14
    { ACTOR_EN_ST,         30 },  // z_en_st.h:51
    { ACTOR_EN_SSH,        30 },  // z_en_ssh.h:14
    { ACTOR_EN_SW,         30 },  // z_en_sw.h:18
    { ACTOR_EN_TITE,       25 },  // z_en_tite.h:19
    { ACTOR_EN_TR,         27 },  // z_en_tr.h:14
    { ACTOR_EN_VALI,       29 },  // EN_VALI_LIMB_MAX
    { ACTOR_EN_VM,         11 },  // z_en_vm.h:14
    { ACTOR_EN_WALLMAS,    25 },  // z_en_wallmas.h:24
    { ACTOR_EN_WEIYER,     19 },  // z_en_weiyer.h:17
    { ACTOR_EN_ZF,         49 },  // ENZF_LIMB_MAX — admitted via per-actor ceiling allowlist

    // ----- Boss admission (IsSyncedBossActor) -----
    { ACTOR_BOSS_GOMA,     24 },  // z_en_goma.h:49 (shared struct family)
};

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
