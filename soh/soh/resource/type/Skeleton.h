#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>
#include <ship/resource/Resource.h>
#include <ship/resource/archive/Archive.h>
#include "SkeletonLimb.h"
#include <z64animation.h>

namespace SOH {

enum class SkeletonType {
    Normal,
    Flex,
    Curve,
};

// typedef struct {
//     /* 0x00 */ Vec3s jointPos; // Root is position in model space, children are relative to parent
//     /* 0x06 */ u8 child;
//     /* 0x07 */ u8 sibling;
//     /* 0x08 */ Gfx* dList;
// } StandardLimb; // size = 0xC

// Model has limbs with only rigid meshes
typedef struct {
    /* 0x00 */ void** segment;
    /* 0x04 */ uint8_t limbCount;
    uint8_t skeletonType;
} SkeletonHeader; // size = 0x8

// Model has limbs with flexible meshes
typedef struct {
    /* 0x00 */ SkeletonHeader sh;
    /* 0x08 */ uint8_t dListCount;
} FlexSkeletonHeader; // size = 0xC

// typedef struct {
//     /* 0x0000 */ u8 firstChildIdx;
//     /* 0x0001 */ u8 nextLimbIdx;
//     /* 0x0004 */ Gfx* dList[2];
// } SkelCurveLimb; // size = 0xC

typedef struct {
    /* 0x0000 */ SkelCurveLimb** limbs;
    /* 0x0004 */ uint8_t limbCount;
} SkelCurveLimbList; // size = 0x8

union SkeletonData {
    SkeletonHeader skeletonHeader;
    FlexSkeletonHeader flexSkeletonHeader;
    SkelCurveLimbList skelCurveLimbList;
};

class Skeleton : public Ship::Resource<SkeletonData> {
  public:
    using Resource::Resource;

    Skeleton() : Resource(std::shared_ptr<Ship::ResourceInitData>()) {
    }

    SkeletonData* GetPointer();
    size_t GetPointerSize();

    SkeletonType type;
    SkeletonData skeletonData;

    LimbType limbType;
    int limbCount;
    int dListCount;
    LimbType limbTableType;
    int limbTableCount;
    std::vector<StandardLimb> standardLimbArray;
    std::vector<SkelCurveLimb> curveLimbArray;
    std::vector<std::string> limbTable;
    std::vector<void*> skeletonHeaderSegments;
};

// Pre-baked display lists for a remote DummyPlayer's character model.
//
// Standard character-pack skeletons reference their display lists via "__OTR__" path
// strings stored in limb dLists[0] pointers.  At render time, gSPDisplayList intercepts
// these and resolves them through the global ResourceManager DL cache — which may return
// the LOCAL player's character pack textures if both packs share the same resource path.
//
// BakedPlayerModel fixes this by:
//   1. Opening the remote player's pack archive transiently (not in ArchiveManager).
//   2. Loading each limb's DL from that archive, then walking the GBI instructions and
//      replacing every OTR path/hash reference with a "coopchar/<folder>/..." unique key.
//   3. Pre-loading all referenced textures/vtx resources into the ResourceManager cache
//      under those unique keys so the interpreter finds them at render time.
//   4. Replacing sub-DL FILEPATH/HASH commands with direct F3DEX2_G_DL (0xDE) calls,
//      so baked sub-DLs are also executed without any global cache lookup.
//   5. Building per-DummyPlayer LodLimb copies whose dLists[0] point to the baked Gfx
//      arrays.  skelAnime->skeleton is pointed at segmentPtrs.data() instead of the
//      shared global skeletonHeaderSegments vector.
//
// Memory stability contract:
//   BakedPlayerModel is held via unique_ptr in AnchorClient so its address is stable
//   even if the clients unordered_map rehashes.  std::deque guarantees that existing
//   element references are not invalidated by push_back, so c_str() pointers embedded
//   in baked GBI instructions remain valid for the lifetime of the BakedPlayerModel.
struct BakedPlayerModel {
    std::vector<LodLimb>          limbCopies;   // per-limb LodLimb with patched dLists[0]
    std::vector<void*>            segmentPtrs;  // &limbCopies[i] — forms the skeleton->segment array
    std::deque<std::vector<Gfx>>  bakedDLs;     // owns baked Gfx arrays; stable data() after push_back
    std::deque<std::string>       pathStrings;  // stable c_str() for GBI w1 path pointers
    // Per-slot "coopchar/<folder>/..." cache keys for the eye and mouth textures
    // bound by Player_DrawImpl via gSPSegment (z_player_lib.c:1050/1062).  These
    // aren't inside any baked DL — they live in the file-scope sEyeTextures /
    // sMouthTextures arrays and are swapped per-DummyPlayer in DummyPlayer_Draw.
    // Indexed [age][slot]: age 0=adult, 1=child; 8 eye slots, 4 mouth slots.
    // Empty string = pack doesn't ship this variant; the swap falls through to the
    // saved original value (render-time ArchiveManager lookup — may bleed on a
    // multi-pack setup, same class as the non-face miss case).
    std::string                   eyeTexKeys[2][8];
    std::string                   mouthTexKeys[2][4];
    bool isValid = false;
};

// KB-19 (#176) — replaces the single-slot retire pattern.
//
// The original retire design held one std::unique_ptr<BakedPlayerModel> as
// the graveyard slot. When two re-bakes landed within kRetireFrames frames,
// the second retire's std::move overwrote and destroyed the first retiree —
// while in-flight Gfx commands still referenced its bakedDLs.back().data() /
// pathStrings.back().c_str(). Symptom: vertex distortion (renderer reads
// freed memory as wrong-but-valid GBI commands) or KB-15-style "Unhandled
// OP code" crash (torn opcode boundary).
//
// Triggered by P2 age switch — 4 BuildVanillaDummyPlayerModel calls landed
// in 41 ms (~10 ms each), much faster than kRetireFrames = 30 frames at
// 60 fps. The single-slot pattern's "≥400 ms per bake" assumption was
// only valid for pack-archive bakes; vanilla revert is much lighter.
//
// Fix: vector. Each retire APPENDS. OnGameFrameUpdate ticks every entry's
// counter; entries reaching zero are erased (and their unique_ptr destroys
// the model). Multiple concurrent retirees coexist safely.
struct RetiredBake {
    std::unique_ptr<BakedPlayerModel> model;
    int                               framesRemaining;
};

// TODO: CLEAN THIS UP LATER
struct SkeletonPatchInfo {
    SkelAnime* skelAnime;
    std::string vanillaSkeletonPath;
    bool isLocalPlayer;
    std::shared_ptr<Skeleton> overrideSkeleton; // keeps folder-loaded skeleton alive for local player override

