/**
 * DirectorDebugWindow — implementation. See header for design notes.
 */

#include "DirectorDebugWindow.h"
#include "Director.h"

#include "../Anchor.h"
#include "../Common/SceneAuthority.h"
#include "soh/cvar_prefixes.h"

#include <imgui.h>
#include <libultraship/bridge/consolevariablebridge.h>

namespace AnchorDirector {

void DirectorDebugWindow::DrawElement() {
    DrawHeader();
    ImGui::Separator();
    DrawDescriptors();
    ImGui::Separator();
    DrawSessionView();
    // TestDescriptor toggle moved to Flotilla -> Game Director menu tab.
}

void DirectorDebugWindow::DrawHeader() {
    Director& director = Director::Instance();

    const bool isHost = ::SceneAuthority::IsEffectiveHost();
    const uint32_t hostId = ::SceneAuthority::GetEffectiveHostClientId();
    if (isHost) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "Global host: this client (%u)", hostId);
    } else if (hostId != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                           "Global host: client %u (Director state shown is cached "
                           "from the last DIRECTOR_STATE_SYNC received)", hostId);
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "Global host: unknown (pre-save-load)");
    }

    if (Anchor::Instance != nullptr) {
        ImGui::Text("Tick interval (EWMA): %u ms", Anchor::Instance->mAvgGameTickMs);
    }

    const auto& descriptors = director.GetDescriptors();
    int enabledCount = 0;
    for (const auto& d : descriptors) if (d->IsEnabled()) ++enabledCount;
    ImGui::Text("Descriptors: %d registered, %d enabled",
                (int)descriptors.size(), enabledCount);
}

void DirectorDebugWindow::DrawDescriptors() {
    ImGui::Text("Registered descriptors");
    ImGui::Indent();

    Director& director = Director::Instance();
    const auto& descriptors = director.GetDescriptors();
    if (descriptors.empty()) {
        ImGui::TextDisabled("(none)");
        ImGui::Unindent();
        return;
    }

    for (const auto& d : descriptors) {
        const uint8_t  descId  = d->GetDescriptorId();
        const char*    name    = d->GetDebugName();
        const bool     enabled = d->IsEnabled();
        const int      live    = director.GetLiveCount(descId);

        ImGui::PushID((int)descId);
        if (ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("ID: %u", descId);
            if (enabled) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Enabled");
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Disabled");
            }
            ImGui::Text("Live count: %d", live);

            const std::string snapshot = d->GetDebugSnapshotLine();
            if (!snapshot.empty()) {
                ImGui::Text("State: %s", snapshot.c_str());
            }

            // Per-descriptor custom rendering. Default is no-op; descriptors
            // override RenderDebugUI to add their own ImGui controls.
            d->RenderDebugUI(director);
        }
        ImGui::PopID();
    }

    ImGui::Unindent();
}

void DirectorDebugWindow::DrawSessionView() {
    if (Anchor::Instance == nullptr) {
        ImGui::TextDisabled("Session view unavailable (Anchor not initialised)");
        return;
    }

    ImGui::Text("Session players");
    ImGui::Indent();

    const auto& clients = Anchor::Instance->clients;
    int onlineCount = 0;
    for (const auto& [clientId, client] : clients) {
        if (!client.online) continue;
        ++onlineCount;

        const char* roleColor = client.self ? "[self]" : "[peer]";
        ImGui::Text("%s clientId=%u team='%s' scene=%d room=%d",
                    roleColor, clientId, client.teamId.c_str(),
                    (int)client.sceneNum, (int)client.curRoomNum);
        ImGui::SameLine();
        if (!client.isSaveLoaded) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(no save)");
        } else {
            ImGui::Text("pos=(%.0f, %.0f, %.0f)",
                        client.posRot.pos.x, client.posRot.pos.y, client.posRot.pos.z);
        }
        if (client.followerActive)         { ImGui::SameLine(); ImGui::TextDisabled("[follower]"); }
        if (client.invincibilityTimer > 0) { ImGui::SameLine(); ImGui::TextDisabled("[invuln]"); }
    }
    if (onlineCount == 0) {
        ImGui::TextDisabled("(no online players)");
    }

    ImGui::Unindent();
}

}  // namespace AnchorDirector
