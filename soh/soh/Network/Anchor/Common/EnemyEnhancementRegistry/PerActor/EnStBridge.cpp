/**
 * EnStBridge — Pillar 5 Phase 3 (GH #210): En_St → En_Sw actor swap.
 *
 * ceiling-dangling Skulltulas (En_St) transform into wall-crawling
 * Skullwalltulas (En_Sw) at defined trigger points for combat variety.
 * Two host-only triggers:
 *
 *   Anchor_Enhance_EnSt_OnGroundImpact   — called when the descending
 *     En_St touches the floor. Rolls `Skulltula.GroundDrop` % chance
 *     (default 30%); on success, fires the swap.
 *
 *   Anchor_Enhance_EnSt_OnHitDuringDescent — called when Link damages
 *     an En_St while it's in MoveToGround state. Gated by
 *     `Skulltula.AggressiveDrop` (bool, default ON); guaranteed swap
 *     when enabled.
 *
 * Swap mechanics (host side):
 *   1. Cache old En_St netId + world position.
 *   2. Bracket Anchor::pendingReplacesNetId = oldEnStNetId around
 *      Actor_Spawn(EN_SW). OnActorSpawn hook picks up the pending
 *      value and includes it in the outgoing ENEMY_SPAWN broadcast.
 *   3. KillNetworkActorSilently(oldEnSt) — no ENEMY_DEFEATED echo
 *      (the swap is not a "kill" per se, just a transformation).
 *
 * Peer side (in HandlePacket_EnemySpawn): when replacesNetId is
 * present, find the actor with that netId and KillNetworkActorSilently
 * BEFORE spawning the replacement. Both clients end with only the
 * En_Sw at the swap coordinate.
 *
 * Design decisions locked by user 2026-08-04:
 *   - Both triggers active
 *   - Ground-impact 30% chance
 *   - Enhanced En_Sw (inherits Skullwalltula.* CVar toggles at spawn time)
 *   - Same feature/vanilla-enemy-enhancements branch
 *
 * 2026-08-04 CORRECTION (log 849): No params guard needed. Per
 * z_en_st.c:4 header the En_St variants are "normal / big / invisible"
 * (params 0/1/2 respectively). Gold-token Skulltulas are a different
 * actor entirely (ACTOR_EN_SSH). All En_St variants swap; spawned
 * En_Sw uses `params = 0` (combat wall-crawler, not gold-token).
 */

// Pitfall 40 — Anchor.h FIRST.
#include "soh/Network/Anchor/Anchor.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"
#include "soh/Network/Anchor/Common/SceneAuthority.h"
#include "soh/Network/Anchor/EnemyNetId.h"
#include "soh/Enhancements/RoomNavData/RoomNavData.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_St/z_en_st.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
}

// EnSw mark helper — declared in EnSwBridge.cpp. Sets a per-actor
// flag consumed by EnSwStateMachine.cpp Tick to enforce visual scale
// (0.06) + floor-snap each frame (see EnStBridge.cpp file header).
extern "C" void Anchor_Enhance_EnSw_MarkFromStSwap(EnSw* actor);

// EnSw swap-transition arm helper — declared in EnSwBridge.cpp.
// Sets per-actor state fields for the animated lerp from the En_St's
// original ceiling position to the En_Sw's intended ground position.
// The state machine's Tick reads these each frame while
// stSwapTransitionActive == true and advances the actor at 60u/sec.
extern "C" void Anchor_Enhance_EnSw_ArmSwapTransition(EnSw* actor,
                                                        const Vec3f* sourcePos,
                                                        const Vec3s* sourceRot,
                                                        const Vec3f* targetPos,
                                                        const Vec3s* targetRot);

// EnSw HP carryover helper — declared in EnSwBridge.cpp (GH #210
// Feature B, 2026-08-06). Writes newEnSw->colChkInfo.health =
// min(sourceHealth, EnSwDescriptor::GetConfiguredMaxHP()). Gates on
// IsInstanceEnhanced internally (combat variant only). No-op when
// sourceHealth == 0.
extern "C" void Anchor_Enhance_EnSw_ApplyCarryoverHealth(EnSw* actor,
                                                          PlayState* play,
                                                          uint8_t sourceHealth);

