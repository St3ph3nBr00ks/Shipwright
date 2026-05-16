/**
 * DirectorDebugDraw — in-world overlay for AI Director state.
 *
 * Step-14 / step-15 development aid. Renders a red vertical post 60u
 * tall at each registered descriptor's last successful spawn position,
 * so users can locate orphan invaders ("liveCount=1 but I can't find
 * the Stalfos") by line-of-sight.
 *
 * Gated by gEnhancements.AI.Director.DebugDraw (default 0). Permanently
 * registered via ShipInit; the per-frame hook short-circuits when the
 * CVar is off, so zero production overhead.
 *
 * Architecture mirrors soh/soh/Enhancements/RoomNavData/RoomNavData.cpp
 * §6850 (OnDebugDrawRender). Specifically:
 *   - file-scope static Gfx + Vtx buffers reused per frame
 *   - InitDebugGfx pushes the cycle/render/combine setup once at the
 *     start of the display list
 *   - AddRedVerticalPost emits two perpendicular quads so the marker
 *     is visible from any camera angle without billboarding math
 *   - splice into POLY_XLU_DISP via global-namespace helper
 *     (DirectorDebugSpliceXluDispList — must be at global scope so
 *     OPEN_DISPS / CLOSE_DISPS macros don't expand inside a namespace,
 *     same linkage rationale as RoomNavData's helper)
 *
 * Plan: ai_director_plan.md §6 (debug panel section) + step 10
 * (DebugDraw overlay — partially landed here for the last-spawn-pos
 * marker only; candidate-node visualization is still deferred).
 */

#include "Director.h"
#include "Descriptors/InvaderDescriptor.h"
#include "Descriptors/TestDescriptor.h"

#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultraship.h>

#include <vector>

extern "C" {
#include "variables.h"
#include "z64.h"
}

extern "C" PlayState* gPlayState;

// ---------------------------------------------------------------------------
// Global-namespace splice helper. Same rationale as RoomNavData.cpp:103 —
// OPEN_DISPS / CLOSE_DISPS macros expand to forward-decls of
// FrameInterpolation_RecordOpen/CloseChild without extern "C" qualifiers,
// which under a C++ namespace would shadow the C-linkage definitions from
// frame_interpolation.h. Keeping the splice at global scope preserves the
// C-linkage resolution. Forward-declared in AnchorDirector via ::-qualified
// call site below.
// ---------------------------------------------------------------------------
static void DirectorDebugSpliceXluDispList(PlayState* play, Gfx* dl) {
    OPEN_DISPS(play->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, dl);
    CLOSE_DISPS(play->state.gfxCtx);
}

namespace AnchorDirector {
namespace {

// Per-frame display list + vertex buffers. Static / reused / cleared.
// vertex storage MUST be a vector with reserve sized for the max marker
// count so pointer stability across the gsSPVertex calls is preserved
// (RoomNavData hit this trap; see RoomNavData.cpp:6864 comment).
static std::vector<Gfx> sDl;
static std::vector<Vtx> sVtxDl;

// Visual constants.
constexpr float kPostHeight    = 60.0f;
constexpr float kPostHalfWidth = 4.0f;
constexpr uint8_t kRedR        = 0xFF;
constexpr uint8_t kRedG        = 0x20;
constexpr uint8_t kRedB        = 0x20;

// Vertex builder. Mirrors RoomNavData.cpp:5956 MakeVtxN.
static Vtx MakeVtx(short x, short y, short z, unsigned char a) {
    Vtx v{};
    v.n.ob[0] = x;  v.n.ob[1] = y;  v.n.ob[2] = z;
    v.n.flag  = 0;
    v.n.tc[0] = 0;  v.n.tc[1] = 0;
    v.n.n[0]  = 0;  v.n.n[1]  = 127;  v.n.n[2]  = 0;
    v.n.a     = a;
    return v;
}

// Initialize the GFX state at the head of the display list.
// Mirrors RoomNavData.cpp:5971 InitDebugGfx.
static void InitDebugGfx() {
    uint32_t rm   = Z_CMP | IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_DEC;
    uint32_t blc1 = GBL_c1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    uint32_t blc2 = GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA);
    constexpr uint8_t alpha = 0xE0;

