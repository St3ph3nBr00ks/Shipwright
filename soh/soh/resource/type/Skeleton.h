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
    bool isValid = false;
};

// TODO: CLEAN THIS UP LATER
struct SkeletonPatchInfo {
    SkelAnime* skelAnime;
    std::string vanillaSkeletonPath;
    bool isLocalPlayer;
    std::shared_ptr<Skeleton> overrideSkeleton; // keeps folder-loaded skeleton alive for local player override
};

class SkeletonPatcher {
  public:
    static void RegisterSkeleton(std::string& path, SkelAnime* skelAnime);
    static void UnregisterSkeleton(SkelAnime* skelAnime);
    static void ClearSkeletons();
    static void UpdateSkeletons();
    static void UpdateCustomSkeletons();
    static void ApplyCustomSkeletonToDummyPlayer(SkelAnime* skelAnime, bool isAdult, uint8_t tunic,
                                                 const std::string& characterFolder,
                                                 std::shared_ptr<Skeleton>& outSkeleton,
                                                 BakedPlayerModel& outBakedModel);

    static std::vector<SkeletonPatchInfo> skeletons;

  private:
    inline static const std::string sOtr = "__OTR__";
    static bool IsLinkSkeletonPath(const std::string& path);
    static bool IsLocalPlayerSkelAnime(SkelAnime* skelAnime);
    static void UpdateTunicSkeletons(SkeletonPatchInfo& skel);
    static void UpdateCustomSkeletonFromPath(const std::string& skeletonPath, SkeletonPatchInfo& skel);
    static void UpdateCustomSkeletonFromFolder(const std::string& skeletonPath, const std::string& folder, SkeletonPatchInfo& skel);
};

} // namespace SOH
