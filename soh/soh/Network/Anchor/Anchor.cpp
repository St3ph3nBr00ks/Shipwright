#include "Anchor.h"
#include "EnemyNetId.h"        // #243.7.2 — explicit (was transitive via Anchor.h)
#include "ItemDropNetId.h"     // #243.7.2 — explicit (was transitive via Anchor.h)
#include "AIDirector/Director.h"
#include "Common/AnchorNavExt.h"  // B.4 — per-navigator nav state extension
#include "Common/CutsceneCatchup.h"  // full type for out-of-line ~Anchor()
#include "Common/PacketSchemas.h"
#include "Common/TitlePeerFormation.h"  // SpawnTitlePeerLink formation math
#include "WorldStateSync/WorldStateSync.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h"  // EnHorse + ENHORSE_ANIM_* (Phase 3 horse animation)
#include "objects/object_horse/object_horse.h"        // gEponaGallopingAnim (Phase 3)
extern PlayState* gPlayState;
// Phase 2 horse integration — primitive lives in HorseSync/HorseSpawnHelpers.cpp.
// No header exists yet for the HorseSync module so we forward-declare here
// (project pattern — same shape as the existing extern PlayState* above).
Actor* Anchor_SpawnHorseForTitleScreen(PlayState* play, uint32_t netId,
                                        uint32_t ownerClientId, int16_t variantParams,
                                        Vec3f pos, int16_t rotY);
}

// MARK: - AnchorClient helpers

void AnchorClient::RetireBakedModel() {
    if (bakedModel == nullptr) {
        return;  // nothing to retire
    }
    // KB-19 (#176): append rather than overwrite. The previous single-slot
    // assumption ("the prior retiree's slot has been occupied for ≥400 ms,
    // safe to destroy") was wrong for vanilla revert bakes (~10 ms each)
    // surfaced by the P2 age-switch trigger. Multiple concurrent retirees
    // coexist in the vector; each is freed only after its own counter ages
    // out. OnGameFrameUpdate ticks the vector each frame.
    retiredBakedModels.push_back({ std::move(bakedModel), kRetireFrames });
}

// MARK: - Bandwidth profiler (#62)

void Anchor::KillNetworkActorSilently(Actor* actor) {
    if (actor == nullptr || actor->update == nullptr) return;
    isKillingNetworkActor = true;
    Actor_Kill(actor);
    isKillingNetworkActor = false;
}

void Anchor::RecordProfileSample(const nlohmann::json& payload, bool tx) {
    if (CVarGetInteger("gEnhancements.AnchorProfiler", 0) == 0) return;
    if (!payload.contains("type")) return;
    std::string type = payload["type"].get<std::string>();
    size_t bytes = payload.dump().size() + 1; // +1 for null-byte delimiter the relay expects

    std::lock_guard<std::mutex> lock(profileMutex);
    auto& bucket = (tx ? profileTx : profileRx)[type];
    bucket.count += 1;
    bucket.bytes += bytes;

    // Phase 5 Step 1 (#62 — bandwidth audit). For ENEMY_STATE outbound
    // packets only, also bucket by actorId so the per-actor-type
    // contributor breakdown shows up in the flush. The OoT actorId
    // field is included in the payload by EnemyStateSync's
    // BuildBaseEnemyStatePayload (see also Plans/decoupling_gap_audit
    // §A.7). netId would be too granular (one bucket per actor
    // instance); actorId is the right "what is the noisy class of
    // actor" axis.
    if (tx && type == ENEMY_STATE && payload.contains("actorId")) {
        const uint16_t actorId =
            (uint16_t)payload.value("actorId", (int)0);
        auto& actorBucket = enemyStateTxByActorId[actorId];
        actorBucket.count += 1;
        actorBucket.bytes += bytes;
    }
}

void Anchor::FlushProfileIfDue() {
    if (CVarGetInteger("gEnhancements.AnchorProfiler", 0) == 0) return;
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(profileMutex);
    if (profileWindowStartMs == 0) { profileWindowStartMs = nowMs; return; }
    uint64_t elapsed = nowMs - profileWindowStartMs;
    if (elapsed < kProfilerWindowMs) return;

    std::unordered_set<std::string> types;
    for (auto& [t, _] : profileTx) types.insert(t);
    for (auto& [t, _] : profileRx) types.insert(t);
    std::vector<std::string> sorted(types.begin(), types.end());
    std::sort(sorted.begin(), sorted.end(), [this](const std::string& a, const std::string& b) {
        return (profileTx[a].bytes + profileRx[a].bytes) >
               (profileTx[b].bytes + profileRx[b].bytes);
    });

    SPDLOG_INFO("[AnchorProfile] window={}ms   type                   tx_pps   tx_Bps   rx_pps   rx_Bps",
                elapsed);
    for (const auto& t : sorted) {
        auto& tx = profileTx[t];
        auto& rx = profileRx[t];
        SPDLOG_INFO("[AnchorProfile] {:<22} {:>8.1f} {:>8.0f} {:>8.1f} {:>8.0f}",
                    t,
                    tx.count * 1000.0 / elapsed, tx.bytes * 1000.0 / elapsed,
                    rx.count * 1000.0 / elapsed, rx.bytes * 1000.0 / elapsed);
    }

    // Phase 5 Step 1 (#62 — bandwidth audit). Per-actorId breakdown of
    // outbound ENEMY_STATE. Sorted by tx_Bps desc; capped at top-N so
    // a busy scene with many actor types doesn't drown the log. Empty
    // → skip the section entirely (single-player or no ENEMY_STATE
    // outbound this window).
    if (!enemyStateTxByActorId.empty()) {
        std::vector<std::pair<uint16_t, ProfileBucket>> actorRanking;
        actorRanking.reserve(enemyStateTxByActorId.size());
        for (const auto& kv : enemyStateTxByActorId) {
            actorRanking.push_back(kv);
        }
        std::sort(actorRanking.begin(), actorRanking.end(),
                  [](const std::pair<uint16_t, ProfileBucket>& a,
                     const std::pair<uint16_t, ProfileBucket>& b) {
                      return a.second.bytes > b.second.bytes;
                  });

        constexpr size_t kBandwidthAuditTopN = 12;
        const size_t reportCount =
            std::min(kBandwidthAuditTopN, actorRanking.size());
        SPDLOG_INFO("[BandwidthAudit] ENEMY_STATE top-{} by tx bytes (window={}ms) "
                    "— actorId    tx_pps    tx_Bps  avg_pkt_bytes",
                    reportCount, elapsed);
        for (size_t i = 0; i < reportCount; ++i) {
            const auto& [actorId, bucket] = actorRanking[i];
            const double pps    = bucket.count * 1000.0 / elapsed;
            const double Bps    = bucket.bytes * 1000.0 / elapsed;
            const double avgPkt = (bucket.count > 0)
                                      ? (double)bucket.bytes / (double)bucket.count
                                      : 0.0;
            SPDLOG_INFO("[BandwidthAudit] actorId=0x{:04X}  {:>8.1f}  {:>8.0f}  {:>14.1f}",
                        actorId, pps, Bps, avgPkt);
        }
    }

    profileTx.clear();
    profileRx.clear();
    enemyStateTxByActorId.clear();
    profileWindowStartMs = nowMs;
}

// MARK: - Overrides

void Anchor::Enable() {
    // Drain the disable-time graveyard from any prior Disable(). Any
    // UI-driven re-enable takes far longer than kRetireFrames frames, so
    // every in-flight Gfx command that referenced these allocations has
    // long since been consumed by the renderer and they are safe to free.
    postDisableBakedModels.clear();
    postDisableCustomSkeletons.clear();

    Network::Enable(CVarGetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org"),
                    CVarGetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383));
    ownClientId = CVarGetInteger(CVAR_REMOTE_ANCHOR("LastClientId"), 0);
    roomState.ownerClientId = 0;
}

