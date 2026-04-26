#pragma once

#include "flatvertices.h"
#include "hw_drawinfo.h"
#include "hw_drawstructs.h"

#include <cstdint>
#include <vector>

class FGameTexture;

namespace nri_scene
{
bool TryGetAverageTextureColor(FGameTexture* texture, float* outColor);
void ResetAverageTextureColorCache();
void Copy3(const float* source, float* destination);

enum class SurfaceSourceType : uint32_t
{
	Unknown = 0,
	DrawListWall,
	MirrorWall,
	FloorFlat,
	CeilingFlat,
	FacingSprite,
	VoxelProxySprite,
	MapWallBand,
	MapFloorSection,
	MapCeilingSection,
	MapPortalSurface,
	DebugSphere,
};

enum MaterialFlags : uint32_t
{
	MaterialFlag_None = 0,
	MaterialFlag_Indexed = 1u << 0,
	MaterialFlag_Fullbright = 1u << 1,
	MaterialFlag_Flat = 1u << 2,
	MaterialFlag_Sprite = 1u << 3,
	MaterialFlag_Mirror = 1u << 4,
	MaterialFlag_Sky = 1u << 5,
	MaterialFlag_Portal = 1u << 6,
	MaterialFlag_OneWay = 1u << 7,
	MaterialFlag_AlphaClip = 1u << 8,
	MaterialFlag_FacingBillboard = 1u << 9,
	MaterialFlag_PointSampled = 1u << 10,
};

enum PrimitiveFlags : uint32_t
{
	PrimitiveFlag_None = 0,
	PrimitiveFlag_ReflectionOnly = 1u << 16,
};

enum class PTSkyMode : uint32_t
{
	None = 0,
	SolidColor,
	Cubemap,
};

enum class PTSkySourceType : uint32_t
{
	None = 0,
	Wall,
	Flat,
	Portal,
};

struct PTSkyDescriptor
{
	PTSkyMode mode = PTSkyMode::None;
	PTSkySourceType sourceType = PTSkySourceType::None;
	FGameTexture* texture = nullptr;
	uint32_t faceMask = 0;
	uint32_t priority = 0;
	bool flipTop = false;
	bool isThreeFace = false;
};

struct SurfaceProvenance
{
	SurfaceSourceType sourceType = SurfaceSourceType::Unknown;
	int32_t sectorIndex = -1;
	int32_t wallIndex = -1;
	int32_t sectionIndex = -1;
	int32_t mapChunkIndex = -1;
	int32_t nextSectorIndex = -1;
	int32_t actorIndex = -1;
	uint32_t drawListType = UINT32_MAX;
	uint32_t cstat = 0;
	uint32_t materialFlags = 0;
};

struct SceneDebugStats
{
	unsigned int totalDrawItems = 0;
	unsigned int wallDrawItems = 0;
	unsigned int flatDrawItems = 0;
	unsigned int spriteDrawItems = 0;
	unsigned int translucentDrawItems = 0;
	unsigned int triangleEstimate = 0;
	unsigned int materialRefs = 0;
	unsigned int mirrorSurfaces = 0;
	unsigned int skySurfaces = 0;
	unsigned int portalViews = 0;
	unsigned int portalCapturesSkipped = 0;
	unsigned int modelDrawItems = 0;
	unsigned int voxelProxyDrawItems = 0;
	unsigned int unsupportedModelDrawItems = 0;
	unsigned int voxelStableCandidates = 0;
	unsigned int voxelStableUncacheable = 0;
	unsigned int voxelStableSignatureHits = 0;
	unsigned int voxelStableSignatureMisses = 0;
	unsigned int voxelStableSignatureChanges = 0;
	unsigned int voxelStableSplitStable = 0;
	unsigned int voxelStableSplitLive = 0;
	unsigned int voxelCacheEntries = 0;
	unsigned int voxelCacheSurfaceHits = 0;
	unsigned int voxelCacheSurfaceStores = 0;
	unsigned int voxelCacheSurfaceRebuilds = 0;
	unsigned int voxelCacheSurfaceRemoves = 0;
	unsigned int voxelCacheNotCaptured = 0;
	unsigned int voxelCachePrimitives = 0;
};

struct SkyPerfStats
{
	uint32_t updateCalls = 0;
	uint32_t wallUpdateCalls = 0;
	uint32_t flatUpdateCalls = 0;
	uint32_t portalUpdateCalls = 0;
	uint32_t inspectCalls = 0;
	uint32_t inspectCubemapCandidates = 0;
	uint32_t inspectSolidCandidates = 0;
	uint32_t inspectFaceWalks = 0;
	uint32_t averageColorBaseCalls = 0;
	uint32_t averageColorRecursiveCalls = 0;
	uint32_t recursiveSkyboxFaceSamples = 0;
	uint64_t averageColorPixels = 0;
	uint64_t updateTimeUs = 0;
	uint64_t inspectTimeUs = 0;
	uint64_t averageColorTimeUs = 0;
};

struct MaterialRef
{
	FGameTexture* texture = nullptr;
	int palette = 0;
	int shade = 0;
	float alpha = 1.0f;
	uint32_t flags = MaterialFlag_None;
};

struct CapturedVertex
{
	float position[3] = {};
	float prevPosition[3] = {};
	float uv[2] = {};
};

struct SurfaceRef
{
	std::vector<CapturedVertex> vertices;
	std::vector<uint32_t> indices;
	MaterialRef material;
	SurfaceProvenance provenance;
};

struct SceneView
{
	HWDrawInfo* drawInfo = nullptr;
	std::vector<SurfaceRef> opaqueWalls;
	std::vector<SurfaceRef> opaqueFlats;
	std::vector<SurfaceRef> opaqueSprites;
	uint32_t primitiveFlags = PrimitiveFlag_None;
	SceneDebugStats stats;
	PTSkyDescriptor sky;
	float skyColor[3] = { 0.38f, 0.48f, 0.65f };
	float groundColor[3] = { 0.08f, 0.08f, 0.08f };
};

struct PersistentVoxelCacheEntryView
{
	uint64_t identityKey = 0;
	uint64_t signature = 0;
	uint64_t geometrySignature = 0;
	uint64_t materialSignature = 0;
	uint32_t primitiveCount = 0;
	SurfaceRef surface;
};

SceneDebugStats CollectDebugStats(HWDrawInfo& di);
MaterialRef MakeMaterialRef(FGameTexture* texture, int palette, int shade, float alpha, uint32_t extraFlags);
void UpdateSceneSky(SceneView& outView, FGameTexture* texture, uint32_t fallbackColor, PTSkySourceType sourceType);
void ResetSkyPerfStats();
SkyPerfStats ConsumeSkyPerfStats();
bool CaptureDynamicScene(HWDrawInfo& di, SceneView& outView);
bool CaptureActorSpriteScene(HWDrawInfo& di, int32_t actorIndex, SceneView& outView);
bool CaptureScene(HWDrawInfo& di, SceneView& outView);
bool BuildPersistentVoxelCacheSceneView(SceneView& outView);
bool BuildPersistentVoxelCacheEntries(std::vector<PersistentVoxelCacheEntryView>& outEntries);
uint64_t GetPersistentVoxelCacheSerial();
}
