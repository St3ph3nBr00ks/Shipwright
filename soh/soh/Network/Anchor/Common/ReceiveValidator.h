#pragma once

// Defensive receive-side validation (Pillar E).
//
// See Claude/Plans/pillar_e_receive_validator.md.
//
// Usage pattern in a HandlePacket_*:
//
//   void Anchor::HandlePacket_EnemyUpdate(nlohmann::json payload) {
//       if (!IsSaveLoaded()) return;
//       Actor* actor = FindActorByNetId(payload["netId"]);
//       if (VALIDATE(ValidateActorStillLive(actor, gPlayState)) != ValidationVerdict::Valid) return;
//       if (VALIDATE(ValidateNetIdMatches(actor, payload["netId"])) != ValidationVerdict::Valid) return;
//       // ... existing handler logic ...
//   }
//
// Q 4.E.1 (answered 2026-04-25): default verdict on failed checks is
// DropWithWarn. Specific checks may be de-escalated to DropSilent
// (legitimate non-applicability — e.g. cross-scene packet) or escalated
// to Disconnect-sender if a check becomes a reliable malicious-peer signal.

#include <cstdint>
#include <string>

extern "C" {
#include "z64.h"
}

namespace Anchor {

enum class ValidationVerdict {
    Valid,         // proceed with packet processing
    DropSilent,    // no-op, no log (legitimate non-applicability)
    DropWithWarn,  // LUSLOG_WARN + no-op (suspicious)
};

// Primitive checks
ValidationVerdict ValidateActorStillLive(Actor* actor, PlayState* play);
ValidationVerdict ValidateNetIdMatches(Actor* actor, uint32_t expectedNetId);
ValidationVerdict ValidateCategoryForPacket(Actor* actor, const std::string& packetType);
ValidationVerdict ValidateSkelAnimePresent(Actor* actor);
ValidationVerdict ValidateSameScene(int16_t sceneNum);
ValidationVerdict ValidateTimelineMatches(uint8_t timeline);
ValidationVerdict ValidateSenderAuthority(uint32_t senderId, const std::string& packetType);

// VALIDATE() macro — wraps a check, adds call-site context to logs.
#define VALIDATE(expr) ::Anchor::ValidateAndLog((expr), #expr, __FILE__, __LINE__)
ValidationVerdict ValidateAndLog(ValidationVerdict v, const char* exprStr,
                                  const char* file, int line);

}  // namespace Anchor
