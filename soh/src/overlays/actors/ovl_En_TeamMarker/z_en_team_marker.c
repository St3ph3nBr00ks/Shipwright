/*
 * File: z_en_team_marker.c
 * Overlay: ovl_En_TeamMarker
 * Description: SoH Team Marker — through-walls fairy indicator for
 *              same-team teammates (Flotilla).
 *
 * Design plan: Plans/team_marker_plan.md.
 * Tracker: St3ph3nBr00ks/Shipwright#219.
 *
 * v1 Phase 1: actor scaffold + static Navi fairy draw at rest pose.
 * Z-tested render mode for smoke-test. Phase 3 flips to G_RM_CLD_SURF2
 * (no-Z) for through-walls visibility. Phase 2 wires per-owner colour
 * tinting via a draw-context flag.
 *
 * No spawn director yet (Phase 5). Smoke-test spawn via debug console.
 */

#include "z_en_team_marker.h"
#include "objects/gameplay_keep/gameplay_keep.h"  // gFairySkel, gFairyAnim

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// Number of game-ticks the appear / disappear fades run for. 15 frames
// gives ~250ms at 60fps and ~750ms at vanilla 20fps — a noticeable
// but not-lingering fade at either extreme. Framerate-agnostic tuning
// via Anchor::MsToGameTicks would be more precise but adds coupling;
// hardcode for v1 and revisit if the fade feels wrong at unlocked
// framerates.
#define EN_TEAM_MARKER_FADE_FRAMES 15

// Owner-lookup + draw-context flag. Bodies live in
// soh/soh/Network/Anchor/TeamMarker/TeamMarker.cpp.
extern void Anchor_TeamMarkerDrawBegin(Actor* marker);
extern void Anchor_TeamMarkerDrawEnd(void);
extern int  Anchor_GetTeamMarkerColor(Actor* marker, unsigned char* r, unsigned char* g, unsigned char* b);

// Cutscene suppression. Returns non-zero when the marker should skip
// draw this frame (cutscene running).
extern int  Anchor_TeamMarkerShouldSuppress(PlayState* play);

// ─── Sparkle port notes (deferred, see comment) ─────────────────────
//
// Skipped for v1 to keep visual noise down when multiple peers are on
// screen. If we want them back:
//
//   1. Copy EnElf_SpawnSparkles verbatim from z_en_elf.c:1185 into this
//      file (or into TeamMarker.cpp with an extern "C" wrapper). It's
//      ~20 lines; the only external primitive it uses is
//      EffectSsKiraKira_SpawnDispersed which is a public helper —
//      accessible from anywhere without adding an extern to en_elf.
//   2. Adapt the color source: EnElf_SpawnSparkles reads
//      this->innerColor / this->outerColor. We'd feed it the peer's
//      Anchor color via Anchor_GetTeamMarkerColor.
//   3. Fire it in EnTeamMarker_Update on the same edges Navi does —
//      once per few frames while APPEARING (visState transition
//      HIDDEN→APPEARING), and again on DISAPPEARING. Navi calls it
//      with sparkleLife=16 for short bursts (z_en_elf.c:625,861,910)
//      and =32 for longer bursts (:738,773). 16 is the right starting
//      point for our fade duration.
//   4. Consider a CVar gate — Cosmetic → Fairies → "Team marker
//      sparkles" — so users on lower-end hardware can disable.
//
// Net effort: ~30 LOC across the actor + tick driver.

// Override-limb callback:
//   - Limb 8 (fairy body sphere) — scale with a small sine-driven pulse,
//     matching Navi's visual (EnElf_OverrideLimbDraw at z_en_elf.c:1468).
//   - Limbs 4/7/11/14 (wings) — prepend a no-Z render mode set into `*gfx`
//     so the wing DLs render through walls. The top-level POLY_XLU_DISP
//     render mode set in Draw() propagates to the body correctly, but the
//     wing limb DLs contain their own gDPSetOtherMode / render-mode
//     ops that override our top-level setting. This per-limb prepend
//     via the override's `gfx` writer forces the correct mode right
//     before each wing limb executes.
static s32 EnTeamMarker_OverrideLimbDraw(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot,
                                         void* thisx, Gfx** gfx) {
    static Vec3f zeroVec = { 0.0f, 0.0f, 0.0f };
    EnTeamMarker* this = (EnTeamMarker*)thisx;
    f32 scale;
    Vec3f mtxMult;

    if (limbIndex == 8) {
        scale = ((Math_SinS(this->timer * 4096) * 0.1f) + 1.0f) * 0.012f;
        scale *= (this->actor.scale.x * 124.99999f);
        Matrix_MultVec3f(&zeroVec, &mtxMult);
        Matrix_Translate(mtxMult.x, mtxMult.y, mtxMult.z, MTXMODE_NEW);
        Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    } else if (limbIndex == 4 || limbIndex == 7 || limbIndex == 11 || limbIndex == 14) {
        // Wing limbs — force no-Z + clear the Z-buffer geometry bit so
        // the wing DL's own state can't reintroduce Z-comparison.
        gDPPipeSync((*gfx)++);
        gSPClearGeometryMode((*gfx)++, G_ZBUFFER);
        gDPSetRenderMode((*gfx)++, G_RM_PASS, G_RM_CLD_SURF2);
    }
    return false;
}