    sDl.push_back(gsSPTexture(0, 0, 0, G_TX_RENDERTILE, G_OFF));
    sDl.push_back(gsDPSetCycleType(G_CYC_1CYCLE));
    sDl.push_back(gsDPSetRenderMode(rm | blc1, rm | blc2));
    sDl.push_back(gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE));
    sDl.push_back(gsSPLoadGeometryMode(G_ZBUFFER));
    sDl.push_back(gsDPSetEnvColor(0xFF, 0xFF, 0xFF, alpha));
    sDl.push_back(gsSPMatrix(&gMtxClear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH));
    sDl.push_back(gsDPSetPrimColor(0, 0, kRedR, kRedG, kRedB, 0xFF));
}

// Append two perpendicular vertical quads forming a "post" from basePos
// to basePos+(0,kPostHeight,0). Visible from any camera angle.
// Mirrors RoomNavData.cpp:6046 AddVerticalPost.
static void AddRedVerticalPost(const Vec3f& basePos) {
    const float h  = kPostHalfWidth;
    const short bx = (short)basePos.x;
    const short by = (short)basePos.y;
    const short bz = (short)basePos.z;
    const short ty = (short)(basePos.y + kPostHeight);

    // Quad 1: extends in ±X (visible from along-Z viewpoints).
    sVtxDl.push_back(MakeVtx((short)(bx - h), by, bz, 0xFF));
    sVtxDl.push_back(MakeVtx((short)(bx + h), by, bz, 0xFF));
    sVtxDl.push_back(MakeVtx((short)(bx + h), ty, bz, 0xFF));
    sVtxDl.push_back(MakeVtx((short)(bx - h), ty, bz, 0xFF));
    sDl.push_back(gsSPVertex((uintptr_t)&sVtxDl[sVtxDl.size() - 4], 4, 0));
    sDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));

    // Quad 2: extends in ±Z (visible from along-X viewpoints).
    sVtxDl.push_back(MakeVtx(bx, by, (short)(bz - h), 0xFF));
    sVtxDl.push_back(MakeVtx(bx, by, (short)(bz + h), 0xFF));
    sVtxDl.push_back(MakeVtx(bx, ty, (short)(bz + h), 0xFF));
    sVtxDl.push_back(MakeVtx(bx, ty, (short)(bz - h), 0xFF));
    sDl.push_back(gsSPVertex((uintptr_t)&sVtxDl[sVtxDl.size() - 4], 4, 0));
    sDl.push_back(gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0));
}

// Per-frame hook. Called from OnPlayDrawEnd. Walks every registered
// descriptor; for those that have a last-spawn position, appends a red
// vertical post to the display list. Splices once at the end.
static void OnDirectorDebugDrawRender() {
    if (CVarGetInteger(CVAR_ENHANCEMENT("AI.Director.DebugDraw"), 0) == 0) {
        return;
    }
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }

    sDl.clear();
    sVtxDl.clear();
    // Reserve enough for current descriptor count × 8 verts per post
    // (2 quads × 4 verts). Two descriptors today; reserve a generous
    // bound for future descriptor adds.
    constexpr size_t kMaxMarkers = 16;
    sDl.reserve(kMaxMarkers * 2 + 16);   // 2 Gfx per quad + setup
    sVtxDl.reserve(kMaxMarkers * 8);

    InitDebugGfx();

    Director& director = Director::Instance();
    bool anyDrawn = false;

    // InvaderDescriptor — render red post at last-spawn position.
    for (const auto& desc : director.GetDescriptors()) {
        if (auto* inv = dynamic_cast<InvaderDescriptor*>(desc.get())) {
            if (inv->HasLastSpawn()) {
                AddRedVerticalPost(inv->LastSpawnPos());
                anyDrawn = true;
            }
        }
        // TestDescriptor not rendered today — InvaderDescriptor is the
        // visualization target per user request. Add additional
        // descriptor visualizations here as needed.
    }

    if (!anyDrawn) {
        return;  // no markers this frame; skip splice
    }

    sDl.push_back(gsSPEndDisplayList());
    ::DirectorDebugSpliceXluDispList(play, sDl.data());
}

}  // namespace

// Registered once at SoH boot via ShipInit. Hook is unconditional;
// per-frame body short-circuits on CVar miss for zero off-state cost.
//
// At namespace scope (not anon) so the global-scope ShipInit registrar
// below can resolve it via qualified name.
void RegisterDirectorDebugDraw() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDrawEnd>(
        OnDirectorDebugDrawRender);
}

}  // namespace AnchorDirector

static RegisterShipInitFunc registerDirectorDebugDraw(
    AnchorDirector::RegisterDirectorDebugDraw);