void Anchor::Disable() {
    Network::Disable();

    // KB-15 / issue #110 — drain BakedPlayerModel and customSkeleton off
    // every client into the disable-time graveyard before clients.clear()
    // destroys them. The previous frame's draw queued Gfx commands that
    // still reference these allocations; freeing them synchronously crashes
    // the renderer on the next frame's Gfx walk (see log 119).
    for (auto& [clientId, client] : clients) {
        if (client.bakedModel != nullptr) {
            postDisableBakedModels.push_back(std::move(client.bakedModel));
        }
        // KB-19 (#176): drain every entry in the retire vector.
        for (auto& entry : client.retiredBakedModels) {
            if (entry.model != nullptr) {
                postDisableBakedModels.push_back(std::move(entry.model));
            }
        }
        client.retiredBakedModels.clear();
        if (client.customSkeleton != nullptr) {
            postDisableCustomSkeletons.push_back(std::move(client.customSkeleton));
        }
    }

    clients.clear();
    RefreshClientActors();

    // Pillar C v1 — drop replicated world state on full Disable. Reset is
    // safe under Disable because we'd otherwise carry stale state into a
    // fresh Enable session pointed at a different room.
    WorldStateSync::Reset();

    // Pillar C2 Phase 2 — drop host-only enemy bookkeeping (pendingKills,
    // sceneDeaths, defeatBroadcasts, damagers). Same rationale as
    // WorldStateSync::Reset() above; pre-extraction these were per-Anchor
    // fields cleared by the implicit `clients.clear()` adjacent block, but
    // the module's static instance survives Disable so we must explicitly
    // wipe.
    EnemyStateSync::HostBookkeeping::Instance().Reset();

    // KB-18 (#177) Option 4 — drop host-authoritative netId snapshot cache.
    // A stale snapshot from a prior Anchor session pointed at a different
    // room/team would mis-reassign netIds on the next scene-spawn.
    sceneActorNetIdSnapshots.clear();
    pendingSceneActorNetIdsBroadcast = false;

    // Heartbeat (#194 follow-up) — drop per-peer liveness state.
    // A reconnect (or fresh Enable into a different room) shouldn't
    // inherit the previous session's "client X frozen" flagging.
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        lastHeartbeatRxByClientId.clear();
        lastHeartbeatGameFrameByClientId.clear();
        lastHeartbeatGameFrameAtByClientId.clear();
        heartbeatFrozenFlaggedByClientId.clear();
        heartbeatGameFrozenFlaggedByClientId.clear();
    }
    lastHeartbeatTx = std::chrono::steady_clock::time_point{};
}

void Anchor::OnConnected() {
    SendPacket_Handshake();
    RegisterHooks();

    // Phase 5 #60 — drop any stale "last sent" snapshots so the first
    // post-reconnect send for every enemy goes out on the next frame
    // instead of waiting for the keepalive timer. Separately, the send
    // path also guards cache writes on isConnected — so if we had been
    // sending into a disconnected socket, nothing accumulated anyway.
    // Doing both ("belt-and-braces") costs one hashmap clear.
    Anchor_ClearEnemyUpdateCache();

    if (IsSaveLoaded()) {
        // Reconnect orphan-actor recovery (log 116 bug — P2 disconnected
        // during the 8-second window between connect and scene init, which
        // unregistered OnActorSpawn. Setup actors then spawned with no
        // EnemyNetId extension; ENEMY_UPDATE / ENEMY_DEFEATED couldn't
        // find them. Walk all syncable categories now and backfill any
        // actor missing the extension.
        BackfillEnemyNetIds();

        SendPacket_RequestTeamState();
        // Bug A (log 69) — on reconnect the host does NOT re-broadcast
        // ENEMY_DEFEATED packets that were sent while we were offline,
        // and RequestTeamState only covers time-of-day + item flags.
        // Bump sceneSpawnEpoch and re-send UpdateClientState; the host's
        // HandlePacket_UpdateClientState detects the epoch change and
        // replays deadEnemiesByScene for our current scene. Harmless on
        // first connect (replay is idempotent — already-dead enemies
        // just get Actor_Kill'd again).
        sceneSpawnEpoch++;
        SendPacket_UpdateClientState();
        SPDLOG_INFO("[Anchor] Reconnect epoch bump → {} (triggers host dead-enemy replay)",
                    sceneSpawnEpoch);

        // Pillar C v1 — request team-mates' WorldState snapshot. They reply
        // via WORLD_STATE_SNAPSHOT; merge is idempotent so first vs.
        // reconnect-after-state-buildup paths converge.
        WorldStateSync::SendRequestWorldState();

        // Late-join cutscene catchup — arm the request gate so that
        // ~1 s from now (once we've received PLAYER_UPDATE from peers
        // and populated `clients[].csCtxState`, plus any live FRAME_SYNC
        // has hydrated cutsceneStartActive), TickCutsceneCatchup fires
        // DetectAndRequestCutsceneCatchup. If a peer is mid-cutscene
        // in our scene, we send a CUTSCENE_CATCHUP_REQUEST and receive
        // the delta.
        //
        // Without this, DetectAndRequestCutsceneCatchup only fires on
        // OnSceneSpawnActors — peers who connect while already IN the
        // cutscene scene (dev-tool warp / return-to-save state) would
        // never trigger detection until they leave and re-enter.
        //
        // See Claude/Analysis/lost_woods_savecontext_no_handler_2026-07-13.md
        // "players that come online after the cutscene started" ask.
        catchupRequestGateArmedAt = std::chrono::steady_clock::now();
        SPDLOG_INFO("[Anchor] Late-join cutscene catchup gate armed on connect");
    }
}

void Anchor::OnDisconnected() {
    RegisterHooks();
}

void Anchor::ProcessOutgoingPackets() {
    // Copy all queued packets while holding the lock, then send them after releasing
    std::queue<nlohmann::json> packetsToSend;
    {
        std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
        packetsToSend.swap(outgoingPacketQueue);
    }

    // Send packets without holding the lock
    while (!packetsToSend.empty()) {
        nlohmann::json payload = packetsToSend.front();
        packetsToSend.pop();

        RecordProfileSample(payload, /*tx=*/true);
        if (!payload.contains("quiet")) {
            SPDLOG_DEBUG("[Anchor] Sending payload:\n{}", payload.dump());
        }
        Network::SendJsonToRemote(payload);
    }
    FlushProfileIfDue();

    // Heartbeat (#194 follow-up) — runs on the network thread (this
    // function is invoked from Network::ReceiveFromServer's loop). No-op
    // until kHeartbeatTxIntervalSec has elapsed since the last send.
    // Survives game-thread freezes because this thread keeps ticking
    // even when OnGameFrameUpdate is starved.
    TickHeartbeat();
}

void Anchor::SendJsonToRemote(nlohmann::json payload) {
    if (!isConnected) {
        return;
    }

    payload["clientId"] = ownClientId;

    // Pillar F — auto-inject schema for all packet types. Per-packet
    // schema lives in Common/PacketSchemas.h and is bumped when fields
    // are added. Senders may override before this point if needed.
    if (!payload.contains("schema") && payload.contains("type")) {
        payload["schema"] = PacketSchemas::GetPacketSchema(payload["type"].get<std::string>());
    }

    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Queuing payload:\n{}", payload.dump());
    }

    if (payload["type"] == HANDSHAKE) {
        Network::SendJsonToRemote(payload);
        return;
    }

    // Queue the packet to be sent on the network thread
    std::lock_guard<std::mutex> lock(outgoingPacketQueueMutex);
    outgoingPacketQueue.push(payload);
}

