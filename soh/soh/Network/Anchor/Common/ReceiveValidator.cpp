#include "ReceiveValidator.h"
#include "ActorSyncHelpers.h"  // kSyncableActorCategories
#include "soh/Network/Anchor/Anchor.h"
#include <libultraship/log/luslog.h>

extern "C" {
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
}

namespace ReceiveValidator {

ValidationVerdict ValidateActorStillLive(Actor* actor, PlayState* play) {
    if (actor == nullptr) return ValidationVerdict::DropWithWarn;
    if (actor->update == nullptr) return ValidationVerdict::DropWithWarn;
    if (play == nullptr) return ValidationVerdict::DropSilent;

    // Walk syncable categories to confirm the pointer is still in a live list.
    for (size_t i = 0; i < kSyncableActorCategoriesCount; ++i) {
        Actor* head = play->actorCtx.actorLists[kSyncableActorCategories[i]].head;
        for (Actor* a = head; a != nullptr; a = a->next) {
            if (a == actor) return ValidationVerdict::Valid;
        }
    }
    return ValidationVerdict::DropWithWarn;
}

ValidationVerdict ValidateNetIdMatches(Actor* actor, uint32_t expectedNetId) {
    if (actor == nullptr) return ValidationVerdict::DropWithWarn;
    const ::EnemyNetId* ext = ObjectExtension::GetInstance().Get<::EnemyNetId>(actor);
    if (ext == nullptr) return ValidationVerdict::DropSilent;  // no ext = not synced
    return (ext->netId == expectedNetId) ? ValidationVerdict::Valid
                                         : ValidationVerdict::DropWithWarn;
}

ValidationVerdict ValidateCategoryForPacket(Actor* actor, const std::string& /*packetType*/) {
    // Phase 1: simple admission check. Per-packet category rules can layer
    // on top later if specific packets need stricter category filtering.
    if (actor == nullptr) return ValidationVerdict::DropWithWarn;
    return IsSyncableActor(actor) ? ValidationVerdict::Valid
                                  : ValidationVerdict::DropSilent;
}

ValidationVerdict ValidateSkelAnimePresent(Actor* actor) {
    if (actor == nullptr) return ValidationVerdict::DropWithWarn;
    SkelAnime* ska = GetEnemySkelAnime(actor);
    return (ska != nullptr) ? ValidationVerdict::Valid
                            : ValidationVerdict::DropWithWarn;
}

ValidationVerdict ValidateSameScene(int16_t sceneNum) {
    if (gPlayState == nullptr) return ValidationVerdict::DropSilent;
    return (gPlayState->sceneNum == sceneNum) ? ValidationVerdict::Valid
                                              : ValidationVerdict::DropSilent;
}

ValidationVerdict ValidateTimelineMatches(uint8_t timeline) {
    if (timeline == 0) return ValidationVerdict::Valid;  // legacy / no timeline
    return (timeline == (uint8_t)gSaveContext.linkAge) ? ValidationVerdict::Valid
                                                      : ValidationVerdict::DropSilent;
}

ValidationVerdict ValidateSenderAuthority(uint32_t /*senderId*/, const std::string& /*packetType*/) {
    // Phase 1: legacy — no sender-authority enforcement beyond what existing
    // host gates do. Pillar A migration adds a real check here that maps
    // packet type → required authority (effective host vs scene host vs anyone).
    return ValidationVerdict::Valid;
}

ValidationVerdict ValidateAndLog(ValidationVerdict v, const char* exprStr,
                                  const char* file, int line) {
    if (v == ValidationVerdict::DropWithWarn) {
        SPDLOG_WARN("[ReceiveValidator] {} failed at {}:{}", exprStr, file, line);
    }
    return v;
}

}  // namespace ReceiveValidator
