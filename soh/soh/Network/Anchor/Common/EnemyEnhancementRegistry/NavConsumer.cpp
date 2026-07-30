/**
 * NavConsumer — Phase 2 SUPERSEDED body (kept as callable helper).
 *
 * Original Phase 2 design delegated to Common/AILocomotion/ substrate
 * to produce yaw + speedXZ writes for enhanced enemies. Field-test
 * log 761 (2026-07-30) proved this architecturally unsound for combat
 * En_Sw: vanilla combat En_Sw never populates actor->wallPoly OR
 * actor->floorPoly during its ambient state, so wall detection via
 * universal vanilla fields is impossible; the fallback floor branch
 * writes world-XZ yaw which rotates wall-attached spiders off their
 * tangent axis.
 *
 * Superseded by the En_Sw enhanced state machine (Option B, per
 * Plans/en_sw_enhanced_state_machine_pilot.md). The state machine
 * installs an actionFunc override at OnInit that owns motion +
 * rotation directly, bypassing this helper entirely.
 *
 * Body reduced to early-return-after-diagnostic. Kept as a callable
 * helper for future wall-crawler descriptors that may want simpler
 * pos+yaw-only nav semantics (no current consumer). Diagnostic
 * scaffolding preserved because the [EEDiag] infrastructure is reused
 * by the state machine's own per-tick logging (see pilot plan §9).
 */

// Pitfall 40 — Anchor.h FIRST so libultraship + nlohmann templates are
// declared in C++ linkage before NavConsumer.h opens its extern "C" block.
#include "soh/Network/Anchor/Anchor.h"

#include "NavConsumer.h"

#include "soh/Network/Anchor/Common/EnforcedCVars.h"  // AnchorCVarSync::GetEnforcedInt

extern "C" {
#include "z64bgcheck.h"  // COLPOLY_GET_NORMAL for diag
}

#include <cstdint>
#include <unordered_map>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>  // SPDLOG_INFO + fmt::format