void Anchor::BroadcastJsonToScenePeers(nlohmann::json& payload) {
    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

// B.3 — Packet handler registry. Maps wire-string packet type to the
// matching Anchor::HandlePacket_* member function. Lookup happens in
// OnIncomingJson's per-packet dispatch (see usage below).
//
// Function-local static is initialised on first call (thread-safe per
// C++11 § [stmt.dcl]/4 magic-statics). The map is `const` after
// construction; no mutation after initialization. Storage is one
// 51-entry std::unordered_map with string keys (the wire-format
// constants from `Common/PacketSchemas.h`).
//
// Out-of-line destructor — required so the implicit dtor of
// cutsceneCatchupLedger (unordered_map<string,
// unique_ptr<CutsceneCatchupEntry>>) is instantiated in a TU where
// CutsceneCatchupEntry is complete. Declared in Anchor.h public
// section; defined here so `#include "Common/CutsceneCatchup.h"`
// above provides the full type.
Anchor::~Anchor() = default;

// Per Plans/decoupling_gap_audit_2026-05-16.md §13.5 (filed as #198):
// the registry's lookup-miss branch is the natural home for the
// terminal "unknown packet type" warning. The previous 50-branch
// if/else-if chain had no fallthrough.
const std::unordered_map<std::string, void (Anchor::*)(nlohmann::json)>&
Anchor::GetPacketHandlerRegistry() {
    using H = void (Anchor::*)(nlohmann::json);
    static const std::unordered_map<std::string, H> kRegistry = {
        { ALL_CLIENT_STATE,        &Anchor::HandlePacket_AllClientState        },
        { ENEMY_STATE,             &Anchor::HandlePacket_EnemyState            },
        { SCENE_ACTOR_NETIDS,      &Anchor::HandlePacket_SceneActorNetIds      },
        { DAMAGE_ENEMY,            &Anchor::HandlePacket_DamageEnemy           },
        { DAMAGE_PLAYER,           &Anchor::HandlePacket_DamagePlayer          },
        { SHIELD_BOUNCE_PLAYER,    &Anchor::HandlePacket_ShieldBouncePlayer    },
        { ENEMY_HIT_PLAYER,        &Anchor::HandlePacket_EnemyHitPlayer        },
        { PROJECTILE_HIT_ENEMY,    &Anchor::HandlePacket_ProjectileHitEnemy    },
        { TALK_REQUEST,            &Anchor::HandlePacket_TalkRequest           },
        { DIALOG_END,              &Anchor::HandlePacket_DialogEnd             },
        { BOSS_GOMA_LOOKED_AT,     &Anchor::HandlePacket_BossGomaLookedAt      },
        { MIDO_POST_DEKU_LEAVE,    &Anchor::HandlePacket_MidoPostDekuLeave     },
        { TELEPORT_EFFECT,         &Anchor::HandlePacket_TeleportEffect        },
        // DIALOG_CHOICE_* (2026-07-09) — MP choice-textbox vote system.
        { DIALOG_CHOICE_VOTE,        &Anchor::HandlePacket_DialogChoiceVote        },
        { DIALOG_CHOICE_VOTE_STATE,  &Anchor::HandlePacket_DialogChoiceVoteState   },
        { DIALOG_CHOICE_APPLIED,     &Anchor::HandlePacket_DialogChoiceApplied     },
        { TALON_CASTLE_STATE,      &Anchor::HandlePacket_TalonCastleState      },
        { HYRULE_CASTLE_GATE_OPEN, &Anchor::HandlePacket_HyruleCastleGateOpen  },
        { KAKARIKO_GATE_OPEN,      &Anchor::HandlePacket_KakarikoGateOpen      },
        { CUTSCENE_START,          &Anchor::HandlePacket_CutsceneStart         },
        { CUTSCENE_END,            &Anchor::HandlePacket_CutsceneEnd           },
        { CUTSCENE_TEXT_VOTE_STATE, &Anchor::HandlePacket_CutsceneTextVoteState },
        // Late-join catchup — Plans/cutscene_late_join_plan.md.
        { CUTSCENE_FRAME_SYNC,       &Anchor::HandlePacket_CutsceneFrameSync       },
        { CUTSCENE_CATCHUP_REQUEST,  &Anchor::HandlePacket_CutsceneCatchupRequest  },
        { CUTSCENE_CATCHUP_RESPONSE, &Anchor::HandlePacket_CutsceneCatchupResponse },
        { ITEM_DROP_SYNC,          &Anchor::HandlePacket_ItemDropSync          },
        { ITEM_COLLECTED,          &Anchor::HandlePacket_ItemCollected         },
        { ITEM_DROP_SNAPSHOT,      &Anchor::HandlePacket_ItemDropSnapshot      },
        { ITEM_PICKUP_REQUEST,     &Anchor::HandlePacket_ItemPickupRequest     },
        { ENV_ACTOR_DROP,          &Anchor::HandlePacket_EnvActorDrop          },
        { ENV_ACTOR_DESTROY,       &Anchor::HandlePacket_EnvActorDestroy       },
        { MODAL_OFFER_CLAIMED,     &Anchor::HandlePacket_ModalOfferClaimed     },
        { CUTSCENE_TEXT_ADVANCE,   &Anchor::HandlePacket_CutsceneTextAdvance   },
        { CUTSCENE_TEXT_ADVANCED,  &Anchor::HandlePacket_CutsceneTextAdvanced  },
        { FOLLOWER_NPC_SPAWN,      &Anchor::HandlePacket_FollowerNpcSpawn      },
        { FOLLOWER_NPC_STATE,      &Anchor::HandlePacket_FollowerNpcState      },
        { FOLLOWER_NPC_DESPAWN,    &Anchor::HandlePacket_FollowerNpcDespawn    },
        { HORSE_SPAWN,             &Anchor::HandlePacket_HorseSpawn            },
        { HORSE_STATE,             &Anchor::HandlePacket_HorseState            },
        { HORSE_DESPAWN,           &Anchor::HandlePacket_HorseDespawn          },
        { NAV_TEST_DIRECTIVE,      &Anchor::HandlePacket_NavTestDirective      },
        { DISABLE_ANCHOR,          &Anchor::HandlePacket_DisableAnchor         },
        { ENTRANCE_DISCOVERED,     &Anchor::HandlePacket_EntranceDiscovered    },
        { GAME_COMPLETE,           &Anchor::HandlePacket_GameComplete          },
        { GIVE_ITEM,               &Anchor::HandlePacket_GiveItem              },
        { OCARINA_SFX,             &Anchor::HandlePacket_OcarinaSfx            },
        { PLAYER_UPDATE,           &Anchor::HandlePacket_PlayerUpdate          },
        { PLAYER_SFX,              &Anchor::HandlePacket_PlayerSfx             },
        { UPDATE_TEAM_STATE,       &Anchor::HandlePacket_UpdateTeamState       },
        { REQUEST_TEAM_STATE,      &Anchor::HandlePacket_RequestTeamState      },
        { REQUEST_TELEPORT,        &Anchor::HandlePacket_RequestTeleport       },
        { SERVER_MESSAGE,          &Anchor::HandlePacket_ServerMessage         },
        { SET_CHECK_STATUS,        &Anchor::HandlePacket_SetCheckStatus        },
        { SET_FLAG,                &Anchor::HandlePacket_SetFlag               },
        { TELEPORT_TO,             &Anchor::HandlePacket_TeleportTo            },
        { TIME_SYNC,               &Anchor::HandlePacket_TimeSync              },
        { UNSET_FLAG,              &Anchor::HandlePacket_UnsetFlag             },
        { UPDATE_BEANS_COUNT,      &Anchor::HandlePacket_UpdateBeansCount      },
        { UPDATE_CLIENT_STATE,     &Anchor::HandlePacket_UpdateClientState     },
        { UPDATE_ROOM_STATE,       &Anchor::HandlePacket_UpdateRoomState       },
        { UPDATE_DUNGEON_ITEMS,    &Anchor::HandlePacket_UpdateDungeonItems    },
        { SCENE_TRANSITION_HANDOFF,&Anchor::HandlePacket_SceneTransitionHandoff},
        { BOSS_EXIT_TEAM_WARP,     &Anchor::HandlePacket_BossExitTeamWarp      },
        { WORLD_FLAG_SET,          &Anchor::HandlePacket_WorldFlagSet          },
        { WORLD_FLAG_UNSET,        &Anchor::HandlePacket_WorldFlagUnset        },
        { WORLD_STATE_REQUEST,     &Anchor::HandlePacket_WorldStateRequest     },
        { WORLD_STATE_SNAPSHOT,    &Anchor::HandlePacket_WorldStateSnapshot    },
        { SCENE_DEATHS_CLEARED,    &Anchor::HandlePacket_SceneDeathsCleared    },
        { DIRECTOR_STATE_SYNC,     &Anchor::HandlePacket_DirectorStateSync     },
    };
    return kRegistry;
}

void Anchor::OnIncomingJson(nlohmann::json payload) {
    // If it doesn't contain a type, it's not a valid payload
    if (!payload.contains("type")) {
        return;
    }

    RecordProfileSample(payload, /*tx=*/false);

    // If it's not a quiet payload, log it
    if (!payload.contains("quiet")) {
        SPDLOG_DEBUG("[Anchor] Received payload:\n{}", payload.dump());
    }

    std::string packetType = payload["type"].get<std::string>();

    // Heartbeat fast-path (#194 follow-up) — handled directly on the
    // network thread, NOT queued for the game thread. This way a peer's
    // own game-thread freeze doesn't delay updating its view of OTHER
    // peers' liveness; lastHeartbeatRx maps stay current regardless of
    // whether this client's game tick is running.
    if (packetType == HEARTBEAT) {
        HandlePacket_Heartbeat(payload);
        return;
    }

    // Pillar F — schema-driven compatibility. Packets carrying a "schema"
    // field opt into the schema layer and DO NOT need the clientVersion
    // check (schema versioning handles cross-version compatibility via
    // additive-only fields + maxSchema clamping).
    //
    // The hand-maintained allowlist (ALL_CLIENT_STATE, UPDATE_CLIENT_STATE,
    // PLAYER_UPDATE, ENEMY_UPDATE/DEFEATED/SPAWN/RESPAWN, DAMAGE_ENEMY,
    // ENEMY_HIT_PLAYER) was retired 2026-04-25 — those packets all carry
    // the schema field now via SendJsonToRemote auto-injection.
    //
    // Legacy fallback: pre-Pillar-F peers (without the schema field)
    // continue to be filtered by the clientVersion mismatch check.
    if (!payload.contains("schema")) {
        if (payload.contains("clientId")) {
            uint32_t clientId = payload["clientId"].get<uint32_t>();
            if (clients.contains(clientId) && clients[clientId].clientVersion != clientVersion) {
                return;
            }
        }
    }

    // Queue all packets to be processed on the game thread
    std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
    incomingPacketQueue.push(payload);
}

void Anchor::ProcessIncomingPacketQueue() {
    // Copy all queued packets while holding the lock, then process them after releasing
    std::queue<nlohmann::json> packetsToProcess;
    {
        std::lock_guard<std::mutex> lock(incomingPacketQueueMutex);
        packetsToProcess.swap(incomingPacketQueue);
    }

    // Drain into a vector so we can two-pass for ENEMY_STATE coalescing.
    // When P2's TCP receive thread stalls (process scheduling, audio
    // pipeline contention, network blip), the relay buffers per-frame
    // ENEMY_STATE packets and dumps them in a burst on recovery —
    // observed up to 153 pps recovery storm in log 177's window
    // 16:53:26-36 after a 30-second stall. Each phase=Alive
    // phaseChanged=false ENEMY_STATE is a complete snapshot of an
    // actor's state at send time, so applying older ones in sequence
    // is wasteful: the receiver ends up at the same place as if it had
    // only applied the latest. Coalesce by netId, keep only latest.
    //
    // What is NOT coalesced (preserve event semantics + ordering):
    //   - phaseChanged=true (spawns / defeats / respawns)
    //   - all non-ENEMY_STATE packet types
    std::vector<nlohmann::json> packets;
    while (!packetsToProcess.empty()) {
        packets.push_back(std::move(packetsToProcess.front()));
        packetsToProcess.pop();
    }

    // First pass (back-to-front): record index of LATEST coalescible
    // ENEMY_STATE per netId. Walking backwards means the first hit per
    // netId is the latest occurrence in the batch.
    std::unordered_map<uint32_t, size_t> latestSteadyByNetId;
    for (size_t i = packets.size(); i-- > 0;) {
        const auto& p = packets[i];
        if (!p.contains("type")) continue;
        if (p.value("type", std::string("")) != ENEMY_STATE) continue;
        if (p.value("phase", std::string("")) != "Alive") continue;
        if (p.value("phaseChanged", false)) continue;
        uint32_t netId = p.value("netId", (uint32_t)0);
        if (netId == 0) continue;
        latestSteadyByNetId.emplace(netId, i);  // first emplace per netId wins
    }

    // Second pass (front-to-back): process each packet, skipping stale
    // coalescible ENEMY_STATE entries. All other types and phaseChanged
    // events flow through in original order.
    int coalescedDropped = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const nlohmann::json& checkPayload = packets[i];
        if (checkPayload.contains("type") &&
            checkPayload.value("type", std::string("")) == ENEMY_STATE &&
            checkPayload.value("phase", std::string("")) == "Alive" &&
            !checkPayload.value("phaseChanged", false)) {
            uint32_t netId = checkPayload.value("netId", (uint32_t)0);
            if (netId != 0) {
                auto it = latestSteadyByNetId.find(netId);
                if (it != latestSteadyByNetId.end() && it->second != i) {
                    coalescedDropped++;
                    continue;  // a newer snapshot exists later in this batch
                }
            }
        }

        nlohmann::json payload = packets[i];

        // #197 — safe packet-type extraction. The previous form
        // `payload["type"].get<std::string>()` threw nlohmann::json::
        // out_of_range when the JSON object was missing "type" (or
        // type_error when "type" was non-string), and the throw was
        // OUTSIDE the try-catch below. A malformed packet from a peer
        // — schema drift, buggy client, or hostile probe — escaped
        // past the safety net and crashed the listener thread. Use
        // `value(...)` for default-on-miss + explicit empty-string
        // bail; the catch below still covers any remaining surprises.
        std::string packetType = payload.value("type", std::string(""));
        if (packetType.empty()) {
            SPDLOG_WARN("[Anchor] Incoming packet missing or non-string 'type' field; dropping");
            continue;
        }

        isProcessingIncomingPacket = true;

        // B.3 — packet-handler registry (was a 50-branch if/else-if
        // chain). The string→member-fn-pointer map is initialised once
        // on first call (function-local static = thread-safe init in
        // C++11+, never re-entered after construction). Lookup is O(1)
        // average vs. O(N) for the previous chain. The terminal #198
        // unknown-type warning is built into the lookup-miss branch.
        //
        // Add a new packet type:
        //   1. Add `Handle Packet_Foo` declaration in Anchor.h.
        //   2. Implement in `Packets/.../Foo.cpp`.
        //   3. Add ONE line to the registry below: `{FOO, &Anchor::HandlePacket_Foo},`.
        // No edit to the dispatch site itself — matches the AIDirector
        // descriptor-pattern + per-X registry convention used elsewhere.
        try {
            const auto& registry = GetPacketHandlerRegistry();
            auto it = registry.find(packetType);
            if (it != registry.end()) {
                (this->*(it->second))(payload);
            } else {
                // #198 — unknown packet types are rare in normal operation;
                // a warning fires for schema drift / client-version skew /
                // hostile-peer probes. Without this, unknown types silently
                // dropped with zero diagnostic.
                SPDLOG_WARN("[Anchor] Unknown packet type: '{}' (payload size={})",
                            packetType, payload.dump().size());
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[Anchor] Exception while processing incoming packet {}", e.what());
            SPDLOG_ERROR("[Anchor] Packet: {}", payload.dump());
        }

        isProcessingIncomingPacket = false;
    }

    // Coalesce diagnostic. Threshold of 5 keeps normal-operation noise
    // out of the log (a 1-2 packet coalesce per tick is routine). A
    // catch-up burst after a stall produces dozens or hundreds at once.
    if (coalescedDropped >= 5) {
        SPDLOG_INFO("[Anchor] Coalesced {} stale ENEMY_STATE packets ({} netIds; total batch {} packets)",
                    coalescedDropped, (int)latestSteadyByNetId.size(), (int)packets.size());
    }
}

// MARK: - Misc/Helpers

// Kills all existing anchor actors and respawns them with the new client data

struct DummyPlayerClientId {
    uint32_t clientId = 0;
};
static ObjectExtension::Register<DummyPlayerClientId> DummyPlayerClientIdRegister;

// Title-screen peer marker (Phase 1 — Plans/title_screen_peer_actors.md).
// Tagged at SpawnTitlePeerLink time on the spawned DummyPlayer so
// DummyPlayer_Update can branch into title-mode position math instead of
// the gameplay-time PLAYER_UPDATE-driven path. Phase 2 will also drive
// horse-mount linkage via the formation index.
struct DummyPlayerTitleMode {
    bool    isTitleMode      = false;
    uint8_t formationIndex   = 0;
};
static ObjectExtension::Register<DummyPlayerTitleMode> DummyPlayerTitleModeRegister;
ObjectExtension::Register<EnemyNetId> EnemyNetIdRegister;
ObjectExtension::Register<ItemDropNetId> ItemDropNetIdRegister;
// B.4 — per-navigator nav-substrate state lifted off EnemyNetId.
// Same actor keys; the two extensions coexist on synced actors.
ObjectExtension::Register<AnchorNavExt> AnchorNavExtRegister;

uint32_t Anchor::GetDummyPlayerClientId(const Actor* actor) {
    const DummyPlayerClientId* clientId = ObjectExtension::GetInstance().Get<DummyPlayerClientId>(actor);
    return clientId != nullptr ? clientId->clientId : 0;
}

void Anchor::SetDummyPlayerClientId(const Actor* actor, uint32_t clientId) {
    ObjectExtension::GetInstance().Set<DummyPlayerClientId>(actor, DummyPlayerClientId{ clientId });
}

// ============================================================================
// Title-screen peer actors — Phase 1
// Plans/title_screen_peer_actors.md.
// Phase 1 ships peers on foot behind the local Link during the Hyrule Field
// title gallop. Phase 2 wires the horse-sync agent's spawn primitive to
// mount them on tinted Eponas. Empty-team / cap-3 / alphabetical-selection
// policy locked in the plan doc (Q1/Q2/Q3 recommendations).
// ============================================================================

void Anchor::SetDummyPlayerTitleMode(const Actor* actor, uint8_t formationIndex) {
    ObjectExtension::GetInstance().Set<DummyPlayerTitleMode>(
        actor, DummyPlayerTitleMode{ /*isTitleMode=*/ true, formationIndex });
}

bool Anchor::IsDummyPlayerTitleMode(const Actor* actor) const {
    const DummyPlayerTitleMode* ext =
        ObjectExtension::GetInstance().Get<DummyPlayerTitleMode>(actor);
    return ext != nullptr && ext->isTitleMode;
}

uint8_t Anchor::GetTitlePeerFormationIndex(const Actor* actor) const {
    const DummyPlayerTitleMode* ext =
        ObjectExtension::GetInstance().Get<DummyPlayerTitleMode>(actor);
    return ext != nullptr ? ext->formationIndex : 0;
}

void Anchor::SpawnTitlePeerLink(uint32_t clientId, uint8_t formationIndex) {
    if (gPlayState == nullptr) return;
    auto it = clients.find(clientId);
    if (it == clients.end() || !it->second.online || it->second.self) return;

    // Initial spawn position: derived from local Link's current pos+rot.
    // DummyPlayer_Update will recompute this every frame from the same source.
    Player* localLink = GET_PLAYER(gPlayState);
    Vec3f spawnPos = { 0.0f, 0.0f, 0.0f };
    s16 spawnRotY = 0;
    if (localLink != nullptr) {
        // Formation derivation lives in Common/TitlePeerFormation.h —
        // single source of truth shared with DummyPlayer_Update's
        // title-mode per-tick path. To tune the formation in v3+
        // (stagger / yaw divergence / animation phase), modify only
        // MakeTitlePeerFormation in that header; this call site needs
        // no changes.
        const AnchorTitlePeer::TitlePeerSlot slot =
            AnchorTitlePeer::ComputeTitlePeerSlot(
                localLink->actor.world.pos,
                localLink->actor.world.rot.y,
                clientId, formationIndex);
        spawnPos  = slot.pos;
        spawnRotY = slot.rotY;
    }

    // Reuse the existing spawningDummyPlayerForClientId pathway so
    // ShouldActorInit / DummyPlayer_Init / cosmetic bake all run identically
    // to the gameplay-time path. We tag the actor as title-mode AFTER
    // Actor_Spawn returns so DummyPlayer_Update sees the flag on its first
    // tick (next frame).
    spawningDummyPlayerForClientId = clientId;
    Actor* dummy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER,
                               spawnPos.x, spawnPos.y, spawnPos.z,
                               0, spawnRotY, 0, 0);
    spawningDummyPlayerForClientId = 0;

    if (dummy != nullptr) {
        SetDummyPlayerTitleMode(dummy, formationIndex);
        // Title-screen peers don't show nametags — DummyPlayer_Init registered
        // one during Actor_Spawn (mirroring the gameplay-time path), so we
        // remove it here. Rationale (per user 2026-06-08): title screen
        // is a cosmetic preview, and with both peers often sharing a
        // nickname the tag was ambiguous and visually noisy. Bypassing
        // the nametag at Init time would require a parallel "spawning
        // title peer" flag the Init hook checks; this post-spawn remove
        // achieves the same outcome with less surface area.
        NameTag_RemoveAllForActor(dummy);
        mTitlePeerActors[clientId] = dummy;
        it->second.player = (Player*)dummy;
        SPDLOG_INFO("[TitlePeer] Spawned clientId={} formationIdx={} pos=({:.1f},{:.1f},{:.1f}) rotY={}",
                    clientId, (int)formationIndex,
                    spawnPos.x, spawnPos.y, spawnPos.z, (int)spawnRotY);

        // Phase 2 horse integration. Spawn paired with the rider so the
        // Link+Horse pair has a consistent lifetime. Gated on
        // IsHorseSyncEnabled CVar — when off, Phase 1 behavior preserved
        // (peers on foot, no horse). The first-frame rider/horse positions
        // overlap; the per-tick title-mode branch in DummyPlayer_Update
        // converges them to (rider = formation slot, horse = slot - riderPos)
        // on the second frame via the inverse-position math + reconciliation.
        if (Anchor::IsHorseSyncEnabled()) {
            const uint32_t horseNetId =
                Anchor::MakeHorseNetId(clientId, SCENE_HYRULE_FIELD, 0);
            Actor* horse = Anchor_SpawnHorseForTitleScreen(
                gPlayState, horseNetId, clientId, /*variantParams=*/0,
                spawnPos, spawnRotY);
            if (horse != nullptr) {
                // Phase 3 — switch horse to looping gallop animation
                // post-spawn. Vanilla EnHorse_Init sets up an IDLE
                // animation; we want gallop (matches vanilla local
                // Link's Epona during the title cutscene). EnHorse_Idle
                // action calls SkelAnime_Update each frame
                // (z_en_horse.c:1842), so a LOOP-mode animation set
                // here keeps ticking forever — the
                //   SkelAnime_Update returns true → transition to idle
                // path (z_en_horse.c:1842-1850) never fires for looping
                // animations. Combined with the horse-sync gate skip
                // (HorseHooks.cpp), the horse stays in IDLE action but
                // its skel data is the GALLOP animation, so legs cycle.
                EnHorse* horseAsHorse = (EnHorse*)horse;
                Animation_PlayLoop(&horseAsHorse->skin.skelAnime,
                                    (AnimationHeader*)&gEponaGallopingAnim);
                horseAsHorse->animationIdx = ENHORSE_ANIM_GALLOP;

                // Path A — vanilla EnHorse_Update stays enabled so the
                // Skin maintains its per-frame matrix state (required
                // for the draw function to render limbs at valid
                // positions). An earlier attempt to NULL the update
                // function froze the pose AND broke rendering entirely
                // because the Skin's per-limb matrices went uninitialised
                // — field test log 468 surfaced this (rider lifted to
                // saddle height, horse invisible).
                //
                // We rely on Actor_UpdateAll's fixed category order:
                // BG (cat 1) runs before NPC (cat 4). So EnHorse_Update
                // fires first, then DummyPlayer_Update's title-mode
                // branch fires and writes the horse's position fresh
                // from the formation slot. Any AI-driven movement
                // during EnHorse_Update gets overridden by our write
                // the same frame. Combined with horse-sync's
                // playerControlled=1 gate (which suppresses most AI on
                // peer-owned horses), the horse stays anchored at the
                // formation slot while its animation still ticks.
                RegisterTitlePeerHorse(clientId, horseNetId, horse);
                SPDLOG_INFO("[TitlePeer] Spawned horse for clientId={} netId=0x{:08X} (vanilla update + per-tick position override)",
                            clientId, horseNetId);
            } else {
                SPDLOG_WARN("[TitlePeer] Anchor_SpawnHorseForTitleScreen returned null "
                            "for clientId={} netId=0x{:08X}",
                            clientId, horseNetId);
            }
        }
    } else {
        SPDLOG_WARN("[TitlePeer] Actor_Spawn returned null for clientId={}", clientId);
    }
}

