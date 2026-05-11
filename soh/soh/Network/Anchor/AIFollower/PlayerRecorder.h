/**
 * Player Diagnostic Recorder — per-frame JSONL capture of the local
 * player's input, position, rotation, and full state-flag set for
 * post-hoc analysis of manual navigation paths.
 *
 * Companion to FollowerRecorder: where FollowerRecorder captures what
 * the AI follower DID, PlayerRecorder captures what a HUMAN did along
 * a chosen path. Compare the two to identify where the follower's
 * logic falls short — e.g. "the human used these drop anchors to
 * descend; the follower routed 300u around them."
 *
 * Output: logs/Ship of Harkinian <N> player.log, one JSON object per
 * line. Same naming scheme as FollowerRecorder; <N> matches the
 * session's main log index. Capture rate gated by CVar
 * gDeveloperTools.PlayerRecorder.CaptureHz (default 15 Hz). Master
 * toggle gDeveloperTools.PlayerRecorder.Enabled (default off).
 *
 * Pure observer pattern — does not mutate any state.
 *
 * Public surface:
 *   - RegisterPlayerRecorder()     — boot-time ShipInit registration.
 *                                    Hooks OnGameFrameUpdate for tick.
 *   - IsPlayerRecorderActive()     — cheap query.
 */
#pragma once

namespace AnchorFollower {

void RegisterPlayerRecorder();
bool IsPlayerRecorderActive();

} // namespace AnchorFollower
