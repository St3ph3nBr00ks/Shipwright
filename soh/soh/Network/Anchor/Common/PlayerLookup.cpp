#include "PlayerLookup.h"
#include "soh/Network/Anchor/Anchor.h"  // DummyPlayer_Update forward decl, AnchorClient
#include "soh/Network/Anchor/AIDirector/Descriptors/InvaderDescriptor.h"  // AnchorDirector::IsSceneFlaggedNoInvaders

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

    // Stage 3 — NPC Follower as alternate target. When the local NPC
    // follower exists, is alive, and is not invulnerable, treat it as
    // an additional target candidate alongside DummyPlayers. Enemy AI
    // that reads the patched cached fields (xzDistToPlayer etc.) will
    // pursue the NPC if it's the closest target. Same shape as the
    // DummyPlayer iteration above; gated through the public Anchor
    // helper so the CVar read is centralized.
    if (Anchor::Instance != nullptr && Anchor::Instance->IsFollowerNpcTargetable()) {
        Actor* npcFollower = Anchor::Instance->GetFollowerNpcLocalActor();
        if (npcFollower != nullptr && npcFollower->update != nullptr) {
            float dx = enemy->world.pos.x - npcFollower->world.pos.x;
            float dy = enemy->world.pos.y - npcFollower->world.pos.y;
            float dz = enemy->world.pos.z - npcFollower->world.pos.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearest = npcFollower;
            }
        }
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

// ---------------------------------------------------------------------------
// PickHostileTargetForInvader — multi-player target picker for NPC Invader.
// ---------------------------------------------------------------------------
//
// Walks the local actor list (local Player + ACTORCAT_NPC DummyPlayers +
// optional local NPC Follower) and returns the closest valid candidate by
// XZ distance from the invader. Every in-world candidate is guaranteed to
// be in the local scene (it's in the local actor list), but the per-peer
// AnchorClient state is still queried to filter on remote save/cutscene
// status — a peer whose DummyPlayer happens to be in our actor list but
// whose remote save is mid-load, or who is currently in a cutscene, is
// not a valid aggro target.
//
// Returns nullptr when no valid candidate exists. Caller behaviour on
// nullptr is "hold IDLE" / suspend pursuit.
//
// Single-player session collapses to "closest = local Player" with the
// same gating; in that case the function returns the local Player when
// it passes the gates, otherwise nullptr.
Actor* PickHostileTargetForInvader(Actor* invader, PlayState* play) {
    if (invader == nullptr || play == nullptr) return nullptr;

    // --- Scene-level gates ---------------------------------------------
    // Apply the blacklist once up front against the local scene. Every
    // in-world candidate lives in the local scene by definition, so a
    // blacklisted scene means "no Invader target on this client this tick"
    // regardless of which candidate we'd otherwise pick. Mirrors the
    // Director-side IsValidTarget which applies the same gate per-player.
    if (AnchorDirector::IsSceneFlaggedNoInvaders(play->sceneNum)) {
        return nullptr;
    }

    Actor* best = nullptr;
    float  bestDistSq = 0.0f;

    auto considerCandidate = [&](Actor* cand) {
        if (cand == nullptr) return;
        const float dx = invader->world.pos.x - cand->world.pos.x;
        const float dz = invader->world.pos.z - cand->world.pos.z;
        const float dSq = dx * dx + dz * dz;
        if (best == nullptr || dSq < bestDistSq) {
            best = cand;
            bestDistSq = dSq;
        }
    };

    // --- Local Player gate ---------------------------------------------
    // Read straight off gPlayState / gSaveContext for local state.
    // Cutscene gate uses csCtx.state (per InvaderDescriptor §7.5 plan).
    // Save-loaded gate proxied by Anchor::Instance->IsSaveLoaded() when
    // Anchor is available (gives the same answer as IsValidTarget — see
    // Anchor.cpp:727); falls back to gSaveContext.fileNum >= 0 when
    // Anchor isn't around (test harness / standalone).
    {
        Player* localPlayer = GET_PLAYER(play);
        if (localPlayer != nullptr) {
            const bool saveLoaded =
                (Anchor::Instance != nullptr)
                    ? Anchor::Instance->IsSaveLoaded()
                    : (gSaveContext.fileNum >= 0);
            const bool inCutscene = (play->csCtx.state != CS_STATE_IDLE);
            if (saveLoaded && !inCutscene) {
                considerCandidate(&localPlayer->actor);
            }
        }
    }

    // --- DummyPlayer iteration -----------------------------------------
    // Mirror FindNearestPlayerActor's iteration shape (NPC category, id
    // == ACTOR_EN_OE2, update == DummyPlayer_Update) plus the Pillar B
    // Phase 3 cross-timeline filter. Additionally apply the
    // online / save-loaded / non-cutscene / same-scene filters via the
    // peer's AnchorClient row.
    Actor* npc = play->actorCtx.actorLists[ACTORCAT_NPC].head;
    while (npc != nullptr) {
        if (npc->id == ACTOR_EN_OE2 && npc->update == DummyPlayer_Update) {
            bool valid = true;
            if (Anchor::Instance != nullptr) {
                const uint32_t clientId =
                    Anchor::Instance->GetDummyPlayerClientId(npc);
                auto it = Anchor::Instance->clients.find(clientId);
                if (it == Anchor::Instance->clients.end()) {
                    valid = false;
                } else {
                    const AnchorClient& c = it->second;
                    // Cross-timeline (Pillar B Phase 3).
                    if (c.linkAge != gSaveContext.linkAge)        valid = false;
                    // Online + save-loaded.
                    else if (!c.online || !c.isSaveLoaded)        valid = false;
                    // Peer in cutscene (csCtxState != CS_STATE_IDLE).
                    else if (c.csCtxState != CS_STATE_IDLE)       valid = false;
                    // Peer in a different scene. The DummyPlayer's world.pos
                    // is parked at -9999 for out-of-scene peers by
                    // DummyPlayer_Update, but this is belt-and-braces — a
                    // same-coord coincidence shouldn't attract a target.
                    else if (c.sceneNum != play->sceneNum)        valid = false;
                }
            }
            if (valid) considerCandidate(npc);
        }
        npc = npc->next;
    }

    // --- NPC Follower (local) ------------------------------------------
    // Same shape as FindNearestPlayerActor's NPC Follower branch. The
    // local NPC Follower is owned by the local client and lives in the
    // local scene by definition, so we only gate on the same liveness
    // predicate the existing path uses.
    if (Anchor::Instance != nullptr && Anchor::Instance->IsFollowerNpcTargetable()) {
        Actor* npcFollower = Anchor::Instance->GetFollowerNpcLocalActor();
        if (npcFollower != nullptr && npcFollower->update != nullptr) {
            considerCandidate(npcFollower);
        }
    }

    return best;
}
