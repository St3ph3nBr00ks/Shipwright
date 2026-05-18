#include "SohMenu.h"
#include "SohGui.hpp"

#include "soh/Network/Anchor/Common/AINavTest.h"

#include <imgui.h>

extern "C" {
extern PlayState* gPlayState;
}

void WarpPointsWidget(WidgetInfo& info);

// AI Diagnostics — Navigation Test Harness statistics widget. Custom
// renderer displays per-actor stats (count, min, max, mean, median).
static void NavTestStatsWidget(WidgetInfo& info) {
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
    renderRow("AI Invader  ", AINavTest::ComputeAIInvaderStats());
    renderRow("AI Follower ", AINavTest::ComputeAIFollowerStats());

    if (!history.empty()) {
        const auto& last = history.back();
        ImGui::Separator();
        ImGui::TextUnformatted("Last run:");
        auto fmtMs = [](int ms) {
            return ms >= 0 ? std::to_string(ms) + "ms" : std::string("(not reached)");
        };
        ImGui::Text("  NPC Follower: %s", fmtMs(last.npcFollowerMs).c_str());
        ImGui::Text("  AI Invader:   %s", fmtMs(last.aiInvaderMs).c_str());
        ImGui::Text("  AI Follower:  %s", fmtMs(last.aiFollowerMs).c_str());
        ImGui::Text("  Status:       %s",
                    last.completedOrDNF ? "Completed / DNF" : "In progress");
    }
}

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

static const std::map<int32_t, const char*> logLevels = {
    { DEBUG_LOG_TRACE, "Trace" }, { DEBUG_LOG_DEBUG, "Debug" }, { DEBUG_LOG_INFO, "Info" },
    { DEBUG_LOG_WARN, "Warn" },   { DEBUG_LOG_ERROR, "Error" }, { DEBUG_LOG_CRITICAL, "Critical" },
    { DEBUG_LOG_OFF, "Off" },
};

#ifdef _DEBUG
DebugLogOption defaultLogLevel = DEBUG_LOG_TRACE;
#else
DebugLogOption defaultLogLevel = DEBUG_LOG_INFO;
#endif

static const std::map<int32_t, const char*> debugSaveFileModes = {
    { 0, "Off" },
    { 1, "Vanilla" },
    { 2, "Maxed" },
};

