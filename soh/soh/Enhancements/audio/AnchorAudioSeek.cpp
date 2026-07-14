#include "AnchorAudioSeek.h"

// Mirror of AudioEditor.cpp:1-22 include preamble. functions.h
// transitively pulls in z64.h + libultraship ultra headers (which
// provide _SHIFTL). Both z64.h and functions.h self-manage their own
// extern "C" guards internally, so bare includes are C++-safe and pick
// up the ultra types + audio function declarations correctly.
// libultraship first to seed the C++ template chain (Pitfall 40).
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>
#include <functions.h>    // Audio_QueueCmdS32 (transitively pulls z64.h
                          // + libultra.h → _SHIFTL macro + ultratypes).

// Tempo constants (verified via source audit — see file-header analysis
// doc reference).
//   - z64audio.h:14      : TATUMS_PER_BEAT = 48
//   - audio_seqplayer.c:1830: default tempo = 120 * TATUMS_PER_BEAT
//                            = 5760 tatums/min = 96 tatums/sec.
//
// cannot verify: songs with in-script tempo changes will drift from this
// constant (audio_seqplayer.c:1145/1300/1314 have in-script `tempo` writes
// that mutate SequencePlayer::tempo mid-song). For v1 pilot (Deku Tree
// intro) empirically-stable tempo is assumed; broader per-song coverage
// deferred to v2 per Analysis/soh_audio_seek_investigation_2026-07-07.md
// §4.2 R1.
namespace {
constexpr int kTatumsPerSecondDefault = 96;

// Player range check: SequencePlayerId enum in sequence.h:120-125
// enumerates 0..3. Vanilla `gAudioContext.seqPlayers` is a fixed array
// of 4; out-of-range write would corrupt neighbouring audio state.
constexpr int kMaxSeqPlayerIdx = 3;

// Sequence-id range check: NA_BGM_DISABLED = 0xFFFF is the "no music"
// sentinel; a valid sequence id fits in one byte (see the vanilla
// _SHIFTL(seqId, 8, 8) at code_800F9280.c:48 — one-byte field). Reject
// anything >= 0xFF at the wrapper to avoid queueing a broken command.
constexpr int kMaxSeqId = 0xFE;
}  // namespace

extern "C" int Anchor_SeekBGMToMs(int playerIdx, int seqId, int offsetMs) {
    // Bounds checks — fail fast, log at INFO for the diagnostic trail.
    if (playerIdx < 0 || playerIdx > kMaxSeqPlayerIdx) {
        SPDLOG_INFO("[AnchorAudioSeek] REJECT playerIdx={} out of range (0..{})",
                    playerIdx, kMaxSeqPlayerIdx);
        return 1;
    }
    if (seqId < 0 || seqId > kMaxSeqId) {
        SPDLOG_INFO("[AnchorAudioSeek] REJECT seqId={} out of range (0..{})",
                    seqId, kMaxSeqId);
        return 2;
    }
    if (offsetMs < 0) {
        SPDLOG_INFO("[AnchorAudioSeek] REJECT offsetMs={} negative", offsetMs);
        return 3;
    }
    if (offsetMs == 0) {
        // Caller should use conventional Audio_StartSeq for start-from-
        // zero; this helper is specifically for non-zero seeks and would
        // enqueue a zero-tick skip that just wastes a queue slot. Return
        // a distinct rejection code so the caller can fall through.
        SPDLOG_INFO("[AnchorAudioSeek] REJECT offsetMs=0 — caller should use "
                    "Audio_StartSeq instead");
        return 4;
    }

    // ms → tatum-ticks. Guarded against 32-bit overflow: at 96 tatums/s
    // the multiplication overflows int32 only for offsetMs > ~22M ms
    // (~6 hours), well beyond any cutscene. Still, promote to 64-bit
    // during the multiply for safety.
    const int64_t skipTicks64 =
        (static_cast<int64_t>(offsetMs) * kTatumsPerSecondDefault) / 1000;
    if (skipTicks64 <= 0) {
        // Shouldn't happen given the offsetMs > 0 check above, but a
        // sub-1000-ms offset combined with a tiny custom tempo constant
        // could theoretically round to zero. Defensive guard.
        SPDLOG_INFO("[AnchorAudioSeek] REJECT skipTicks<=0 after conversion "
                    "(offsetMs={})", offsetMs);
        return 4;
    }
    const int32_t skipTicks = static_cast<int32_t>(skipTicks64);

    // Enqueue via Audio_QueueCmdS32 — same conveyor vanilla uses at
    // code_800F9280.c:48. Opcode 0x85 decodes as {arg0=playerIdx,
    // arg1=seqId, data=skipTicks} at code_800E4FE0.c:257 which dispatches
    // to AudioLoad_SyncInitSeqPlayerSkipTicks at audio_load.c:565.
    //
    // The u32 command word: 0x85 in bits 24-31, playerIdx in bits 16-23,
    // seqId in bits 8-15, low byte unused. See vanilla pattern.
    //
    // cannot verify: whether the audio-thread execution of the queued
    // command interacts with SoH's Music Randomizer sequence-replacement
    // path (audio_load.c:589-591 reads gAudioContext.seqReplaced[]).
    // Analysis doc §6 flags this — expected to work because sequence
    // swap happens BEFORE the fast-forward runs, but not runtime-verified.
    const u32 cmdWord = 0x85000000u
                        | _SHIFTL(static_cast<u32>(playerIdx), 16, 8)
                        | _SHIFTL(static_cast<u32>(seqId), 8, 8);
    Audio_QueueCmdS32(cmdWord, skipTicks);

    SPDLOG_INFO("[AnchorAudioSeek] playerIdx={} seqId={:#x} offsetMs={} skipTicks={}",
                playerIdx, seqId, offsetMs, skipTicks);
    return 0;
}
