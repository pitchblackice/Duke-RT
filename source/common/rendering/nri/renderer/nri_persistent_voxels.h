#pragma once

#include "nri_resources.h"
#include "nri_scene_lights.h"

#include "../scene/nri_geometry_bridge.h"
#include "../scene/nri_material_bridge.h"
#include "../scene/nri_scene_bridge.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct PersistentVoxelBatch
{
	struct ActorEntry
	{
		uint64_t identityKey = 0;
		uint64_t signature = 0;
		uint64_t geometrySignature = 0;
		uint64_t surfaceSignature = 0;
		uint64_t bakedSurfaceSignature = 0;
		uint64_t materialSignature = 0;
		uint64_t meshResourceKey = 0;
		uint64_t meshKeyHash = 0;
		uint64_t materialKeyHash = 0;
		uint64_t lastSeenFrame = 0;
		uint64_t retainedFrameAge = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		uint32_t visibilityChunkIndex = UINT32_MAX;
		bool capturedThisFrame = false;
		bool inWorldTlasThisFrame = false;
		bool active = true;
		uint32_t primitiveOffset = 0;
		uint32_t primitiveCount = 0;
		uint32_t indexOffset = 0;
		uint32_t indexCount = 0;
		uint32_t materialOffset = 0;
		uint32_t materialCount = 0;
		std::array<float, 12> instanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		std::array<float, 12> previousInstanceTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		nri_scene::MaterialBridgeData materialBridge;
		std::vector<SceneLightSystem::SurfaceRecord> lightRecords;
	};

	bool valid = false;
	uint64_t sourceSerial = 0;
	uint32_t surfaceCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t activeActorCount = 0;
	uint32_t rebuildCount = 0;
	nri_scene::SceneDebugStats stats;
	nri_scene::MaterialBridgeData materialBridge;
	std::vector<ActorEntry> actors;
};

struct PersistentVoxelMeshVariantResource
{
	uint64_t resourceKey = 0;
	uint64_t meshKeyHash = 0;
	uint64_t transformBasisSignature = 0;
	nri_scene::VoxelMeshBakeSpace meshBakeSpace = nri_scene::VoxelMeshBakeSpace::Unknown;
	uint32_t primitiveCount = 0;
	uint32_t indexCount = 0;
	uint32_t vertexCount = 0;
	uint32_t vertexOffset = 0;
	uint32_t vertexCapacity = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCapacity = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCapacity = 0;
	uint32_t tlasReadyFrame = 0;
	uint32_t lastDesiredMapGeneration = 0;
	uint32_t lastUsedMapGeneration = 0;
	uint32_t lastUsedFrame = 0;
	uint32_t sourceBits = 0;
	uint32_t activeActorReferences = 0;
	int32_t priority = 0;
	uint64_t residentBytes = 0;
	bool tlasPublished = false;
	bool cold = false;
	bool gpuForce = false;
	bool gpuPrefer = false;
	bool lightTemplateValid = false;
	float lightTemplateCenter[3] = {};
	float lightTemplateBoundsRadius = 0.0f;
	float lightTemplateSurfaceArea = 0.0f;
	float bakedTranslation[3] = {};
	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIAccelerationStructureResource accelerationStructure;
};

struct PersistentVoxelMaterialVariantResource
{
	uint64_t materialKeyHash = 0;
	uint64_t materialSignature = 0;
	uint64_t materialPayloadHash = 0;
	uint32_t materialOffset = 0;
	uint32_t materialCount = 0;
	uint32_t materialCapacity = 0;
	uint32_t lastDesiredMapGeneration = 0;
	uint32_t lastUsedMapGeneration = 0;
	uint32_t lastUsedFrame = 0;
	uint32_t sourceBits = 0;
	uint32_t activeActorReferences = 0;
	int32_t priority = 0;
	uint64_t residentBytes = 0;
	uint64_t materialUploadHash = 0;
	bool cold = false;
	bool gpuForce = false;
	bool gpuPrefer = false;
	nri_scene::MaterialBridgeData materialBridge;
};

enum class PersistentVoxelAdmissionState : uint8_t
{
	Pending,
	UploadingVertices,
	UploadingIndices,
	UploadingPrimitives,
	BuildingBlas,
	Ready,
	Deferred,
	Failed,
};

struct PersistentVoxelAdmissionEntry
{
	uint64_t pairKey = 0;
	nri_scene::PrecachedVoxelVariantView variant;
	PersistentVoxelAdmissionState state = PersistentVoxelAdmissionState::Pending;
	uint32_t sourceBits = 0;
	int32_t priority = 0;
	int32_t admissionRank = 0;
	bool gpuForce = false;
	bool gpuPrefer = false;
	bool runtimeRequested = false;
	uint32_t retryCount = 0;
	uint32_t mapGeneration = 0;
	uint64_t estimatedBytes = 0;
	uint64_t bytesUploaded = 0;
	bool uploadPrepared = false;
	uint32_t shaderVertexOffset = 0;
	uint32_t shaderIndexOffset = 0;
	uint32_t shaderPrimitiveOffset = 0;
	uint32_t savedVertexCursor = 0;
	uint32_t savedIndexCursor = 0;
	uint32_t savedPrimitiveCursor = 0;
	uint32_t savedMaterialCursor = 0;
	uint64_t vertexBytesUploaded = 0;
	uint64_t vertexArenaBytesUploaded = 0;
	uint64_t indexBytesUploaded = 0;
	uint64_t indexArenaBytesUploaded = 0;
	uint64_t primitiveBytesUploaded = 0;
	bool uploadSubmittedBeforeBlas = false;
	nri_scene::GeometryData uploadGeometry;
	std::vector<uint32_t> uploadGpuIndices;
	std::vector<nri_scene::PrimitiveData> uploadGpuPrimitives;
	PersistentVoxelMeshVariantResource uploadMeshResource;
	PersistentVoxelMaterialVariantResource uploadMaterialResource;
	const char* lastReason = "none";
};