namespace {

// 2026-08-04 latest (log 849): CORRECTED variant understanding.
// Per z_en_st.c:4 file header: "Skulltula (normal, big, invisible)".
//   params 0 = normal combat Skulltula
//   params 1 = BIG variant (larger radius, different naviEnemyId)
//   params 2 = invisible (Lens-of-Truth reactive)
// Gold-token Skulltulas are a DIFFERENT actor entirely
// (ACTOR_EN_SSH / ACTOR_EN_STH) — they never call these hooks.
// All three En_St variants can swap. No params guard needed.

// Spawned En_Sw uses combat variant params — (params & 0xE000) == 0
// selects the wall-crawler combat form (NOT gold-token variants of
// En_Sw). Per EnSwDescriptor.h §D12 the enhancement layer's
// IsInstanceEnhanced() gates on this exact discriminator.
constexpr s16 kEnSwCombatParams = 0;

// CVar names.
constexpr const char* kGroundDropCVar     = "gEnhancements.Skulltula.GroundDrop";
constexpr const char* kAggressiveDropCVar = "gEnhancements.Skulltula.AggressiveDrop";

// Read the En_St's netId from the EnemyNetId extension. Returns 0
// when no netId assigned (e.g. host hasn't finished OnActorSpawn
// bookkeeping yet — unlikely by ground-impact time but defensive).
uint32_t GetEnStNetId(Actor* actor) {
    if (actor == nullptr) return 0;
    const EnemyNetId* ext =
        ObjectExtension::GetInstance().Get<EnemyNetId>(actor);
    return (ext != nullptr) ? ext->netId : 0;
}

// Actual swap execution. Host-only. Assumes eligibility already checked.
//
// 2026-08-05 (Phase 3 v3) — spawn at the En_St's exact position + rot,
// then arm an animated lerp to the intended ground nav-node position.
// This gives the swap a visible transition (spider "falls" from ceiling
// to floor over ~1-2 seconds at 60u/sec) and prevents the state
// machine's wall-attach paths from firing at spawn time (they can't
// fire during the transition — state machine dispatch is suppressed
// by TickStSwapTransition's early-return).
void FireSwap(Actor* oldEnSt, PlayState* play, const char* triggerReason) {
    if (oldEnSt == nullptr || play == nullptr) return;

    const uint32_t oldNetId = GetEnStNetId(oldEnSt);
    // Source pose = En_St's exact current position + rotation.
    // The En_Sw will spawn here, then lerp to the target ground pos.
    const Vec3f sourcePos = oldEnSt->world.pos;
    const Vec3s sourceRot = oldEnSt->shape.rot;
    // targetPos starts as the En_St pos; the raycast + nav-node
    // search below refines it to the intended landing coordinate.
    Vec3f spawnPos = sourcePos;
    const s16 spawnYaw = oldEnSt->shape.rot.y;

    // 2026-08-05 (user) — establish a floor-Y reference via downward
    // raycast BEFORE nav-node search. The raycast starts 10u ABOVE the
    // En_St because the ceiling-Skulltula can clip into ground geometry
    // during its LandOnGround animation; a raycast from body-center
    // risks starting inside a floor poly and missing the surface
    // entirely. This raycast establishes the intended landing altitude
    // (deepest floor directly below), which then filters nav-node
    // candidates via the Y-delta gate.
    constexpr float kRaycastStartYOffset = 10.0f;
    constexpr float kRaycastMaxDepth     = 3000.0f;
    constexpr float kFloorNormalYMin     = 0.5f;  // matches wall/floor split threshold used elsewhere
    Vec3f rayFrom = spawnPos;
    rayFrom.y += kRaycastStartYOffset;
    Vec3f rayTo = spawnPos;
    rayTo.y -= kRaycastMaxDepth;
    CollisionPoly* refFloorPoly = nullptr;
    s32 refFloorBgId = 0;
    Vec3f refFloorHit = {0.0f, 0.0f, 0.0f};
    bool haveFloorRef = false;
    Vec3f searchRef = spawnPos;
    if (BgCheck_EntityLineTest1(&play->colCtx, &rayFrom, &rayTo,
                                 &refFloorHit, &refFloorPoly,
                                 /*chkWall*/ 0, /*chkFloor*/ 1,
                                 /*chkCeil*/ 0, /*chkOneFace*/ 1,
                                 &refFloorBgId) && refFloorPoly != nullptr) {
        const float ny = COLPOLY_GET_NORMAL(refFloorPoly->normal.y);
        if (ny > kFloorNormalYMin) {
            searchRef.y = refFloorHit.y;
            haveFloorRef = true;
            SPDLOG_INFO("[EnStSwap] Downward raycast reference floor Y={:.0f} (from En_St Y={:.0f}, +{:.0f}u start offset)",
                        refFloorHit.y, spawnPos.y, kRaycastStartYOffset);
        }
    }
    if (!haveFloorRef) {
        SPDLOG_INFO("[EnStSwap] Downward raycast missed floor — nav-node search reference falls back to En_St world.pos.y={:.0f}",
                    spawnPos.y);
    }

    // 2026-08-05 (user) — nav-node search with Y-delta gate. Prior
    // code used the default maxYDelta=1e9f which effectively disabled
    // the Y filter, so a Deku Tree ceiling-Skulltula could pick a pit
    // floor 200-400u below the intended arena because the pit's floor
    // node had smaller XZ distance to the En_St's overhead position.
    // Tight Y-delta around the raycast reference filters nav-node
    // candidates to the walkable level Link would actually stand on.
    constexpr float kMaxSpawnSearchXZ         = 300.0f;
    constexpr float kMaxSpawnSearchYWithRef   = 100.0f;
    constexpr float kMaxSpawnSearchYFallback  = 300.0f;  // wider when raycast missed
    const float maxYDelta = haveFloorRef ? kMaxSpawnSearchYWithRef
                                         : kMaxSpawnSearchYFallback;
    const AnchorNavRoom::RoomNavData* navData =
        AnchorNavRoom::GetForRoom(gPlayState->sceneNum,
                                    (int8_t)gPlayState->roomCtx.curRoom.num);
    if (navData != nullptr) {
        const int nodeIdx = AnchorNavRoom::FindNearestFloorNodeXZRadius(
            navData, searchRef, kMaxSpawnSearchXZ, maxYDelta);
        if (nodeIdx >= 0) {
            const Vec3f oldSpawn = spawnPos;
            spawnPos = navData->nodes[nodeIdx].pos;
            SPDLOG_INFO("[EnStSwap] Nav-node spawn adjust: ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}) (searchRefY={:.0f} XZ<={:.0f}u |dY|<={:.0f}u)",
                        oldSpawn.x, oldSpawn.y, oldSpawn.z,
                        spawnPos.x, spawnPos.y, spawnPos.z,
                        searchRef.y, kMaxSpawnSearchXZ, maxYDelta);
        } else if (haveFloorRef) {
            // No nav node matches the tight window — fall back to the
            // raycast-hit floor Y so we at least land on real geometry
            // rather than the En_St's overhead position.
            spawnPos.y = searchRef.y;
            SPDLOG_INFO("[EnStSwap] No floor node within XZ<={:.0f}u |dY|<={:.0f}u — falling back to raycast floor Y={:.0f}",
                        kMaxSpawnSearchXZ, maxYDelta, searchRef.y);
        } else {
            SPDLOG_INFO("[EnStSwap] No floor node within XZ<={:.0f}u |dY|<={:.0f}u AND raycast missed — using En_St world.pos as-is",
                        kMaxSpawnSearchXZ, maxYDelta);
        }
    } else if (haveFloorRef) {
        // No nav data for this room, but the downward raycast found a
        // real floor — use its Y so we don't spawn floating in midair.
        spawnPos.y = searchRef.y;
        SPDLOG_INFO("[EnStSwap] RoomNavData unavailable for scene {}/{} — using raycast floor Y={:.0f}",
                    gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num,
                    searchRef.y);
    } else {
        SPDLOG_INFO("[EnStSwap] RoomNavData unavailable for scene {}/{} AND raycast missed — using En_St world.pos as-is",
                    gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num);
    }

    // Target rotation for the lerp landing pose. Ground orientation
    // matches TickGroundPursue idle: pitch = -0x4000 (dorsal-up,
    // skull-forward), yaw preserved from source (spider ends facing
    // the same direction the ceiling Skulltula was), roll = 0.
    const Vec3s targetRot = {(s16)-0x4000, spawnYaw, 0};

    SPDLOG_INFO("[EnStSwap] Firing En_St→En_Sw swap — trigger={} oldNetId={} "
                "source=({:.0f},{:.0f},{:.0f}) target=({:.0f},{:.0f},{:.0f})",
                triggerReason, oldNetId,
                sourcePos.x, sourcePos.y, sourcePos.z,
                spawnPos.x, spawnPos.y, spawnPos.z);

    // 2026-08-05 (Phase 3 v3) — spawn the En_Sw AT THE SOURCE (En_St's
    // position) rather than at the target ground position. The animated
    // transition (armed below) then lerps from source to target at
    // 60u/sec. Peers see the transition too because the wire spawn
    // carries source pos; if we ever want peers to also see the lerp
    // they'd need a separate wire signal for the target — TODO for a
    // follow-up if the visual mismatch matters. For v1 the peer just
    // sees the En_Sw appear at source and immediately be there (since
    // peer's local ENEMY_UPDATE from host will drive its position via
    // the sync pipeline).
    //
    // GH #210 Feature B (2026-08-06, Option B semantics) — snapshot
    // the En_St's current HP BEFORE Actor_Spawn / KillNetworkActor-
    // Silently touch it. This value flows through to
    // Anchor_Enhance_EnSw_ApplyCarryoverHealth below.
    //
    // Both trigger sites deliver POST-hit HP:
    //   OnHitDuringDescent — called from z_en_st.c AFTER
    //     Actor_ApplyDamage returns "survived", so `colChkInfo.health`
    //     is already the post-hit value. A lethal hit never reaches
    //     this bridge (the death branch runs first). Result: hit
    //     that triggers swap ALSO deducts HP — a 2-HP En_St hit for
    //     1 damage produces a 1-HP En_Sw.
    //   OnGroundImpact — called from z_en_st.c:965 during a
    //     non-damage transition (LandOnGround → WaitOnGround), so
    //     the HP is whatever it was after any prior hits.
    const uint8_t sourceHealth = oldEnSt->colChkInfo.health;

    // Bracket the spawn with pendingReplacesNetId so the outgoing
    // ENEMY_SPAWN broadcast carries the field. HookHandlers consumes
    // + clears the value inside SendPacket_EnemySpawn.
    Anchor::Instance->pendingReplacesNetId = oldNetId;
    Actor* newEnSw = Actor_Spawn(&play->actorCtx, play,
                                  ACTOR_EN_SW,
                                  sourcePos.x, sourcePos.y, sourcePos.z,
                                  /*rot.x=*/sourceRot.x,
                                  /*rot.y=*/sourceRot.y,
                                  /*rot.z=*/sourceRot.z,
                                  kEnSwCombatParams);
    // Defensive clear in case OnActorSpawn didn't fire (Actor_Spawn
    // returned null — actor limit or invalid id).
    Anchor::Instance->pendingReplacesNetId = 0;

    if (newEnSw == nullptr) {
        SPDLOG_WARN("[EnStSwap] Actor_Spawn(EN_SW) failed — actor limit reached. "
                    "Old En_St stays alive.");
        return;
    }

    // 2026-08-04 (Phase 3 v2) — mark the new En_Sw so the state
    // machine's per-tick handler enforces (a) scale = 0.06 to match
    // the source En_St BIG variant visual size (vanilla En_Sw is
    // 0.02 ≈ 3× smaller than En_St) and (b) floor-snap so the actor
    // sits on the ground instead of dangling in midair looking for
    // a wall to cling to.
    Anchor_Enhance_EnSw_MarkFromStSwap((EnSw*)newEnSw);

    // 2026-08-05 (Phase 3 v3) — arm the animated transition from
    // source (En_St's pos + rot) to target (ground nav-node + ground
    // orientation). State machine's Tick advances the actor at
    // 60u/sec and lerps rotation to match; when arrival threshold is
    // crossed, transition state becomes GroundPursue and normal state
    // machine dispatch takes over.
    Anchor_Enhance_EnSw_ArmSwapTransition((EnSw*)newEnSw,
                                            &sourcePos, &sourceRot,
                                            &spawnPos, &targetRot);

    // GH #210 Feature B (2026-08-06) — carry over the En_St's HP into
    // the new En_Sw. Applied after Anchor_Enhance_EnSw_OnInit already
    // ran during Actor_Spawn (which set health to the CVar-configured
    // max via Feature A). This call overrides that with the
    // min(sourceHealth, MaxHP) carryover so a partial-HP En_St swap
    // preserves damage taken. Runs on host; peer replicas converge via
    // the next ENEMY_UPDATE tick which carries host-authoritative HP.
    SPDLOG_INFO("[EnStSwap] Carryover source En_St HP={} → applying to new En_Sw netId={} (cap = MaxHP CVar)",
                (int)sourceHealth, oldNetId);
    Anchor_Enhance_EnSw_ApplyCarryoverHealth((EnSw*)newEnSw, play, sourceHealth);

    // Silently kill the old En_St on host. Peer already handled its own
    // kill in HandlePacket_EnemySpawn via replacesNetId when the SPAWN
    // arrives. Silent = no ENEMY_DEFEATED echo (the swap isn't a "defeat").
    Anchor::Instance->KillNetworkActorSilently(oldEnSt);
}

}  // namespace