void EnTeamMarker_Init(Actor* thisx, PlayState* play) {
    EnTeamMarker* this = (EnTeamMarker*)thisx;

    ActorShape_Init(&thisx->shape, 0.0f, NULL, 15.0f);
    thisx->shape.shadowAlpha = 0;  // marker floats — no ground shadow

    SkelAnime_Init(play, &this->skelAnime, &gFairySkel, &gFairyAnim, this->jointTable, this->morphTable, 15);

    // Base scale matches Navi's ICHAIN_VEC3F_DIV1000(scale, 8) — 0.008 world units.
    Actor_SetScale(thisx, 0.008f);

    this->timer      = 0;
    this->obscured   = 0;
    this->visState   = EN_TEAM_MARKER_VIS_HIDDEN;
    this->fadeTimer  = 0;
    this->baseScale  = 0.008f;
}

void EnTeamMarker_Destroy(Actor* thisx, PlayState* play) {
    EnTeamMarker* this = (EnTeamMarker*)thisx;

    SkelAnime_Free(&this->skelAnime, play);
    // Phase 4: NameTag_RemoveAllForActor(thisx) if not auto-cleaned via
    // OnActorDestroy hook. Audit at Phase 4 time.
}

void EnTeamMarker_Update(Actor* thisx, PlayState* play) {
    EnTeamMarker* this = (EnTeamMarker*)thisx;

    this->timer++;
    SkelAnime_Update(&this->skelAnime);
    // world.pos + shape.rot driven by spawn director (Phase 5); Phase 1
    // uses the actor's spawn position + zero rotation.

    // Visibility state machine — drives the appear / disappear fade
    // based on LOS transitions. `this->obscured` is set by the spawn
    // director's per-tick raycast in TeamMarker.cpp; state machine
    // reacts to the boolean edge and ticks a countdown timer during
    // fades. Shape mirrors Navi's alpha-scale mechanic
    // (z_en_elf.c disappearTimer + Draw scale/alpha ramp).
    switch (this->visState) {
        case EN_TEAM_MARKER_VIS_HIDDEN:
            if (this->obscured) {
                this->visState  = EN_TEAM_MARKER_VIS_APPEARING;
                this->fadeTimer = EN_TEAM_MARKER_FADE_FRAMES;
            }
            break;
        case EN_TEAM_MARKER_VIS_APPEARING:
            if (!this->obscured) {
                // LOS flipped mid-fade — reverse.
                this->visState = EN_TEAM_MARKER_VIS_DISAPPEARING;
            } else if (this->fadeTimer > 0) {
                this->fadeTimer--;
            } else {
                this->visState = EN_TEAM_MARKER_VIS_SHOWN;
            }
            break;
        case EN_TEAM_MARKER_VIS_SHOWN:
            if (!this->obscured) {
                this->visState  = EN_TEAM_MARKER_VIS_DISAPPEARING;
                this->fadeTimer = EN_TEAM_MARKER_FADE_FRAMES;
            }
            break;
        case EN_TEAM_MARKER_VIS_DISAPPEARING:
            if (this->obscured) {
                this->visState = EN_TEAM_MARKER_VIS_APPEARING;
            } else if (this->fadeTimer > 0) {
                this->fadeTimer--;
            } else {
                this->visState = EN_TEAM_MARKER_VIS_HIDDEN;
            }
            break;
    }
}