void Anchor::MaybeRebuildTitlePeers() {
    // Reentrancy guard — Actor_Spawn's init path can pump packet handlers
    // synchronously (HandlePacket_UpdateClientState in particular) which
    // would re-enter this function and double-rebuild.
    if (mIsRebuildingTitlePeers) return;
    mIsRebuildingTitlePeers = true;

    // Always clear cached pointers first — they may be dangling after a
    // scene reload, and a stale formation may be incomplete after a
    // peer arrival.
    ClearTitlePeerActors();

    auto bail = [this]() { mIsRebuildingTitlePeers = false; };

    if (gPlayState == nullptr)                                                  { bail(); return; }
    if (gSaveContext.gameMode != GAMEMODE_TITLE_SCREEN)                         { bail(); return; }
    if (gPlayState->sceneNum != SCENE_HYRULE_FIELD)                             { bail(); return; }
    if (CVarGetInteger(CVAR_ENHANCEMENT("Anchor.TitleScreenPeers"), 0) == 0)    { bail(); return; }

    // Build candidate list: online, not-self, same teamId as local.
    // Local team comes from the self-entry in clients map.
    std::string localTeamId;
    for (auto& [id, c] : clients) {
        if (c.self) { localTeamId = c.teamId; break; }
    }

    struct Candidate { uint32_t clientId; std::string name; };
    std::vector<Candidate> candidates;
    for (auto& [id, c] : clients) {
        if (c.self || !c.online) continue;
        if (c.teamId != localTeamId) continue;
        candidates.push_back({ id, c.name });
    }

    if (candidates.empty()) { bail(); return; }

    // Selection criterion — first-N by clientId, which reflects join
    // order (lower id = earlier connect; assigned by the relay). Matches
    // the Anchor menu's player-list ordering. Stable across rejoins for
    // the duration of any one session since clientIds don't reshuffle.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.clientId < b.clientId; });

    // Q1 locked — cap at 3 visible peers.
    constexpr size_t kMaxVisiblePeers = 3;
    const size_t count = std::min(candidates.size(), kMaxVisiblePeers);
    SPDLOG_INFO("[TitlePeer] Spawning {} peer(s) for title gallop (team='{}', {} candidates)",
                count, localTeamId, candidates.size());
    for (size_t i = 0; i < count; i++) {
        SpawnTitlePeerLink(candidates[i].clientId, (uint8_t)i);
    }

    mIsRebuildingTitlePeers = false;
}

