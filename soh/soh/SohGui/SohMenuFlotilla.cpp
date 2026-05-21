#include "SohMenu.h"
#include "SohGui.hpp"
#include "soh/OTRGlobals.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/NavCVars.h"
#include "soh/Network/Anchor/Common/AINavTest.h"
#include "soh/resource/type/Skeleton.h"
#include <soh/Enhancements/RoomNavData/RoomNavData.h>
#include <filesystem>
#include <imgui.h>

extern "C" {
extern PlayState* gPlayState;
}

// Returns {"", <FolderA>, <FolderB>, ...} where "" represents Default Link / Default Voices.
// Each entry is the name of a subdirectory of coopplayercharacters/ (a sibling of mods/).
static std::vector<std::string> GetInstalledCoopModelMods_Flotilla() {
    std::vector<std::string> mods;
    mods.push_back("");
    std::string coopPath = Ship::Context::LocateFileAcrossAppDirs("coopplayercharacters", appShortName);
    std::filesystem::path coopDir(coopPath);
    if (std::filesystem::exists(coopDir) && std::filesystem::is_directory(coopDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(coopDir)) {
            if (entry.is_directory()) {
                mods.push_back(entry.path().filename().string());
            }
        }
    }
    return mods;
}

// AI Diagnostics — Navigation Test Harness statistics widget. Custom
// renderer displays per-actor stats (count, min, max, mean, median).
static void FlotillaNavTestStatsWidget(WidgetInfo& info) {
    using AINavTest::Stats;
    const auto& history = AINavTest::GetRunHistory();
    ImGui::TextUnformatted("Run history:");
    ImGui::Text("  Total runs: %d", (int)history.size());

    auto renderRow = [](const char* name, Stats s) {
        if (s.count == 0) {
            ImGui::Text("  %s: no completions", name);
        } else {
            ImGui::Text("  %s: count=%d  min=%dms  max=%dms  mean=%dms  median=%dms",
                        name, s.count, s.min, s.max, s.mean, s.median);
        }
    };
    renderRow("NPC Follower", AINavTest::ComputeNpcFollowerStats());
    renderRow("NPC Invader  ", AINavTest::ComputeAIInvaderStats());
    renderRow("AI Player Follower ", AINavTest::ComputeAIFollowerStats());

    if (!history.empty()) {
        const auto& last = history.back();
        ImGui::Separator();
        ImGui::TextUnformatted("Last run:");
        auto fmtMs = [](int ms) {
            return ms >= 0 ? std::to_string(ms) + "ms" : std::string("(not reached)");
        };
        ImGui::Text("  NPC Follower: %s", fmtMs(last.npcFollowerMs).c_str());
        ImGui::Text("  NPC Invader:   %s", fmtMs(last.aiInvaderMs).c_str());
        ImGui::Text("  AI Player Follower:  %s", fmtMs(last.aiFollowerMs).c_str());
        ImGui::Text("  Status:       %s",
                    last.completedOrDNF ? "Completed / DNF" : "In progress");
    }
}

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

