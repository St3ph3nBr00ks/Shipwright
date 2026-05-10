/**
 * AI Follower Diagnostic Recorder — per-frame JSONL capture of follower
 * decision state for post-hoc debug analysis.
 *
 * Spec: Claude/Plans/follower_recorder_plan.md.
 *
 * Output: roommanifests/follower_recordings/follower_<msEpoch>.jsonl,
 * one JSON object per line. Capture rate gated by CVar; default off.
 *
 * Pure observer pattern — does not mutate follower state.
 *
 * Public surface:
 *   - RegisterFollowerRecorder()   — boot-time ShipInit registration.
 *   - IsRecorderActive()           — cheap inline-check; CaptureFrame /
 *                                    QueueRecorderEvent return early when
 *                                    false.
 *   - CaptureFrame(ctx)            — write one JSONL line. Called from
 *                                    Anchor::TickFollower at end-of-tick.
 *   - QueueRecorderEvent(ev)       — push edge-event into current frame's
 *                                    events array (drained on CaptureFrame).
 */
#pragma once

#include <string_view>

namespace AnchorFollower {

struct FollowerFrameContext;

void RegisterFollowerRecorder();
bool IsRecorderActive();
void CaptureFrame(const FollowerFrameContext& ctx);
void QueueRecorderEvent(std::string_view ev);

} // namespace AnchorFollower
