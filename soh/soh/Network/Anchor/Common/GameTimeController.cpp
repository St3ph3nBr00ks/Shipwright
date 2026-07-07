#include "GameTimeController.h"
#include "GameTimeControllerBridge.h"
#include "soh/Network/Anchor/Anchor.h"  // ::Anchor::Instance / isEnabled
                                        // (also pre-loads libultraship.h + z64.h)
#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt (G.ii host-auth read)
#include "soh/cvar_prefixes.h"          // CVAR_REMOTE_ANCHOR for live-world toggle
#include "soh/Notification/Notification.h"  // Notification::Emit for G.ii NotificationOnly path
#include "soh/SohGui/ImGuiUtils.h"          // GetTextureForItemId (notification icon)
#include "soh/util.h"                       // SohUtils::GetItemName

#include <libultraship/bridge/consolevariablebridge.h>  // CVarGetInteger

extern "C" {
#include "macros.h"
#include "variables.h"  // gSaveContext (Pitfall 9): SceneTransition rule reads gameMode
extern PlayState* gPlayState;
// Pillar G.ii Cutscene routing — the legacy rule calls Player_InCsMode
// directly (NOT Play_InCsMode) to avoid recursion, since Play_InCsMode
// is the site being routed through the gate. Declared explicitly to
// avoid a full functions.h include.
extern s32 Player_InCsMode(PlayState* play);  // z_player_lib.c:510
}