void SohMenu::AddMenuDevTools() {
    // Add Dev Tools Menu
    AddMenuEntry("Dev Tools", CVAR_SETTING("Menu.DevToolsSidebarSection"));

    // General
    AddSidebarEntry("Dev Tools", "General", 3);
    WidgetPath path = { "Dev Tools", "General", SECTION_COLUMN_1 };

    AddWidget(path, "Popout Menu", WIDGET_CVAR_CHECKBOX)
        .CVar("gSettings.Menu.Popout")
        .Options(CheckboxOptions().Tooltip("Changes the menu display from overlay to windowed."));
    AddWidget(path, "Debug Mode", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugEnabled"))
        .Options(
            CheckboxOptions().Tooltip("Enables Debug Mode, allowing you to select maps with L + R + Z, noclip "
                                      "with L + D-pad Right, and open the debug menu with L on the pause screen."));
    AddWidget(path, "Map Select Button Combination:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar("gDeveloperTools.MapSelectBtn")
        .Options(BtnSelectorOptions().DefaultValue(BTN_R | BTN_L | BTN_Z))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); });
    AddWidget(path, "No Clip Button Combination:", WIDGET_CVAR_BTN_SELECTOR)
        .CVar("gDeveloperTools.NoClipBtn")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(BtnSelectorOptions().DefaultValue(BTN_L | BTN_DRIGHT));
    AddWidget(path, "OoT Registry Editor", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("RegEditEnabled"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(CheckboxOptions().Tooltip("Enables the registry editor."));
    AddWidget(path, "Debug Save File Mode", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugSaveFileMode"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(ComboboxOptions()
                     .Tooltip("Changes the behavior of debug file select creation (creating a save file on slot 1 "
                              "with debug mode on):\n"
                              "- Off: The debug save file will be a normal savefile.\n"
                              "- Vanilla: The debug save file will be the debug save file from the original game.\n"
                              "- Maxed: The debug save file will be a save file with all of the items & upgrades.")
                     .ComboMap(debugSaveFileModes)
                     .DefaultIndex(1));
    AddWidget(path, "OoT Skulltula Debug", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("SkulltulaDebugEnabled"))
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0); })
        .Options(CheckboxOptions().Tooltip("Enables Skulltula Debug, when moving the cursor in the menu above various "
                                           "map icons (boss key, compass, map screen locations, etc.) will set the GS "
                                           "bits in that area.\nUSE WITH CAUTION AS IT DOES NOT UPDATE THE GS COUNT!"));
    AddWidget(path, "Resource logging", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("ResourceLogging"))
        .Options(CheckboxOptions().Tooltip("Logs some resources as XML when they're loaded in binary format."));

    AddWidget(path, "Frame Advance", WIDGET_CHECKBOX)
        .Options(CheckboxOptions().Tooltip(
            "This allows you to advance through the game one frame at a time on command. "
            "To advance a frame, hold Z and tap R on the second controller. Holding Z "
            "and R will advance a frame every half second. You can also use the buttons below."))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_NULL_PLAY_STATE).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
            if (gPlayState != nullptr) {
                info.valuePointer = (bool*)&gPlayState->frameAdvCtx.enabled;
            } else {
                info.valuePointer = (bool*)nullptr;
            }
        });
    AddWidget(path, "Advance 1", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Advance 1 frame.").Size(Sizes::Inline))
        .Callback([](WidgetInfo& info) { CVarSetInteger(CVAR_DEVELOPER_TOOLS("FrameAdvanceTick"), 1); })
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_FRAME_ADVANCE_OFF).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
        });
    AddWidget(path, "Advance (Hold)", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Advance frames while the button is held.").Size(Sizes::Inline))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = mSohMenu->disabledMap.at(DISABLE_FOR_FRAME_ADVANCE_OFF).active ||
                            mSohMenu->disabledMap.at(DISABLE_FOR_DEBUG_MODE_OFF).active;
        })
        .PostFunc([](WidgetInfo& info) {
            if (ImGui::IsItemActive()) {
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("FrameAdvanceTick"), 1);
            }
        })
        .SameLine(true);
    AddWidget(path, "Log Level", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("LogLevel"))
        .Options(ComboboxOptions()
                     .Tooltip("The log level determines which messages are printed to the console."
                              " This does not affect the log file output")
                     .ComboMap(logLevels)
                     .DefaultIndex(defaultLogLevel))
        .Callback([](WidgetInfo& info) {
            Ship::Context::GetInstance()->GetLogger()->set_level(
                (spdlog::level::level_enum)CVarGetInteger(CVAR_DEVELOPER_TOOLS("LogLevel"), defaultLogLevel));
        });

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Warping", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Better Debug Warp Screen", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("BetterDebugWarpScreen"))
        .Options(CheckboxOptions()
                     .Tooltip("Optimized Debug Warp Screen, with the added ability to chose entrances and time of day.")
                     .DefaultValue(true));
    AddWidget(path, "Debug Warp Screen Translation", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("DebugWarpScreenTranslation"))
        .Options(CheckboxOptions()
                     .Tooltip("Translate the Debug Warp Screen based on the game language.")
                     .DefaultValue(true));
    AddWidget(path, "Warp Points", WIDGET_CUSTOM).CustomFunction(WarpPointsWidget).HideInSearch(true);

    // Scene Log / Auto-Traverse — one-click full-game data capture.
    // See Plans/agent_brief_scenelog_completion.md for the full design.
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
            "curRoom.num: 0xN\" → look up roommanifests/scene_<scene>_room_<N>.json "
            "to confirm room contents during bug investigations.\n\n"
            "Default: off. Zero overhead when disabled."));

    AddWidget(path, "Auto-Traverse Full Game##SceneLog", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.OneClickStart"))
        .Callback([](WidgetInfo& info) {
            bool turningOn = CVarGetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.OneClickStart"), 0) != 0;
            if (turningOn) {
                // Force-enable the manifest logger so the auto-traverse data
                // actually lands on disk. User can later turn the master log
                // toggle off but auto-traverse needs it on while running.
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.Level"), 1);
                // Set defaults for a full-game traversal. Order matters:
                // Mode is set LAST so the state machine picks up valid
                // Cursor/MaxEntrance/HoldFrames on session-init.
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Cursor"), 0);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.MaxEntrance"), 65535);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.HoldFrames"), 120);
                CVarSetInteger(CVAR_DEVELOPER_TOOLS("SceneLog.AutoTraverse.Mode"), 1);
            } else {
                // Stop the traversal. Mode=0 also resets the session
                // counters in OnFrameTick (sSessionInitialized flips
                // back to false).
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
            "The cursor preserves its current value — toggling back ON resumes from "
            "where you stopped.\n\n"
            "Prerequisite: must be in a loaded save (not at title screen / file "
            "select). The state machine will wait silently until gameplay begins."));

    // AI Follower Recorder — per-frame JSONL capture of follower decision
    // state for post-hoc debug analysis. See Plans/follower_recorder_plan.md.
    AddWidget(path, "AI Follower Recorder", WIDGET_SEPARATOR_TEXT);

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
            "300 = 5 min) — the toggle flips back off when the cap is hit.\n\n"
            "Hand the recording file to the next debug session instead of "
            "describing what you saw.\n\n"
            "Default: off. Zero overhead when disabled."));

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
            "300 = 5 min) — the toggle flips back off when the cap is hit.\n\n"
            "Default: off. Zero overhead when disabled."));

    // Stats
    path.sidebarName = "Stats";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Stats Window", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohStats"))
        .RaceDisable(false)
        .WindowName("Stats##Soh")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Stats Window."));

    // Console
    path.sidebarName = "Console";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Console", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohConsole"))
        .WindowName("Console##SoH")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Console Window."));

    // Save Editor
    path.sidebarName = "Save Editor";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Save Editor", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SaveEditor"))
        .WindowName("Save Editor")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Save Editor Window."));

    // Hook Debugger
    path.sidebarName = "Hook Debugger";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Hook Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("HookDebugger"))
        .WindowName("Hook Debugger")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Hook Debugger Window."));

    // Collision Viewer
    path.sidebarName = "Collision Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Collision Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("CollisionViewer"))
        .WindowName("Collision Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Collision Viewer Window."));

    // Actor Viewer
    path.sidebarName = "Actor Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Actor Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ActorViewer"))
        .WindowName("Actor Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Actor Viewer Window."));

    // Display List Viewer
    path.sidebarName = "DList Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Display List Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("DisplayListViewer"))
        .WindowName("Display List Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Display List Viewer Window."));

    // Value Viewer
    path.sidebarName = "Value Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Value Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("ValueViewer"))
        .WindowName("Value Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Value Viewer Window."));

    // Message Viewer
    path.sidebarName = "Message Viewer";
    AddSidebarEntry("Dev Tools", path.sidebarName, 2);
    AddWidget(path, "Popout Message Viewer", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("MessageViewer"))
        .WindowName("Message Viewer")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Message Viewer Window."));

    // Gfx Debugger
    path.sidebarName = "Gfx Debugger";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);
    AddWidget(path, "Popout Gfx Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_WINDOW("SohGfxDebugger"))
        .WindowName("GfxDebugger##SoH")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Enables the separate Gfx Debugger Window."));

    // AI Diagnostics — Navigation Test Harness.
    // Plan: Claude/Plans/ai_nav_test_harness_plan.md
    path.sidebarName = "AI Diagnostics";
    AddSidebarEntry("Dev Tools", path.sidebarName, 1);

    AddWidget(path, "Navigation Test Harness", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Enable Navigation Test Harness", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.Enabled"))
        .Options(CheckboxOptions().Tooltip(
            "Master gate for the Navigation Test Harness. When enabled, "
            "AI Invader / NPC Follower / Player AI Follower honor the "
            "harness's combat-disable + reach-detection logic. "
            "Disable when not testing — vanilla AI resumes."));

    AddWidget(path, "Disable Combat for AI Actors", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.CombatDisabled"))
        .Options(CheckboxOptions()
                     .Tooltip("When the harness is enabled, gate the engagement "
                              "tier dispatchers on all three AI actors so "
                              "ATTACK / RANGED_ATTACK / BLOCK never fire. "
                              "Keeps the locomotion trace clean.")
                     .DefaultValue(true));

    AddWidget(path, "Include AI Follower (P2)", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("AI.NavTest.IncludeAIFollower"))
        .Options(CheckboxOptions()
                     .Tooltip("Broadcast NAV_TEST_DIRECTIVE to P2 on Run Test "
                              "(P2 teleports to spawn + enables AI Follower mode). "
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
            "Spawn or relocate NPC Follower + AI Invader at the spawn "
            "point, broadcast RUN to P2 (if Include AI Follower is on), "
            "and start the run timer. Press again to start a new run "
            "(actors are relocated, not respawned)."))
        .Callback([](WidgetInfo& info) {
            AINavTest::RunTest();
        });

    AddWidget(path, "Kill All Enemies in Current Room", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Calls Actor_Kill on every ACTORCAT_ENEMY actor in the "
            "current room EXCEPT AI Invader instances (which are kept "
            "alive as test agents)."))
        .Callback([](WidgetInfo& info) {
            AINavTest::KillAllEnemiesInRoom(gPlayState);
        });

    AddWidget(path, "Statistics", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "NavTest Stats", WIDGET_CUSTOM)
        .CustomFunction(NavTestStatsWidget)
        .HideInSearch(true);

    AddWidget(path, "Clear Run History", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip(
            "Resets the run history vector. Statistics display shows "
            "fresh data after the next Run Test."))
        .Callback([](WidgetInfo& info) {
            AINavTest::ClearRunHistory();
        });
}

} // namespace SohGui
