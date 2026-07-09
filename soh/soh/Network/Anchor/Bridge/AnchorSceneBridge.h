// AnchorSceneBridge — read accessors for game-thread scene state.
//
// Purpose (variant C landing, 2026-07-09): before this file existed,
// Anchor catchup code read `gPlayState->roomCtx.status` and
// `gPlayState->roomCtx.curRoom.segment` directly to gate deferred
// teleport application. Two-level member access from Anchor code
// violates Law of Demeter and scatters the "is the room really
// live?" predicate across multiple sites.
//
// This bridge centralises the room-ready predicate behind an
// intent-named accessor. Other scene-state reads (scene number,
// linkAge, csCtx queries) belong here as they arise.
//
// Threading: all functions run on the game thread. gPlayState-scoped
// state must not be read from network threads.
//
// See Analysis/peer_room_load_vs_cutscene_catchup_2026-07-09.md
// §5 variant C and §8.1 for the design rationale.

#pragma once

namespace AnchorSceneBridge {

// ---- Room readiness gate (variant C) --------------------------------
//
// Returns true iff the current room is fully loaded AND its mesh
// pointer is live. Both checks are required:
//
//   - roomCtx.status == 0 — no in-flight DMA load. `curRoom.num`
//     transitions synchronously at func_8009728C call time (before
//     DMA completes), so `curRoom.num == target` alone is NOT a
//     "room is ready" signal.
//
//   - roomCtx.curRoom.segment != nullptr — mesh pointer valid. This
//     covers two invalidation paths:
//       (a) mid-DMA state where func_800973FC hasn't set segment
//           yet (see z_room.c:614-618).
//       (b) mid-cutscene script null-out via
//           Cutscene_Command_TransitionFX case 24 (z_demo.c:380).
//
// Only when both conditions hold can Anchor code safely teleport
// Link or perform floor-poly reads against the current room's mesh.
// Returns false when gPlayState is null (pre-Play_Init window).
bool IsCurrentRoomFullyLoaded();

}  // namespace AnchorSceneBridge
