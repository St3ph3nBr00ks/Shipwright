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
#include "overlays/actors/ovl_En_Md/z_en_md.h"
extern PlayState* gPlayState;
}

/**
 * MIDO_POST_DEKU_LEAVE — dialog client → team broadcast.
 *
 * Architecture context (#184 follow-up, post-Deku-Tree confrontation):
 *   After defeating Queen Gohma, the player encounters Mido on the way
 *   back to Kokiri Forest. Vanilla flow (z_en_md.c):
 *     1. Mido's Init drops him into BlockPath case 2 (DEKU_TREE_DEAD
 *        condition + SHOWED_MIDO_SWORD_SHIELD already set).
 *     2. Player approaches → dialog 0x1045 plays.
 *     3. Player advances → talkState = NPC_TALK_STATE_ACTION.
 *     4. BlockPath transitions to Walk (z_en_md.c:735-758).
 *     5. Walk follows the path → reaches end → Actor_Kill +
 *        SetEventChkInf(SPOKE_TO_MIDO_AFTER_DEKU_TREES_DEATH).
 *
 *   In MP, only the dialog client's local talkState fires. Without
 *   this packet, peers' local Mido stays in BlockPath until SPOKE
 *   syncs in via the existing flag-replication infrastructure, then
 *   the EnMd_Update despawn check Actor_Kills the remote actor —
 *   abruptly, with no walk-away cinematic.
 *
 *   This packet bridges the transition: the dialog client's BlockPath
 *   → Walk firing also broadcasts MIDO_POST_DEKU_LEAVE; receivers
 *   force their local Mido through the same transition (via
 *   EnMd_ApplyNetState case 0x03, which mirrors the BlockPath
 *   transition body). Both clients then walk the path in parallel
 *   and Actor_Kill at the path end, giving every team member the
 *   vanilla walk-away appearance.
 *
 *   Backstop: if the packet is dropped or a peer arrives at the
 *   scene mid-walk, the EnMd_Update SPOKE-flag despawn check still
 *   fires on the BlockPath instance (gated to actionFunc ==
 *   EnMd_BlockPath so it doesn't interrupt an in-progress Walk).
 *
 * Wire fields:
 *   sceneNum     — sender's scene at send time (Pillar E ValidateSameScene).
 *   timeline     — Pillar B linkAge.
 *   targetTeamId — team-scoped (Mido is per-team per #184 Team scope).
 *
 * NOT a generic NPC walk-away primitive — Mido's case is unique because
 * (a) the canonical kill condition is gated on a flag that races with
 * the Walk traversal across the network, and (b) the actor isn't in
 * the synced-enemy categories so generic ENEMY_STATE actionState sync
 * doesn't apply. Other NPCs with similar walk-away patterns can copy
 * this packet's shape.
 */

void Anchor::SendPacket_MidoPostDekuLeave() {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"]         = MIDO_POST_DEKU_LEAVE;
    payload["sceneNum"]     = (int)gPlayState->sceneNum;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[MidoPostDekuLeave] Sending sceneNum={}", (int)gPlayState->sceneNum);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_MidoPostDekuLeave(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    if (PacketTimeline::IsCrossTimelinePacket(payload)) {
        SPDLOG_INFO("[MidoPostDekuLeave] Drop — cross-timeline packet");
        return;
    }

    s16 sceneNum = (s16)payload.value("sceneNum", -1);
    if (sceneNum != (s16)gPlayState->sceneNum) {
        SPDLOG_INFO("[MidoPostDekuLeave] Drop — local scene {} != sender scene {}",
                    (int)gPlayState->sceneNum, (int)sceneNum);
        return;
    }

    // Mido is unique per scene; walk the NPC category and route the
    // first ACTOR_EN_MD we find. ACTOR_EN_MD is fixed-category NPC —
    // unlike some allowlisted actors that re-categorise mid-life,
    // Mido stays in NPC for his whole lifecycle.
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (actor != nullptr) {
        if (actor->id == ACTOR_EN_MD) {
            EnMd* mido = (EnMd*)actor;
            // Reuse the existing #184 sync primitive — case 0x03 mirrors
            // the BlockPath → Walk transition body at z_en_md.c:752-757.
            EnMd_ApplyNetState(mido, gPlayState, 0x03);
            SPDLOG_INFO("[MidoPostDekuLeave] Forced local Mido BlockPath → Walk "
                        "(scene={})", (int)gPlayState->sceneNum);
            return;
        }
        actor = actor->next;
    }

    SPDLOG_INFO("[MidoPostDekuLeave] No local Mido in scene {} — packet dropped "
                "(EnMd_Update SPOKE-despawn backstop will handle it when SPOKE "
                "syncs)", (int)gPlayState->sceneNum);
}