// ============================================================================
// Title-screen peer horses — Phase 2 dual-map helpers (Fix A).
//
// The gameplay horse cache (mPeerHorses, keyed by netId) is the lookup the
// existing DummyPlayer mounted-pose reconciliation reads. The title-cleanup
// path needs clientId-keyed access (we walk Anchor::clients during rebuild,
// no netId in hand). Since MakeHorseNetId encodes only the bottom 8 bits of
// clientId, we can't derive full clientId from netId — so two maps coexist.
//
// To prevent write-skew + cleanup-skew bugs, all map manipulation goes
// through these three helpers. Direct mTitlePeerHorses / mPeerHorses writes
// outside these methods should be a code-review red flag.
//
// See Plans/title_screen_peer_actors.md §"Dual-map fix A (helper-wrapped)".
// ============================================================================

void Anchor::RegisterTitlePeerHorse(uint32_t clientId, uint32_t horseNetId,
                                      Actor* horse) {
    if (horse == nullptr || horseNetId == 0) return;
    mTitlePeerHorses[clientId] = horse;
    mPeerHorses[horseNetId]    = horse;
    // Set client.mountedHorseNetId so the existing mounted-pose
    // reconciliation (used by both gameplay and title paths) snaps the
    // rider DummyPlayer onto this horse on its next tick.
    auto it = clients.find(clientId);
    if (it != clients.end()) {
        it->second.mountedHorseNetId = horseNetId;
    }
}

