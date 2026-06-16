#include "TimeOfDayReconcile.h"

#include <spdlog/spdlog.h>

extern "C" {
#include "variables.h"  // gSaveContext (Pitfall 9)
}

namespace AnchorTimeOfDay {

bool ApplyIfAhead(u16 receivedDayTime, s32 receivedNightFlag,
                  const char* logTag) {
    // OPTION A — respect vanilla's "time-frozen island" scene design
    // (added 2026-06-16 after log 535 field test surfaced day/night
    // setup desync in Lon Lon Ranch).
    //
    // Vanilla freezes dayTime in certain scenes by setting envCtx.
    // timeIncrement = 0 from the scene header (z_scene.c:354-368):
    // all 10 dungeons, all boss rooms, and Lon Lon Ranch. The design
    // contract is that gSaveContext.sceneSetupIndex — picked exactly
    // ONCE at scene entry from LINK_IS_ADULT x IS_DAY (z_play.c:470-
    // 478) — stays valid for the entire visit. Vanilla has NO logic
    // anywhere to re-pick the setup or reload the scene on a mid-
    // visit nightFlag transition; it relies on the timeIncrement=0
    // freeze to guarantee the day/night setup is never wrong.
    //
    // Without this gate, our TIME_SYNC packet would force dayTime
    // forward on the LLR-side player; nightFlag auto-recomputes (z_
    // kankyo.c:974-978) but the actor list, music, and lighting all
    // stay in the day setup. Result: visible day actors at night,
    // wrong ambience, etc. — a state vanilla never anticipated.
    //
    // Suppressing the apply when gTimeIncrement == 0 preserves the
    // vanilla invariant: LLR-side player experiences single-player
    // vanilla LLR (frozen at entry time). The local clock catches
    // up on scene exit via the existing UPDATE_CLIENT_STATE
    // handshake (which uses this same helper but fires AFTER
    // Play_Init sets the new scene's gTimeIncrement to a non-zero
    // value — so the apply succeeds on the new scene).
    //
    // KNOWN LIMITATION + MAY BE REVISITED: the LLR-side player's
    // sky doesn't change while their HF teammate's does. If we
    // later add per-actor day/night state re-evaluation (option C
    // partial — without a full scene reload), this gate can be
    // removed. See Claude/Analysis/time_sync_wraparound_bug_analysis_
    // 2026-06-16.md §"Option A — LLR follow-up" and GitHub #63.
    //
    // gTimeIncrement is declared in z_kankyo.c:61 and exposed via
    // soh/include/variables.h:80 (extern). Already in scope here
    // through the extern "C" #include "variables.h" at file top.
    if (gTimeIncrement == 0) {
        return false;
    }

    // Modular forward distance on the circular u16 clock.
    //
    // Wraparound-aware semantics (see TimeOfDayReconcile.h for full
    // rationale): received is "ahead" iff the forward distance on the
    // circular clock is in (0, 0x8000). 0x8000 = half-day = 32768 units
    // = the boundary at which a value > half forward is treated as
    // behind (the shorter circular path is backward).
    //
    // u16 unsigned subtraction handles wrap naturally:
    //   - Same-side: (30100 - 30000) = 100 -> apply.
    //   - Reversed: (30000 - 30100) & 0xFFFF = 65436 -> not ahead.
    //   - Through-wrap forward: (0x0100 - 0xFF80) & 0xFFFF = 0x0180 (384
    //     forward) -> apply. Correct catch-up after sender wrapped.
    //   - Through-wrap backward: (0xFF80 - 0x0100) & 0xFFFF = 0xFE80
    //     (~65152 forward) > 0x8000 -> not ahead. Correctly rejects
    //     stale-high broadcasts from frozen-time-scene peers post-wrap.
    //
    // NIGHTFLAG-MISMATCH CLAUSE REMOVED (was in pre-fix logic). The OR-
    // with-nightFlag-mismatch was a partial workaround for the same wrap
    // problem; modular distance handles wrap cleanly, and keeping the
    // nightFlag OR clause introduces a NEW spurious-apply hazard at the
    // day<->night transition boundary (a 1-frame lag in vanilla's
    // auto-recompute could yank Link backwards across the boundary).
    // nightFlag is still written on apply for visual immediacy (vanilla
    // recomputes from dayTime each frame anyway per z_kankyo.c:974-978).
    const u16 localDayTime    = (u16)gSaveContext.dayTime;
    const u16 forwardDistance = (u16)(receivedDayTime - localDayTime);
    const bool timeIsAhead    = (forwardDistance != 0) &&
                                (forwardDistance < 0x8000);

    if (!timeIsAhead) {
        return false;
    }

    const u16 prevDayTime   = localDayTime;
    const s32 prevNightFlag = gSaveContext.nightFlag;

    gSaveContext.dayTime   = receivedDayTime;
    gSaveContext.nightFlag = receivedNightFlag;

    // Force skyboxTime to match — eliminates the brief visual lag where
    // the sun/moon would hang in the prior position for 1-2 frames while
    // vanilla z_kankyo.c:966-970's catch-up overtakes. Setting equal is
    // safe: the catch-up predicate only triggers when dayTime > skyboxTime,
    // so an equal write is a no-op next frame.
    gSaveContext.skyboxTime = receivedDayTime;

    if (logTag != nullptr && logTag[0] != '\0') {
        SPDLOG_INFO("[TimeSync] apply ({}) dayTime {}->{} nightFlag {}->{} "
                    "forwardDist={} (skyboxTime forced to match)",
                    logTag, prevDayTime, (u16)gSaveContext.dayTime,
                    prevNightFlag, gSaveContext.nightFlag,
                    forwardDistance);
    }

    return true;
}

}  // namespace AnchorTimeOfDay
