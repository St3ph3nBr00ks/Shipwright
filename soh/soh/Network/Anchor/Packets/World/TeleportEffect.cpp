// TELEPORT_EFFECT packet — team-scoped broadcast to trigger a
// sparkle-burst visual at a specific world position + color across all
// same-scene same-team clients.
//
// See PacketTypes.h TELEPORT_EFFECT comment for wire fields + design
// intent. See Common/TeleportEffect.{h,cpp} for the vanilla-parity
// rendering primitive (EffectSsKiraKira_SpawnDispersed).

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/PacketTimeline.h"
#include "soh/Network/Anchor/Common/TeleportEffect.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

void Anchor::SendPacket_TeleportEffect(float x, float y, float z,
                                        uint8_t primR, uint8_t primG, uint8_t primB,
                                        uint8_t envR, uint8_t envG, uint8_t envB) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    SendPacket_TeleportEffect((int16_t)gPlayState->sceneNum, x, y, z,
                               primR, primG, primB, envR, envG, envB);
}

void Anchor::SendPacket_TeleportEffect(int16_t sceneNum,
                                        float x, float y, float z,
                                        uint8_t primR, uint8_t primG, uint8_t primB,
                                        uint8_t envR, uint8_t envG, uint8_t envB) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;

    nlohmann::json payload;
    payload["type"]         = TELEPORT_EFFECT;
    payload["sceneNum"]     = (int)sceneNum;
    payload["pos"]          = nlohmann::json::array({x, y, z});
    payload["primR"]        = (int)primR;
    payload["primG"]        = (int)primG;
    payload["primB"]        = (int)primB;
    payload["envR"]         = (int)envR;
    payload["envG"]         = (int)envG;
    payload["envB"]         = (int)envB;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    PacketTimeline::SetTimelineField(payload);

    SPDLOG_INFO("[TeleportEffect] Broadcasting pos=({:.0f},{:.0f},{:.0f}) "
                "primRGB=({},{},{}) sceneNum={}",
                x, y, z, (int)primR, (int)primG, (int)primB,
                (int)sceneNum);

    SendJsonToRemote(payload);

    // Local spawn ONLY if the effect's scene matches our current scene.
    // The Anchor player-teleport arrival case broadcasts sparkles for
    // the TARGET scene (not sender's current scene) — the sender is
    // still in the departure scene when this fires; local spawn at
    // arrival pos would render in the wrong scene.
    if (sceneNum == (int16_t)gPlayState->sceneNum) {
        TeleportEffect::SpawnSparkleBurst(gPlayState, x, y, z,
                                           primR, primG, primB,
                                           envR, envG, envB);
    }
}

void Anchor::BroadcastTeleportSparklesForOwnColor(float x, float y, float z) {
    if (gPlayState == nullptr) return;
    BroadcastTeleportSparklesForOwnColor((int16_t)gPlayState->sceneNum,
                                          x, y, z);
}

void Anchor::BroadcastTeleportSparklesForOwnColor(int16_t sceneNum,
                                                    float x, float y, float z) {
    auto it = clients.find(ownClientId);
    if (it == clients.end()) return;
    const auto& color = it->second.color;
    const uint8_t envR = (uint8_t)((int)color.r * 3 / 5);
    const uint8_t envG = (uint8_t)((int)color.g * 3 / 5);
    const uint8_t envB = (uint8_t)((int)color.b * 3 / 5);
    SendPacket_TeleportEffect(sceneNum, x, y, z,
                               color.r, color.g, color.b,
                               envR, envG, envB);
}

void Anchor::HandlePacket_TeleportEffect(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) return;
    if (PacketTimeline::IsCrossTimelinePacket(payload)) return;

    const int16_t sceneNum = (int16_t)payload.value("sceneNum", -1);
    if (sceneNum != (int16_t)gPlayState->sceneNum) {
        // Not visible from our current scene — silently drop.
        return;
    }

    if (!payload.contains("pos") || !payload["pos"].is_array() ||
        payload["pos"].size() != 3) {
        SPDLOG_WARN("[TeleportEffect] Drop — malformed pos field");
        return;
    }
    const float x = payload["pos"][0].get<float>();
    const float y = payload["pos"][1].get<float>();
    const float z = payload["pos"][2].get<float>();

    const uint8_t primR = (uint8_t)payload.value("primR", 255);
    const uint8_t primG = (uint8_t)payload.value("primG", 255);
    const uint8_t primB = (uint8_t)payload.value("primB", 220);
    const uint8_t envR  = (uint8_t)payload.value("envR",  180);
    const uint8_t envG  = (uint8_t)payload.value("envG",  180);
    const uint8_t envB  = (uint8_t)payload.value("envB",  100);

    SPDLOG_INFO("[TeleportEffect] Applying at ({:.0f},{:.0f},{:.0f}) "
                "primRGB=({},{},{})",
                x, y, z, (int)primR, (int)primG, (int)primB);

    TeleportEffect::SpawnSparkleBurst(gPlayState, x, y, z,
                                       primR, primG, primB,
                                       envR, envG, envB);
}
