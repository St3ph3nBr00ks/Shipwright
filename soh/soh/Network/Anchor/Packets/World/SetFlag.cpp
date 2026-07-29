#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "z64.h"
#include "functions.h"

extern PlayState* gPlayState;
}

/**
 * SET_FLAG
 *
 * Fired when a flag is set in the save context
 */

void Anchor::SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    // Note: intentionally NOT gating on IsSaveLoaded(). Flags_SetEventChkInf
    // / Flags_SetInfTable etc. fire from Cutscene_HandleConditionalTriggers
    // (invoked by Play_Init) DURING scene transitions when Player actor is
    // torn down + not yet re-spawned → GET_PLAYER(gPlayState) is null →
    // IsSaveLoaded() returns false. Gating here silently drops the outbound
    // SET_FLAG for every transition-triggered save-context flag change
    // (Saria's Song learn, dungeon-medallion post-boss save, etc.) — sibling
    // to the receive-side silent-drop fixed 2026-07-28. Payload doesn't
    // need gPlayState; sceneNum/flagType/flag are all in scope.
    // See Analysis/ocarina_send_side_silent_drop_2026-07-28.md.
    if (!roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = SET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetFlag(nlohmann::json payload) {
    if (!roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload.at("sceneNum").get<s16>();
    s16 flagType = payload.at("flagType").get<s16>();
    s16 flag = payload.at("flag").get<s16>();

    // sceneNum == SCENE_ID_MAX is a sentinel meaning "global flag" (handled below); only larger
    // values would index gSaveContext.sceneFlags out of bounds.
    if (sceneNum < 0 || sceneNum > SCENE_ID_MAX) {
        SPDLOG_ERROR("[Anchor] SET_FLAG: sceneNum {} out of range", sceneNum);
        return;
    }

    if (sceneNum == SCENE_ID_MAX) {
        // Global flags (EventChkInf/ItemGetInf/InfTable/EventInf/RandomizerInf)
        // write directly to gSaveContext static arrays via RawAction::SetFlag
        // — no gPlayState deref, always safe to apply even during scene
        // transitions.
        //
        // Previously used GameInteractionEffect::SetFlag::Apply() which
        // silently early-returns via CanBeApplied's !IsSaveLoaded gate. If
        // the receiver was mid-scene-transition (gPlayState transiently
        // null → IsSaveLoaded() false), the SET_FLAG packet was dropped
        // with no retry — leaving peer state divergent. Traced via log
        // 751 Saria bridge crash (P1 crashed on KF→LW crossing because
        // EVENTCHKINF_SPOKE_TO_SARIA_ON_BRIDGE wasn't set from P2's
        // earlier broadcast; the vanilla give-item branch re-fired on
        // P1). See Analysis/saria_bridge_crash_2026-07-28.md.

        // Queue item 40 (Option A denylist) — cutscene-gating EventChkInf
        // flags where syncing them from peer causes the recipient's
        // vanilla `!Flags_GetEventChkInf(...)` gate to reject their local
        // cutscene branch (z_demo.c Cutscene_HandleConditionalTriggers).
        // Each client must set these flags via their OWN local branch
        // when they cross the trigger entrance, otherwise catchup falls
        // into Savecontext_ApplyForce case (b) → aborts fast-forward →
        // recipient stays in gameplay, misses the cutscene, and gets
        // stuck in an infinite "Catching up to peer cutscene" toast.
        // Not syncing these specific flags is safe — they gate their
        // own cutscene entrance; other consumers don't depend on
        // cross-client parity of the flag pre-cutscene.
        // See Analysis/saria_bridge_peer_flag_gates_cutscene_2026-07-29.md.
        if (flagType == FLAG_EVENT_CHECK_INF) {
            switch (flag) {
                case EVENTCHKINF_SPOKE_TO_SARIA_ON_BRIDGE:      // 0xC1 — Saria bridge Fairy Ocarina
                case EVENTCHKINF_LEARNED_REQUIEM_OF_SPIRIT:     // 0xAC — Desert Colossus Requiem
                case EVENTCHKINF_BONGO_BONGO_ESCAPED_FROM_WELL: // 0xAA — Kakariko Nocturne
                    SPDLOG_INFO("[Anchor] SET_FLAG dropped for cutscene-gating "
                                "EventChkInf 0x{:02X} — recipient must set locally "
                                "via own cutscene branch (queue item 40)",
                                (unsigned)flag);
                    return;
            }
        }

        GameInteractor::RawAction::SetFlag(flagType, flag);

        // Special case: If King Zora moved, and the player has Ruto's Letter, convert it to an empty bottle.
        // Requires gPlayState + Player; keep behind IsSaveLoaded gate.
        if (IsSaveLoaded() && flagType == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
            Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
            Inventory_ReplaceItem(gPlayState, ITEM_LETTER_RUTO, ITEM_BOTTLE);
        }
    } else {
        // Scene-specific flag path — RawAction::SetSceneFlag dereferences
        // gPlayState->sceneNum + actorCtx, so cannot bypass IsSaveLoaded
        // without a null-check + deferred-apply queue. Retain the gate
        // here until a queue is implemented. Log dropped packets so we
        // can measure the miss rate.
        // TODO(#37 follow-up): deferred queue for scene-flag SET_FLAG
        // when !IsSaveLoaded, drained on OnSaveLoaded.
        if (!IsSaveLoaded()) {
            SPDLOG_WARN("[Anchor] SET_FLAG scene={} flagType={} flag={} dropped "
                        "(!IsSaveLoaded) — scene-flag deferred queue not yet implemented",
                        sceneNum, flagType, flag);
            return;
        }

        // Special case: Ignore water temple water level flags, stored at 0x1C, 0x1D, 0x1E.
        if (sceneNum == SCENE_WATER_TEMPLE && flagType == FLAG_SCENE_SWITCH &&
            (flag == 0x1C || flag == 0x1D || flag == 0x1E)) {
            return;
        }

        // Special case: Ignore forest temple elevator flag, stored at 0x1B.
        if (sceneNum == SCENE_FOREST_TEMPLE && flagType == FLAG_SCENE_SWITCH && flag == 0x1B) {
            return;
        }

        GameInteractor::RawAction::SetSceneFlag(sceneNum, flagType, flag);

        // #193 fix 2 — active-despawn pass for static collectibles.
        //
        // SetSceneFlag::_Apply updates the runtime + persisted flag
        // bitmasks, but the `VB_ITEM00_DESPAWN` check at
        // z_en_item00.c:373 only fires inside `EnItem00_Init`. An
        // EN_ITEM00 actor that's already alive when the flag arrives
        // keeps rendering and remains pickable — so a peer who picked
        // up a static heart in another scene works fine via late
        // re-entry (Init re-checks), but simultaneous same-room pickup
        // leaves the second client with a live stale copy.
        //
        // Walk ACTORCAT_MISC, find EnItem00 actors whose
        // `collectibleFlag` matches, and kill them. This is the
        // mirror of the local pickup path (z_en_item00.c:969,
        // 980, 989) which calls both `Flags_SetCollectible` AND
        // `Actor_Kill` atomically.
        //
        // FLAG_SCENE_TREASURE (chest open) is intentionally NOT
        // mirrored here. Chests don't despawn on open — they animate
        // into an open pose and stay in the scene. Cross-client chest
        // open sync needs a separate animation/contents-distribution
        // story (design doc Q on shared vs per-player chest contents).
        // Skip the walk for flag == 0. Heart/rupee/nut drops from grass +
        // killed enemies have `collectibleFlag = (params & 0x3F00) >> 8 = 0`
        // (no scene-collectible association). Vanilla `Flags_SetCollectible`
        // is a no-op for flag == 0 (z_actor.c:824 guards the bitmask update),
        // but `GameInteractor_ExecuteOnSceneFlagSet` fires regardless, so
        // the host's pickup-of-untracked-drop broadcasts SET_FLAG flag=0.
        // Without this guard, the receiver walks ACTORCAT_MISC and kills
        // EVERY ground drop with `collectibleFlag == 0` — every grass-cut
        // heart, nut, rupee in the scene disappears when one is picked up.
        if (flag != 0 && sceneNum == gPlayState->sceneNum &&
            flagType == FLAG_SCENE_COLLECTIBLE) {
            Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head;
            while (a != nullptr) {
                Actor* next = a->next;
                if (a->id == ACTOR_EN_ITEM00) {
                    EnItem00* it = (EnItem00*)a;
                    if (it->collectibleFlag == flag) {
                        Actor_Kill(a);
                    }
                }
                a = next;
            }
        }
    }
}