// ---- Extern C bridge entry points ---------------------------------

// Ground-impact trigger. Called from z_en_st.c when the descending
// Skulltula touches the floor and would transition from LandOnGround
// to WaitOnGround. If this returns 1, the vanilla caller should
// early-return (the actor has been swapped and is about to die).
extern "C" int Anchor_Enhance_EnSt_OnGroundImpact(EnSt* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return 0;

    // Diagnostic: log every hook entry so we can confirm reach.
    // Prints host-vs-peer, params, CVar values. If user reports "no
    // swaps," greppable [EnStSwap] entry line proves reachability.
    const int chancePercent =
        AnchorCVarSync::GetEnforcedInt(kGroundDropCVar, 30);
    const int enabled =
        AnchorCVarSync::GetEnforcedInt(kAggressiveDropCVar, 100);
    SPDLOG_INFO("[EnStSwap] OnGroundImpact ENTRY — actor={} params={} isHost={} groundDrop={}%% aggressiveDrop={}%%",
                (void*)&actor->actor, actor->actor.params,
                SceneAuthority::IsMyCurrentRoomHost() ? 1 : 0,
                chancePercent, enabled);

    // Host-only decision (sync-rule 1). Peers observe via ENEMY_SPAWN
    // with replacesNetId.
    if (!SceneAuthority::IsMyCurrentRoomHost()) return 0;

    // No params guard — all En_St variants (normal/big/invisible)
    // swap. Gold-token Skulltulas use a different actor id entirely
    // and never enter this hook.

    // CVar chance gate — default 30% if CVar unset (solo before any
    // UI toggle).
    if (chancePercent <= 0) return 0;

    const int roll = (int)(Rand_ZeroOne() * 100.0f);
    if (roll >= chancePercent) {
        SPDLOG_INFO("[EnStSwap] Ground-impact roll FAILED — chance={}%% roll={}",
                    chancePercent, roll);
        return 0;
    }

    SPDLOG_INFO("[EnStSwap] Ground-impact roll SUCCEEDED — chance={}%% roll={}",
                chancePercent, roll);
    FireSwap(&actor->actor, play, "ground-impact");
    return 1;
}