void Anchor::UnregisterTitlePeerHorse(uint32_t clientId) {
    auto titleIt = mTitlePeerHorses.find(clientId);
    if (titleIt == mTitlePeerHorses.end()) return;
    Actor* horse = titleIt->second;

    // Recover the netId from the client entry so we can erase from
    // mPeerHorses too. (We can't derive it from clientId alone — only
    // the bottom 8 bits make it into the netId encoding.)
    auto clientIt = clients.find(clientId);
    if (clientIt != clients.end()) {
        const uint32_t horseNetId = clientIt->second.mountedHorseNetId;
        if (horseNetId != 0) {
            mPeerHorses.erase(horseNetId);
        }
        clientIt->second.mountedHorseNetId = 0;
    }

    mTitlePeerHorses.erase(titleIt);

    // KillNetworkActorSilently is defensive — early-returns on null or
    // already-killed actors (Anchor.cpp:43). Safe to call on cached
    // pointers within the current scene's actor list lifetime.
    KillNetworkActorSilently(horse);
}

Actor* Anchor::FindTitlePeerHorse(uint32_t clientId) const {
    auto it = mTitlePeerHorses.find(clientId);
    return (it != mTitlePeerHorses.end()) ? it->second : nullptr;
}

bool Anchor::IsTitlePeerHorse(const Actor* actor) const {
    if (actor == nullptr) return false;
    for (const auto& [clientId, horse] : mTitlePeerHorses) {
        if (horse == actor) return true;
    }
    return false;
}

