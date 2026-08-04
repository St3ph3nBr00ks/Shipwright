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
void FireSwap(Actor* oldEnSt, PlayState* play, const char* triggerReason) {
    if (oldEnSt == nullptr || play == nullptr) return;

    const uint32_t oldNetId = GetEnStNetId(oldEnSt);
    Vec3f spawnPos = oldEnSt->world.pos;
    const s16 spawnYaw = oldEnSt->shape.rot.y;

    // 2026-08-04 (user) — find nearest floor node within 300u. Prior
    // code snapped to actor.floorHeight which is the floor DIRECTLY
    // below the actor — if the Skulltula dangles over a pit, that
    // returns the pit floor (two levels below the intended arena).
    // RoomNavData's floor-node graph knows about the actual walkable
    // ground in the room; use that as the truth for the spawn coord.
    constexpr float kMaxSpawnSearchXZ = 300.0f;
    const AnchorNavRoom::RoomNavData* navData =
        AnchorNavRoom::GetForRoom(gPlayState->sceneNum,
                                    (int8_t)gPlayState->roomCtx.curRoom.num);
    if (navData != nullptr) {
        const int nodeIdx = AnchorNavRoom::FindNearestFloorNodeXZRadius(
            navData, spawnPos, kMaxSpawnSearchXZ);
        if (nodeIdx >= 0) {
            const Vec3f oldSpawn = spawnPos;
            spawnPos = navData->nodes[nodeIdx].pos;
            SPDLOG_INFO("[EnStSwap] Nav-node spawn adjust: ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}) (nearest floor node within {:.0f}u)",
                        oldSpawn.x, oldSpawn.y, oldSpawn.z,
                        spawnPos.x, spawnPos.y, spawnPos.z, kMaxSpawnSearchXZ);
        } else {
            SPDLOG_INFO("[EnStSwap] No floor node within {:.0f}u — using En_St world.pos as-is",
                        kMaxSpawnSearchXZ);
        }
    } else {
        SPDLOG_INFO("[EnStSwap] RoomNavData unavailable for scene {}/{} — using En_St world.pos as-is",
                    gPlayState->sceneNum, gPlayState->roomCtx.curRoom.num);
    }

    SPDLOG_INFO("[EnStSwap] Firing En_St→En_Sw swap — trigger={} oldNetId={} pos=({:.0f},{:.0f},{:.0f})",
                triggerReason, oldNetId,
                spawnPos.x, spawnPos.y, spawnPos.z);

    // Bracket the spawn with pendingReplacesNetId so the outgoing
    // ENEMY_SPAWN broadcast carries the field. HookHandlers consumes
    // + clears the value inside SendPacket_EnemySpawn.
    Anchor::Instance->pendingReplacesNetId = oldNetId;
    Actor* newEnSw = Actor_Spawn(&play->actorCtx, play,
                                  ACTOR_EN_SW,
                                  spawnPos.x, spawnPos.y, spawnPos.z,
                                  /*rot.x=*/0, /*rot.y=*/spawnYaw, /*rot.z=*/0,
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