// Hit-during-descent trigger. Called from the En_St damage handler
// when Link damages the plant while it's in MoveToGround state.
// 2026-08-04 (user): CVar is now a percentage 0-100, not a bool.
// Default 50% (halfway between "never" and "always") if CVar unset.
extern "C" int Anchor_Enhance_EnSt_OnHitDuringDescent(EnSt* actor, PlayState* play) {
    if (actor == nullptr || play == nullptr) return 0;

    const int chancePercent =
        AnchorCVarSync::GetEnforcedInt(kAggressiveDropCVar, 50);
    SPDLOG_INFO("[EnStSwap] OnHitDuringDescent ENTRY — actor={} params={} isHost={} chance={}%%",
                (void*)&actor->actor, actor->actor.params,
                SceneAuthority::IsMyCurrentRoomHost() ? 1 : 0,
                chancePercent);

    if (!SceneAuthority::IsMyCurrentRoomHost()) return 0;

    // No params guard — all En_St variants swap (see OnGroundImpact
    // comment). Gold-token Skulltulas use a different actor id.

    if (chancePercent <= 0) return 0;

    const int roll = (int)(Rand_ZeroOne() * 100.0f);
    if (roll >= chancePercent) {
        SPDLOG_INFO("[EnStSwap] Hit-during-descent roll FAILED — chance={}%% roll={}",
                    chancePercent, roll);
        return 0;
    }

    SPDLOG_INFO("[EnStSwap] Hit-during-descent roll SUCCEEDED — chance={}%% roll={}",
                chancePercent, roll);
    FireSwap(&actor->actor, play, "hit-during-descent");
    return 1;
}
