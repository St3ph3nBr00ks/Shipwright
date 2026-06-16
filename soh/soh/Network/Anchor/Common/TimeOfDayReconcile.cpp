#include "TimeOfDayReconcile.h"

#include <spdlog/spdlog.h>

extern "C" {
#include "variables.h"  // gSaveContext (Pitfall 9)
}

namespace AnchorTimeOfDay {

bool ApplyIfAhead(u16 receivedDayTime, s32 receivedNightFlag,
                  const char* logTag) {
    // Forward-only check. Lifted verbatim from the original inline block at
    // UpdateClientState.cpp:432-443 (the receive-side reconcile that has
    // shipped since the early Anchor implementation). Two clauses ensure
    // the apply happens on either a nightFlag transition (day<->night flip)
    // OR a strictly-greater dayTime. The OR-of-clauses naturally handles
    // wrap-around at the day-night boundary even though dayTime itself
    // never wraps in normal vanilla gameplay.
    const bool timeIsAhead =
        (receivedNightFlag != gSaveContext.nightFlag) ||
        (receivedDayTime   >  (u16)gSaveContext.dayTime);

    if (!timeIsAhead) {
        return false;
    }

    const u16 prevDayTime   = (u16)gSaveContext.dayTime;
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
                    "(skyboxTime forced to match)",
                    logTag, prevDayTime, (u16)gSaveContext.dayTime,
                    prevNightFlag, gSaveContext.nightFlag);
    }

    return true;
}

}  // namespace AnchorTimeOfDay
