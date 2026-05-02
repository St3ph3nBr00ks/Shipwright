#include "PlayerLookup.h"
#include "soh/Network/Anchor/Anchor.h"  // DummyPlayer_Update forward decl, AnchorClient

extern "C" {
#include "macros.h"  // GET_PLAYER, SQ
extern SaveContext gSaveContext;
}

Actor* FindNearestPlayerActor(Actor* enemy, PlayState* play) {
    Player* localPlayer = GET_PLAYER(play);

    // Compute the seed distance directly from positions rather than reading
    // `enemy->xzDistToPlayer` / `yDistToPlayer`. Those cached fields are
    // patched every frame by the host's `ShouldActorUpdate` hook
    // (HookHandlers.cpp around line 3577) to point at whichever player
    // FindNearestPlayerActor previously decided was nearest — usable as
    // an aim hint for vanilla AI but a tie-breaking landmine when this
    // function later runs again from inside `actor->update()` (e.g.,
    // `EnDekubaba_Grow` → `Anchor_GetNearestPlayerActor`):
    //   1. Seed = patched distance (= distance to the actually-nearest
    //      player, possibly a DummyPlayer).
    //   2. Seed actor = `&localPlayer->actor` (hardcoded).
    //   3. Loop walks DummyPlayers; for the one that was the patch
    //      source, distSq exactly matches the seed.
    //   4. `<` strict-less-than fails on tie; localPlayer wins.
    //   5. Function returns localPlayer even when a DummyPlayer was
    //      genuinely closer.
    // Result observed in Inside Great Deku Tree (KB-08 follow-up,
    // 2026-04-28): Dekubaba lunges at host even when host is out of
    // range and non-host stands adjacent.
    //
    // Recomputing from positions removes the patched-seed dependency.
    // The arithmetic is two extra subtracts + three extra multiplies vs.
    // the field reads — negligible.
    float dxL = enemy->world.pos.x - localPlayer->actor.world.pos.x;
    float dyL = enemy->world.pos.y - localPlayer->actor.world.pos.y;
    float dzL = enemy->world.pos.z - localPlayer->actor.world.pos.z;
    float nearestDistSq = dxL * dxL + dyL * dyL + dzL * dzL;
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

int GetSyncedPlayerActors(PlayState* play, Actor** outActors, int maxCount) {
    if (outActors == nullptr || maxCount <= 0) {
        return 0;
    }

    int count = 0;
    Player* localPlayer = GET_PLAYER(play);
    if (localPlayer != nullptr && count < maxCount) {
        outActors[count++] = &localPlayer->actor;
    }

    Actor* npc = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr && count < maxCount) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            // Cross-timeline filter (same shape as FindNearestPlayerActor):
            // exclude DummyPlayers whose linkAge differs from local — they
            // are invisible / non-interactive on this timeline (Pillar B
            // Phase 3) and must not be targeted for spawn placement.
            bool include = true;
            if (Anchor::Instance != nullptr) {
                uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(npc);
                auto it = Anchor::Instance->clients.find(clientId);
                if (it != Anchor::Instance->clients.end() &&
                    it->second.linkAge != gSaveContext.linkAge) {
                    include = false;
                }
            }
            if (include) {
                outActors[count++] = npc;
            }
        }
        npc = npc->next;
    }

    return count;
}
