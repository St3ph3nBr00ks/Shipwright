#include "TimeOfDayReconcile.h"

#include <spdlog/spdlog.h>

extern "C" {
#include "variables.h"  // gSaveContext (Pitfall 9)
}

namespace AnchorTimeOfDay {

bool ApplyIfAhead(u16 receivedDayTime, s32 receivedNightFlag,
                  const char* logTag) {
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
