#include "Anchor.h"
#include <libultraship/libultraship.h>
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/util.h"
#include "soh/OTRGlobals.h"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
extern std::shared_ptr<AnchorRoomWindow> mAnchorRoomWindow;
} // namespace SohGui

static const char* pvpModes[3] = { "Off", "On", "On + Friendly Fire" };
static std::vector<const char*> teleportModes = { "None", "Team Only", "All" };
static std::vector<const char*> showLocationsModes = { "None", "Team Only", "All" };

void AnchorMainMenu(WidgetInfo& info) {
    auto anchor = Anchor::Instance;

    std::string host = CVarGetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org");
    uint16_t port = CVarGetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383);
    std::string anchorTeamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    std::string anchorRoomId = CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), "");
    std::string anchorName = CVarGetString(CVAR_REMOTE_ANCHOR("Name"), "");
    bool isFormValid = !SohUtils::IsStringEmpty(host) && port > 1024 && port < 65535 &&
                       !SohUtils::IsStringEmpty(anchorRoomId) && !SohUtils::IsStringEmpty(anchorName);

    ImGui::SeparatorText("Connection Settings");

    ImGui::BeginDisabled(anchor->isEnabled);
    ImGui::Text("Host & Port");
    if (UIWidgets::InputString("##Host", &host,
                               UIWidgets::InputOptions()
                                   .Size(ImGui::GetContentRegionAvail() -
                                         ImVec2((ImGui::GetFontSize() * 5 + ImGui::GetStyle().ItemSpacing.x), 0))
                                   .Color(THEME_COLOR))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("Host"), host.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::SameLine();
    UIWidgets::PushStyleInput(THEME_COLOR);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::InputScalar("##Port", ImGuiDataType_U16, &port)) {
        CVarSetInteger(CVAR_REMOTE_ANCHOR("Port"), port);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    UIWidgets::PopStyleInput();

    ImGui::Text("Name & Color");
    static Color_RGBA8 defaultColor = { 100, 255, 100, 255 };
    UIWidgets::CVarColorPicker("##Color", CVAR_REMOTE_ANCHOR("Color"), defaultColor);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##Name", &anchorName, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("Name"), anchorName.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Room ID");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##RoomId", &anchorRoomId,
                               UIWidgets::InputOptions().IsSecret(anchor->isEnabled).Color(THEME_COLOR))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("RoomId"), anchorRoomId.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Team ID (Items & Flags Shared)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (UIWidgets::InputString("##TeamId", &anchorTeamId, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("TeamId"), anchorTeamId.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    if (UIWidgets::Button("Restore Defaults", UIWidgets::ButtonOptions()
                                                  .Size(ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))
                                                  .Color(UIWidgets::Colors::Red))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org");
        CVarSetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383);
        CVarSetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        CVarSetString(CVAR_REMOTE_ANCHOR("RoomId"), "");
        CVarSetString(CVAR_REMOTE_ANCHOR("Name"), "");
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::SameLine();

    if (UIWidgets::Button("Global Room", UIWidgets::ButtonOptions()
                                             .Color(UIWidgets::Colors::Blue)
                                             .Tooltip("Always-online public room so you don't have to experience "
                                                      "Hyrule alone. PVP and syncing are disabled."))) {
        CVarSetString(CVAR_REMOTE_ANCHOR("Host"), "anchor.hm64.org");
        CVarSetInteger(CVAR_REMOTE_ANCHOR("Port"), 43383);
        CVarSetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
        CVarSetString(CVAR_REMOTE_ANCHOR("RoomId"), "soh-global");
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::EndDisabled();

    ImGui::Spacing();

    ImGui::BeginDisabled(!isFormValid);
    const char* buttonLabel = anchor->isEnabled ? "Disable" : "Enable";
    UIWidgets::PushStyleButton(anchor->isEnabled ? UIWidgets::ColorValues.at(UIWidgets::Colors::Red)
                                                 : UIWidgets::ColorValues.at(UIWidgets::Colors::Green));
    if (ImGui::Button(buttonLabel, ImVec2(-1.0f, 0.0f))) {
        if (anchor->isEnabled) {
            CVarClear(CVAR_REMOTE_ANCHOR("Enabled"));
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            anchor->Disable();
        } else {
            CVarSetInteger(CVAR_REMOTE_ANCHOR("Enabled"), 1);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            anchor->Enable();
        }
    }
    UIWidgets::PopStyleButton();
    ImGui::EndDisabled();
    ImGui::Spacing();

    if (!anchor->isEnabled) {
        return;
    }

    if (!anchor->isConnected) {
        ImGui::Text("Connecting...");
        return;
    }

    ImGui::SeparatorText("Current Room");
    ImGui::Text("%s Connected", ICON_FA_CHECK);

    UIWidgets::PushStyleButton(THEME_COLOR);
    if (ImGui::Button("Request Team State")) {
        anchor->SendPacket_RequestTeamState();
    }
    UIWidgets::Tooltip("Try this if you are missing items or flags that your team members have collected");
    UIWidgets::PopStyleButton();

    ImGui::SameLine();

    UIWidgets::WindowButton("Toggle Anchor Room Window", CVAR_WINDOW("AnchorRoom"), SohGui::mAnchorRoomWindow);

    ImGui::Spacing();

    bool hideLocations = Anchor::Instance->roomState.showLocationsMode == 0;
    ImGui::BeginDisabled(hideLocations);
    UIWidgets::CVarCheckbox(
        "Show Other Players on Minimap", CVAR_REMOTE_ANCHOR("ShowOtherPlayersOnMinimap"),
        UIWidgets::CheckboxOptions()
            .Color(THEME_COLOR)
            .DefaultValue(true)
            .Tooltip(!hideLocations
                         ? "Other players will appear on the minimap in areas where you have the compass. "
                           "Visibility is restricted according to the Show Locations mode for the room."
                         : "Cannot show other players because the room's Show Locations mode is set to None."));
    ImGui::EndDisabled();

    // AI Follower — dev testing tool, non-host only.
    // Activates the P2 shadow-AI that auto-follows P1 and engages nearby enemies.
    // Only visible when not the effective host; any controller input cancels it
    // mid-session. Pillar A Phase 1 — gate on effective-host so the menu stays
    // consistent when the original room owner has disconnected.
    if (anchor->effectiveHostClientId != anchor->ownClientId) {
        ImGui::Spacing();
        ImGui::SeparatorText("Developer Tools");
        bool followerActive = anchor->IsFollowerActive();
        if (UIWidgets::Checkbox("AI Follower", &followerActive,
                                UIWidgets::CheckboxOptions()
                                    .Color(THEME_COLOR)
                                    .Tooltip("P2 automatically follows P1 and engages nearby enemies. "
                                             "Any controller input cancels it and returns manual control."))) {
            anchor->SetFollowerActive(followerActive);
        }

        // Option B — allow the follower to temporarily override the player's
        // C-button items when it needs something (slingshot/bow for ranged
        // combat, fairy/potion for the future revive system). The original
        // loadout is snapshot at first override and restored on state exit
        // and on any follower deactivation.
        bool allowChooseItems =
            CVarGetInteger(CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems"), 0) != 0;
        if (UIWidgets::Checkbox("Allow Choose Items", &allowChooseItems,
                                UIWidgets::CheckboxOptions()
                                    .Color(THEME_COLOR)
                                    .Tooltip("Allow the AI follower to temporarily assign items to C-buttons "
                                             "when it needs them (e.g., slingshot/bow for ranged combat against "
                                             "targets out of sword reach). The original loadout is restored when "
                                             "the follower finishes or deactivates."))) {
            CVarSetInteger(CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems"),
                           allowChooseItems ? 1 : 0);
            CVarSave();
        }

        // Nav data usage. Mirrors the canonical CVars under
        // gEnhancements.Nav.* / gEnhancements.RoomNavData.* — flipping
        // these here is identical to flipping them in
        // Enhancements → Multiplayer Nav. Surfaced here so they're reachable
        // in the same place the follower itself is enabled. All default
        // off per the vanilla-altering-feature rule.
        //
        // Each toggle is independently load-bearing for a different layer
        // of ComputePathTo (the substrate's path planner):
        //   - Nav System          → master gate; everything else is no-op
        //                           when this is off
        //   - Pathfinding         → consumer gate; without this the follower
        //                           uses legacy direct-yaw pursuit even with
        //                           the layers below enabled
        //   - Trail Breadcrumbs   → Layer 2 of ComputePathTo; without this
        //                           the trail-walk fallback is empty
        //   - Room Graph (BFS)    → Layer 3 of ComputePathTo; without this
        //                           the BFS fallback is no-op
        //   - Pre-scan Room Data  → data layer that Layer 3 reads from;
        //                           without this Layer 3 has no graph
        //   - Vertical Teleport   → Shape A wrapper around the climb
        //                           pipeline (ladders/vines/hang-state
        //                           resolution)
        ImGui::SeparatorText("Nav Data Usage");

        struct NavToggle {
            const char* label;
            const char* cvar;
            const char* tooltip;
        };
        static const NavToggle kFollowerNavToggles[] = {
            { "Nav System (master)",
              CVAR_ENHANCEMENT("Nav.Enabled"),
              "Master gate for the whole multiplayer nav system. When off, every "
              "other nav toggle below is a no-op and the follower falls back to "
              "legacy direct-yaw pursuit." },
            { "Use Nav Pathfinding",
              CVAR_ENHANCEMENT("Nav.AiFollowerConsumer"),
              "Routes follower pursuit (FOLLOW / RETURN / ENGAGE / STUCK) through "
              "the nav substrate's ComputePathTo planner instead of the legacy "
              "bespoke direct-yaw steering. Requires Nav System on." },
            { "Track Trail Breadcrumbs (Layer 2)",
              CVAR_ENHANCEMENT("Nav.ActorTrail"),
              "Captures recent leader / enemy / follower positions at 30 Hz. "
              "ComputePathTo's Layer 2 walks the leader's trail to find a "
              "reachable breadcrumb when direct line-of-sight pursuit is blocked. "
              "Requires Nav System on." },
            { "Use Room Graph (Layer 3)",
              CVAR_ENHANCEMENT("Nav.RoomNavConsumer"),
              "ComputePathTo's Layer 3 consults the pre-scanned RoomNavData graph "
              "(BFS over walkable nodes) when LOS and trail layers fail. The "
              "load-bearing fallback for cross-room and obstacle-rich pursuit. "
              "Requires Nav System on AND Pre-scan Room Data on." },
            { "Pre-scan Room Data (data layer)",
              CVAR_ENHANCEMENT("RoomNavData.Enabled"),
              "Pre-scans each visited room into a walkable-node graph stored on "
              "disk under roomnavdata/. The Use Room Graph toggle above reads "
              "from this. Recommended on whenever Use Room Graph is on." },
            { "Detect Vine / Static Climb Surfaces (Path B)",
              CVAR_ENHANCEMENT("RoomNavData.PathBClimbDetection"),
              "Scan static scene collision for climbable wall flags (vine walls "
              "and any climbable surface baked into static geometry). Without "
              "this toggle, only discrete ladder ACTORS (Path A allowlist: "
              "Goron City interior ladder, Dodongo's Cavern ladder, Inside Deku "
              "Tree falling ladder) get climb anchors and grids — vine walls "
              "stay invisible to the nav system. Required for the climb-surface "
              "nav grid (schema v7) to cover vines. Recommended ON whenever "
              "Pre-scan Room Data is on." },
            { "Use Vertical Teleport (climb)",
              CVAR_ENHANCEMENT("Nav.VerticalTeleport"),
              "Wraps the existing follower CLIMBING pipeline with the nav "
              "substrate's Shape A logic (real ladder/vine animation + hang-"
              "state BTN_A/BTN_B resolution). Requires Nav System on." },
        };
        for (const auto& t : kFollowerNavToggles) {
            bool v = CVarGetInteger(t.cvar, 0) != 0;
            if (UIWidgets::Checkbox(t.label, &v,
                                    UIWidgets::CheckboxOptions()
                                        .Color(THEME_COLOR)
                                        .Tooltip(t.tooltip))) {
                CVarSetInteger(t.cvar, v ? 1 : 0);
                CVarSave();
            }
        }
    }

    ImGui::Spacing();

    if (!SohGui::mAnchorRoomWindow->IsVisible()) {
        SohGui::mAnchorRoomWindow->DrawElement();
    }
}

void AnchorAdminMenu(WidgetInfo& info) {
    auto anchor = Anchor::Instance;
    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!anchor->isEnabled || !anchor->isConnected || anchor->roomState.ownerClientId != anchor->ownClientId ||
        isGlobalRoom) {
        return;
    }

    ImGui::SeparatorText("Room Settings (Admin Only)");

    UIWidgets::PushStyleButton(THEME_COLOR);
    if (ImGui::Button("Clear All Team State")) {
        std::set<std::string> teams;
        for (auto& [clientId, client] : Anchor::Instance->clients) {
            teams.insert(client.teamId);
        }
        for (auto& team : teams) {
            anchor->SendPacket_ClearTeamState(team);
        }
    }
    UIWidgets::PopStyleButton();

    if (UIWidgets::CVarCombobox("PvP Mode:", CVAR_REMOTE_ANCHOR("RoomSettings.PvpMode"), pvpModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .LabelPosition(UIWidgets::LabelPositions::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCombobox("Show Locations For:", CVAR_REMOTE_ANCHOR("RoomSettings.ShowLocationsMode"),
                                showLocationsModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .LabelPosition(UIWidgets::LabelPositions::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCombobox("Allow Teleporting To:", CVAR_REMOTE_ANCHOR("RoomSettings.TeleportMode"), teleportModes,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .LabelPosition(UIWidgets::LabelPositions::Above)
                                    .Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
    if (UIWidgets::CVarCheckbox("Sync Items & Flags", CVAR_REMOTE_ANCHOR("RoomSettings.SyncItemsAndFlags"),
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR))) {
        anchor->SendPacket_UpdateRoomState();
    }
}

void AnchorInstructionsMenu(WidgetInfo& info) {
    auto anchor = Anchor::Instance;

    ImGui::SeparatorText("Usage Instructions");

    ImGui::TextWrapped("1. All players involved should start at the file select screen");

    ImGui::TextWrapped("2. Come up with a unique Room ID (this is basically your password) and enter it, along with "
                       "your desired player name and team ID and click Enable");

    ImGui::TextWrapped("3. The host should configure the randomizer settings and generate a seed, then share the newly "
                       "generated JSON spoiler file with other players.");

    ImGui::TextWrapped("4. All players should load the same JSON spoiler file (drag it into SoH window), make sure "
                       "seed icons match, then create a new file.");

    ImGui::TextWrapped("5. All players should now load into their game. IMPORTANT! If using an existing save/seed "
                       "ensure the player with the most progress loads the file first.");

    ImGui::TextWrapped("6. After everyone has loaded in, verify on the network tab that it doesn't warn about anyone "
                       "being on a wrong version or seed.");

    ImGui::Spacing();

    ImGui::TextWrapped(
        "Note: Team ID is used to group players together in the same team, sharing items and flags. Make sure all "
        "players who want to share progress use the same Team ID. All players with the same Team ID should be using "
        "the same randomizer seed, while players on different teams can use different seeds.");
}

#ifdef ENABLE_REMOTE_CONTROL
void RegisterAnchorMenu() {
    WidgetPath path = { "Network", "Anchor", SECTION_COLUMN_1 };
    SohGui::mSohMenu->AddWidget(path, "AnchorMainMenu", WIDGET_CUSTOM)
        .CustomFunction(AnchorMainMenu)
        .HideInSearch(true);
    path.column = SECTION_COLUMN_2;
    SohGui::mSohMenu->AddWidget(path, "AnchorAdminMenu", WIDGET_CUSTOM)
        .CustomFunction(AnchorAdminMenu)
        .HideInSearch(true);
    SohGui::mSohMenu->AddWidget(path, "AnchorInstructionsMenu", WIDGET_CUSTOM)
        .CustomFunction(AnchorInstructionsMenu)
        .HideInSearch(true);
}

static RegisterMenuInitFunc menuInitFunc(RegisterAnchorMenu);
#endif
