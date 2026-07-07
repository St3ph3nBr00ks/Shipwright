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
#include "overlays/actors/ovl_Bg_Spot15_Saku/z_bg_spot15_saku.h"
extern PlayState* gPlayState;
}

/**
 * HYRULE_CASTLE_GATE_OPEN — any-client→all broadcast for the 10-rupee
 * En_Heishi2 bribe gate in SCENE_HYRULE_CASTLE.
 *
 * ## Why a dedicated packet
 *
 * The gate's opening animation is gated on `unk_168 && !Flags_GetInfTable(
 * INFTABLE_71)` (z_bg_spot15_saku.c:62). The vanilla flow:
 *   1. En_Heishi2 accepts the 10-rupee bribe (choice 0 at z_en_heishi2.c:250).
 *   2. Guard slams spear anim, then sets `gate->unk_168 = 1` (line 305).
 *   3. Gate's Update sees `unk_168 && !INFTABLE_71` → transitions to
 *      func_808B4978 (opening animation, slides down over ~1 second).
 *   4. Dialog closes → z_actor.c:6000 fires `Flags_SetInfTable(INFTABLE_71)`.
 *
 * Note the ORDER: gate triggers BEFORE flag is set. Once the gate is
 * animating in `func_808B4978`, it no longer reads INFTABLE_71 — so the
 * flag write races safely with the animation.
 *
 * The `FLAG_INF_TABLE` case in SetFlag already syncs INFTABLE_71 across
 * clients, but that only guarantees Init pre-positions the gate on the
 * NEXT scene load. It does NOT trigger the current in-scene animation
 * on peers, because by the time SET_FLAG arrives the flag is already set
 * — the gate's `unk_168 && !INFTABLE_71` check fails-closed forever.
 *
 * This packet solves the animation gap: it fires from En_Heishi2's
 * unk_168 trigger site synchronously, BEFORE the flag write. Receivers
 * locate their local Bg_Spot15_Saku and set its `unk_168 = 1`. Peer's
 * next actor Update tick then sees `unk_168 && !INFTABLE_71` = true
 * (flag has not yet been broadcast) and transitions to the opening
 * animation. When the flag broadcast arrives seconds later, the gate
 * is already animating in a state that doesn't care about the flag.
 *
 * ## Late-joiner
 *
 * Handled by the standard save-state replay layer: INFTABLE_71 is in
 * `gSaveContext.infTable`, so a late-joiner receives it via
 * UPDATE_CLIENT_STATE. Their `BgSpot15Saku_Init` reads
 * `Flags_GetInfTable(INFTABLE_71) = true` at scene entry and pre-positions
 * the gate open (z_bg_spot15_saku.c:49). No animation needed.
 *
 * ## Wire fields
 *
 * sceneNum     — sender's scene at send time (Pillar E ValidateSameScene).
 * timeline     — Pillar B linkAge.
 * targetTeamId — team-scoped (gate progression is per-team).
 *
 * ## Scope
 *
 * SCENE_HYRULE_CASTLE only. If another vanilla actor with the same
 * "guard triggers gate.unk_168 then flag-set-later" shape surfaces
 * (haven't seen one yet), extract a general primitive at that time.
 * Until then, YAGNI: keep the packet specific to the one call site.
 */

void Anchor::SendPacket_HyruleCastleGateOpen() {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = HYRULE_CASTLE_GATE_OPEN;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[HyruleCastleGateOpen] Sending sceneNum={}", (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_HyruleCastleGateOpen(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[HyruleCastleGateOpen] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[HyruleCastleGateOpen] Drop — local scene {} != sender scene {} "
                    "(no-op; late-joiner will get gate open via INFTABLE_71 save-state replay)",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    // Walk ITEMACTION category — Bg_Spot15_Saku registers under
    // ACTORCAT_ITEMACTION (z_bg_spot15_saku.c:23). The gate is unique per
    // scene so first-match short-circuits.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head;
    while (actor != nullptr) {
        if (actor->id == ACTOR_BG_SPOT15_SAKU) {
            BgSpot15Saku* gate = (BgSpot15Saku*)actor;
            gate->unk_168 = 1;
            SPDLOG_INFO("[HyruleCastleGateOpen] Set unk_168=1 on local gate "
                        "(scene={})", (int)gPlayState->sceneNum);
            return;
        }
        actor = actor->next;
    }

    SPDLOG_INFO("[HyruleCastleGateOpen] No local Bg_Spot15_Saku in scene {} — "
                "packet dropped (late-joiner or gate destroyed)", (int)gPlayState->sceneNum);
}

// C bridge — called from z_en_heishi2.c at the unk_168 trigger site
// (line 305 in vanilla). Fires synchronously before the dialog closes
// and INFTABLE_71 is set, preserving the ordering the gate's Update
// gate relies on.
extern "C" void Anchor_NotifyHyruleCastleGateOpen(void) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isEnabled) {
        return;
    }
    Anchor::Instance->SendPacket_HyruleCastleGateOpen();
}