namespace AnchorEnemyEnhancement {

namespace {

// ---------------------------------------------------------------------
// Diagnostic instrumentation (CVar-gated, rate-limited to ~1 Hz per actor)
// ---------------------------------------------------------------------
// Toggle via `set gEnhancements.EnemyEnhancement.Diag 1` at the console.
// Emits `[EEDiag]` lines showing per-actor state (surface poly, bg flags,
// pos, rot). Originally added during log-761 investigation to disambiguate
// wall/floor branch behaviour; the state machine (Option B) reuses this
// same infrastructure per pilot plan §9. Current NavConsumer body is a
// stub (`branch=STUB`) since the state machine handles motion directly.
//
// Rate limit: max one log per actor per ~20 frames (~1 Hz at 20fps).
// Additional log on ANY branch change so we don't miss transitions.
constexpr const char* kDiagCVar = "gEnhancements.EnemyEnhancement.Diag";
constexpr uint32_t   kDiagLogIntervalFrames = 20;

struct DiagState {
    uint64_t lastLogFrame       = 0;
    Vec3s    lastWrittenRot     = {0, 0, 0};
    bool     haveLastWrittenRot = false;
    int      lastBranch         = -1;  // 0=OOR 1=ATK 2=WALL 3=FLOOR 4=NOTGT 5=STUB
};
std::unordered_map<Actor*, DiagState> sDiag;

const char* BranchName(int b) {
    switch (b) {
        case 0: return "OOR";
        case 1: return "ATK";
        case 2: return "WALL";
        case 3: return "FLOOR";
        case 4: return "NOTGT";
        case 5: return "STUB";  // NavConsumer body superseded by state machine
        default: return "?";
    }
}

// Emit one log line. `branch` is 0-3 (see BranchName). If `preWriteRot`
// or `postWriteRot` is nullptr, that field is omitted from the log —
// keeps the OOR / ATK branches from logging meaningless rotations.
void EmitDiag(Actor* actor, PlayState* play, DiagState& d, int branch,
              const Vec3f* subgoal, float speed,
              const Vec3s* preWriteRot, const Vec3s* postWriteRot) {
    const uint32_t frame = (play != nullptr) ? play->gameplayFrames : 0;

    // Wall-poly source + normal (if any).
    const char* polySrc = "none";
    Vec3f normal = {0.0f, 0.0f, 0.0f};
    if (actor->wallPoly != nullptr) {
        polySrc = "wallPoly";
        normal.x = COLPOLY_GET_NORMAL(actor->wallPoly->normal.x);
        normal.y = COLPOLY_GET_NORMAL(actor->wallPoly->normal.y);
        normal.z = COLPOLY_GET_NORMAL(actor->wallPoly->normal.z);
    } else if (actor->floorPoly != nullptr) {
        polySrc = "floorPoly";
        normal.x = COLPOLY_GET_NORMAL(actor->floorPoly->normal.x);
        normal.y = COLPOLY_GET_NORMAL(actor->floorPoly->normal.y);
        normal.z = COLPOLY_GET_NORMAL(actor->floorPoly->normal.z);
    }

    // Delta since our last write — nonzero means vanilla clobbered.
    Vec3s clobberDelta = {0, 0, 0};
    bool haveClobber = false;
    if (d.haveLastWrittenRot) {
        haveClobber = true;
        clobberDelta.x = (s16)(actor->world.rot.x - d.lastWrittenRot.x);
        clobberDelta.y = (s16)(actor->world.rot.y - d.lastWrittenRot.y);
        clobberDelta.z = (s16)(actor->world.rot.z - d.lastWrittenRot.z);
    }

    // Substitute sentinel values (-1) for absent optional fields so the
    // format string stays fixed-shape (grep-friendly, no ternaries in
    // the SPDLOG args to avoid depending on fmt::format visibility from
    // this include depth).
    const float sgX = subgoal ? subgoal->x : -1.0f;
    const float sgY = subgoal ? subgoal->y : -1.0f;
    const float sgZ = subgoal ? subgoal->z : -1.0f;
    const s16   preX  = preWriteRot  ? preWriteRot->x  : (s16)-1;
    const s16   preY  = preWriteRot  ? preWriteRot->y  : (s16)-1;
    const s16   preZ  = preWriteRot  ? preWriteRot->z  : (s16)-1;
    const s16   postX = postWriteRot ? postWriteRot->x : (s16)-1;
    const s16   postY = postWriteRot ? postWriteRot->y : (s16)-1;
    const s16   postZ = postWriteRot ? postWriteRot->z : (s16)-1;

    SPDLOG_INFO(
        "[EEDiag] actor=0x{:x} frame={} branch={} polySrc={} "
        "normal=({:.2f},{:.2f},{:.2f}) bgFlags=0x{:x} "
        "actorPos=({:.0f},{:.0f},{:.0f}) subgoal=({:.0f},{:.0f},{:.0f}) speed={:.2f} "
        "entryRot=({},{},{}) lastWrittenRot=({},{},{}) clobberDelta=({},{},{}) haveLast={} "
        "preWriteRot=({},{},{}) postWriteRot=({},{},{})",
        (uintptr_t)actor, frame, BranchName(branch), polySrc,
        normal.x, normal.y, normal.z, actor->bgCheckFlags,
        actor->world.pos.x, actor->world.pos.y, actor->world.pos.z,
        sgX, sgY, sgZ, speed,
        actor->world.rot.x, actor->world.rot.y, actor->world.rot.z,
        d.lastWrittenRot.x, d.lastWrittenRot.y, d.lastWrittenRot.z,
        clobberDelta.x, clobberDelta.y, clobberDelta.z,
        haveClobber ? 1 : 0,
        preX, preY, preZ,
        postX, postY, postZ);
}

// Rate-gate: returns true if we should emit this tick.
bool ShouldEmitDiag(Actor* actor, PlayState* play, DiagState& d, int branch) {
    const uint32_t frame = (play != nullptr) ? play->gameplayFrames : 0;
    const bool branchChanged = (d.lastBranch != branch);
    const bool intervalElapsed = (frame - d.lastLogFrame) >= kDiagLogIntervalFrames;
    if (branchChanged || intervalElapsed || d.lastLogFrame == 0) {
        d.lastLogFrame = frame;
        d.lastBranch   = branch;
        return true;
    }
    return false;
}

bool DiagEnabled() {
    return CVarGetInteger(kDiagCVar, 0) != 0;
}

}  // namespace

bool TickNavMovement(EnemyEnhancementDescriptor& descriptor,
                     NavConsumerState& state,
                     Actor* actor,
                     PlayState* play) {
    (void)state;  // reserved — future non-En_Sw consumers may reuse per-actor state
    if (actor == nullptr || play == nullptr) return false;

    // CVar gate — descriptor supplies the CVar name; nullptr means
    // "always on when Capabilities().canNavConsume is true" (v1 always
    // supplies a name so this branch is defensive).
    const char* cvarName = descriptor.NavConsumeCVar();
    if (cvarName != nullptr && AnchorCVarSync::GetEnforcedInt(cvarName, 0) == 0) {
        return false;
    }

    // SUPERSEDED by En_Sw enhanced state machine (Option B pilot). The
    // state machine installs an actionFunc override at OnInit and owns
    // motion + rotation directly, so this per-tick helper never needs
    // to fire for En_Sw. If a future wall-crawler descriptor with
    // simpler needs (no state-machine lifecycle) wants pos+yaw-only
    // nav, the original body lived here — see git history for the
    // previous implementation.
    //
    // Diagnostic emission preserved so log inspection can confirm
    // OnNavTick is being reached (not silently gated at the bridge).
    const bool diag = DiagEnabled();
    DiagState* dp = diag ? &sDiag[actor] : nullptr;
    if (dp && ShouldEmitDiag(actor, play, *dp, /*STUB*/ 5)) {
        EmitDiag(actor, play, *dp, 5, nullptr, 0.0f, nullptr, nullptr);
    }
    return false;
}

}  // namespace AnchorEnemyEnhancement
