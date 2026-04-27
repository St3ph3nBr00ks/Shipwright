#include "PlayerLookup.h"
#include "soh/Network/Anchor/Anchor.h"  // DummyPlayer_Update forward decl, AnchorClient

extern "C" {
#include "macros.h"  // GET_PLAYER, SQ
extern SaveContext gSaveContext;
}

Actor* FindNearestPlayerActor(Actor* enemy, PlayState* play) {
    Player* localPlayer = GET_PLAYER(play);

    // Seed with the pre-computed squared distance to the local player so we
    // avoid an extra sqrt and stay consistent with the automatic field values.
    float nearestDistSq = SQ(enemy->xzDistToPlayer) + SQ(enemy->yDistToPlayer);
    Actor* nearest = &localPlayer->actor;

    Actor* npc = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            // Pillar B Phase 3 / Phase 4 v1 — exclude cross-timeline
            // DummyPlayers from targeting candidates. Their bodies are
            // invisible and they are non-interactive; enemies must not
            // aim at them either, otherwise child-timeline Deku Babas
            // would lunge at the position of an adult-timeline peer
            // (and vice versa).
            if (Anchor::Instance != nullptr) {
                uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(npc);
                auto it = Anchor::Instance->clients.find(clientId);
                if (it != Anchor::Instance->clients.end() &&
                    it->second.linkAge != gSaveContext.linkAge) {
                    npc = npc->next;
                    continue;
                }
            }

            float dx = enemy->world.pos.x - npc->world.pos.x;
            float dy = enemy->world.pos.y - npc->world.pos.y;
            float dz = enemy->world.pos.z - npc->world.pos.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearest = npc;
            }
        }
        npc = npc->next;
    }

    return nearest;
}