namespace GameTimeController {

// Returns the legacy single-player answer for each context — i.e. what
// the existing OoT call site at the routed location would have answered
// before this gate existed. ShouldAdvanceWorldTime layers Pillar-G
// multiplayer rules on top.
static bool LegacyAdvanceWorldTimeRule(TimeContext ctx) {
    if (gPlayState == nullptr) return true;
    switch (ctx) {
        case TimeContext::PauseMenu:
            // Time advances when pause menu is OFF (state == 0; the codebase
            // uses the literal — there is no PAUSE_STATE_OFF macro).
            return gPlayState->pauseCtx.state == 0;
        case TimeContext::TextBox:
            // Vanilla z_kankyo.c:945 dayTime gate accepts EITHER no textbox
            // open (msgLength == 0 && msgMode == MSGMODE_NONE) OR
            // gameMode == GAMEMODE_END_CREDITS (credits scroll may run with
            // a text box on-screen). Bundle both clauses so consumers get
            // the full behavior via single call — parallels the
            // SceneTransition predicate's gameMode OR clause.
            return (gPlayState->msgCtx.msgLength == 0 &&
                    gPlayState->msgCtx.msgMode == MSGMODE_NONE) ||
                   gSaveContext.gameMode == GAMEMODE_END_CREDITS;
        case TimeContext::ItemGet: {
            Player* player = GET_PLAYER(gPlayState);
            return player == nullptr || !(player->stateFlags1 & PLAYER_STATE1_GETTING_ITEM);
        }
        case TimeContext::Cutscene:
            // Canonical predicate mirrors z_play.c:1811 Play_InCsMode, but
            // calls Player_InCsMode DIRECTLY (NOT Play_InCsMode) to avoid
            // recursion — Play_InCsMode's body is being routed through this
            // gate below. The csCtx.state check is inlined here;
            // Player_InCsMode's 7-way OR (Player_InBlockingCsMode +
            // unk_6AD == 4 at z_player_lib.c:503-514) is preserved by the
            // direct call. Returns true when time SHOULD advance — i.e. no
            // cutscene active. R7 analysis: Plans/soh_cutscene_flow_analysis.md.
            return gPlayState->csCtx.state == CS_STATE_IDLE &&
                   !Player_InCsMode(gPlayState);
        case TimeContext::Ocarina:
            return gPlayState->msgCtx.ocarinaMode == OCARINA_MODE_00;
        case TimeContext::SceneTransition:
            // Legacy z_kankyo.c:927 dayTime gate accepts EITHER no-transition
            // OR not-in-normal-gamemode (file-select / name-entry / end-credits
            // bypass the transition-mode freeze). The gamemode-OR is part of
            // the SceneTransition predicate, not a separate context — bundle
            // both clauses so consumers get the full behavior.
            return gPlayState->transitionMode == TRANS_MODE_OFF ||
                   gSaveContext.gameMode != GAMEMODE_NORMAL;
        case TimeContext::GameOver:
            // Legacy: world freezes during the death cycle. Routed via
            // PLAYER_STATE1_DEAD on the local player's stateFlags1, which
            // gates D_80116068 suppression in z_actor.c:Actor_UpdateAll.
            // Returning true here means "advance time" — i.e. the gate
            // SHOULD NOT suppress. Returning false means "freeze" — the
            // gate suppresses normally. Equivalence to vanilla legacy
            // behaviour: false iff PLAYER_STATE1_DEAD is set on the local
            // Player. Use gameOverCtx as a proxy since stateFlags1 isn't
            // directly accessible here — the death cycle holds
            // gameOverCtx.state != GAMEOVER_INACTIVE throughout.
            return gPlayState->gameOverCtx.state == GAMEOVER_INACTIVE;
    }
    return true;
}

bool ShouldAdvanceWorldTime(TimeContext context) {
    // Pillar G.i: in multiplayer mode, the local pause menu does NOT
    // freeze world time — other players continue acting, so enemies,
    // NPCs, and scripted timers must keep advancing on this client too.
    // The pause-menu UI itself still renders normally; only the
    // "world ticks" gate flips.
    //
    // Pillar G.iii (#239): same rule for the death cycle on the dying
    // client. Without this, on host death the actor suppression at
    // z_actor.c:2625-2658 (driven by D_80116068 & PLAYER_STATE1_DEAD)
    // freezes Actor_UpdateAll for ENEMY/BG/SWITCH/BOSS/PROP/MISC/NPC/
    // EXPLOSIVE/CHEST categories. Peers see all host-authoritative
    // state freeze for the ~10s death cycle. With this gate flipped,
    // the dying client's actor updates continue → host keeps
    // broadcasting → peers see normal sync throughout death.
    //
    // Pillar G.ii TextBox flip: in MP, when the non-blocking text-box
    // path is enabled (default), the day/night clock keeps advancing
    // even while an NPC dialog is on-screen. Reader stays paused
    // locally via vanilla msgCtx state; peers see the world keep
    // moving. Host-authoritative CVar so every session member gets
    // the same behavior. Sibling to the item-get non-blocking path
    // (which is realised at Player_ActionHandler_2, not via the
    // gate flip — the two features are decoupled by design).
    //
    // Remaining contexts (item-get, cutscene, ocarina, scene-
    // transition) continue returning the legacy answer until the
    // §4.G.ii rules land for each.
    const bool multiplayerActive = (::Anchor::Instance != nullptr) &&
                                   ::Anchor::Instance->isEnabled;
    if (multiplayerActive) {
        if (context == TimeContext::PauseMenu ||
            context == TimeContext::GameOver) {
            return true;
        }
        if (context == TimeContext::TextBox &&
            IsNonBlockingTextBoxEnabled()) {
            return true;
        }
    }
    return LegacyAdvanceWorldTimeRule(context);
}

// ---------------------------------------------------------------------------
// Pillar G.ii — item-get presentation policy.
//
// Centralised gate that decides, per getItem id, whether the standard
// item-get sequence (z_player.c func_8083A434) plays the full vanilla
// freeze cutscene or falls through to the Notification toast path.
//
// Allowlist: items where the narrative moment justifies preserving the
// interruption. Songs, medallions, spiritual stones, and Master Sword
// are NOT listed here because they use dedicated cutscene paths that
// don't go through func_8083A434 at all — they keep their full cutscenes
// automatically.
// ---------------------------------------------------------------------------

bool IsNonBlockingItemGetEnabled() {
    // Host-authoritative read via AnchorCVarSync — when a session is
    // connected, all clients see the host's value regardless of their
    // own local CVar. Defaults to 1 (enabled) for single-player and
    // pre-connect MP. Registered in EnforcedCVarRegistry.cpp Class B.
    return AnchorCVarSync::GetEnforcedInt(
               CVAR_ENHANCEMENT("Anchor.NonBlockingItemGet"), 1) != 0;
}

bool IsNonBlockingTextBoxEnabled() {
    // Same host-authoritative read pattern as its item-get sibling.
    // Registered in EnforcedCVarRegistry.cpp Class B. Defaults to 1.
    return AnchorCVarSync::GetEnforcedInt(
               CVAR_ENHANCEMENT("Anchor.NonBlockingTextBox"), 1) != 0;
}

ItemPresentationMode GetItemPresentationMode(int16_t getItemId) {
    // Single-player → never override.
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isEnabled) {
        return ItemPresentationMode::Vanilla;
    }
    // User kill switch → always vanilla.
    if (!IsNonBlockingItemGetEnabled()) {
        return ItemPresentationMode::Vanilla;
    }
    // Iconic allowlist — quest-cutscene items whose narrative moment
    // justifies the freeze. Songs / medallions / spiritual stones /
    // Master Sword are intentionally NOT here — they use separate
    // cutscene paths that bypass func_8083A434.
    //
    // Ice Trap is also allowlisted: the vanilla cutscene action has a
    // randomizer-specific damage path (z_player.c Player_Action_8084E6D4)
    // that spawns EN_CLEAR_TAG and fires PLAYER_HIT_RESPONSE_ICE_TRAP.
    // Bypassing the cutscene would silently no-op the trap → broken
    // randomizer experience. Vanilla also uses GI_ICE_TRAP for some
    // surprise chests, so this protects both modes.
    switch (getItemId) {
        case GI_OCARINA_OOT:     // Princess Zelda escape (sometimes Sheik gives in Rando)
        case GI_OCARINA_FAIRY:   // Saria's gift
        case GI_ARROW_LIGHT:     // Princess Zelda (post-Tower) — sage-grant flavour
        case GI_DINS_FIRE:       // Great Fairy
        case GI_FARORES_WIND:    // Great Fairy
        case GI_NAYRUS_LOVE:     // Great Fairy
        case GI_ICE_TRAP:        // randomizer damage path; safety allowlist
            return ItemPresentationMode::Vanilla;
        default:
            return ItemPresentationMode::NotificationOnly;
    }
}

}  // namespace GameTimeController

