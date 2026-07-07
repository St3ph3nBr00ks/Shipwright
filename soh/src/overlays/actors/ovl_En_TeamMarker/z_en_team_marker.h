#ifndef Z_EN_TEAM_MARKER_H
#define Z_EN_TEAM_MARKER_H

#include <libultraship/libultra.h>
#include "global.h"

// SoH Team Marker (Flotilla — see Plans/team_marker_plan.md, tracker #219).
//
// Through-walls fairy indicator over each same-team peer's head. Owned +
// spawned locally by the spawn director in
// soh/soh/Network/Anchor/TeamMarker/TeamMarker.cpp; no wire packets.
// Reads position / color / name / team / scene from the existing
// Anchor::clients map.
//
// v1 Phase 1 scaffold: registers via ActorDB, renders the vanilla Navi
// fairy DL at rest pose. Colour tinting (Phase 2), through-walls draw
// mode (Phase 3), nameplate (Phase 4), spawn director (Phase 5), and
// suppression rules (Phase 6) land incrementally.

struct EnTeamMarker;

// Visibility state machine — copies the shape of Navi's disappearTimer /
// alpha-scale mechanic (z_en_elf.c:1520 + 654-663), adapted to trigger on
// LOS transitions instead of Navi's inventory / bottle logic.
typedef enum {
    EN_TEAM_MARKER_VIS_HIDDEN       = 0,  // not drawn
    EN_TEAM_MARKER_VIS_APPEARING    = 1,  // ramping alpha 0 → 1 + scale up
    EN_TEAM_MARKER_VIS_SHOWN        = 2,  // full opacity
    EN_TEAM_MARKER_VIS_DISAPPEARING = 3,  // ramping alpha 1 → 0 + scale down
} EnTeamMarkerVisState;

typedef struct EnTeamMarker {
    /* 0x0000 */ Actor actor;
    /* 0x014C */ SkelAnime skelAnime;
    /* 0x0190 */ Vec3s jointTable[15];
    /* 0x01EA */ Vec3s morphTable[15];
    /* 0x0244 */ u32 timer;      // drives envAlpha pulse + limb-8 scale sine
    /* 0x0248 */ s32 obscured;   // LOS gate — set by spawn director each tick.
                                 // Drives fade state machine below (1 == peer obscured,
                                 // want fairy visible; 0 == peer visible, want hidden).
    /* 0x024C */ s32 visState;   // one of EnTeamMarkerVisState (see enum above)
    /* 0x0250 */ s32 fadeTimer;  // counts down during APPEARING/DISAPPEARING (frames)
    /* 0x0254 */ f32 baseScale;  // captured at Init; multiplied by fade factor in Draw
} EnTeamMarker;

#ifdef __cplusplus
extern "C" {
#endif

// Dynamic actor id allocated by ActorDB::AddBuiltInCustomActors. Defined
// in soh/src/code/z_play.c alongside gEnPartnerId / gEnFollowerId /
// gEnInvaderId.
extern s16 gEnTeamMarkerId;

void EnTeamMarker_Init(Actor* thisx, PlayState* play);
void EnTeamMarker_Destroy(Actor* thisx, PlayState* play);
void EnTeamMarker_Update(Actor* thisx, PlayState* play);
void EnTeamMarker_Draw(Actor* thisx, PlayState* play);

#ifdef __cplusplus
}
#endif

#endif