struct PersistentVoxelReadinessStatus
{
	const char* reason = "ready";
	bool ready = false;
	bool meshPresent = false;
	bool meshPublished = false;
	bool meshKeyMatches = false;
	bool meshCountsValid = false;
	bool meshPrivateBuffersReady = false;
	bool meshArenaBuffersReady = false;
	bool blasReady = false;
	bool materialPresent = false;
	bool materialPublished = false;
	bool materialKeyMatches = false;
	bool materialCountValid = false;
	bool materialBridgeReady = false;
	uint64_t meshResourceKey = 0;
	uint32_t meshVertexCount = 0;
	uint32_t meshIndexCount = 0;
	uint32_t meshPrimitiveCount = 0;
	uint32_t materialCount = 0;
	uint32_t materialBridgeCount = 0;
};

struct PersistentVoxelAdmissionStats
{
	uint32_t queued = 0;
	uint32_t ready = 0;
	uint32_t deferred = 0;
	uint32_t failed = 0;
	uint32_t enqueued = 0;
	uint32_t deduped = 0;
	uint32_t promoted = 0;
	uint32_t uploaded = 0;
	uint32_t force = 0;
	uint32_t prefer = 0;
	uint32_t runtime = 0;
	uint32_t skippedBudget = 0;
	uint32_t failedThisPump = 0;
	uint64_t bytesPending = 0;
	uint64_t bytesUploaded = 0;
};

struct PersistentVoxelInstanceRecord
{
	uint64_t identityKey = 0;
	uint64_t signature = 0;
	uint64_t geometrySignature = 0;
	uint64_t surfaceSignature = 0;
	uint64_t bakedSurfaceSignature = 0;
	uint64_t materialSignature = 0;
	uint64_t meshKeyHash = 0;
	uint64_t materialKeyHash = 0;
	uint64_t meshVariantHash = 0;
	uint64_t materialVariantHash = 0;
	uint64_t meshResourceKey = 0;
	uint32_t primitiveCount = 0;
	uint32_t lastSeenFrame = 0;
	bool active = false;
	bool pending = false;
	std::array<float, 12> currentTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	std::array<float, 12> previousTransform = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
};

struct NRIPersistentVoxelResetServices
{
	using RetireBufferFn = void (*)(void* user, NRIBufferResource& resource);
	using RetireAccelerationStructureFn = void (*)(void* user, NRIAccelerationStructureResource& resource);
	using ClearBoundCountsFn = void (*)(void* user);
	using InvalidateSceneDataDescriptorsFn = void (*)(void* user);

	void* user = nullptr;
	RetireBufferFn retireBuffer = nullptr;
	RetireAccelerationStructureFn retireAccelerationStructure = nullptr;
	ClearBoundCountsFn clearBoundCounts = nullptr;
	InvalidateSceneDataDescriptorsFn invalidateSceneDataDescriptors = nullptr;

	void RetireBuffer(NRIBufferResource& resource) const;
	void RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const;
	void ClearBoundCounts() const;
	void InvalidateSceneDataDescriptors() const;
};

class NRIPersistentVoxelResidency
{
public:
	void Reset(const char* reason, bool clearSharedResources, bool traceReset, const NRIPersistentVoxelResetServices& services);

	NRIBufferResource vertexBuffer;
	NRIBufferResource indexBuffer;
	NRIBufferResource primitiveBuffer;
	NRIBufferResource materialBuffer;
	PersistentVoxelBatch batch = {};
	std::unordered_map<uint64_t, PersistentVoxelMeshVariantResource> meshVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelMaterialVariantResource> materialVariantResources;
	std::unordered_map<uint64_t, PersistentVoxelInstanceRecord> instances;
	std::unordered_map<uint64_t, uint64_t> actorRejectedSignatures;
	std::unordered_map<uint64_t, PersistentVoxelAdmissionEntry> admissionQueue;
	std::unordered_set<uint64_t> publishedMeshKeys;
	std::unordered_set<uint64_t> publishedMaterialKeys;
	uint32_t arenaVertexCursor = 0;
	uint32_t arenaIndexCursor = 0;
	uint32_t arenaPrimitiveCursor = 0;
	uint32_t arenaMaterialCursor = 0;
	uint32_t residencyMapGeneration = 0;
	uint64_t residencyLastBuildSerial = 0;
	bool loadingWarmupActive = false;
	bool preloadPending = false;
};