    // Local-player bake (issue #82). When the local player switches coop packs (or
    // reverts to Default Link), we bake the new skeleton's DLs under pack-unique
    // "coopchar/<folder>/..." / "vanilla-dummy/..." cache keys and point skelAnime
    // at bakedModel->segmentPtrs. Without this, the renderer's DL cache key
    // collides across packs (identical alt-path strings) and the visual lags a
    // full scene transition behind the skelAnime pointer swap.
    //
    // retiredBakedModels mirrors the KB-15 / #110 pattern on AnchorClient: the
    // replaced bakedModel cannot be destroyed synchronously because the last
    // submitted Gfx frame still holds raw pointers into its pathStrings /
    // bakedDLs. OnGameFrameUpdate ticks each entry's counter down; entries
    // reaching zero are erased and the unique_ptr destroys the model.
    //
    // KB-19 (#176): vector replaces the single-slot pattern. Single-slot's
    // std::move overwrite destroyed prior retirees during rapid re-bakes
    // (4 BuildVanillaDummyPlayerModel calls in 41 ms during P2 age switch),
    // while their Gfx commands were still in-flight. Vector lets multiple
    // concurrent retirees coexist safely.
    std::unique_ptr<BakedPlayerModel> bakedModel;
    std::vector<RetiredBake>          retiredBakedModels;

    // KB-19 follow-up #3 — bake cache key. UpdateTunicSkeletons short-circuits
    // when (lastBakedFolder, lastBakedAge, lastBakedTunic, lastBakedTunicPath)
    // matches the current resolution — the bake output would be byte-identical.
    // Default sentinel values guarantee the first bake always runs.
    std::string lastBakedFolder = "<uninit>";
    std::string lastBakedTunicPath;       // tunic-variant skeleton path baked into
    int lastBakedAge = -1;                // gSaveContext.linkAge at bake time
    int lastBakedTunic = -1;              // CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) at bake time
};

class SkeletonPatcher {
  public:
    static void RegisterSkeleton(std::string& path, SkelAnime* skelAnime);
    static void UnregisterSkeleton(SkelAnime* skelAnime);
    static void ClearSkeletons();
    static void UpdateSkeletons();
    static void UpdateCustomSkeletons();
    // KB-19 follow-up #2 — targeted bake. Bakes only the SkeletonPatchInfo
    // matching `triggerSkelAnime` instead of every local entry. Falls back to
    // the broadcast-bake form when the trigger is null or not registered.
    static void UpdateCustomSkeletons(SkelAnime* triggerSkelAnime);
    static void ApplyCustomSkeletonToDummyPlayer(SkelAnime* skelAnime, bool isAdult, uint8_t tunic,
                                                 const std::string& characterFolder,
                                                 std::shared_ptr<Skeleton>& outSkeleton,
                                                 BakedPlayerModel& outBakedModel);

    // Promoted to public for KB-19 fix b: CustomSkeletons.cpp's
    // OnLinkSkeletonInit consumer needs to early-return when the firing
    // SkelAnime is not the local player's, to avoid re-baking our own Link
    // during the gSaveContext.linkAge swap window in DummyPlayer_Init.
    static bool IsLocalPlayerSkelAnime(SkelAnime* skelAnime);

    static std::vector<SkeletonPatchInfo> skeletons;

  private:
    inline static const std::string sOtr = "__OTR__";
    static bool IsLinkSkeletonPath(const std::string& path);
    static void UpdateTunicSkeletons(SkeletonPatchInfo& skel);
    static void UpdateCustomSkeletonFromPath(const std::string& skeletonPath, SkeletonPatchInfo& skel);
    static void UpdateCustomSkeletonFromFolder(const std::string& skeletonPath, const std::string& folder, SkeletonPatchInfo& skel);
};

} // namespace SOH
