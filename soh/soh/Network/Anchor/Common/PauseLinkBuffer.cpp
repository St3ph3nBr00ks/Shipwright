#include "PauseLinkBuffer.h"

#include <cstddef>
#include <cstdint>

namespace {

// Layout requirement (from z_player_lib.c:1970-1992 func_80091738):
//   - gameplay_keep DMAs to segment + 0x3800 (~14 KB)
//   - Link object DMAs to segment + 0x8800 (variable, ~70-150 KB)
// Worst-case total: 0x8800 + ~150 KB + small alignment. 256 KB is
// comfortably above that and trivial on PC.
constexpr std::size_t kPauseLinkBufferSize = 256 * 1024;

// 64-byte alignment matches the original allocation math at
// z_kaleido_scope_PAL.c:3886 (`& ~0x3F`).
alignas(64) unsigned char sBuffer[kPauseLinkBufferSize];

// Single-threaded flag, set/cleared by KaleidoScope_DrawPlayerWork
// around its Player_DrawPause call. See header comment for rationale.
bool sIsDrawingPauseLink = false;

}  // namespace

extern "C" void* Anchor_GetPauseLinkBuffer(void) {
    return static_cast<void*>(sBuffer);
}

extern "C" void Anchor_PauseLinkDrawBegin(void) {
    sIsDrawingPauseLink = true;
}

extern "C" void Anchor_PauseLinkDrawEnd(void) {
    sIsDrawingPauseLink = false;
}

extern "C" bool Anchor_IsDrawingPauseLink(void) {
    return sIsDrawingPauseLink;
}
