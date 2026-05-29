#pragma once

// Per-client connection-state snapshot. Tracks per-peer position/animation
// state, cosmetic-sync resources, dialogue voting state, and follower
// activity flags.
//
// Extracted from Anchor.h 2026-05-21 per Plans/decoupling_gap_audit_2026-05-16.md
// §3.3 / A.3. Anchor.h re-includes this header so existing consumers continue
// to compile unchanged.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <libultraship/libultraship.h>
#include "soh/resource/type/Skeleton.h"

extern "C" {
#include "z64.h"
}

typedef struct AnchorClient {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
    // Peer's per-packet-type maxSchema, populated from HANDSHAKE.
    // Pillar F: senders consult `PeerSupportsField()` to decide whether
    // to include schema-N optional fields. Empty for pre-Pillar-F peers.
    std::unordered_map<std::string, int> peerMaxSchema;
    bool online;
    bool self;
    uint32_t seed;
    bool isSaveLoaded;
    bool isGameComplete;
    s16 sceneNum;
    s8 curRoomNum;
    s32 entranceIndex;
    // Monotonic counter incremented on every OnSceneSpawnActors on the sender.
    // Host uses it to detect same-scene scene-reloads (Game Over continue, void-out,
    // Farore's Wind back to current scene) — cases where sceneNum and isSaveLoaded
    // both stay unchanged but the scene was freshly respawned, so dead-enemy replay
    // must fire. Starts at 0; first scene-spawn after connect increments to 1.
    uint32_t sceneSpawnEpoch = 0;

    // Only available in PLAYER_UPDATE packets
    s32 linkAge;
    PosRot posRot;

    // Active camera state, sourced from GET_ACTIVE_CAM(play). Drives the
    // host-side "is the candidate spawn visible to this peer?" gate in
    // AIDirector PickSpawnPosition — Link's posRot is a poor proxy for
    // what the player can actually see (camera is offset behind/above
    // Link and looks where the player aims it).
    // Default {0,0,0}/{0,0,0} for pre-update peers; safe because the gate
    // tests in-frustum which collapses to "not visible" when eye == at.
    Vec3f cameraEye = { 0.0f, 0.0f, 0.0f };
    Vec3f cameraAt  = { 0.0f, 0.0f, 0.0f };

    Vec3s jointTable[24];
    u8 movementFlags;
    Vec3s prevTransl;
    Vec3s upperLimbRot;
    // Upper-body anim joint table — drives carry / hookshot / bow draw
    // poses. The vanilla upper→main merge runs only inside Player_Update
    // (z_player.c:3634 AnimationContext_SetCopyTrue), which never fires
    // for DummyPlayers — DummyPlayer_Update replicates the merge manually
    // using a duplicate of sUpperBodyLimbCopyMap.
    // hasUpperJointTable gates the observer-side merge: when false (peer
    // didn't send the field — pre-update peer or missing payload), the
    // merge is skipped so the main joint table is rendered as-is. An
    // empty default would otherwise overlay zero-rotations on upper-body
    // limbs and break the pose.
    // See Plans/carry_held_actor_sync.md §3.1.
    Vec3s upperJointTable[24];
    bool hasUpperJointTable = false;
    s8 currentBoots;
    s8 currentShield;
    s8 currentTunic;
    u32 stateFlags1;
    u32 stateFlags2;
    u8 buttonItem0;
    s8 itemAction;
    s8 heldItemAction;
    u8 modelGroup;
    s8 invincibilityTimer;
    f32 unk_85C;
    // Deku-Stick burning timer (Player.unk_860). 0 = unlit, >0 = burning
    // and counting down. Synced separately from unk_85C (which is a
    // visual scale field that stays at 1.0 for held-but-unlit sticks).
    // Used by DummyPlayer_Update to gate the burning-stick flame VFX.
    s16 unk_860;
    s16 unk_862;
    s8 actionVar1;
    u8 ocarinaNote;
    f32 ocarinaModulator;
    s8 ocarinaBend;

    // Multi-player dialogue redesign (#191 follow-up) — peer's
    // current `play->csCtx.state`. Used by `Anchor_ShouldAdvanceCutsceneTextLocal`
    // to detect "alone in cutscene": when no online team-member in the
    // local scene has `csCtxState != CS_STATE_IDLE`, the local player
    // is the only one with the active cutscene textbox and should
    // advance immediately on button press (vanilla parity), not
    // through the voting countdown which would block until the timer
    // elapses (no peer to vote). Defaults to CS_STATE_IDLE (0) for
    // pre-update peers — they're treated as out-of-cutscene, which
    // is the safe assumption when state is unknown.
    s8 csCtxState = 0;

    // AI follower mode (remote client is running the follower AI and should not be
    // selected as a follower's leader target). Defaults to false for pre-update peers.
    bool followerActive = false;

    // Phase 2 (#193 item_drop_behavior_spec.md §4 Phase 2) —
    // per-client eligibility bitmap, one bit per ITEM00_* type
    // (layout in ItemEligibility::EligibilityBitForItem00). Updated
    // on save-load and on every OnItemReceive (vanilla VB_GIVE_ITEM
    // proxy). Consumed by Layer 2 of the local pickup gate in
    // VB_GIVE_ITEM_FROM_ITEM_00: when local player is ineligible AND
    // SOME teammate's bitmap has the relevant bit set, defer pickup
    // (drop stays for the eligible teammate); when NO teammate is
    // eligible, allow vanilla pickup (silent-truncate parity with
    // single-player). 0 for pre-update peers (acts as "no teammate
    // eligible", which is the safe / lenient default).
    uint32_t eligibilityBitmap = 0;

    // G1/G2 — set when the remote client's local Player is in a climbing state
    // (PLAYER_STATE1_CLIMBING_LADDER for vines/ladders, or HANGING_OFF_LEDGE /
    // CLIMBING_LEDGE during ledge-hoist). Broadcast on edge change so the
    // follower can teleport-and-ride along. Defaults to false for pre-update peers.
    bool isClimbing = false;

    // Test 5 (log 71) — set when the remote client's local Player has
    // PLAYER_STATE2_CRAWLING set (inside a dungeon crawlspace). Follower
    // uses this to switch its FOLLOW target from the +kFollowOffset
    // sideTarget to the leader's exact XZ — sideTarget lands off-axis
    // from the crawlspace hole and DO_ACTION_ENTER never fires on the
    // follower. Default false for pre-update peers.
    bool isCrawling = false;

    // Multiplayer cosmetic sync
    std::string customModelFilename;                    // folder name of remote client's coop pack, or ""
    std::shared_ptr<SOH::Skeleton> customSkeleton;      // keeps loaded skeleton alive; nullptr = vanilla
    std::string lastAppliedModelFilename;               // folder that was last passed to ApplyCustomSkeletonToDummyPlayer;
                                                        // suppresses per-frame retries when the lookup fails
    std::unique_ptr<SOH::BakedPlayerModel> bakedModel;  // per-DummyPlayer baked display lists;
                                                        // unique_ptr so its address (and all c_str() inside) is
                                                        // stable even if this unordered_map entry is rehashed

    // KB-15 / issue #110 — deferred destruction of a replaced BakedPlayerModel.
    //
    // When cosmetic state changes (remote pack change, local model/tunic change,
    // DummyPlayer re-spawn), the outgoing bakedModel cannot be destroyed
    // synchronously: the most recently submitted frame's Gfx buffer still holds
    // raw pointers into its pathStrings / bakedDLs / eyeTexKeys. Destroying it
    // now leaves those pointers dangling and the renderer walks freed heap memory
    // (Unhandled OP code crash flood observed in Test 17 log 113).
    //
    // Instead: append the outgoing bakedModel to retiredBakedModels with a
    // framesRemaining counter, and let OnGameFrameUpdate tick the counter
    // down. When an entry's counter hits 0 it is erased and the unique_ptr
    // destroys the model — by which point every Gfx frame that could have
    // referenced it has been fully consumed by the renderer.
    //
    // KB-19 (#176): vector replaces the prior single-slot pattern. The
    // single-slot's "≥400 ms per bake" assumption only held for pack-archive
    // bakes; vanilla revert (BuildVanillaDummyPlayerModel) is ~10 ms, so
    // rapid re-bakes from a P2 age switch could land 4 retires in 41 ms —
    // well within kRetireFrames — and the second retire's std::move
    // destroyed the first while its Gfx commands were still in-flight.
    std::vector<SOH::RetiredBake> retiredBakedModels;

    // Helper: move current bakedModel into the retire vector, armed with
    // kRetireFrames. No-op if bakedModel is already null.
    void RetireBakedModel();

    // Ptr to the dummy player
    Player* player;
} AnchorClient;

// Number of render frames a retired BakedPlayerModel must sit idle before
// destruction. See commentary on AnchorClient::retiredBakedModels for rationale.
//
// History:
//   N=4 (original) — covers one bake per transition + LUS double-buffer.
//     67 ms at 60 fps / 200 ms at 20 fps.
//   N=30 (#171 step 2, 2026-04-22) — basement-transition crash
//     reproduced on log 69 and log 74. Post-teleport scene reload into
//     Deku Tree basement packs FIVE skeleton bakes into ~250 ms (4x local
//     Malon-Heroine adult+child tunic bakes + 1x DummyPlayer 3dsLink),
//     and the 5th swap's retire slot overwrote the 1st's buffer while
//     the GPU / an in-flight ENEMY_UPDATE iteration still held a raw
//     pointer. Access-violation on freed skelAnime.
//     30 frames = 500 ms at 60 fps / 1.5 s at 20 fps — comfortably
//     covers the observed 250 ms worst case. Memory cost is ~10 KB per
//     additional slot (already-retired model sits idle until the next
//     bake evicts it), trivial.
static constexpr int kRetireFrames = 30;
