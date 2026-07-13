#ifndef SOH_ENHANCEMENTS_AUDIO_ANCHOR_AUDIO_SEEK_H
#define SOH_ENHANCEMENTS_AUDIO_ANCHOR_AUDIO_SEEK_H

// Anchor cutscene-catchup audio-seek helper.
//
// Wraps vanilla `AudioLoad_SyncInitSeqPlayerSkipTicks` (audio_load.c:565)
// via audio command opcode 0x85 (code_800E4FE0.c:257), following the
// vanilla `func_800F9280` pattern at code_800F9280.c:44-48. Given an
// offset in ms, translates to tatum-ticks using the default tempo
// (120 BPM * TATUMS_PER_BEAT=48 => 96 tatums/sec, verified at
// audio_seqplayer.c:1830 + z64audio.h:14) and enqueues a silent
// fast-forward followed by audible resume.
//
// Plan: Claude/Plans/cutscene_late_join_plan.md — jump-and-catch-up
//   apply path (§3.4) uses this helper to align music playback with
//   the leader's mid-cutscene position.
// Analysis: Claude/Analysis/soh_audio_seek_investigation_2026-07-07.md
//   — full source-verified rationale for choosing this primitive over
//   any custom seek path.
//
// SOLID / SoC / KISS / Law of Demeter: three-line delegator to an
// existing production-tested vanilla primitive; no new audio-thread
// state, no new opcode, no mutex work. See §7 of the analysis doc.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Seek BGM playback on `playerIdx` for sequence `seqId` to an offset
// of `offsetMs` from the sequence start. Silently fast-forwards the
// sequence script by the tatum-tick equivalent of `offsetMs`, then
// resumes audible playback from that position.
//
// playerIdx: 0..3 — SEQ_PLAYER_BGM_MAIN (0), SEQ_PLAYER_FANFARE (1),
//            SEQ_PLAYER_SFX (2), SEQ_PLAYER_BGM_SUB (3). See
//            sequence.h:120-125. Typical cutscene BGM: 0.
// seqId:    NA_BGM_* — the sequence to load. 0..0x7F valid.
// offsetMs: ms to skip from sequence start; 0 falls through to a
//           normal load-from-zero (delegated to vanilla path).
//
// Returns 0 on success. Non-zero on rejection:
//   1 = playerIdx out of range
//   2 = seqId out of range (>= 0xFF)
//   3 = offsetMs negative
//   4 = offsetMs is 0 — caller should use the normal Audio_StartSeq
//       path instead (this helper is specifically for non-zero seeks).
//
// Caller (game-thread only — this queues via Audio_QueueCmdS32 which
// is the same conveyor vanilla uses at code_800F9280.c:48).
int Anchor_SeekBGMToMs(int playerIdx, int seqId, int offsetMs);

#ifdef __cplusplus
}
#endif

#endif  // SOH_ENHANCEMENTS_AUDIO_ANCHOR_AUDIO_SEEK_H
