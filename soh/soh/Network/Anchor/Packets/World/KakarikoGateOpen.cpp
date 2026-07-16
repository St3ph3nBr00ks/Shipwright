#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64.h"
#include "macros.h"
#include "src/overlays/actors/ovl_Bg_Gate_Shutter/z_bg_gate_shutter.h"
extern PlayState* gPlayState;
}

/**
 * KAKARIKO_GATE_OPEN — any-client → all broadcast for the Kakariko →
 * Death Mountain Trail gate (Bg_Gate_Shutter). Playtest 2026-07-15
 * Bug S2-4 (see Claude/Analysis/playthrough_2026-07-15_session2_triage.md).
 *
 * ## Why a dedicated packet
 *
 * Direct mirror of HYRULE_CASTLE_GATE_OPEN — same shape, different
 * gate. Vanilla flow:
 *   1. Player shows Zelda's Letter (or a rando/adult prereq) to the
 *      Kakariko guard (En_Heishi2 type 3 / "Kakariko guard" variant).
 *   2. Guard's spear-slam animation reaches the frame where
 *      `gate->openingState = 1` fires (z_en_heishi2.c:489).
 *   3. Bg_Gate_Shutter's `func_8087828C` sees
 *      `openingState == 1 && !INFTABLE_SHOWED_ZELDAS_LETTER_TO_GATE_GUARD`
 *      → transitions to `func_80878300` (opening slide + audio).
 *   4. Dialog closes → z_actor.c:6000 fires
 *      Flags_SetInfTable(INFTABLE_SHOWED_ZELDAS_LETTER_TO_GATE_GUARD).
 *
 * The FLAG_INF_TABLE SetFlag broadcast covers late-joiners (their
 * BgGateShutter_Init pre-positions the gate on scene entry when the
 * flag is set — z_bg_gate_shutter.c:48-54). It does NOT trigger the
 * in-scene ANIMATION on already-loaded peers: by the time SET_FLAG
 * arrives, the gate's `!INFTABLE_*` check fails-closed, and its
 * Update never transitions to the opening state.
 *
 * This packet fires synchronously at the same instant the guard sets
 * `openingState = 1` on their own local gate. Receivers walk
 * ACTORCAT_ITEMACTION for their local Bg_Gate_Shutter and set the
 * same field. Peer's next Update tick sees the transition condition
 * true (flag not yet broadcast) and starts animating. When SET_FLAG
 * arrives later, the gate is already in a state that doesn't read
 * the flag.
 *
 * ## Scope
 *
 * SCENE_KAKARIKO_VILLAGE only. Two "guard-triggers-gate" instances
 * exist in the game — Hyrule Castle (HYRULE_CASTLE_GATE_OPEN) and
 * this one. A general primitive would be worth extracting if a
 * third surfaces; until then, YAGNI, keep the two packets specific.
 */

void Anchor::SendPacket_KakarikoGateOpen() {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = KAKARIKO_GATE_OPEN;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[KakarikoGateOpen] Sending sceneNum={}", (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_KakarikoGateOpen(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[KakarikoGateOpen] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[KakarikoGateOpen] Drop — local scene {} != sender scene {} "
                    "(no-op; late-joiner will get gate pre-positioned via "
                    "INFTABLE_SHOWED_ZELDAS_LETTER_TO_GATE_GUARD save-state replay)",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    // Walk ITEMACTION category — Bg_Gate_Shutter registers under
    // ACTORCAT_ITEMACTION (z_bg_gate_shutter.c:27). The gate is unique
    // per scene so first-match short-circuits.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head;
    while (actor != nullptr) {
        if (actor->id == ACTOR_BG_GATE_SHUTTER) {
            BgGateShutter* gate = (BgGateShutter*)actor;
            gate->openingState = 1;
            SPDLOG_INFO("[KakarikoGateOpen] Set openingState=1 on local gate "
                        "(scene={})", (int)gPlayState->sceneNum);
            return;
        }
        actor = actor->next;
    }

    SPDLOG_INFO("[KakarikoGateOpen] No local Bg_Gate_Shutter in scene {} — "
                "packet dropped (late-joiner or gate destroyed)",
                (int)gPlayState->sceneNum);
}

// C bridge — called from z_en_heishi2.c at the openingState trigger
// site (line 489 vanilla). Fires synchronously BEFORE the dialog
// closes and INFTABLE_SHOWED_ZELDAS_LETTER_TO_GATE_GUARD is set,
// preserving the ordering the gate's Update relies on.
extern "C" void Anchor_NotifyKakarikoGateOpen(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isEnabled) {
        return;
    }
    Anchor::Instance->SendPacket_KakarikoGateOpen();
}
