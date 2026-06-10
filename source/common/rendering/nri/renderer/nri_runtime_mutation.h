#pragma once

#include "nri_resources.h"
#include "nri_scene_lights.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct RuntimeMutationCacheStats
{
	uint32_t activeChunkCount = 0;
	uint32_t validChunkCount = 0;
	uint32_t excludedStaticChunkCount = 0;
	uint32_t cachedSurfaceCount = 0;
	uint32_t cachedTriangleCount = 0;
	uint32_t cachedMaterialCount = 0;
	uint32_t cachedMaterialStateCount = 0;
};

struct RuntimeMapMutationCache
{
	struct ChunkReplacement
	{
		struct MaterialStateCacheEntry
		{
			uint64_t animatedGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint32_t surfaceCount = 0;
			nri_scene::MaterialBridgeData materialBridge;
		};

		nri_scene::PTMapChunkMutationBaseline baseline;
		nri_scene::PTMapChunkMutationBaseline replacementBaseline;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t lastTraceSignature = UINT64_MAX;
		uint64_t lastTraceAnimatedMaterialSignature = UINT64_MAX;
		uint32_t reasonMask = 0;
		uint32_t sectionDirtyCount = 0;
		uint32_t stableMutationFrameCount = 0;
		uint32_t lastTraceReasonMask = UINT32_MAX;
		uint32_t traceCount = 0;
		bool active = false;
		bool valid = false;
		bool residentAuthoritative = false;
		bool sectorDirty = false;
		bool dragged = false;
		bool blindSpot = false;
		bool excludeStaticChunk = false;
		bool staticAnimatedReplacement = false;
		bool lastTraceActive = false;
		bool lastTraceBlindSpot = false;
		bool animationOnlyRefreshed = false;
		bool lastTraceAnimationOnlyRefreshed = false;
		bool lastTraceStaticAnimatedReplacement = false;
		bool deferredMaterialRefresh = false;
		uint64_t deferredMaterialFrame = 0;
		bool deferredStructuralRebuild = false;
		uint64_t deferredStructuralFrame = 0;
		uint32_t surfaceCount = 0;
		uint32_t triangleCount = 0;
		SceneLightSystem::SurfaceIdentityOverrides lightIdentityOverrides;
		nri_scene::SceneView sceneView;
		nri_scene::GeometryData geometry;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<MaterialStateCacheEntry> materialStateCache;
	};

	std::vector<ChunkReplacement> chunks;
};

struct RuntimeMutationResidentApplyMode
{
	bool materialOnlyReplacement = false;
	bool exclusiveMaterialOnlyReplacement = false;
	bool fastResidentMaterialOnlyUpdate = false;
};

struct RuntimeMutationResidentUploadRange
{
	int uploadKind = 0;
	uint64_t byteOffset = 0;
	uint64_t size = 0;
	uint64_t dirtySize = 0;
};

struct RuntimeMapMutationFrameState
{
	bool active = false;
	uint32_t dirtyChunkCount = 0;
	uint32_t residentAppliedChunkCount = 0;
	uint32_t residentGeometryChunkCount = 0;
	uint32_t residentMaterialChunkCount = 0;
	uint32_t residentAtlasGrowCount = 0;
	uint32_t residentFallbackChunkCount = 0;
	uint32_t rebuiltChunkCount = 0;
	uint32_t heldChunkCount = 0;
	uint32_t blindSpotChunkCount = 0;
	uint32_t sectorGeometryChunkCount = 0;
	uint32_t sectorMaterialChunkCount = 0;
	uint32_t wallGeometryChunkCount = 0;
	uint32_t wallMaterialChunkCount = 0;
	uint32_t sectorDirtyChunkCount = 0;
	uint32_t sectionDirtyChunkCount = 0;
	uint32_t draggedChunkCount = 0;
	uint32_t animatedRefreshChunkCount = 0;
	uint32_t replacementSurfaceCount = 0;
	uint32_t replacementTriangleCount = 0;
	uint32_t materialCount = 0;
};

class NRIRuntimeMutationSystem
{
public:
	RuntimeMapMutationCache cache;
	RuntimeMapMutationFrameState lastFrame = {};
	RuntimeMutationCacheStats cacheHighWaterStats = {};
	std::vector<RuntimeMutationResidentUploadRange> residentGeometryUploadRanges;
	std::vector<uint8_t> signatureWatchlist;
	uint64_t signatureWatchlistBuildSerial = 0;
	uint32_t worklistSweepCursor = 0;
};