// ---------------------------------------------------------------------------
// C bridge — lets C translation units (z_play.c, z_actor.c, etc.) call into
// the C++ gate without exposing the namespace or enum class.
//
// Enum values must match the integer ordering of GameTimeController::TimeContext
// AND the ANCHOR_TIME_CTX_* macros in GameTimeControllerBridge.h. Keep all
// three in sync if a new context is added.
// ---------------------------------------------------------------------------
extern "C" bool Anchor_ShouldAdvanceWorldTime(int contextEnum) {
    return GameTimeController::ShouldAdvanceWorldTime(
        static_cast<GameTimeController::TimeContext>(contextEnum));
}

// Inverse predicate for direct call-site replacement of `pauseCtx.state != 0`.
// Reads more naturally at gameplay-freeze sites than negating the advance form.
extern "C" bool Anchor_PauseMenuFreezesWorld(void) {
    return !GameTimeController::ShouldAdvanceWorldTime(
        GameTimeController::TimeContext::PauseMenu);
}

// Tracker #182. Returns true when the live-world pause-menu rendering
// feature should activate this frame. Composition:
//   - Multiplayer is active (Anchor enabled).
//   - Pause UI is open (pauseCtx.state != 0). Equivalently, the world-
//     time gate above has flipped — Anchor_PauseMenuFreezesWorld() is
//     false because we're in MP-during-pause.
//   - The gAnchor.PauseLiveWorld CVar is set (default 0).
//
// All four pause-rendering gates (kaleido pause-Link init, equipment-
// screen rotating-Link draw, z_play.c mode-3 captured-frame backdrop,
// DummyPlayer_Draw early-return) consult this single predicate so the
// CVar acts as an atomic on/off for the feature. Default-off ships
// current safe behaviour while the rendering work soaks for one
// release cycle.
extern "C" bool Anchor_PauseLiveWorldRendering(void) {
    if (gPlayState == nullptr) return false;
    if (gPlayState->pauseCtx.state == 0) return false;  // pause UI not open
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isEnabled) return false;
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("PauseLiveWorld"), 0) != 0;
}

// Pause-menu animation acceleration carry. Returns true ~30% of calls so
// the kaleido path can fire one extra `KaleidoScope_Update` on those
// frames — net pause animation rate ≈ 1.3× world tick (20fps × 1.3 ≈
// 26fps for pause UI animations) without disturbing world-tick pacing.
//
// Only fires when live-world pause rendering is active; otherwise the
// carry resets to 0 so the rate is exact on the first pause-open.
extern "C" bool Anchor_PauseMenuShouldExtraTick(void) {
    static float sCarry = 0.0f;
    if (!Anchor_PauseLiveWorldRendering()) {
        sCarry = 0.0f;
        return false;
    }
    sCarry += 0.3f;
    if (sCarry >= 1.0f) {
        sCarry -= 1.0f;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Pillar G.ii — item-get presentation bridge.
//
// Returns the mode as an int (matches ANCHOR_ITEM_PRESENTATION_*). C
// callers branch on the return value to decide whether to run the
// vanilla cutscene or fall through to the Notification-only path.
// ---------------------------------------------------------------------------
extern "C" int Anchor_GetItemPresentationMode(int16_t getItemId) {
    return static_cast<int>(GameTimeController::GetItemPresentationMode(getItemId));
}

// Emit a corner Notification toast for an item-get that was routed
// through the silent (non-cutscene) path. NO-OP in single-player so
// vanilla FastDrops users don't suddenly see toasts when this helper
// is reachable. Designed to be called from z_player.c right after the
// silent give (Path 1 → func_8083E4C4; Path 2 → inline Item_Give).
//
// Inventory write is NOT done here — caller is responsible. This
// helper purely provides the user-visible toast for the MP non-
// blocking pickup experience. Sound chime is emitted by the
// Notification system (`mute = false`).
//
// `getItemId` reserved for future per-id branching (e.g. heart-
// container variant), currently unused — keep in signature for ABI
// stability.
extern "C" bool Anchor_IsPillarGiiActive(void) {
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isEnabled) {
        return false;
    }
    return GameTimeController::IsNonBlockingItemGetEnabled();
}

extern "C" void Anchor_EmitItemGetToast(int16_t getItemId, uint8_t itemId) {
    if (::Anchor::Instance == nullptr || !::Anchor::Instance->isEnabled) {
        return;  // single-player or MP off → silent, vanilla behavior
    }
    Notification::Emit({
        .itemIcon = GetTextureForItemId(itemId),
        .prefix = "You",
        .prefixColor = ImVec4(0.8f, 0.8f, 1.0f, 1.0f),
        .message = "got",
        .messageColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        .suffix = SohUtils::GetItemName(itemId),
        .suffixColor = ImVec4(1.0f, 0.9f, 0.5f, 1.0f),
        .remainingTime = 6.0f,
        .mute = false,
    });
    (void)getItemId;
}