void Anchor::ClearTitlePeerActors() {
    if (mTitlePeerActors.empty() && mTitlePeerHorses.empty()) return;
    const size_t prevCount = mTitlePeerActors.size();
    const size_t prevHorseCount = mTitlePeerHorses.size();

    // Phase 2 horse cleanup. Collect keys first so we can call
    // UnregisterTitlePeerHorse without iterator invalidation
    // (UnregisterTitlePeerHorse erases from mTitlePeerHorses).
    if (!mTitlePeerHorses.empty()) {
        std::vector<uint32_t> horseClientIds;
        horseClientIds.reserve(mTitlePeerHorses.size());
        for (auto& [clientId, _] : mTitlePeerHorses) {
            horseClientIds.push_back(clientId);
        }
        for (uint32_t clientId : horseClientIds) {
            UnregisterTitlePeerHorse(clientId);
        }
        SPDLOG_INFO("[TitlePeer] Cleared {} peer horse cache entries", prevHorseCount);
    }

    // Don't dereference cached pointers — they may be dangling after a
    // scene reload (per session_state.md "Scene-transition pointer cleanup"
    // — reading actor->update on a freed pointer is UB). The scene
    // transition's actor-list destruction has already killed the actors
    // we cached. For intra-scene cleanup (rare in Phase 1), the actors
    // will remain alive but invisible (DummyPlayer_Update's offline branch
    // hides them at -9999) until the next OnSceneSpawnActors fires.
    //
    // We also walk the live ACTORCAT_NPC list and Actor_Kill any surviving
    // title-mode peer actor. This handles the same-scene cleanup case
    // (peer disconnect mid-title-cycle) safely because we're using fresh
    // actor-list traversal, not cached pointers.
    if (gPlayState != nullptr) {
        Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (a != nullptr) {
            Actor* next = a->next;
            if (a->id == ACTOR_EN_OE2 && a->update == DummyPlayer_Update &&
                IsDummyPlayerTitleMode(a)) {
                KillNetworkActorSilently(a);
            }
            a = next;
        }
    }

    // Clear per-client cached player pointers that point into our (possibly
    // dangling) actor cache, so RefreshClientActors doesn't reuse a dead
    // pointer after save load.
    for (auto& [clientId, cachedActor] : mTitlePeerActors) {
        auto it = clients.find(clientId);
        if (it != clients.end() && it->second.player == (Player*)cachedActor) {
            it->second.player = nullptr;
        }
    }
    mTitlePeerActors.clear();
    SPDLOG_INFO("[TitlePeer] Cleared {} peer actor cache entries", prevCount);
}

bool Anchor::GetClientIsClimbing(uint32_t clientId) const {
    auto it = clients.find(clientId);
    return (it != clients.end()) ? it->second.isClimbing : false;
}

void Anchor::RefreshClientActors() {
    if (!IsSaveLoaded()) {
        return;
    }

    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;

    while (actor != NULL) {
        if (actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update) {
            NameTag_RemoveAllForActor(actor);
            Actor_Kill(actor);
        }
        actor = actor->next;
    }

    for (auto& [clientId, client] : clients) {
        if (!client.online || client.self) {
            continue;
        }

        spawningDummyPlayerForClientId = clientId;
        // We are using a hook `ShouldActorInit` to override the init/update/draw/destroy functions of the Player we
        // spawn We quickly store a mapping of "index" to clientId, then within the init function we use this to get the
        // clientId and store it on player->zTargetActiveTimer (unused s32 for the dummy) for convenience
        auto dummy =
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, client.posRot.pos.x, client.posRot.pos.y,
                        client.posRot.pos.z, client.posRot.rot.x, client.posRot.rot.y, client.posRot.rot.z, 0);
        client.player = (Player*)dummy;
    }
    spawningDummyPlayerForClientId = 0;
}

void Anchor::RecomputeEffectiveHost() {
    // Pillar A Phase 1 — pure (a) election rule. Phase 1.5 hybrid (b) adds
    // the pendingMigrateBack short-circuit for mid-migration reconnect. See
    // Anchor.h doc on effectiveHostClientId + pendingMigrateBack for full
    // semantics.
    uint32_t orig = roomState.ownerClientId;
    uint32_t newEffective = ownClientId;  // safety fallback

    auto origIt = clients.find(orig);
    const bool origOnline = (origIt != clients.end() && origIt->second.online);

    // Phase 1.5 pendingMigrateBack rising edge: original host reappeared in
    // ALL_CLIENT_STATE, and the currently-elected effective host isn't the
    // original owner (i.e. we already migrated away from them). Set the
    // flag; don't flip authority yet. Migration back happens on the
    // original owner's next OnSceneSpawnActors (which calls
    // ClearPendingMigrateBackOnSceneEntry — see below).
    //
    // Edge trigger vs level: only fires on the online transition. Once
    // pendingMigrateBack is true, subsequent RecomputeEffectiveHost calls
    // preserve current effectiveHostClientId (the else branch below) until
    // the flag is cleared.
    if (origOnline && effectiveHostClientId != 0 &&
        effectiveHostClientId != orig && !pendingMigrateBack) {
        pendingMigrateBack = true;
        SPDLOG_INFO("[Anchor] pendingMigrateBack armed: orig owner={} back online, "
                    "acting host={} keeps authority until orig owner re-enters a scene",
                    orig, effectiveHostClientId);
    }

    if (pendingMigrateBack) {
        // Hold authority with the acting host. If the acting host
        // themselves went offline during the hold, fall through to the
        // pure-(a) election below so authority doesn't stall.
        auto actingIt = clients.find(effectiveHostClientId);
        if (actingIt != clients.end() && actingIt->second.online) {
            // Acting host still online — preserve the election result.
            newEffective = effectiveHostClientId;
        } else {
            // Acting host went offline mid-hold. Drop the flag; run pure-(a)
            // so a new acting host takes over immediately.
            SPDLOG_INFO("[Anchor] pendingMigrateBack cleared: acting host {} went offline; "
                        "re-electing", effectiveHostClientId);
            pendingMigrateBack = false;
            newEffective = origOnline ? orig : ownClientId;
            uint32_t lowest = UINT32_MAX;
            for (auto& [clientId, client] : clients) {
                if (client.online && clientId < lowest) {
                    lowest = clientId;
                }
            }
            if (!origOnline && lowest != UINT32_MAX) {
                newEffective = lowest;
            }
        }
    } else if (origOnline) {
        newEffective = orig;
    } else {
        // Original owner is offline (or not in clients). Pick the lowest-
        // numbered online clientId. All clients converge on the same answer
        // because they all see the same ALL_CLIENT_STATE.
        uint32_t lowest = UINT32_MAX;
        for (auto& [clientId, client] : clients) {
            if (client.online && clientId < lowest) {
                lowest = clientId;
            }
        }
        if (lowest != UINT32_MAX) {
            newEffective = lowest;
        }
    }

    if (newEffective != effectiveHostClientId) {
        SPDLOG_INFO("[Anchor] Effective host changed: {} -> {} (orig owner={} {}{})",
                    effectiveHostClientId, newEffective, orig,
                    origOnline ? "online" : "offline",
                    pendingMigrateBack ? " pendingMigrateBack" : "");
        bool localBecameHost = (newEffective == ownClientId &&
                                effectiveHostClientId != ownClientId);
        effectiveHostClientId = newEffective;
        if (localBecameHost) {
            OnBecameEffectiveHost();
        }
    }
}