void SohMenu::AddMenuFlotilla() {
    AddMenuEntry("Flotilla", CVAR_SETTING("Menu.FlotillaSidebarSection"));
    WidgetPath path;

    // -----------------------------------------------------------------
    // Player
    // -----------------------------------------------------------------
    path = { "Flotilla", "Player", SECTION_COLUMN_1 };
    AddSidebarEntry("Flotilla", path.sidebarName, 2);

    AddWidget(path, "Character", WIDGET_SEPARATOR_TEXT);

    // Character Model — picked from coopplayercharacters/ (sibling of mods/).
    // Works while connected: applies locally via UpdateCustomSkeletons() and
    // broadcasts to remote clients via UPDATE_CLIENT_STATE.
    AddWidget(path, "Character Model", WIDGET_CUSTOM)
        .HideInSearch(true)
        .CustomFunction([](WidgetInfo& info) {
            auto anchor = Anchor::Instance;
            static std::vector<std::string> mods;
            if (mods.empty()) {
                mods = GetInstalledCoopModelMods_Flotilla();
            }
            std::string currentModel = CVarGetString(CVAR_REMOTE_ANCHOR("CharacterModel"), "");

            static bool autoPopulateDone = false;
            if (!autoPopulateDone && currentModel.empty() && mods.size() > 1) {
                autoPopulateDone = true;
                currentModel = mods[1];
                CVarSetString(CVAR_REMOTE_ANCHOR("CharacterModel"), currentModel.c_str());
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
            if (!currentModel.empty()) {
                autoPopulateDone = true;
            }

            std::vector<const char*> displayNames;
            int currentIdx = 0;
            for (int i = 0; i < (int)mods.size(); i++) {
                displayNames.push_back(mods[i].empty() ? "Default Link" : mods[i].c_str());
                if (mods[i] == currentModel) currentIdx = i;
            }

            ImGui::Text("Character Model");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##CharacterModel", &currentIdx,
                             (const char* const*)displayNames.data(), (int)displayNames.size())) {
                CVarSetString(CVAR_REMOTE_ANCHOR("CharacterModel"), mods[currentIdx].c_str());
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                SOH::SkeletonPatcher::UpdateCustomSkeletons();
                if (anchor && anchor->isConnected) {
                    anchor->SendPacket_UpdateClientState();
                }
            }
        });

    // Voice Pack and Player Pronouns slots reserved here; backend code in
    // SohMenuSettings.cpp is still #if 0'd pending the audio routing and
    // verb-agreement work. Re-enable here when those land.

    AddWidget(path, "AI Player Follower (non-host only)", WIDGET_SEPARATOR_TEXT);

    // AI Player Follower — dev testing tool, non-host only. Activates the P2
    // shadow-AI that auto-follows P1 and engages nearby enemies. Hidden
    // when the local client is the effective host so the menu stays
    // consistent across host migrations.
    AddWidget(path, "AI Player Follower (AFK mode - controls Link)", WIDGET_CUSTOM)
        .HideInSearch(true)
        .CustomFunction([](WidgetInfo& info) {
            auto anchor = Anchor::Instance;
            if (!anchor || anchor->effectiveHostClientId == anchor->ownClientId) {
                return;
            }
            bool followerActive = anchor->IsFollowerActive();
            if (UIWidgets::Checkbox("AI Player Follower (AFK mode - controls Link)", &followerActive,
                                    UIWidgets::CheckboxOptions()
                                        .Color(THEME_COLOR)
                                        .Tooltip("P2 automatically follows P1 and engages nearby enemies. "
                                                 "Any controller input cancels it and returns manual control."))) {
                anchor->SetFollowerActive(followerActive);
            }
        });

    AddWidget(path, "Allow Choose Items", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_ANCHOR("FollowerAllowChooseItems"))
        .PreFunc([](WidgetInfo& info) {
            auto anchor = Anchor::Instance;
            info.isHidden = !anchor || anchor->effectiveHostClientId == anchor->ownClientId;
        })
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Allow the AI follower to temporarily assign items to C-buttons when it needs "
            "them (e.g., slingshot/bow for ranged combat against targets out of sword reach). "
            "The original loadout is restored when the follower finishes or deactivates."));

    AddWidget(path, "Recording", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Player Recorder Enabled", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("PlayerRecorder.Enabled"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Captures per-frame LOCAL PLAYER (Link) state to "
            "logs/Ship of Harkinian <N> player.log (sibling to the matching "
            "main log file <N>), one JSON object per line. Each line carries: "
            "scene/room, world position + shape rotation + world rotation, "
            "speedXZ, controller stick + button input + just-pressed buttons, "
            "all PLAYER_STATE1/2/3 flag bitmaps, decoded climb / ladder / "
            "hang-ledge / climb-ledge / crawl / damaged / talk / cutscene / "
            "input-disabled booleans, and the player's actionFunc pointer.\n\n"
            "Companion to the Follower Recorder. Record your own movement "
            "through a difficult area; compare the player's path to what the "
            "follower attempted; identify where the follower's logic falls "
            "short.\n\n"
            "Capture rate: gDeveloperTools.PlayerRecorder.CaptureHz (default 15). "
            "Auto-stop after gDeveloperTools.PlayerRecorder.MaxSeconds (default "
            "300 = 5 min) - the toggle flips back off when the cap is hit.\n\n"
            "Default: off. Zero overhead when disabled."));

    AddWidget(path, "Follower Recorder Enabled", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("FollowerRecorder.Enabled"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Captures per-frame follower decision state to "
            "logs/Ship of Harkinian <N> follower.log (sibling to the matching "
            "main log file <N>), one JSON object per line. Each line carries: "
            "scene/room, leader pos + state, follower pos + state-machine state "
            "+ state-frames, distances, NavPath presence + cursor, all G10/G12/"
            "G14/G15 timer values, autonomous-climb flag, and any teleport "
            "events fired this frame.\n\n"
            "Capture rate: gDeveloperTools.FollowerRecorder.CaptureHz (default 15). "
            "Auto-stop after gDeveloperTools.FollowerRecorder.MaxSeconds (default "
            "300 = 5 min) - the toggle flips back off when the cap is hit.\n\n"
            "Hand the recording file to the next debug session instead of "
            "describing what you saw.\n\n"
            "Default: off. Zero overhead when disabled."));

    // -----------------------------------------------------------------
    // Scene Info
    // -----------------------------------------------------------------
    path.sidebarName = "Scene Info";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 2);

    AddWidget(path, "Scene Log", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Manifest Logging Enabled", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("SceneLog.Level"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Master toggle for the Scene Log system. When on (Level >= 1), each room "
            "visited during play produces a per-room manifest at "
            "roommanifests/scene_<n>_room_<m>.json with: schemaVersion, scene, "
            "sceneName, room, provenance (build commit + version + date + visitCount), "
            "staticActors[] (actorId, name, params, pos, category), cutscenesObserved[] "
            "(populated when Cutscene_SetSegment fires), and otrResourceHashes (CRC64 "
            "of the scene + current-room OTR resources for content-hash freshness).\n\n"
            "Used in conjunction with SoH's built-in log: SoH log says \"Room Init - "
            "curRoom.num: 0xN\" -> look up roommanifests/scene_<scene>_room_<N>.json "
            "to confirm room contents during bug investigations.\n\n"
            "Default: off. Zero overhead when disabled."));

    AddWidget(path, "Auto-Traverse Full Game##SceneLog", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.OneClickStart"))
        .Callback([](WidgetInfo& info) {
            bool turningOn = CVarGetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.OneClickStart"), 0) != 0;
            if (turningOn) {
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.Level"), 1);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Cursor"), 0);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.MaxEntrance"), 65535);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.HoldFrames"), 120);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Mode"), 1);
            } else {
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Mode"), 0);
            }
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        })
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "One-click toggle for the auto-traverse data-capture pass. When ON: forces "
            "Manifest Logging Enabled, sets Cursor=0, MaxEntrance=65535 (entire entrance "
            "table), HoldFrames=120 (~2sec/entrance), then starts traversal. The game "
            "will warp through every primary entrance in the table (~390 unique "
            "entrances over ~13 minutes), populating roommanifests/scene_*_room_*.json "
            "and roomnavdata/roomnavdata_*_*.bin for every visited room. Status writes "
            "to roommanifests/_autotraverse_state.json on every advance.\n\n"
            "When OFF: stops the traversal (sets Mode=0 and resets session counters). "
            "The cursor preserves its current value - toggling back ON resumes from "
            "where you stopped.\n\n"
            "Prerequisite: must be in a loaded save (not at title screen / file "
            "select). The state machine will wait silently until gameplay begins."));

    // -----------------------------------------------------------------
    // Nav System
    //
    // Consolidated: removed pure-diagnostic toggles + advanced tuning sliders.
    // Phase-1 detection-only toggles (Ledge-Grab / Jump Anchor / Drop Anchor /
    // Crawlspace) live under Diagnostics until their Phase-2 consumers land.
    // Imply-on sub-CVars (Auto Scan / Path B Climb Detection / Auto-Expand /
    // Climbable Surfaces) are force-set when their master toggle goes ON.
    // -----------------------------------------------------------------
    path.sidebarName = "Nav System";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 3);

    AddWidget(path, "Improve enemy navigation.", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Nav System", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Enabled##NavSystem", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Nav.Enabled"))
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            // Master ON force-enables every sub-feature. Each was previously a
            // separate user toggle, but they're implementation-detail layers
            // (foundation captures + steering biases + fallback search) with
            // no scenario in which a user wants the master on but a specific
            // layer off. Console override remains available for bisect-class
            // debugging (`set gEnhancements.Nav.<Feature> 0`).
            if (CVarGetInteger(CVAR_ENHANCEMENT("Nav.Enabled"), 0)) {
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.ActorTrail"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.TargetSelection"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.GroundFollowing"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.ClimbableSurfaces"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.VerticalTeleport"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.LeashRespawn"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("Nav.RoomNavConsumer"), 1);
                CVarSave();
            }
        })
        .Options(CheckboxOptions().Tooltip(
            "Master toggle for the multiplayer navigation system. When on, all sub-"
            "features layer onto enemy AI: trail breadcrumbs, sticky target selection, "
            "ground-following steering, climbable-surface detection, vertical teleport, "
            "leash-driven respawn rehydration, and Layer-3 room-graph pathfinding. "
            "Bosses are categorically excluded from every consumer-side feature.\n\n"
            "Default: off."));

    AddWidget(path, "Room Nav Data", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Enabled##RoomNavData", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RoomNavData.Enabled"))
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) {
            // Force-enable the implied-on sub-CVars when the master turns on.
            // These were previously individual user toggles, but each is either
            // "recommended on" per its own tooltip (Path B) or a pure default-on
            // convenience (Auto Scan, Auto-Expand).
            if (CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0)) {
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoScan"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoExpandOnExploration"), 1);
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.PathBClimbDetection"), 1);
                CVarSave();
            }
        })
        .Options(CheckboxOptions().Tooltip(
            "Master toggle for the Room Nav Data system. When enabled, each room is "
            "scanned on first entry to produce a per-room navigation graph stored at "
            "roomnavdata/roomnavdata_<scene>_<room>.bin. Subsequent room entries load "
            "the cached graph from disk."));

    // Auto Refresh on Scene Flag — consolidated combobox replacing the two
    // mutually-exclusive Tier 1 + Tier 2 checkboxes. Tier 2 supersedes Tier 1
    // so writing both as "1" was always ambiguous; the combobox makes intent
    // explicit. CVars stay independent so console power-users can still set
    // them piecemeal.
    AddWidget(path, "Auto Refresh on Scene Flag", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .HideInSearch(true)
        .CustomFunction([](WidgetInfo& info) {
            int tier1 = CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoRefreshAnchorsOnSceneFlag"), 0);
            int tier2 = CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoFullRescanOnSceneFlag"), 0);
            int current = tier2 ? 2 : (tier1 ? 1 : 0);
            const char* items[] = {
                "Off",
                "Tier 1 - anchors only (~5-30ms)",
                "Tier 2 - full rescan (50-150ms)",
            };
            ImGui::Text("Auto Refresh on Scene Flag");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##NavAutoRefresh", &current, items, 3)) {
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoRefreshAnchorsOnSceneFlag"), current == 1 ? 1 : 0);
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.AutoFullRescanOnSceneFlag"), current == 2 ? 1 : 0);
                CVarSave();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How aggressively to refresh nav data when a scene flag is set "
                                  "(switch / treasure / cleared flags etc.):\n"
                                  "  Off    - no dynamic refresh; user must Force Rescan manually.\n"
                                  "  Tier 1 - anchor-only refresh (climb + ledge); fast, misses topology shifts.\n"
                                  "  Tier 2 - full rescan; catches push-block + moving-platform topology changes.");
            }
        });

    // Debug Draw Mode — consolidated combobox replacing DebugDraw + Components
    // + Computed Paths. Combined Components+Paths or other combinations are
    // still settable via console (CVars stay independent).
    AddWidget(path, "Debug Draw Mode", WIDGET_CUSTOM)
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .HideInSearch(true)
        .CustomFunction([](WidgetInfo& info) {
            int dd = CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDraw"), 0);
            int comp = CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDrawComponents"), 0);
            int paths = CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDrawPaths"), 0);
            int current = 0;
            if (dd && paths) current = 3;
            else if (dd && comp) current = 2;
            else if (dd) current = 1;
            const char* items[] = {
                "Off",
                "Walkable nodes",
                "Component coloring",
                "Computed paths",
            };
            ImGui::Text("Debug Draw Mode");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##NavDebugDraw", &current, items, 4)) {
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDraw"), current >= 1 ? 1 : 0);
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDrawComponents"), current == 2 ? 1 : 0);
                CVarSetInteger(CVAR_ENHANCEMENT("RoomNavData.DebugDrawPaths"), current == 3 ? 1 : 0);
                CVarSave();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("In-world overlay of the room's nav graph:\n"
                                  "  Walkable        - green quads + climb posts + edges.\n"
                                  "  Component       - per-connected-component palette (shows floodfill partitions).\n"
                                  "  Computed Paths  - teal posts at each waypoint of recent ComputePathTo results.");
            }
        });

    // -----------------------------------------------------------------
    // Game Director
    // -----------------------------------------------------------------
    path.sidebarName = "Game Director";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 1);

    AddWidget(path, "Director debug panel", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Open AI Director Debug Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("AIDirectorDebug"))
        .RaceDisable(false)
        .WindowName("AI Director Debug")
        .Options(WindowButtonOptions().Tooltip(
            "Floating window showing Director state: global-host status, registered "
            "descriptors with live counts and cooldown info, and session-view player "
            "snapshots.\n\n"
            "Read-only on non-host clients (state shown is the most recent "
            "DIRECTOR_STATE_SYNC received from the host)."));

    AddWidget(path, "Invader (hostile NPC)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Enable Invader spawns", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.Invaders.Enabled"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Master gate for the Invader descriptor - a host-authoritative spawner of "
            "hostile Link-look-alike NPCs that hunt your team. Inspired by Dark Souls "
            "invader summons.\n\n"
            "Requires the Nav System master toggle to also be on (the Invader's spawn-"
            "position selection consumes the Nav substrate). Default off."));

    AddWidget(path, "Max alive invaders: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_ENHANCEMENT("AI.Invaders.MaxAlive"))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.Enabled"), 0);
        })
        .RaceDisable(false)
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(4)
                     .DefaultValue(1)
                     .Format("%d")
                     .Tooltip(
            "Maximum simultaneous live invaders. Director will not propose another spawn "
            "until count drops below this. Plan default: 1."));

    AddWidget(path, "Cooldown: %ds", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_ENHANCEMENT("AI.Invaders.CooldownSeconds"))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("AI.Invaders.Enabled"), 0);
        })
        .RaceDisable(false)
        .Options(IntSliderOptions()
                     .Min(30)
                     .Max(600)
                     .DefaultValue(90)
                     .Format("%ds")
                     .Tooltip(
            "Minimum seconds between spawn attempts per (scene, room). Plan default: 90s."));

    AddWidget(path, "Enable TestDescriptor (dev)", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.Director.TestDescriptorEnabled"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Dev-only descriptor that spawns ACTOR_EN_TEST (Stalfos) at the most-isolated "
            "player every 30s, max 1 alive. Validates the full Director pipeline "
            "(proposal -> spawn -> ledger -> ENEMY_SPAWN broadcast -> ENEMY_DEFEATED -> "
            "cleanup) without an Invader actor existing."));

    AddWidget(path, "Diagnostic logging", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "In-world DebugDraw (red post at last spawn)", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.Director.DebugDraw"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Renders a red vertical post 60u tall in the world at each descriptor's most "
            "recent successfully-spawned actor position. Marker persists across kills."));
    AddWidget(path, "Log Proposals", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.Director.LogProposals"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Emits SPDLOG_INFO lines for per-tick descriptor proposal decisions: which "
            "gate blocked a spawn (live-count cap / no target / target invalid / cooldown), "
            "or when a proposal was offered. Throttled to once per ~5s per descriptor."));

    // -----------------------------------------------------------------
    // NPC Companion
    // -----------------------------------------------------------------
    path.sidebarName = "NPC Companion";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 3);

    AddWidget(path, "Friendly Link-skel NPC that walks beside you.", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "NPC Companion##FollowerNPC", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.FollowerNPC.Enabled"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Spawn a Link-skel NPC that walks beside you using player-like navigation. "
            "Pure pathfinding in v1 - no combat, invulnerable. Substrate path-driven; "
            "routes around obstacles instead of pressing into them.\n\n"
            "Independent of the AI Player Follower in Flotilla -> Player (which is the AFK-mode "
            "tool that hijacks Link's body and is non-host-only). The NPC Companion works "
            "in single-player AND as host.\n\n"
            "Toggle ON spawns the NPC at your current position; toggle OFF despawns. "
            "Auto-respawns on scene transitions.\n\n"
            "Requires: Nav System (master) + Use Nav Pathfinding + Use Room Graph "
            "(Layer 3) ticked under Nav System for substrate path consumption to work."));

    AddWidget(path, "Invulnerable##FollowerNPC", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.FollowerNPC.Invulnerable"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "When ON, the NPC Companion takes no damage from any source (environmental "
            "hazards, future combat damage, etc.). Default ON.\n\n"
            "Turn OFF to enable Stage 2 death/respawn:\n"
            "- Drowns after 30s in deep water\n"
            "- Dies on void fall (Y < -3000)\n"
            "- Respawns 10s later at the door closest to the leader\n"
            "  (or at the leader if no door is in range).\n\n"
            "Max HP mirrors Link's heart capacity (clamped to 3..20)."));

    // -----------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------
    path.sidebarName = "Diagnostics";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 1);

    AddWidget(path, "Navigation Test Harness", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Enable Navigation Test Harness", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.Enabled"))
        .Options(CheckboxOptions().Tooltip(
            "Master gate for the Navigation Test Harness. When enabled, "
            "NPC Invader / NPC Follower / AI Player Follower honor the "
            "harness's combat-disable + reach-detection logic. "
            "Disable when not testing - vanilla AI resumes."));

    AddWidget(path, "Disable Combat for AI Actors", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.CombatDisabled"))
        .Options(CheckboxOptions()
                     .Tooltip("When the harness is enabled, gate the engagement "
                              "tier dispatchers on all three AI actors so "
                              "ATTACK / RANGED_ATTACK / BLOCK never fire. "
                              "Keeps the locomotion trace clean.")
                     .DefaultValue(true));

    AddWidget(path, "Include AI Player Follower (P2)", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.IncludeAIFollower"))
        .Options(CheckboxOptions()
                     .Tooltip("Broadcast NAV_TEST_DIRECTIVE to P2 on Run Test "
                              "(P2 teleports to spawn + enables AI Player Follower mode). "
                              "Disable for single-client tests.")
                     .DefaultValue(true));

    AddWidget(path, "Set Spawn Point at Player Position", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Captures the player's current position + scene + room as "
            "the spawn point for AI test runs."))
        .Callback([](WidgetInfo& info) {
            AINavTest::SetSpawnPointAtPlayer(gPlayState);
        });

    AddWidget(path, "Run Test", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Spawn or relocate NPC Follower + NPC Invader at the spawn "
            "point, broadcast RUN to P2 (if Include AI Player Follower is on), "
            "and start the run timer. Press again to start a new run "
            "(actors are relocated, not respawned)."))
        .Callback([](WidgetInfo& info) {
            AINavTest::RunTest();
        });

    AddWidget(path, "Kill All Enemies in Current Room", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Calls Actor_Kill on every ACTORCAT_ENEMY actor in the "
            "current room EXCEPT NPC Invader instances (which are kept "
            "alive as test agents)."))
        .Callback([](WidgetInfo& info) {
            AINavTest::KillAllEnemiesInRoom(gPlayState);
        });

    AddWidget(path, "Statistics", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "NavTest Stats", WIDGET_CUSTOM)
        .CustomFunction(FlotillaNavTestStatsWidget)
        .HideInSearch(true);

    AddWidget(path, "Clear Run History", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Resets the run history vector. Statistics display shows "
            "fresh data after the next Run Test."))
        .Callback([](WidgetInfo& info) {
            AINavTest::ClearRunHistory();
        });

    // Nav Data Detection (Phase 1 detection-only). Each toggle lights up the
    // debug overlay for anchors of that type. Consumers that actually USE the
    // anchors land in Phase 2; until then these are pure visualization aids.
    // Hidden when Room Nav Data master is off (no graph to overlay).
    AddWidget(path, "Nav Data Detection (Phase 1)", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Ledge-Grab Detection", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RoomNavData.LedgeGrabDetection"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Scans for walkable-node pairs separated by 30-150u of vertical delta with a "
            "wall between them - the geometry signature of a ledge that Link can jump up "
            "and grab. Visualized as light-purple ground quads with a connecting post. "
            "Apply via Force Rescan after toggling."));

    AddWidget(path, "Jump Anchor Detection", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RoomNavData.JumpAnchorDetection"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Scans for platform-rim pairs separated by a true air gap. Same-altitude and "
            "upward jumps (up to broad-jump apex) are pre-baked into NavEdges. Visualized "
            "as orange ground quads with a connecting post. Apply via Force Rescan."));

    AddWidget(path, "Drop Anchor Detection", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RoomNavData.DropAnchorDetection"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Scans for walkable-node pairs separated by 30-200u of vertical drop with no "
            "wall between them - descent points where a navigator can step off and safely "
            "fall to a lower walkable area. Apply via Force Rescan."));

    AddWidget(path, "Crawlspace Detection", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RoomNavData.CrawlspaceDetection"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Scans static scene collision polys for the crawlspace wall-flag bits "
            "(4 + 5 - 0x30 mask). Each cluster produces one CrawlspaceAnchor. "
            "Apply via Force Rescan."));

    AddWidget(path, "Nav Data Maintenance", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Force Rescan Current Room", WIDGET_BUTTON)
        .Callback([](WidgetInfo& info) { AnchorNavRoom::ForceRescanCurrentRoom(); })
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_ENHANCEMENT("RoomNavData.Enabled"), 0); })
        .Options(ButtonOptions()
                     .Tooltip("Drops the cached nav data (in-memory + disk) for the room you're "
                              "currently in and re-scans on the next frame.")
                     .Size(UIWidgets::Sizes::Inline));
}

} // namespace SohGui
