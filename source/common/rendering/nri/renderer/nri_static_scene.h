#pragma once

#include "nri_frame_resources.h"
#include "nri_resources.h"
#include "nri_runtime_mutation.h"
#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_map_builder.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <cstdint>
#include <vector>

struct StaticMapChunkAtlas
{
	struct FreeRange
	{
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	struct ChunkEntry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		bool valid = false;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t vertexCapacity = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t materialCapacity = 0;
	std::vector<ChunkEntry> chunks;
	std::vector<FreeRange> freeVertexRanges;
	std::vector<FreeRange> freeIndexRanges;
	std::vector<FreeRange> freePrimitiveRanges;
	std::vector<FreeRange> freeMaterialRanges;
};

struct StaticMapSceneCache
{
	struct ChunkCache
	{
		struct ResidentMaterialSliceCacheEntry
		{
			uint64_t animatedGeometrySignature = 0;
			uint64_t animatedMaterialSignature = 0;
			uint64_t materialBridgeHash = 0;
			uint64_t actorOverrideHash = 0;
			uint64_t emissiveOverrideHash = 0;
			uint32_t materialCount = 0;
			nri_scene::MaterialBridgeData remappedMaterialBridge;
			std::vector<nri_scene::MaterialData> gpuMaterials;
		};

		uint32_t chunkIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t geometryTopologySignature = 0;
		uint64_t primitiveLayoutSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t animatedGeometrySignature = 0;
		bool active = true;
		bool blasUpdateEligible = false;
		uint32_t lastResidentBlasReasonMask = 0;
		uint32_t lastResidentBlasSurfaceCount = 0;
		uint32_t lastResidentBlasTriangleCount = 0;
		uint32_t lastResidentBlasMaterialCount = 0;
		bool lastResidentBlasForceTopology = false;
		bool lastResidentBlasRecoveredEmpty = false;
		bool lastResidentBlasKeptGeometrySlice = false;
		bool lastResidentBlasTopologyChanged = false;
		nri::AccelerationStructure* residentBlasScratchSizeCacheKey = nullptr;
		uint64_t residentBlasBuildScratchSize = 0;
		uint64_t residentBlasUpdateScratchSize = 0;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<ResidentMaterialSliceCacheEntry> residentMaterialSliceCache;
		NRIAccelerationStructureResource accelerationStructure;
	};

	bool valid = false;
	bool texturesResident = false;
	bool buffersResident = false;
	bool accelerationResident = false;
	uint64_t buildSerial = 0;
	uint32_t sceneBuildCount = 0;
	uint32_t gpuUploadCount = 0;
	uint32_t accelerationBuildCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshCount = 0;
	uint32_t animatedRefreshUploadCount = 0;
	uint32_t animatedGeometryFallbackCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	uint32_t reuseCount = 0;
	nri_scene::SceneView sceneView;
	std::vector<nri_scene::SceneView> lightChunkViews;
	nri_scene::GeometryData geometry;
	nri_scene::MaterialBridgeData materialBridge;
	std::vector<nri_scene::MaterialData> gpuMaterials;
	std::vector<ChunkCache> chunks;
	uint32_t tlasInstanceCount = 0;
};

struct ResidentMapChunkRegistry
{
	struct Entry
	{
		uint32_t chunkIndex = UINT32_MAX;
		uint32_t staticSceneChunkListIndex = UINT32_MAX;
		uint32_t vertexOffset = 0;
		uint32_t vertexCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		uint64_t geometryTopologySignature = 0;
		uint64_t baselineSignature = 0;
		uint64_t liveSignature = 0;
		uint64_t exactGeometrySignature = 0;
		uint64_t animatedMaterialSignature = 0;
		uint64_t materialPayloadHash = 0;
		uint64_t geometryPayloadHash = 0;
		uint64_t animatedGeometrySignature = 0;
		bool valid = false;
		bool active = false;
		bool mappedInStaticScene = false;
		bool accelerationResident = false;
		bool hasAnimatedTextureCandidates = false;
		bool animatedRefreshSuppressed = false;
		bool wasVisibleLastFrame = false;
		bool visibleValidationTraceEmitted = false;
		uint8_t visibleValidationFramesRemaining = 0;
		uint32_t animatedSuppressionEmitCount = 0;
		uint32_t runtimeAnimatedAttemptCount = 0;
		uint32_t runtimeAnimatedResidentApplyCount = 0;
		uint32_t runtimeAnimatedSyncSkipCount = 0;
		nri_scene::PTMapChunkMutationBaseline appliedBaseline;
	};

	bool valid = false;
	uint64_t buildSerial = 0;
	uint32_t chunkCount = 0;
	uint32_t activeChunkCount = 0;
	uint32_t mappedChunkCount = 0;
	uint32_t accelerationResidentChunkCount = 0;
	uint32_t animatedCandidateChunkCount = 0;
	uint32_t animatedRefreshSuppressedChunkCount = 0;
	std::vector<Entry> entries;
};

struct NRIStaticSceneRegistrySyncInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const std::vector<RuntimeMutationResidentReplacementInfo>* replacements = nullptr;
	uint64_t (*hashResidentMaterialPayload)(const nri_scene::MaterialBridgeData& materials) = nullptr;
};

struct NRIPreservedStaticMapSkyState;

