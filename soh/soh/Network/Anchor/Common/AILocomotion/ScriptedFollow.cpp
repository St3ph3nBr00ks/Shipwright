/**
 * ScriptedFollow — implementation. See ScriptedFollow.h for design.
 */

#include "ScriptedFollow.h"

#include "../../../Enhancements/RoomNavData/RoomNavData.h"  // NODE_CLIMB_ANY

namespace AnchorAI {

ScriptedFollowResult RunScriptedFollowStep(
    const Actor*          navigator,
    const Vec3f&          target,
    NavState&             navState,
    const FallbackPolicy& policy,
    PlayState*            play)
{
    ScriptedFollowResult result;
    result.nav = ChooseSubgoal(navigator, target, navState, policy, play);

    // Climb-cell detection — true iff the substrate path's current
    // subgoal carries any of NODE_CLIMB_LADDER / NODE_CLIMB_VINE /
    // NODE_CLIMB_DESIGNATED_WALL / NODE_CLIMB_GENERIC_WALL. Only
    // meaningful when usingNavMesh = true (direct-yaw fallback's
    // subgoal is the raw target position, no flags).
    result.shouldEngageClimb =
        result.nav.usingNavMesh &&
        (result.nav.subgoalFlags & ::AnchorNavRoom::NODE_CLIMB_ANY) != 0;

    return result;
}

}  // namespace AnchorAI