void Anchor::ClearPendingMigrateBackOnSceneEntry() {
    // Phase 1.5 — called from OnSceneSpawnActors when the local client is
    // roomState.ownerClientId (the returning original host). Clears the
    // flag and re-runs the election so authority can flip back to us for
    // the scene we just entered. Acting host retains authority for
    // scenes we haven't re-entered because their scene's IsRoomHost
    // check (Phase 2 per-room authority) already picks them there.
    if (!pendingMigrateBack) return;
    if (ownClientId != roomState.ownerClientId) return;

    SPDLOG_INFO("[Anchor] pendingMigrateBack cleared: original owner={} re-entered scene, "
                "election re-runs (acting host was {})",
                roomState.ownerClientId, effectiveHostClientId);
    pendingMigrateBack = false;
    RecomputeEffectiveHost();
}

void Anchor::OnBecameEffectiveHost() {
    SPDLOG_INFO("[Anchor] This client is now effective host (clientId={})", ownClientId);
    // Per migration plan A1: accept empty deadEnemiesByScene. Original host
    // already broadcast ENEMY_DEFEATED for every kill up to the disconnect,
    // so other clients' world-state is already correct. Only future late-
    // joiners are affected — they'll see corpses for post-migration kills
    // only. Acceptable for First Dungeon Demo scope.
    EnemyStateSync::HostBookkeeping::Instance().ClearAllDefeatBroadcasts();

    // AI Director migration handoff (step 5). The local Director already
    // has the latest cached ledger state from a prior DIRECTOR_STATE_SYNC
    // (peers always apply on receive). Notify descriptors that this
    // client is now the authority so they can rebuild host-perspective
    // caches, log "I am authority", etc.
    AnchorDirector::Director::Instance().OnHostMigrated(/*isNewHost=*/true);

    // Vacancy reconciliation across migration boundary.
    //
    // The exit-gated SCENE_DEATHS_CLEARED detection (HookHandlers.cpp
    // OnSceneSpawnActors host-out path + UpdateClientState peer-out
    // path) fires when an active host observes a scene becoming empty.
    // But if the original host disconnected ungracefully WHILE a scene
    // was emptying — last player exited and host crashed before the
    // broadcast reached the relay — the new effective host has no
    // record of the vacancy. Pre-migration mSceneDeaths entries persist
    // on the new host (or arrive via no-op since this is the first
    // host); next late-joiner sees stale corpses for scenes that are
    // logically vacant.
    //
    // One-shot reconciliation: walk every scene currently represented
    // in mSceneDeaths AND every scene any client is reporting; for each
    // scene that has zero current occupants (host + every online
    // client), broadcast SCENE_DEATHS_CLEARED. Catches scenes that
    // became vacant during the migration window.
    //
    // Cheap (O(scenes × clients), both small) and idempotent — receivers
    // clear empty state harmlessly, so re-broadcast on a scene that
    // peers already cleared is a no-op.
    std::unordered_set<int16_t> scenesToCheck;
    if (gPlayState != nullptr) {
        scenesToCheck.insert((int16_t)gPlayState->sceneNum);
    }
    for (auto& [otherId, other] : clients) {
        if (other.online && other.isSaveLoaded) {
            scenesToCheck.insert(other.sceneNum);
        }
    }
    // Also include every scene that has dead-enemy bookkeeping — those
    // are scenes where mSceneDeaths might be stale from pre-migration.
    // (Inferred via Serialize() since SceneDeaths(s) requires knowing s.)
    {
        nlohmann::json snap = EnemyStateSync::HostBookkeeping::Instance().Serialize();
        if (snap.contains("sceneDeaths") && snap["sceneDeaths"].is_object()) {
            for (auto it = snap["sceneDeaths"].begin(); it != snap["sceneDeaths"].end(); ++it) {
                try {
                    scenesToCheck.insert((int16_t)std::stoi(it.key()));
                } catch (...) { /* skip non-numeric keys */ }
            }
        }
    }
    int reconciled = 0;
    for (int16_t scene : scenesToCheck) {
        if (scene < 0) continue;
        bool anyOccupant = (gPlayState != nullptr && (int16_t)gPlayState->sceneNum == scene);
        if (!anyOccupant) {
            for (auto& [otherId, other] : clients) {
                if (!other.online || !other.isSaveLoaded || other.self) continue;
                if (other.sceneNum == scene) {
                    anyOccupant = true;
                    break;
                }
            }
        }
        if (!anyOccupant) {
            SendPacket_SceneDeathsCleared(scene, 0xFF);
            reconciled++;
        }
    }
    if (reconciled > 0) {
        SPDLOG_INFO("[Anchor] Migration reconciliation: broadcast SCENE_DEATHS_CLEARED for {} vacant scene(s)",
                    reconciled);
    }
}

void Anchor::BackfillEnemyNetIds() {
    // Recovery for the reconnect-during-scene-init scenario (log 116):
    // if the OnActorSpawn hook was unregistered when the scene set up its
    // initial actors, those actors stayed in the actor lists with no
    // EnemyNetId extension. ENEMY_UPDATE / ENEMY_DEFEATED then can't find
    // them by netId and the scene's enemy sync is silently broken until
    // the player leaves the scene.
    //
    // This function walks every syncable actor category and assigns the
    // same deterministic netId that OnActorSpawn would have set. It uses
    // the identical hash/encoding as OnActorSpawn so a future hit through
    // the normal hook produces the same netId — idempotent.
    //
    // Skip actors that already have an extension; only fill in missing.
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    int filled = 0;
    for (size_t i = 0; i < kSyncableActorCategoriesCount; i++) {
        Actor* actor = gPlayState->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        while (actor != nullptr) {
            // Use the same admission predicate OnActorSpawn uses.
            // Per-variant skip guards must ALSO mirror OnActorSpawn —
            // without this, a reconnect retroactively assigns netIds
            // to variants OnActorSpawn intentionally skipped (Dekunuts
            // flower, Hintnut reveal-child, Honotrap flame). The
            // resulting netId collision with the parent variant caused
            // the 2026-07-13 hint nut reflect-stun desync. See
            // Claude/Analysis/hintnut_reflect_stun_desync_2026-07-13.md.
            if (IsSyncableActor(actor) &&
                !ShouldSkipNetIdAssignment(actor) &&
                ObjectExtension::GetInstance().Get<EnemyNetId>(actor) == nullptr) {
                // Single source of truth for the netId encoding lives in
                // ActorSyncHelpers::EncodeEnemyNetId. Both this backfill path
                // and OnActorSpawn route through it so a future hit through
                // the normal hook produces the same netId — idempotent.
                uint32_t netId = EncodeEnemyNetId(actor);

                EnemyNetId ext;
                ext.netId = netId;
                ext.skelAnime = GetEnemySkelAnime(actor);
                ext.limbCount = ext.skelAnime ? ext.skelAnime->limbCount : 0;
                ObjectExtension::GetInstance().Set<EnemyNetId>(actor, std::move(ext));

                SPDLOG_INFO("[BackfillNetId] Assigned netId={} to actorId={} ptr={} "
                            "(scene init missed by hook during disconnect)",
                            netId, actor->id, (void*)actor);
                filled++;
            }
            actor = actor->next;
        }
    }
    if (filled > 0) {
        SPDLOG_INFO("[BackfillNetId] Backfilled {} actor(s) on reconnect", filled);
    }
}

bool Anchor::IsSaveLoaded() {
    if (gPlayState == nullptr) {
        return false;
    }

    if (GET_PLAYER(gPlayState) == nullptr) {
        return false;
    }

    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }

    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }

    return true;
}