struct NRIStaticSceneCacheBuildServices
{
	void* user = nullptr;
	void (*resetMutationCacheForStaticSceneBuild)(void* user, uint32_t chunkCount) = nullptr;
	void (*initializeStaticChunkReplacement)(void* user, const nri_scene::PTMapChunk& chunk) = nullptr;
	void (*buildMaterialsWithActorOverrides)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) = nullptr;
	bool (*chunkHasAnimatedStaticMapSurfaceCandidates)(void* user, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk) = nullptr;
	double* geometryBuildStaticChunkMs = nullptr;
	uint32_t* geometryBuildStaticChunkCalls = nullptr;
	uint32_t* geometryBuildStaticChunkPrimitives = nullptr;
	bool ceilingNudge = false;
	float ceilingNudgeDistance = 0.0f;
};

struct NRIStaticSceneAnimatedMaterialRefreshInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	StaticMapSceneCache* staticScene = nullptr;
	StaticMapChunkAtlas* atlas = nullptr;
	ResidentMapChunkRegistry* registry = nullptr;
	const nri_scene::SceneView* preservedSkyView = nullptr;
	const std::vector<uint32_t>* visibleChunkWords = nullptr;
	uint32_t* runtimeAnimatedSuppressionEmitCount = nullptr;
	bool traceStats = false;
	bool traceMaterialBridgeFailures = false;
};

struct NRIStaticSceneAnimatedMaterialRefreshServices
{
	void* user = nullptr;
	bool (*refreshAnimatedBindingsForStaticMapChunk)(void* user, const nri_scene::PTMapWorld& mapWorld, const nri_scene::PTMapChunk& chunk, nri_scene::SceneView& ioChunkView) = nullptr;
	void (*buildMaterialsWithActorOverrides)(void* user, nri_scene::SceneView& sceneView, nri_scene::MaterialBridgeData& materials, const char* label) = nullptr;
	bool (*ensurePaletteTexture)(void* user, const nri_scene::MaterialBridgeData& materials) = nullptr;
	bool (*ensureSceneTextures)(void* user, const nri_scene::SceneView& sceneView, const nri_scene::MaterialBridgeData& materials, std::vector<nri_scene::MaterialData>& gpuMaterials, bool preserveExistingSky, const char* reason) = nullptr;
	bool (*uploadStaticMaterialAtlas)(void* user) = nullptr;
	bool (*recoverStaticScene)(void* user, const char* reason) = nullptr;
	void (*syncResidentRegistry)(void* user) = nullptr;
	void (*markUploadedStaticMapSceneLastFrame)(void* user) = nullptr;
};

struct NRIStaticMapInstanceBuildInput
{
	const nri_scene::PTMapWorld* mapWorld = nullptr;
	const StaticMapSceneCache* staticScene = nullptr;
	const StaticMapChunkAtlas* atlas = nullptr;
	const ResidentMapChunkRegistry* registry = nullptr;
};

struct NRIStaticMapInstanceBuildServices
{
	void* user = nullptr;
	uint64_t (*getAccelerationStructureHandle)(void* user, const NRIAccelerationStructureResource& accelerationStructure) = nullptr;
};

namespace nri_static_scene
{
	void InitializeStaticMapSceneCacheBuild(
		const nri_scene::PTMapWorld& mapWorld,
		const NRIPreservedStaticMapSkyState* preservedSkyState,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	void AppendStaticMapSceneCacheChunk(
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::PTMapChunk& chunk,
		const nri_scene::SceneView* preservedSkyView,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	bool BuildStaticMapSceneCache(
		const nri_scene::PTMapWorld& mapWorld,
		const NRIPreservedStaticMapSkyState* preservedSkyState,
		const NRIStaticSceneCacheBuildServices& services,
		StaticMapSceneCache& outStaticScene);

	bool RebuildResidentStaticMaterialBridgeFromChunks(
		StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		bool traceFailures);

	bool RefreshStaticMapAnimatedMaterials(
		const NRIStaticSceneAnimatedMaterialRefreshInput& input,
		const NRIStaticSceneAnimatedMaterialRefreshServices& services);

	void BuildStaticMapInstances(
		const NRIStaticMapInstanceBuildInput& input,
		const NRIStaticMapInstanceBuildServices& services,
		std::vector<nri::TopLevelInstance>& outTlasInstances,
		std::vector<SceneInstanceData>& outSceneInstances);

	void PrintStaticMapSceneStatus(
		const StaticMapSceneCache& staticScene,
		bool usedStaticMapSceneLastFrame,
		bool uploadedStaticMapSceneLastFrame,
		bool builtStaticMapSceneASLastFrame);
}

class NRIStaticSceneResidency
{
public:
	ResidentMapChunkRegistry& Registry() { return mResidentMapChunkRegistry; }
	const ResidentMapChunkRegistry& Registry() const { return mResidentMapChunkRegistry; }
	void ResetResidentMapChunkRegistry() { mResidentMapChunkRegistry = {}; }
	void SyncResidentMapChunkRegistryFromStaticScene(const NRIStaticSceneRegistrySyncInput& input);

	static uint32_t GetStaticSceneChunkSlotPreference(
		const StaticMapSceneCache& staticScene,
		const StaticMapChunkAtlas& atlas,
		uint32_t chunkListIndex);

private:
	ResidentMapChunkRegistry mResidentMapChunkRegistry;
};

struct StaticMapSceneResources
{
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	StaticMapChunkAtlas chunkAtlas;
	NRIBufferResource tlasInstanceBuffer;
	NRIBufferResource scratchBuffer;
	NRIBufferResource topLevelScratchBuffer;
	NRIAccelerationStructureResource topLevelAS;
	uint64_t accelerationBuildSerial = 0;
	uint32_t tlasInstanceCount = 0;
	std::vector<SceneInstanceData> sceneInstances;
};
