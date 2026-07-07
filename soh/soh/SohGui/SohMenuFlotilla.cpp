#include "SohMenu.h"
#include "SohGui.hpp"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/Enhancements/audio/VoicePack.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/Common/EnforcedCVars.h"
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

// Voice-pack analog of GetInstalledCoopModelMods_Flotilla.  Returns {"",
// <FolderA>, ...} where "" represents Default Voices.  Each entry is the
// name of a subdirectory of player-voices/ (a sibling of coopplayercharacters/
// and mods/).  Issues #83 / #84.
static std::vector<std::string> GetInstalledVoicePacks_Flotilla() {
    std::vector<std::string> packs;
    packs.push_back("");
    std::string voicePath = Ship::Context::LocateFileAcrossAppDirs("player-voices", appShortName);
    std::filesystem::path voiceDir(voicePath);
    if (std::filesystem::exists(voiceDir) && std::filesystem::is_directory(voiceDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(voiceDir)) {
            if (entry.is_directory()) {
                packs.push_back(entry.path().filename().string());
            }
        }
    }
    return packs;
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

// Combobox maps for the Phase 3 Flotilla → Host Settings sidebar.
// Duplicated from SohMenuEnhancements.cpp's file-static maps so widgets
// here don't reach into a different TU's anonymous namespace. The same
// underlying CVar values back both widgets — host's enforced value
// always wins via AnchorCVarSync::GetEnforcedInt regardless of which
// widget the user clicked.
static const std::map<int32_t, const char*> sFlotillaTimeTravelOptions = {
    { TIME_TRAVEL_DISABLED, "Disabled" },
    { TIME_TRAVEL_OOT, "Ocarina of Time" },
    { TIME_TRAVEL_OOT_MS, "Ocarina of Time + Master Sword" },
    { TIME_TRAVEL_ANY, "Any Ocarina" },
    { TIME_TRAVEL_ANY_MS, "Any Ocarina + Master Sword" },
};

static const std::map<int32_t, const char*> sFlotillaDamageMultPowers = {
    { DAMAGE_VANILLA, "Vanilla (1x)" },      { DAMAGE_DOUBLE, "Double (2x)" },
    { DAMAGE_QUADRUPLE, "Quadruple (4x)" },  { DAMAGE_OCTUPLE, "Octuple (8x)" },
    { DAMAGE_FOOLISH, "Foolish (16x)" },     { DAMAGE_RIDICULOUS, "Ridiculous (32x)" },
    { DAMAGE_MERCILESS, "Merciless (64x)" }, { DAMAGE_TORTURE, "Pure Torture (128x)" },
    { DAMAGE_OHKO, "OHKO (256x)" },
};

static const std::map<int32_t, const char*> sFlotillaFallDamagePowers = {
    { DAMAGE_VANILLA, "Vanilla (1x)" },      { DAMAGE_DOUBLE, "Double (2x)" },
    { DAMAGE_QUADRUPLE, "Quadruple (4x)" },  { DAMAGE_OCTUPLE, "Octuple (8x)" },
    { DAMAGE_FOOLISH, "Foolish (16x)" },     { DAMAGE_RIDICULOUS, "Ridiculous (32x)" },
    { DAMAGE_MERCILESS, "Merciless (64x)" }, { DAMAGE_TORTURE, "Pure Torture (128x)" },
};

static const std::map<int32_t, const char*> sFlotillaVoidDamagePowers = {
    { DAMAGE_VANILLA, "Vanilla (1x)" },      { DAMAGE_DOUBLE, "Double (2x)" },
    { DAMAGE_QUADRUPLE, "Quadruple (4x)" },  { DAMAGE_OCTUPLE, "Octuple (8x)" },
    { DAMAGE_FOOLISH, "Foolish (16x)" },     { DAMAGE_RIDICULOUS, "Ridiculous (32x)" },
    { DAMAGE_MERCILESS, "Merciless (64x)" },
};

static const std::map<int32_t, const char*> sFlotillaBonkDamageValues = {
    { BONK_DAMAGE_NONE, "No Damage" },        { BONK_DAMAGE_QUARTER_HEART, "0.25 Hearts" },
    { BONK_DAMAGE_HALF_HEART, "0.5 Hearts" }, { BONK_DAMAGE_1_HEART, "1 Heart" },
    { BONK_DAMAGE_2_HEARTS, "2 Hearts" },     { BONK_DAMAGE_4_HEARTS, "4 Hearts" },
    { BONK_DAMAGE_8_HEARTS, "8 Hearts" },     { BONK_DAMAGE_OHKO, "OHKO" },
};

// Shared PreFunc — disables the widget when the local client is not
// the owner of an active non-global session. Centralised so every
// enforced widget in the sidebar uses the same evaluation.
static void FlotillaHostSettingsPreFunc(WidgetInfo& info) {
    if (mSohMenu->GetDisabledMap().at(DISABLE_FOR_ANCHOR_NOT_OWNER).active) {
        info.activeDisables.push_back(DISABLE_FOR_ANCHOR_NOT_OWNER);
    }
}

// Shared Callback — fires an immediate UPDATE_ROOM_STATE so peers see
// the change within the relay RTT (~100ms) rather than waiting for the
// 1-second auto-poll. No-op on non-owner / disconnected (gated inside
// TriggerOwnerBroadcastNow).
static void FlotillaHostSettingsCallback(WidgetInfo& info) {
    AnchorCVarSync::TriggerOwnerBroadcastNow();
}

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
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
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
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                SOH::SkeletonPatcher::UpdateCustomSkeletons();
                if (anchor && anchor->isConnected) {
                    anchor->SendPacket_UpdateClientState();
                }
            }
        });

    // Voice Pack — picked from player-voices/ (sibling of coopplayercharacters/).
    // Pack swap triggers loader reload via VoicePack_OnAudioModChanged and
    // broadcasts the selection over UPDATE_CLIENT_STATE so peers can apply
    // per-emitter routing when their B3 receive lands.  Issues #83 / #84.
    AddWidget(path, "Voice Pack", WIDGET_CUSTOM)
        .HideInSearch(true)
        .CustomFunction([](WidgetInfo& info) {
            auto anchor = Anchor::Instance;
            static std::vector<std::string> packs;
            if (packs.empty()) {
                packs = GetInstalledVoicePacks_Flotilla();
            }
            std::string currentPack = CVarGetString(CVAR_REMOTE_ANCHOR("AudioMod"), "");

            std::vector<const char*> displayNames;
            int currentIdx = 0;
            for (int i = 0; i < (int)packs.size(); i++) {
                displayNames.push_back(packs[i].empty() ? "Default Voices" : packs[i].c_str());
                if (packs[i] == currentPack) currentIdx = i;
            }

            ImGui::Text("Voice Pack");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##VoicePack", &currentIdx,
                             (const char* const*)displayNames.data(), (int)displayNames.size())) {
                CVarSetString(CVAR_REMOTE_ANCHOR("AudioMod"), packs[currentIdx].c_str());
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                // Loader reload for local pack: scans player-voices/<folder>/
                // and registers samples in the resource cache + sequenceMap.
                VoicePack_OnAudioModChanged(packs[currentIdx].c_str());
                // Broadcast selection so peers' B3 receive layer (when wired)
                // can resolve (senderClientId → audioModFilename → pack samples).
                if (anchor && anchor->isConnected) {
                    anchor->SendPacket_UpdateClientState();
                }
            }
        });

    // Phase α.7+ — per-pack tuning multiplier. Applied at substitution
    // time to the final sample tuning. 1.0 = identity. Useful when a
    // voice pack's samples are at a non-vanilla sample rate (the binary
    // VRP _META format doesn't carry per-sample tuning metadata, so by
    // default the pack inherits vanilla tuning at substitution time).
    // Applies to BOTH local and cross-client substitution (this client's
    // listener-side adjustment, not broadcast over the wire).
    AddWidget(path, "Voice Pack Pitch: %.2fx##Flotilla", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_AUDIO("VoicePackTuningMultiplier"))
        .Options(FloatSliderOptions()
                     .Format("%.2f")
                     .Min(0.4f)
                     .Max(2.5f)
                     .DefaultValue(1.0f)
                     .Tooltip(
                         "Tunes voice-pack sample playback by multiplying the "
                         "tuning ratio. 1.0x = identity (pack plays as authored). "
                         "Lower values slow down + lower pitch; higher values "
                         "speed up + raise pitch. Useful when a pack's samples "
                         "are at a non-vanilla sample rate. Adjustment is "
                         "listener-side only (not synced over the wire); each "
                         "player can tune to their preference."));

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

    // Title-screen peers — Phase 1+2 feature. Spawns same-team peers as
    // additional Link+Epona pairs behind the local player during the
    // Hyrule Field title-cutscene gallop. Cosmetic-only; does not affect
    // gameplay. See Plans/title_screen_peer_actors.md.
    AddWidget(path, "Title Screen", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Show team-mates on title screen", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Anchor.TitleScreenPeers"))
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Spawn same-team peers as visible Link+Epona pairs behind the local "
            "player during the Hyrule Field title-cutscene gallop. Peers ride "
            "alongside the local player in formation, in their selected cosmetic "
            "model + colour.\n\n"
            "Cosmetic only — no gameplay effect. The horse render uses the existing "
            "horse-sync infrastructure; if Anchor → Horse Sync is off, peers appear "
            "on foot instead of mounted.\n\n"
            "Cap of 3 visible peers (first to join the room). Default: off."));

    // Team Marker (#219) — through-walls fairy indicator over each same-team
    // peer's head, tinted to the peer's Anchor colour. Same-team only. See
    // Plans/team_marker_plan.md.
    AddWidget(path, "Team Marker", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Show teammate markers", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Anchor.TeamMarker.Enabled"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Draws a Navi-style fairy above each same-team teammate's head, tinted "
            "with their Anchor colour. The marker renders on top of walls / geometry "
            "and appears whenever the teammate is out of line-of-sight.\n\n"
            "Auto-hides when the teammate is directly visible (a per-frame LOS "
            "raycast from your camera to the teammate decides). Same-team only "
            "(based on Team ID). Suppressed during cutscenes.\n\n"
            "Default: on."));

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
    // Items
    // -----------------------------------------------------------------
    path.sidebarName = "Items";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 1);

    AddWidget(path, "Pickup Sharing", WIDGET_SEPARATOR_TEXT);

    // #193 Q1 — Team Shares Pickups toggle.
    AddWidget(path, "Team shares pickups", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_ANCHOR("TeamSharesPickups"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "When ON (default — cooperative): transient pickups (sticks, "
            "nuts, rupees, hearts, bombs, arrows, magic, bombchus) are "
            "cross-credited to all teammates. Each Dekubaba/Karebaba "
            "kill yields one stick + one nut per teammate.\n\n"
            "When OFF (competitive): each player keeps only what they "
            "personally picked up. Per-player modal drops (Karebaba "
            "stick, Dekubaba stem stick) are host-only — peer cannot "
            "claim them. Ground drops (nuts, hearts, rupees) go to "
            "whoever picks them up first; the other player's local "
            "copy is killed by ITEM_COLLECTED.\n\n"
            "Progression items (keys, bag upgrades, heart pieces, etc.) "
            "always cross-broadcast regardless of this setting.\n\n"
            "All clients in a session should use the same setting for "
            "consistent results."));

    // -----------------------------------------------------------------
    // Host Settings (Phase 3 of host-authoritative settings sync)
    //
    // Every widget here writes to a CVar that runtime gameplay code
    // reads through AnchorCVarSync::GetEnforcedInt / GetEnforcedFloat
    // — when a session is connected, host's value wins regardless of
    // peer's local CVar (the strict-host rule: original room owner is
    // the sole writer; effective-host migration does NOT transfer write
    // authority; see Plans/settings_sync_design.md §2).
    //
    // UI gate via DISABLE_FOR_ANCHOR_NOT_OWNER + the shared PreFunc
    // helper: editable on owner+connected, dimmed (with "Only the room
    // owner..." tooltip) for peers / effective-host / disconnected /
    // global-room. Disconnected / global users still see the displayed
    // value (= their local CVar), so the sidebar doubles as a preview
    // of what the user would broadcast if they hosted.
    //
    // Callback path: TriggerOwnerBroadcastNow() forces an immediate
    // UPDATE_ROOM_STATE so peers see the change within relay RTT
    // (~100ms). Without it the 1-second auto-poll would still propagate
    // — just less snappy.
    // -----------------------------------------------------------------
    path.sidebarName = "Host Settings";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 2);

    AddWidget(path, "Class A — must match (drift causes desync / crash)", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Time Travel with Song of Time", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("TimeTravel"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(ComboboxOptions()
                     .ComboMap(sFlotillaTimeTravelOptions)
                     .DefaultIndex(TIME_TRAVEL_OOT)
                     .Tooltip("Cross-timeline age switch tier. Host-authoritative — "
                              "peer playing Song of Time uses the host's tier "
                              "(refused entirely if host has Disabled). Class A: "
                              "drift would let peers warp into a different timeline "
                              "than the host."));

    AddWidget(path, "Damage Multiplier", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("DamageMult"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(ComboboxOptions()
                     .ComboMap(sFlotillaDamageMultPowers)
                     .DefaultIndex(0)
                     .Tooltip("Multiplies all enemy / hazard damage. Host-authoritative "
                              "— peer takes the same multiplied damage from a Stalfos "
                              "hit as host would. Class A: drift = HP desync."));

    AddWidget(path, "Fall Damage Multiplier", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("FallDamageMult"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(ComboboxOptions()
                     .ComboMap(sFlotillaFallDamagePowers)
                     .DefaultIndex(0)
                     .Tooltip("Multiplies fall damage. Class A."));

    AddWidget(path, "Void Damage Multiplier", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("VoidDamageMult"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(ComboboxOptions()
                     .ComboMap(sFlotillaVoidDamagePowers)
                     .DefaultIndex(0)
                     .Tooltip("Multiplies damage taken on void-out. Class A."));

    AddWidget(path, "Freeze Time##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("FreezeTime"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Locks the world clock. Host-authoritative — peers' time "
            "freezes/unfreezes in lockstep with host. Class A: drift = "
            "event-gate misfires + scene corruption."));

    AddWidget(path, "Randomized Enemies##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RandomizedEnemies"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Randomizes enemy actor types at spawn. Host-authoritative — "
            "every client spawns the same randomized roster. Class A: "
            "drift = netId lookup fails on differing actor types."));

    AddWidget(path, "New Drops##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("NewDrops"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Rewrites item drop tables. Host-authoritative — peers see "
            "the same drops host's table would produce. Class A: drift = "
            "inventory mismatch."));

    AddWidget(path, "Hyper Enemies##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("HyperEnemies"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Runs enemy update twice per frame (2x speed). Host-authoritative "
            "— peers' enemies double-tick to match host. Class A: drift = "
            "animation desync."));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Class B — should match (drift causes UX / fairness divergence)",
              WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Non-Blocking Item Pickups##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Anchor.NonBlockingItemGet"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Pillar G.ii — Replaces the freeze + cutscene + textbox for "
            "non-iconic item pickups (chest items, ground drops, scrub "
            "gives, etc.) with a corner Notification toast. World keeps "
            "moving throughout — Link stays controllable, NPCs / "
            "Invaders / projectiles continue. Iconic items (ocarinas, "
            "Light Arrows, Great Fairy spells, Ice Trap) keep their "
            "vanilla cutscenes. Songs / medallions / spiritual stones / "
            "Master Sword use dedicated cutscene paths that are "
            "unaffected by this toggle.\n\n"
            "Host-authoritative — every client in the session uses "
            "host's value. Default: on. Disable to revert to the "
            "vanilla freeze behavior everywhere."));

    AddWidget(path, "Non-Blocking Text Boxes##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Anchor.NonBlockingTextBox"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Pillar G.ii — Keeps the day/night clock advancing on the "
            "reading client while an NPC dialog is open (Zora, Mido, "
            "shop dialog, etc.). Reader is still locked in dialog "
            "locally; peers see the world keep moving instead of "
            "freezing every time someone talks to an NPC.\n\n"
            "Host-authoritative — every client in the session uses "
            "host's value. Default: on. Disable to revert to vanilla "
            "'text-box freezes world time' behavior."));

    AddWidget(path, "Unrestricted Items##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("NoRestrictItems"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Allows any item to be equipped/used regardless of restrictions."));

    AddWidget(path, "Bonk Damage Multiplier##Flotilla", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_ENHANCEMENT("BonkDamageMult"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(ComboboxOptions()
                     .ComboMap(sFlotillaBonkDamageValues)
                     .DefaultIndex(BONK_DAMAGE_NONE)
                     .Tooltip("Damage from bonking into walls / objects."));

    AddWidget(path, "Infinite Ammo##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("InfiniteAmmo"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Per-frame refill of ammo (arrows, nuts, sticks, bombs, etc.). "
            "Host-authoritative — peers' refill hook (re)registers to "
            "match host."));

    AddWidget(path, "Climb Everything##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("ClimbEverything"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Treats every wall as climbable. Host-authoritative."));

    AddWidget(path, "Hookshot Everything##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("HookshotEverything"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Allows hookshot/longshot to latch onto any surface."));

    AddWidget(path, "Speed Modifier: %.2fx##Flotilla", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_CHEAT("SpeedModifier.Value"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(FloatSliderOptions()
                     .Format("%.2f")
                     .Min(0.1f)
                     .Max(10.0f)
                     .DefaultValue(1.0f)
                     .Tooltip(
                         "Movement speed multiplier — only the .Value is "
                         "synced. Activation (.SpeedToggle / .Btn) stays "
                         "local: peer still needs to bind a modifier button "
                         "or set SpeedToggle locally for the boost to "
                         "actually apply."));

    AddWidget(path, "Super Tunic##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("SuperTunic"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "All tunics grant all environmental immunities (Goron + Zora "
            "+ Kokiri behaviour combined)."));

    AddWidget(path, "Timeless Equipment##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("TimelessEquipment"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Child can equip adult items and vice versa."));

    AddWidget(path, "Bomb Timer Multiplier: %.2fx##Flotilla", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_CHEAT("BombTimerMultiplier"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(FloatSliderOptions()
                     .Format("%.2f")
                     .Min(0.1f)
                     .Max(5.0f)
                     .DefaultValue(1.0f)
                     .Tooltip("Multiplies bomb fuse duration. Applied at "
                              "spawn — new bombs use host's value, existing "
                              "ones keep their current fuse."));

    AddWidget(path, "Shield with Two-Handed Weapons##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("ShieldTwoHanded"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Allows shielding while wielding Megaton Hammer / similar "
            "two-handed weapons."));

    AddWidget(path, "Remove Explosive Limit##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("RemoveExplosiveLimit"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Removes the 3-active-explosive cap; allows 4+ bombs / "
            "bombchus simultaneously."));

    AddWidget(path, "Fireproof Deku Shield##Flotilla", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_CHEAT("FireproofDekuShield"))
        .PreFunc(FlotillaHostSettingsPreFunc)
        .Callback(FlotillaHostSettingsCallback)
        .Options(CheckboxOptions().Tooltip(
            "Prevents the Deku Shield from burning on fire damage."));

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
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
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
    // Time-of-day sync (issue #63) — periodic + edge-triggered broadcasts.
    //
    // Existing UPDATE_CLIENT_STATE carries dayTime+nightFlag on scene/room
    // change, climbing edge, etc. — but no periodic baseline. Without
    // periodic, drift accumulates when one client is in a dungeon (time
    // frozen) while another is in overworld (time advancing). The new
    // TIME_SYNC packet adds a 5s baseline + cutscene-start/end edges +
    // scene-transition (TRANS_TRIGGER_START) edge.
    //
    // The Enabled toggle gates the periodic timer only. Edge sends
    // (cutscene-start / cutscene-end / scene_transition) always fire when
    // connected + save loaded + gameMode==NORMAL — they're correctness-
    // critical for boundary smoothness and can't be disabled here.
    // -----------------------------------------------------------------
    AddWidget(path, "Time-of-Day Sync", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Periodic Time Sync Enabled", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_REMOTE_ANCHOR("TimeSync.Enabled"))
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "When ON, broadcasts the time-of-day (dayTime + nightFlag) every "
            "few seconds to keep all clients' clocks aligned. Edge sends "
            "(cutscene start/end, scene transition) always fire regardless "
            "of this setting — turning this off only suppresses the periodic "
            "baseline.\n\n"
            "Without periodic sync, drift accumulates over minutes when one "
            "player is in a dungeon (time frozen) while another is in "
            "overworld (time advancing). Default: ON."));

    AddWidget(path, "Time Sync Interval: %ds", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_REMOTE_ANCHOR("TimeSync.IntervalSeconds"))
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger(CVAR_REMOTE_ANCHOR("TimeSync.Enabled"), 1);
        })
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(60)
                     .DefaultValue(5)
                     .Format("%ds")
                     .Tooltip(
            "Periodic time-sync broadcast interval. Shorter = clocks stay "
            "tighter across clients, slightly more bandwidth. Longer = more "
            "drift between syncs, less bandwidth. Default 5s gives ~5 in-game "
            "minutes max drift at the receive side. Range 1-60s; clamped on "
            "read."));

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
    // Horse Sync (Plans/horse_sync_plan.md)
    // -----------------------------------------------------------------
    path.sidebarName = "Horse Sync";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Flotilla", path.sidebarName, 1);

    AddWidget(path, "Bidirectional sync for mounted Epona across connected players.",
              WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Enable Horse Sync##HorseSync", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENHANCEMENT("Anchor.HorseSyncEnabled"))
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Owner-authoritative bidirectional sync for mounted ACTOR_EN_HORSE "
            "(Epona) instances. Each player owns the Epona they're mounted on; "
            "peers see a synced replica with derived rider position.\n\n"
            "v1 scope: mounted Epona only. Cutscene horses, Ingo race minigame, "
            "Horseback Archery, and the five sibling horse actors (En_Horse_Ganon "
            "et al.) are intentionally NOT synced — see Plans/horse_sync_plan.md "
            "for the per-actor policy table.\n\n"
            "No horse-side color identification in v1 — peer riders are "
            "identified by tunic color + cosmetic model + nametag. v2 polish "
            "will add saddle tint or custom textures after the runtime probe "
            "resolves the saddle DList combiner question.\n\n"
            "Default OFF per the project 'vanilla-altering features ship "
            "default-off' convention. Both players must enable to see each "
            "other's horses."));

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