void EnTeamMarker_Draw(Actor* thisx, PlayState* play) {
    EnTeamMarker* this = (EnTeamMarker*)thisx;
    Gfx* dListHead;
    s32 envAlpha;
    f32 fadeFactor;  // 0.0 (fully hidden) .. 1.0 (fully shown)
    u8 ownerR = 0xFF, ownerG = 0xFF, ownerB = 0xFF;  // white fallback

    // Cutscene suppression (only remaining suppression rule).
    if (Anchor_TeamMarkerShouldSuppress(play)) {
        return;
    }

    // Skip draw entirely in HIDDEN state (fully faded out or never
    // engaged). All other states (APPEARING/SHOWN/DISAPPEARING) render
    // with a computed fade factor below.
    if (this->visState == EN_TEAM_MARKER_VIS_HIDDEN) {
        return;
    }

    // Compute fade factor from the visibility state machine.
    //   APPEARING:    factor ramps 0 → 1 (fadeTimer counts down from
    //                 EN_TEAM_MARKER_FADE_FRAMES to 0).
    //   SHOWN:        factor = 1.
    //   DISAPPEARING: factor ramps 1 → 0 (fadeTimer counts down from
    //                 EN_TEAM_MARKER_FADE_FRAMES to 0).
    // Mirrors Navi's alphaScale mechanic (z_en_elf.c:1520) — same idea,
    // different trigger source (LOS instead of Navi's timed
    // disappearance).
    switch (this->visState) {
        case EN_TEAM_MARKER_VIS_APPEARING:
            fadeFactor = 1.0f - ((f32)this->fadeTimer / (f32)EN_TEAM_MARKER_FADE_FRAMES);
            break;
        case EN_TEAM_MARKER_VIS_DISAPPEARING:
            fadeFactor = (f32)this->fadeTimer / (f32)EN_TEAM_MARKER_FADE_FRAMES;
            break;
        case EN_TEAM_MARKER_VIS_SHOWN:
        default:
            fadeFactor = 1.0f;
            break;
    }
    if (fadeFactor < 0.0f) fadeFactor = 0.0f;
    if (fadeFactor > 1.0f) fadeFactor = 1.0f;

    // Apply the fade to the actor's scale as well, so the fairy visibly
    // grows in / shrinks out. Rebuilds the scale each frame from the
    // captured baseScale so successive fades don't compound.
    {
        f32 s = this->baseScale * (0.5f + 0.5f * fadeFactor);
        Actor_SetScale(thisx, s);
    }

    // Resolve owner Anchor colour. Falls back to white when the sidecar
    // map has no entry (e.g. marker being despawned this frame).
    Anchor_GetTeamMarkerColor(thisx, &ownerR, &ownerG, &ownerB);

    Anchor_TeamMarkerDrawBegin(thisx);

    dListHead = Graph_Alloc(play->state.gfxCtx, sizeof(Gfx) * 4);

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_27Xlu(play->state.gfxCtx);

    // Pulsing outer alpha — sawtooth 0..255..0 over ~5s, matching Navi.
    // Multiplied by fadeFactor so the pulse also fades during transitions.
    envAlpha = (this->timer * 50) & 0x1FF;
    envAlpha = (envAlpha > 255) ? 511 - envAlpha : envAlpha;
    envAlpha = (s32)(envAlpha * fadeFactor);

    // Force no-Z render mode at the TOP level of POLY_XLU_DISP for
    // limbs that DON'T touch render mode (body). Wing limbs get their
    // own per-limb no-Z injection in the override callback.
    gDPPipeSync(POLY_XLU_DISP++);
    gSPClearGeometryMode(POLY_XLU_DISP++, G_ZBUFFER);
    gDPSetRenderMode(POLY_XLU_DISP++, G_RM_PASS, G_RM_CLD_SURF2);

    gSPSegment(POLY_XLU_DISP++, 0x08, dListHead);
    gDPPipeSync(dListHead++);

    // Inner (prim) is a slightly-lightened owner colour so the fairy
    // body has a hot core; envColor is the fully-saturated owner tint
    // for the outer glow. Prim alpha and env alpha both scale with the
    // fade factor.
    gDPSetPrimColor(dListHead++, 0, 0x01,
                    (u8)MIN((s32)ownerR + 80, 255),
                    (u8)MIN((s32)ownerG + 80, 255),
                    (u8)MIN((s32)ownerB + 80, 255),
                    (u8)(255.0f * fadeFactor));

    // Belt-and-suspenders: mirror the top-level no-Z render mode inside
    // the segment-0x08 setup DL so limbs that DO reference segment 0x08
    // still get the correct mode after any intervening pipe syncs.
    gDPSetRenderMode(dListHead++, G_RM_PASS, G_RM_CLD_SURF2);

    gSPEndDisplayList(dListHead++);
    gDPSetEnvColor(POLY_XLU_DISP++, ownerR, ownerG, ownerB, (u8)envAlpha);

    POLY_XLU_DISP = SkelAnime_DrawSkeleton2(play, &this->skelAnime, EnTeamMarker_OverrideLimbDraw, NULL, this,
                                            POLY_XLU_DISP);

    CLOSE_DISPS(play->state.gfxCtx);

    Anchor_TeamMarkerDrawEnd();
}
