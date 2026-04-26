#include "PlayerLookup.h"
#include "soh/Network/Anchor/Anchor.h"  // DummyPlayer_Update forward decl

extern "C" {
#include "macros.h"  // GET_PLAYER, SQ
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
