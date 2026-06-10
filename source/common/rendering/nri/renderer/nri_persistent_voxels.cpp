#include "nri_persistent_voxels.h"

#include "../scene/nri_hash.h"
#include "nri_upload_hash.h"
#include "printf.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>

const char* GetPersistentVoxelBakeSpaceName(nri_scene::VoxelMeshBakeSpace bakeSpace)
{
	switch (bakeSpace)
	{
	case nri_scene::VoxelMeshBakeSpace::LocalSpace: return "local";
	case nri_scene::VoxelMeshBakeSpace::BakedTransform: return "baked";
	default: return "unknown";
	}
}

void CopyPersistentVoxelInstanceTransform(const float source[12], std::array<float, 12>& target)
{
	for (size_t i = 0; i < target.size(); ++i)
	{
		target[i] = source[i];
	}
}

bool SamePersistentVoxelInstanceTransform(const std::array<float, 12>& left, const float right[12])
{
	constexpr float Epsilon = 0.0001f;
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (std::abs(left[i] - right[i]) > Epsilon)
		{
			return false;
		}
	}
	return true;
}

void FillPersistentVoxelInstanceTransform(
	const float currentTranslation[3],
	const float bakedTranslation[3],
	std::array<float, 12>& target)
{
	target = { 1.0f, 0.0f, 0.0f, currentTranslation[0] - bakedTranslation[0],
		0.0f, 1.0f, 0.0f, currentTranslation[1] - bakedTranslation[1],
		0.0f, 0.0f, 1.0f, currentTranslation[2] - bakedTranslation[2] };
}

void FillPersistentVoxelActorInstanceTransform(
	const nri_scene::PersistentVoxelCacheEntryView& cacheEntry,
	const PersistentVoxelMeshVariantResource& meshResource,
	std::array<float, 12>& target)
{
	if (cacheEntry.meshBakeSpace == nri_scene::VoxelMeshBakeSpace::LocalSpace)
	{
		CopyPersistentVoxelInstanceTransform(cacheEntry.instanceTransform, target);
		return;
	}
	FillPersistentVoxelInstanceTransform(cacheEntry.currentTranslation, meshResource.bakedTranslation, target);
}

uint64_t EstimatePersistentVoxelActorUploadBytes(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry)
{
	if (cacheEntry.surface == nullptr)
	{
		return 0;
	}

	const uint64_t vertexBytes = (uint64_t)cacheEntry.surface->vertices.size() * sizeof(nri_scene::SceneVertex);
	const uint64_t indexBytes = (uint64_t)cacheEntry.surface->indices.size() * sizeof(uint32_t);
	const uint64_t primitiveBytes = (uint64_t)cacheEntry.primitiveCount * sizeof(nri_scene::PrimitiveData);
	const uint64_t materialBytes = sizeof(nri_scene::MaterialData);
	return vertexBytes + indexBytes + primitiveBytes + materialBytes;
}

bool IsPersistentVoxelMeshResourceTransformKeyed(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings)
{
	return settings.transformKeyed ||
		cacheEntry.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace;
}

uint64_t BuildPersistentVoxelMeshResourceKey(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry, const NRIPersistentVoxelSettings& settings)
{
	if (!IsPersistentVoxelMeshResourceTransformKeyed(cacheEntry, settings))
	{
		return cacheEntry.meshKeyHash;
	}
	uint64_t hash = cacheEntry.meshKeyHash;
	hash = nri_scene::HashCombine64(hash, cacheEntry.transformBasisSignature);
	return hash;
}

uint32_t ResolvePersistentVoxelActorVisibilityChunk(const nri_scene::PersistentVoxelCacheEntryView& cacheEntry)
{
	(void)cacheEntry;
	// Persistent voxel actors move independently from the cached mesh surface that sourced them.
	// Let static map geometry own chunk gating; dynamic actor instances stay ray-visible by position.
	return UINT32_MAX;
}

namespace
{
	double PersistentVoxelDurationMs(
		const std::chrono::steady_clock::time_point& start,
		const std::chrono::steady_clock::time_point& end)
	{
		return std::chrono::duration<double, std::milli>(end - start).count();
	}
}

void NRIPersistentVoxelResetServices::RetireBuffer(NRIBufferResource& resource) const
{
	if (retireBuffer != nullptr)
	{
		retireBuffer(user, resource);
	}
}

void NRIPersistentVoxelResetServices::RetireAccelerationStructure(NRIAccelerationStructureResource& resource) const
{
	if (retireAccelerationStructure != nullptr)
	{
		retireAccelerationStructure(user, resource);
	}
}

void NRIPersistentVoxelResetServices::InvalidateSceneDataDescriptors() const
{
	if (invalidateSceneDataDescriptors != nullptr)
	{
		invalidateSceneDataDescriptors(user);
	}
}

bool NRIPersistentVoxelPreloadServices::PumpAdmissionQueue(const char* phase) const
{
	if (pumpAdmissionQueue == nullptr)
	{
		return false;
	}
	return pumpAdmissionQueue(user, phase);
}

bool NRIPersistentVoxelPreloadServices::EnsureBatch() const
{
	return ensureBatch != nullptr && ensureBatch(user);
}

bool NRIPersistentVoxelAdmissionServices::AdmitVariantResource(
	PersistentVoxelAdmissionEntry& entry,
	uint64_t byteBudget,
	uint32_t& blasBudget,
	uint64_t& outUploadBytes,
	bool& outReusedMesh,
	bool& outReusedMaterial,
	bool& outInProgress,
	bool isolateBlasBuild,
	const char*& outFailureReason) const
{
	if (admitVariantResource == nullptr)
	{
		outFailureReason = "admission-service-missing";
		return false;
	}
	return admitVariantResource(
		user,
		entry,
		byteBudget,
		blasBudget,
		outUploadBytes,
		outReusedMesh,
		outReusedMaterial,
		outInProgress,
		isolateBlasBuild,
		outFailureReason);
}

bool NRIPersistentVoxelAdmissionServices::SubmitWaitAndRestart(const char* reason) const
{
	return submitWaitAndRestart != nullptr && submitWaitAndRestart(user, reason);
}

bool NRIPersistentVoxelAccelerationServices::BuildBottomLevel(
	const NRIBufferResource& vertexBuffer,
	const NRIBufferResource& indexBuffer,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveCount,
	NRIAccelerationStructureResource& outAccelerationStructure) const
{
	return buildBottomLevel != nullptr && buildBottomLevel(
		user,
		vertexBuffer,
		indexBuffer,
		vertexCount,
		indexOffset,
		indexCount,
		primitiveCount,
		outAccelerationStructure);
}

bool NRIPersistentVoxelAccelerationServices::BarrierBuildInputs(const NRIBufferResource& vertexBuffer, const NRIBufferResource& indexBuffer) const
{
	return barrierBuildInputs != nullptr && barrierBuildInputs(user, vertexBuffer, indexBuffer);
}

bool NRIPersistentVoxelMaterialWarmupServices::EnsurePalette(const nri_scene::MaterialBridgeData& materials) const
{
	return ensurePalette != nullptr && ensurePalette(user, materials);
}

bool NRIPersistentVoxelMaterialWarmupServices::WarmTextures(
	const nri_scene::MaterialBridgeData& materials,
	NRIPersistentVoxelMaterialWarmupStats& stats) const
{
	return warmTextures != nullptr && warmTextures(user, materials, stats);
}

bool NRIPersistentVoxelMaterialUploadServices::EnsureMaterialArenaBuffer(NRIBufferResource& resource, uint64_t sizeBytes) const
{
	return ensureMaterialArenaBuffer != nullptr && ensureMaterialArenaBuffer(user, resource, sizeBytes);
}

bool NRIPersistentVoxelMaterialUploadServices::StageMaterialRanges(
	const NRIBufferResource& targetBuffer,
	const std::vector<RuntimeMutationResidentUploadRange>& ranges,
	const uint8_t* data,
	uint64_t availableBytes) const
{
	return stageMaterialRanges != nullptr && stageMaterialRanges(user, targetBuffer, ranges, data, availableBytes);
}

void NRIPersistentVoxelMaterialUploadServices::NoteMaterialUpload(uint64_t sizeBytes) const
{
	if (noteMaterialUpload != nullptr)
	{
		noteMaterialUpload(user, sizeBytes);
	}
}

uint64_t NRIPersistentVoxelTlasServices::GetAccelerationStructureHandle(const NRIAccelerationStructureResource& resource) const
{
	return getAccelerationStructureHandle != nullptr ? getAccelerationStructureHandle(user, resource) : 0ull;
}

NRIPersistentVoxelOverlayStats NRIPersistentVoxelResidency::BuildOverlayStats() const
{
	NRIPersistentVoxelOverlayStats stats = {};
	stats.actorCount = batch.activeActorCount;
	stats.primitiveCount = batch.primitiveCount;
	stats.materialCount = (uint32_t)batch.materialBridge.materials.size();
	stats.byteCount =
		(uint64_t)batch.primitiveCount * sizeof(nri_scene::PrimitiveData) +
		nri_scene::EstimateMaterialBridgeBytes(batch.materialBridge);
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active)
		{
			stats.indexCount += actor.indexCount;
		}
	}
	stats.byteCount += (uint64_t)stats.indexCount * sizeof(uint32_t);
	return stats;
}

bool NRIPersistentVoxelResidency::HasValidBatch() const
{
	return batch.valid;
}

bool NRIPersistentVoxelResidency::HasRenderableOverlay() const
{
	return
		batch.valid &&
		batch.activeActorCount > 0 &&
		batch.primitiveCount > 0 &&
		!batch.materialBridge.materials.empty();
}

bool NRIPersistentVoxelResidency::HasPreloadPending() const
{
	return preloadPending;
}

uint32_t NRIPersistentVoxelResidency::OverlayMaterialCount() const
{
	return (uint32_t)batch.materialBridge.materials.size();
}

uint32_t NRIPersistentVoxelResidency::EstimatePrimitiveCountForInstanceOffset(uint32_t primitiveOffset) const
{
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active && actor.primitiveOffset == primitiveOffset && actor.primitiveCount > 0)
		{
			return actor.primitiveCount;
		}
	}
	return 0;
}

nri_scene::SceneDebugStats NRIPersistentVoxelResidency::BuildOverlayDebugStats() const
{
	nri_scene::SceneDebugStats stats = batch.stats;
	stats.voxelStableCandidates = 0;
	stats.voxelStableUncacheable = 0;
	stats.voxelStableSignatureHits = 0;
	stats.voxelStableSignatureMisses = 0;
	stats.voxelStableSignatureChanges = 0;
	stats.voxelStableSplitStable = 0;
	stats.voxelStableSplitLive = 0;
	stats.voxelCacheEntries = 0;
	stats.voxelCacheSurfaceHits = 0;
	stats.voxelCacheSurfaceStores = 0;
	stats.voxelCacheSurfaceRebuilds = 0;
	stats.voxelCacheTransformRebakes = 0;
	stats.voxelCacheSurfaceRemoves = 0;
	stats.voxelCacheNotCaptured = 0;
	stats.voxelCachePrimitives = 0;
	return stats;
}

uint64_t NRIPersistentVoxelResidency::BuildSceneGenerationHash() const
{
	if (!HasRenderableOverlay())
	{
		return 0;
	}

	return nri_scene::HashCombine64(
		nri_scene::HashCombine64(
			nri_scene::HashCombine64(
				nri_scene::HashCombine64(batch.sourceSerial, (uint64_t)batch.rebuildCount),
				(uint64_t)batch.activeActorCount),
			(uint64_t)batch.primitiveCount),
		(uint64_t)batch.materialCount);
}

void NRIPersistentVoxelResidency::RebuildBatchMaterialBridge(PersistentVoxelBatch& targetBatch)
{
	targetBatch.materialBridge = {};
	std::vector<PersistentVoxelMaterialVariantResource*> materialResources;
	materialResources.reserve(materialVariantResources.size());
	for (auto& pair : materialVariantResources)
	{
		if (pair.second.materialCount > 0)
		{
			materialResources.push_back(&pair.second);
		}
	}
	std::sort(
		materialResources.begin(),
		materialResources.end(),
		[](const PersistentVoxelMaterialVariantResource* left, const PersistentVoxelMaterialVariantResource* right)
		{
			return left->materialOffset < right->materialOffset;
		});
	for (PersistentVoxelMaterialVariantResource* resource : materialResources)
	{
		if (resource == nullptr)
		{
			continue;
		}
		if (targetBatch.materialBridge.materials.size() < resource->materialOffset)
		{
			targetBatch.materialBridge.materials.resize(resource->materialOffset);
			targetBatch.materialBridge.lightMetadata.resize(resource->materialOffset);
		}
		nri_scene::AppendMaterialBridge(resource->materialBridge, targetBatch.materialBridge);
		const uint64_t materialSize = (uint64_t)resource->materialCount * sizeof(nri_scene::MaterialData);
		if (resource->materialOffset <= targetBatch.materialBridge.materials.size() &&
			resource->materialCount <= targetBatch.materialBridge.materials.size() - resource->materialOffset)
		{
			resource->materialPayloadHash = NRIHashUploadPayloadBytes(
				targetBatch.materialBridge.materials.data() + resource->materialOffset,
				materialSize);
		}
	}
	for (PersistentVoxelBatch::ActorEntry& actor : targetBatch.actors)
	{
		if (!actor.active)
		{
			actor.materialOffset = 0;
		}
	}
}

void NRIPersistentVoxelResidency::RecomputeBatchState(PersistentVoxelBatch& targetBatch) const
{
	targetBatch.primitiveCount = 0;
	targetBatch.materialCount = 0;
	targetBatch.activeActorCount = 0;
	for (const PersistentVoxelBatch::ActorEntry& actor : targetBatch.actors)
	{
		if (actor.active)
		{
			targetBatch.activeActorCount++;
			targetBatch.primitiveCount += actor.primitiveCount;
		}
	}
	targetBatch.materialCount = (uint32_t)targetBatch.materialBridge.materials.size();
	targetBatch.surfaceCount = targetBatch.activeActorCount;
	targetBatch.stats = {};
	targetBatch.stats.triangleEstimate = targetBatch.primitiveCount;
	targetBatch.stats.voxelCachePrimitives = targetBatch.primitiveCount;
	targetBatch.stats.materialRefs = targetBatch.materialCount;
	targetBatch.stats.spriteDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.modelDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.voxelProxyDrawItems = targetBatch.surfaceCount;
	targetBatch.stats.voxelCacheEntries = targetBatch.surfaceCount;
	targetBatch.stats.totalDrawItems = targetBatch.surfaceCount;
	targetBatch.valid =
		targetBatch.activeActorCount > 0 &&
		targetBatch.primitiveCount > 0 &&
		!targetBatch.materialBridge.materials.empty();
}

void NRIPersistentVoxelResidency::ClearActorInstances(const NRIPersistentVoxelResetServices& services)
{
	batch = {};
	instances.clear();
	const bool keepSharedVariantArena = !meshVariantResources.empty();
	if (!keepSharedVariantArena)
	{
		services.RetireBuffer(vertexBuffer);
		services.RetireBuffer(indexBuffer);
		services.RetireBuffer(primitiveBuffer);
		arenaVertexCursor = 0;
		arenaIndexCursor = 0;
		arenaPrimitiveCursor = 0;
	}
	services.InvalidateSceneDataDescriptors();
}

void NRIPersistentVoxelResidency::AppendMaterialBridgeTo(nri_scene::MaterialBridgeData& destination) const
{
	nri_scene::AppendMaterialBridge(batch.materialBridge, destination);
}

bool NRIPersistentVoxelResidency::WarmMaterialResources(
	const NRIPersistentVoxelMaterialWarmupServices& services,
	NRIPersistentVoxelMaterialWarmupResult& outResult) const
{
	outResult = {};
	outResult.paletteReady = true;
	outResult.hasMaterials = batch.valid && !batch.materialBridge.materials.empty();
	outResult.materialCount = outResult.hasMaterials ? (uint32_t)batch.materialBridge.materials.size() : 0u;
	outResult.variantResourceCount = (uint32_t)materialVariantResources.size();
	if (!outResult.hasMaterials)
	{
		return true;
	}

	outResult.paletteReady = services.EnsurePalette(batch.materialBridge);
	return outResult.paletteReady && services.WarmTextures(batch.materialBridge, outResult.textureStats);
}

bool NRIPersistentVoxelResidency::UploadArenaMaterialBuffers(
	const std::vector<nri_scene::MaterialData>& materials,
	const NRIPersistentVoxelMaterialUploadServices& services,
	uint32_t frameIndex,
	bool voxelStatsEnabled,
	NRIPersistentVoxelMaterialUploadStats& outStats)
{
	outStats = {};
	if (!batch.valid)
	{
		return true;
	}

	if (!services.EnsureMaterialArenaBuffer(
		materialBuffer,
		(uint64_t)arenaMaterialCursor * sizeof(nri_scene::MaterialData)))
	{
		return false;
	}

	struct PendingMaterialUpload
	{
		PersistentVoxelMaterialVariantResource* resource = nullptr;
		uint64_t materialHash = 0;
	};

	std::vector<RuntimeMutationResidentUploadRange> dirtyMaterialRanges;
	std::vector<PendingMaterialUpload> pendingMaterialUploads;
	dirtyMaterialRanges.reserve(materialVariantResources.size());
	pendingMaterialUploads.reserve(materialVariantResources.size());

	for (auto& pair : materialVariantResources)
	{
		PersistentVoxelMaterialVariantResource& resource = pair.second;
		if (resource.materialCount == 0)
		{
			continue;
		}
		if ((uint64_t)resource.materialOffset + resource.materialCount > materials.size())
		{
			continue;
		}

		const nri_scene::MaterialData* actorMaterials = materials.data() + resource.materialOffset;
		const uint64_t materialSize = (uint64_t)resource.materialCount * sizeof(nri_scene::MaterialData);
		outStats.requestedBytes += materialSize;
		outStats.domainPayloadBytes += materialSize;
		outStats.domainMaterialPayloadBytes += materialSize;
		outStats.domainHashChecks++;
		const uint64_t materialHash = NRIHashUploadPayloadBytes(actorMaterials, materialSize);
		const bool uploadMaterials = resource.materialUploadHash != materialHash;
		if (uploadMaterials)
		{
			outStats.domainHashMisses++;
			outStats.uploads++;
			outStats.dirtyBytes += materialSize;
			dirtyMaterialRanges.push_back({
				ResidentUploadKind_Material,
				(uint64_t)resource.materialOffset * sizeof(nri_scene::MaterialData),
				materialSize,
				materialSize });
			pendingMaterialUploads.push_back({ &resource, materialHash });
		}
		else
		{
			resource.materialUploadHash = materialHash;
		}
	}

	if (dirtyMaterialRanges.empty())
	{
		return true;
	}

	constexpr uint64_t kMaterialUploadCoalesceMaxGapBytes = 4ull * 1024ull;
	constexpr uint64_t kMaterialUploadCoalesceMaxByteExpansion = 2ull;
	std::sort(
		dirtyMaterialRanges.begin(),
		dirtyMaterialRanges.end(),
		[](const RuntimeMutationResidentUploadRange& a, const RuntimeMutationResidentUploadRange& b)
		{
			return a.byteOffset < b.byteOffset;
		});

	std::vector<RuntimeMutationResidentUploadRange> coalescedRanges;
	coalescedRanges.reserve(dirtyMaterialRanges.size());
	for (const RuntimeMutationResidentUploadRange& range : dirtyMaterialRanges)
	{
		if (coalescedRanges.empty())
		{
			coalescedRanges.push_back(range);
			continue;
		}

		RuntimeMutationResidentUploadRange& tail = coalescedRanges.back();
		const uint64_t tailEnd = tail.byteOffset + tail.size;
		const uint64_t rangeEnd = range.byteOffset + range.size;
		const uint64_t gapBytes = range.byteOffset > tailEnd ? range.byteOffset - tailEnd : 0;
		const uint64_t candidateSize = rangeEnd > tailEnd ? rangeEnd - tail.byteOffset : tail.size;
		const uint64_t candidateDirtySize = tail.dirtySize + range.size;
		const bool acceptableByteExpansion =
			candidateDirtySize > UINT64_MAX / kMaterialUploadCoalesceMaxByteExpansion ||
			candidateSize <= candidateDirtySize * kMaterialUploadCoalesceMaxByteExpansion;
		if (gapBytes <= kMaterialUploadCoalesceMaxGapBytes && acceptableByteExpansion)
		{
			if (rangeEnd > tailEnd)
			{
				tail.size = rangeEnd - tail.byteOffset;
			}
			tail.dirtySize += range.size;
			continue;
		}

		outStats.batchRejects++;
		coalescedRanges.push_back(range);
	}

	uint64_t uploadedBytes = 0;
	for (const RuntimeMutationResidentUploadRange& range : coalescedRanges)
	{
		uploadedBytes += range.size;
		if (range.size > range.dirtySize)
		{
			outStats.batchGapBytes += range.size - range.dirtySize;
		}
		services.NoteMaterialUpload(range.size);
	}

	outStats.uploadedBytes += uploadedBytes;
	outStats.domainUploadedBytes += uploadedBytes;
	outStats.domainMaterialUploadedBytes += uploadedBytes;

	const uint64_t materialArenaSize = materials.size() * sizeof(nri_scene::MaterialData);
	if (!services.StageMaterialRanges(
		materialBuffer,
		coalescedRanges,
		reinterpret_cast<const uint8_t*>(materials.data()),
		materialArenaSize))
	{
		return false;
	}

	for (const PendingMaterialUpload& upload : pendingMaterialUploads)
	{
		if (upload.resource == nullptr)
		{
			continue;
		}
		upload.resource->materialUploadHash = upload.materialHash;
		if (voxelStatsEnabled)
		{
			const uint64_t materialSize =
				(uint64_t)upload.resource->materialCount * sizeof(nri_scene::MaterialData);
			Printf("PERF pt voxel material variant NRI: frame=%u action=upload reason=arena-sync actor_key=0x0 mat_key=0x%llx ref_count=0 material_offset=%u material_count=%u material_capacity=%u upload_hash=0x%llx upload_bytes=%llu ready=1\n",
				frameIndex,
				(unsigned long long)upload.resource->materialKeyHash,
				upload.resource->materialOffset,
				upload.resource->materialCount,
				upload.resource->materialCapacity,
				(unsigned long long)upload.materialHash,
				(unsigned long long)materialSize);
		}
	}

	return true;
}

bool NRIPersistentVoxelResidency::AppendTlasInstances(
	std::vector<nri::TopLevelInstance>& instances,
	std::vector<SceneInstanceData>& sceneInstances,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelTlasServices& services,
	NRIPersistentVoxelTlasBuildStats& outStats)
{
	constexpr uint32_t PersistentVoxelSceneDataSource = 2u;
	outStats = {};

	std::unordered_set<uint64_t> persistentVoxelTlasMeshResources;
	persistentVoxelTlasMeshResources.reserve(batch.actors.size());
	struct PersistentVoxelTlasGroupStats
	{
		uint64_t meshResourceKey = 0;
		uint64_t meshKeyHash = 0;
		uint32_t primitiveCount = 0;
		uint32_t instanceCount = 0;
		uint32_t capturedCount = 0;
		uint32_t retainedCount = 0;
		uint64_t instancePrimitiveCount = 0;
		uint32_t newInstanceCount = 0;
		uint64_t newInstancePrimitiveCount = 0;
		uint64_t maxRetainedFrameAge = 0;
		uint32_t tlasReadyFrame = 0;
		int32_t resolvedVoxelIndex = -1;
		bool newlyPublished = false;
	};
	const bool tracePersistentVoxelTlasSummary = voxelStatsEnabled;
	std::unordered_map<uint64_t, PersistentVoxelTlasGroupStats> persistentVoxelTlasGroups;
	std::unordered_set<uint64_t> persistentVoxelTlasNewMeshResources;
	if (tracePersistentVoxelTlasSummary)
	{
		persistentVoxelTlasGroups.reserve(batch.actors.size());
		persistentVoxelTlasNewMeshResources.reserve(batch.actors.size());
	}
	uint32_t persistentVoxelTlasCandidateCount = 0;
	uint32_t persistentVoxelTlasPublishedCount = 0;
	uint32_t persistentVoxelTlasSkippedCount = 0;
	uint32_t persistentVoxelTlasMissingSkipCount = 0;
	uint32_t persistentVoxelTlasReadyFrameSkipCount = 0;
	uint32_t persistentVoxelTlasExcludedSkipCount = 0;
	uint32_t persistentVoxelTlasNewInstanceCount = 0;
	uint32_t persistentVoxelTlasNewMeshCount = 0;
	uint32_t persistentVoxelTlasCapturedCount = 0;
	uint32_t persistentVoxelTlasRetainedCount = 0;
	uint64_t persistentVoxelTlasMissingSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasReadyFrameSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasExcludedSkipPrimitiveCount = 0;
	uint64_t persistentVoxelTlasNewInstancePrimitiveCount = 0;
	uint64_t persistentVoxelTlasNewUniquePrimitiveCount = 0;
	uint64_t persistentVoxelTlasInstancePrimitiveCount = 0;
	uint64_t persistentVoxelTlasUniquePrimitiveCount = 0;
	uint64_t persistentVoxelTlasMaxRetainedFrameAge = 0;
	const int32_t persistentVoxelExcludeIndex0 = settings.excludeIndices[0];
	const int32_t persistentVoxelExcludeIndex1 = settings.excludeIndices[1];
	const int32_t persistentVoxelExcludeIndex2 = settings.excludeIndices[2];
	const uint32_t persistentVoxelExcludeMinPrims = settings.excludeMinPrimitives;
	uint64_t persistentVoxelRetainedTlasPrimitives = 0;
	auto computePersistentVoxelRetainedAge = [frameIndex](const PersistentVoxelBatch::ActorEntry& actor) -> uint64_t
	{
		if (actor.capturedThisFrame)
		{
			return 0;
		}
		const uint64_t frameAge = actor.lastSeenFrame != 0 && (uint64_t)frameIndex >= actor.lastSeenFrame ?
			(uint64_t)frameIndex - actor.lastSeenFrame :
			0;
		return std::max(actor.retainedFrameAge, frameAge);
	};
	std::vector<PersistentVoxelBatch::ActorEntry*> persistentVoxelTlasActors;
	persistentVoxelTlasActors.reserve(batch.actors.size());
	for (PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		actor.inWorldTlasThisFrame = false;
		if (actor.active)
		{
			persistentVoxelTlasActors.push_back(&actor);
		}
	}
	std::stable_sort(persistentVoxelTlasActors.begin(), persistentVoxelTlasActors.end(),
		[&](const PersistentVoxelBatch::ActorEntry* left, const PersistentVoxelBatch::ActorEntry* right)
		{
			if (left->capturedThisFrame != right->capturedThisFrame)
			{
				return left->capturedThisFrame;
			}
			const uint64_t leftRetainedAge = computePersistentVoxelRetainedAge(*left);
			const uint64_t rightRetainedAge = computePersistentVoxelRetainedAge(*right);
			if (leftRetainedAge != rightRetainedAge)
			{
				return leftRetainedAge < rightRetainedAge;
			}
			if (left->primitiveCount != right->primitiveCount)
			{
				return left->primitiveCount < right->primitiveCount;
			}
			return left->identityKey < right->identityKey;
		});
	for (PersistentVoxelBatch::ActorEntry* actorPtr : persistentVoxelTlasActors)
	{
		PersistentVoxelBatch::ActorEntry& actor = *actorPtr;
		persistentVoxelTlasCandidateCount++;
		const bool excludedByIndex = actor.resolvedVoxelIndex >= 0 &&
			(actor.resolvedVoxelIndex == persistentVoxelExcludeIndex0 ||
				actor.resolvedVoxelIndex == persistentVoxelExcludeIndex1 ||
				actor.resolvedVoxelIndex == persistentVoxelExcludeIndex2);
		const bool excludedByPrimitiveCount = persistentVoxelExcludeMinPrims > 0 &&
			actor.primitiveCount >= persistentVoxelExcludeMinPrims;
		if (excludedByIndex || excludedByPrimitiveCount)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx voxel=%d instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					excludedByIndex ? "excluded-index" : "excluded-prims",
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					actor.resolvedVoxelIndex,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasExcludedSkipCount++;
			persistentVoxelTlasExcludedSkipPrimitiveCount += actor.primitiveCount;
			continue;
		}
		const uint64_t actorRetainedFrameAge = computePersistentVoxelRetainedAge(actor);

		auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
		const char* tlasSkipReason = nullptr;
		if (meshResourceIt == meshVariantResources.end())
		{
			tlasSkipReason = "missing-mesh";
		}
		else if (meshResourceIt->second.accelerationStructure.accelerationStructure == nullptr)
		{
			tlasSkipReason = "missing-blas";
		}
		else if (meshResourceIt->second.indexBuffer.shaderView == nullptr ||
			meshResourceIt->second.vertexBuffer.shaderView == nullptr)
		{
			tlasSkipReason = "missing-mesh-view";
		}
		else if (vertexBuffer.shaderView == nullptr ||
			indexBuffer.shaderView == nullptr ||
			primitiveBuffer.shaderView == nullptr ||
			materialBuffer.shaderView == nullptr)
		{
			tlasSkipReason = "missing-arena-view";
		}
		if (tlasSkipReason != nullptr)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					tlasSkipReason,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			continue;
		}
		auto persistentVoxelTransformFinite = [](const std::array<float, 12>& transform) -> bool
		{
			for (float value : transform)
			{
				if (!std::isfinite(value))
				{
					return false;
				}
			}
			return true;
		};
		const bool primitiveArenaRangeValid =
			(uint64_t)actor.primitiveOffset + (uint64_t)actor.primitiveCount <= (uint64_t)arenaPrimitiveCursor;
		const bool materialArenaRangeValid =
			(uint64_t)actor.materialOffset + (uint64_t)actor.materialCount <= (uint64_t)arenaMaterialCursor;
		const bool meshRangeMatches =
			actor.primitiveOffset == meshResourceIt->second.primitiveOffset &&
			actor.primitiveCount == meshResourceIt->second.primitiveCount &&
			actor.indexOffset == meshResourceIt->second.indexOffset &&
			actor.indexCount == meshResourceIt->second.indexCount;
		const bool transformValid =
			persistentVoxelTransformFinite(actor.instanceTransform) &&
			persistentVoxelTransformFinite(actor.previousInstanceTransform);
		const char* invalidTlasReason = nullptr;
		if (!primitiveArenaRangeValid)
		{
			invalidTlasReason = "invalid-primitive-range";
		}
		else if (!materialArenaRangeValid)
		{
			invalidTlasReason = "invalid-material-range";
		}
		else if (!meshRangeMatches)
		{
			invalidTlasReason = "mesh-range-mismatch";
		}
		else if (!transformValid)
		{
			invalidTlasReason = "invalid-transform";
		}
		if (invalidTlasReason != nullptr)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=%s actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u primitive_cursor=%u mesh_primitive_offset=%u mesh_primitive_count=%u index_offset=%u index_count=%u mesh_index_offset=%u mesh_index_count=%u material_offset=%u material_count=%u material_cursor=%u blas=1 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					invalidTlasReason,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					arenaPrimitiveCursor,
					meshResourceIt->second.primitiveOffset,
					meshResourceIt->second.primitiveCount,
					actor.indexOffset,
					actor.indexCount,
					meshResourceIt->second.indexOffset,
					meshResourceIt->second.indexCount,
					actor.materialOffset,
					actor.materialCount,
					arenaMaterialCursor,
					meshResourceIt->second.tlasReadyFrame,
					meshResourceIt->second.tlasPublished ? 1u : 0u);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			continue;
		}
		if (!meshResourceIt->second.tlasPublished &&
			meshResourceIt->second.tlasReadyFrame > frameIndex)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel tlas NRI: frame=%u action=skip reason=ready-frame actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=1 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					(unsigned long long)actor.materialKeyHash,
					(uint32_t)sceneInstances.size(),
					actor.primitiveOffset,
					actor.primitiveCount,
					actor.materialOffset,
					actor.materialCount,
					meshResourceIt->second.tlasReadyFrame,
					meshResourceIt->second.tlasPublished ? 1u : 0u);
			}
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasReadyFrameSkipCount++;
			persistentVoxelTlasReadyFrameSkipPrimitiveCount += actor.primitiveCount;
			continue;
		}
		const bool meshResourceFirstPublish = !meshResourceIt->second.tlasPublished;
		const bool meshResourceNewThisFrame = meshResourceFirstPublish ||
			(tracePersistentVoxelTlasSummary && persistentVoxelTlasNewMeshResources.find(actor.meshResourceKey) != persistentVoxelTlasNewMeshResources.end());

		nri::TopLevelInstance persistentVoxelInstance = {};
		for (uint32_t row = 0; row < 3; ++row)
		{
			for (uint32_t column = 0; column < 4; ++column)
			{
				persistentVoxelInstance.transform[row][column] = actor.instanceTransform[row * 4u + column];
			}
		}
		persistentVoxelInstance.instanceId = (uint32_t)sceneInstances.size();
		persistentVoxelInstance.mask = 0xFF;
		persistentVoxelInstance.shaderBindingTableLocalOffset = 0;
		persistentVoxelInstance.flags = nri::TopLevelInstanceBits::TRIANGLE_CULL_DISABLE;
		persistentVoxelInstance.accelerationStructureHandle = services.GetAccelerationStructureHandle(meshResourceIt->second.accelerationStructure);
		if (persistentVoxelInstance.accelerationStructureHandle == 0)
		{
			persistentVoxelTlasSkippedCount++;
			persistentVoxelTlasMissingSkipCount++;
			persistentVoxelTlasMissingSkipPrimitiveCount += actor.primitiveCount;
			continue;
		}
		instances.push_back(persistentVoxelInstance);
		SceneInstanceData sceneInstance = {};
		sceneInstance.primitiveOffset = actor.primitiveOffset;
		sceneInstance.dataSource = PersistentVoxelSceneDataSource;
		sceneInstance.reserved0 = actor.materialOffset;
		sceneInstance.reserved1 = actor.materialCount;
		sceneInstance.visibilityChunk = actor.visibilityChunkIndex;
		for (uint32_t i = 0; i < 12; ++i)
		{
			sceneInstance.currentTransform[i] = actor.instanceTransform[i];
			sceneInstance.previousTransform[i] = actor.previousInstanceTransform[i];
		}
		sceneInstances.push_back(sceneInstance);
		actor.inWorldTlasThisFrame = true;
		if (meshResourceFirstPublish)
		{
			persistentVoxelTlasNewMeshCount++;
			persistentVoxelTlasNewUniquePrimitiveCount += actor.primitiveCount;
			if (tracePersistentVoxelTlasSummary)
			{
				persistentVoxelTlasNewMeshResources.insert(actor.meshResourceKey);
			}
		}
		if (meshResourceNewThisFrame)
		{
			persistentVoxelTlasNewInstanceCount++;
			persistentVoxelTlasNewInstancePrimitiveCount += actor.primitiveCount;
		}
		meshResourceIt->second.tlasPublished = true;
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel tlas NRI: frame=%u action=publish reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx mat_key=0x%llx instance_id=%u primitive_offset=%u primitive_count=%u material_offset=%u material_count=%u blas=1 tlas_ready=%u tlas_published=1 first_publish=%u new_this_frame=%u ready=1\n",
				frameIndex,
				(unsigned long long)actor.identityKey,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				(unsigned long long)actor.materialKeyHash,
				persistentVoxelInstance.instanceId,
				actor.primitiveOffset,
				actor.primitiveCount,
				actor.materialOffset,
				actor.materialCount,
				meshResourceIt->second.tlasReadyFrame,
				meshResourceFirstPublish ? 1u : 0u,
				meshResourceNewThisFrame ? 1u : 0u);
		}
		persistentVoxelTlasMeshResources.insert(actor.meshResourceKey);
		persistentVoxelTlasPublishedCount++;
		persistentVoxelTlasInstancePrimitiveCount += actor.primitiveCount;
		if (actor.capturedThisFrame)
		{
			persistentVoxelTlasCapturedCount++;
		}
		else
		{
			persistentVoxelTlasRetainedCount++;
			persistentVoxelRetainedTlasPrimitives += actor.primitiveCount;
			persistentVoxelTlasMaxRetainedFrameAge = std::max(persistentVoxelTlasMaxRetainedFrameAge, actorRetainedFrameAge);
		}
		if (tracePersistentVoxelTlasSummary)
		{
			PersistentVoxelTlasGroupStats& group = persistentVoxelTlasGroups[actor.meshResourceKey];
			if (group.instanceCount == 0)
			{
				group.meshResourceKey = actor.meshResourceKey;
				group.meshKeyHash = actor.meshKeyHash;
				group.primitiveCount = actor.primitiveCount;
				group.resolvedVoxelIndex = actor.resolvedVoxelIndex;
				group.tlasReadyFrame = meshResourceIt->second.tlasReadyFrame;
			}
			group.instanceCount++;
			group.instancePrimitiveCount += actor.primitiveCount;
			if (meshResourceFirstPublish)
			{
				group.newlyPublished = true;
			}
			if (meshResourceNewThisFrame)
			{
				group.newInstanceCount++;
				group.newInstancePrimitiveCount += actor.primitiveCount;
			}
			group.primitiveCount = std::max(group.primitiveCount, actor.primitiveCount);
			if (actor.capturedThisFrame)
			{
				group.capturedCount++;
			}
			else
			{
				group.retainedCount++;
				group.maxRetainedFrameAge = std::max(group.maxRetainedFrameAge, actorRetainedFrameAge);
			}
		}
		outStats.instanceCount++;
		if (meshResourceIt->second.meshBakeSpace != nri_scene::VoxelMeshBakeSpace::LocalSpace)
		{
			outStats.bakedFallbackInstanceCount++;
		}
	}
	outStats.sharedMeshResourceCount = (uint32_t)persistentVoxelTlasMeshResources.size();
	if (tracePersistentVoxelTlasSummary)
	{
		for (const auto& groupPair : persistentVoxelTlasGroups)
		{
			persistentVoxelTlasUniquePrimitiveCount += groupPair.second.primitiveCount;
		}
		Printf("PERF pt voxel tlas summary NRI: frame=%u candidates=%u published=%u skipped=%u captured=%u retained=%u unique_meshes=%u instance_prims=%llu unique_prims=%llu max_retained_age=%llu actors=%u active=%u\n",
			frameIndex,
			persistentVoxelTlasCandidateCount,
			persistentVoxelTlasPublishedCount,
			persistentVoxelTlasSkippedCount,
			persistentVoxelTlasCapturedCount,
			persistentVoxelTlasRetainedCount,
			(uint32_t)persistentVoxelTlasMeshResources.size(),
			(unsigned long long)persistentVoxelTlasInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasUniquePrimitiveCount,
			(unsigned long long)persistentVoxelTlasMaxRetainedFrameAge,
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);
		Printf("PERF pt voxel tlas pressure NRI: frame=%u new_meshes=%u new_instances=%u new_instance_prims=%llu new_unique_prims=%llu ready_frame_skips=%u ready_frame_skip_prims=%llu missing_skips=%u missing_skip_prims=%llu excluded_skips=%u excluded_skip_prims=%llu retained_prims=%llu active_instances=%u active_instance_prims=%llu active_unique_prims=%llu active_unique_meshes=%u actors=%u active=%u\n",
			frameIndex,
			persistentVoxelTlasNewMeshCount,
			persistentVoxelTlasNewInstanceCount,
			(unsigned long long)persistentVoxelTlasNewInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasNewUniquePrimitiveCount,
			persistentVoxelTlasReadyFrameSkipCount,
			(unsigned long long)persistentVoxelTlasReadyFrameSkipPrimitiveCount,
			persistentVoxelTlasMissingSkipCount,
			(unsigned long long)persistentVoxelTlasMissingSkipPrimitiveCount,
			persistentVoxelTlasExcludedSkipCount,
			(unsigned long long)persistentVoxelTlasExcludedSkipPrimitiveCount,
			(unsigned long long)persistentVoxelRetainedTlasPrimitives,
			persistentVoxelTlasPublishedCount,
			(unsigned long long)persistentVoxelTlasInstancePrimitiveCount,
			(unsigned long long)persistentVoxelTlasUniquePrimitiveCount,
			(uint32_t)persistentVoxelTlasMeshResources.size(),
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);

		std::vector<PersistentVoxelTlasGroupStats> sortedTlasGroups;
		sortedTlasGroups.reserve(persistentVoxelTlasGroups.size());
		for (const auto& groupPair : persistentVoxelTlasGroups)
		{
			sortedTlasGroups.push_back(groupPair.second);
		}
		std::sort(sortedTlasGroups.begin(), sortedTlasGroups.end(), [](const PersistentVoxelTlasGroupStats& left, const PersistentVoxelTlasGroupStats& right)
		{
			if (left.instancePrimitiveCount != right.instancePrimitiveCount)
			{
				return left.instancePrimitiveCount > right.instancePrimitiveCount;
			}
			if (left.instanceCount != right.instanceCount)
			{
				return left.instanceCount > right.instanceCount;
			}
			return left.meshResourceKey < right.meshResourceKey;
		});
		const uint32_t topCount = std::min<uint32_t>(8u, (uint32_t)sortedTlasGroups.size());
		for (uint32_t i = 0; i < topCount; ++i)
		{
			const PersistentVoxelTlasGroupStats& group = sortedTlasGroups[i];
			Printf("PERF pt voxel tlas top NRI: frame=%u rank=%u mesh_resource=0x%llx mesh_key=0x%llx voxel=%d instances=%u captured=%u retained=%u primitive_count=%u instance_prims=%llu max_retained_age=%llu tlas_ready=%u\n",
				frameIndex,
				i + 1u,
				(unsigned long long)group.meshResourceKey,
				(unsigned long long)group.meshKeyHash,
				group.resolvedVoxelIndex,
				group.instanceCount,
				group.capturedCount,
				group.retainedCount,
				group.primitiveCount,
				(unsigned long long)group.instancePrimitiveCount,
				(unsigned long long)group.maxRetainedFrameAge,
				group.tlasReadyFrame);
		}
		std::vector<PersistentVoxelTlasGroupStats> sortedNewTlasGroups;
		sortedNewTlasGroups.reserve(persistentVoxelTlasGroups.size());
		for (const PersistentVoxelTlasGroupStats& group : sortedTlasGroups)
		{
			if (group.newlyPublished || group.newInstanceCount > 0)
			{
				sortedNewTlasGroups.push_back(group);
			}
		}
		std::sort(sortedNewTlasGroups.begin(), sortedNewTlasGroups.end(), [](const PersistentVoxelTlasGroupStats& left, const PersistentVoxelTlasGroupStats& right)
		{
			if (left.newInstancePrimitiveCount != right.newInstancePrimitiveCount)
			{
				return left.newInstancePrimitiveCount > right.newInstancePrimitiveCount;
			}
			if (left.primitiveCount != right.primitiveCount)
			{
				return left.primitiveCount > right.primitiveCount;
			}
			return left.meshResourceKey < right.meshResourceKey;
		});
		const uint32_t topNewCount = std::min<uint32_t>(8u, (uint32_t)sortedNewTlasGroups.size());
		for (uint32_t i = 0; i < topNewCount; ++i)
		{
			const PersistentVoxelTlasGroupStats& group = sortedNewTlasGroups[i];
			Printf("PERF pt voxel tlas new top NRI: frame=%u rank=%u mesh_resource=0x%llx mesh_key=0x%llx voxel=%d new_mesh=%u new_instances=%u primitive_count=%u new_instance_prims=%llu tlas_ready=%u\n",
				frameIndex,
				i + 1u,
				(unsigned long long)group.meshResourceKey,
				(unsigned long long)group.meshKeyHash,
				group.resolvedVoxelIndex,
				group.newlyPublished ? 1u : 0u,
				group.newInstanceCount,
				group.primitiveCount,
				(unsigned long long)group.newInstancePrimitiveCount,
				group.tlasReadyFrame);
		}
	}

	return true;
}

NRIPersistentVoxelDescriptorSnapshot NRIPersistentVoxelResidency::BuildDescriptorSnapshot(
	const NRIBufferResource& fallbackVertexBuffer,
	const NRIBufferResource& fallbackIndexBuffer,
	const NRIBufferResource& fallbackPrimitiveBuffer,
	const NRIBufferResource& fallbackMaterialBuffer) const
{
	auto selectView = [](const NRIBufferResource& primary, const NRIBufferResource& fallback) -> nri::Descriptor*
	{
		return primary.shaderView != nullptr ? primary.shaderView : fallback.shaderView;
	};

	NRIPersistentVoxelDescriptorSnapshot snapshot = {};
	snapshot.vertex = selectView(vertexBuffer, fallbackVertexBuffer);
	snapshot.index = selectView(indexBuffer, fallbackIndexBuffer);
	snapshot.primitive = selectView(primitiveBuffer, fallbackPrimitiveBuffer);
	snapshot.material = selectView(materialBuffer, fallbackMaterialBuffer);
	snapshot.primitiveCount = BoundPrimitiveCount();
	snapshot.materialCount = BoundMaterialCount();
	return snapshot;
}

uint32_t NRIPersistentVoxelResidency::BoundPrimitiveCount() const
{
	return primitiveBuffer.shaderView != nullptr ? arenaPrimitiveCursor : 0u;
}

uint32_t NRIPersistentVoxelResidency::BoundMaterialCount() const
{
	return materialBuffer.shaderView != nullptr ? arenaMaterialCursor : 0u;
}

void NRIPersistentVoxelDestroyServices::DestroyBuffer(NRIBufferResource& resource) const
{
	if (destroyBuffer != nullptr)
	{
		destroyBuffer(user, resource);
	}
}

void NRIPersistentVoxelResidency::DestroyArenaBuffers(const NRIPersistentVoxelDestroyServices& services)
{
	services.DestroyBuffer(vertexBuffer);
	services.DestroyBuffer(indexBuffer);
	services.DestroyBuffer(primitiveBuffer);
	services.DestroyBuffer(materialBuffer);
}

NRIPersistentVoxelLightAppendStats NRIPersistentVoxelResidency::AppendSceneLights(
	SceneLightSystem& sceneLights,
	uint32_t frameIndex,
	bool voxelStatsEnabled) const
{
	NRIPersistentVoxelLightAppendStats stats = {};
	if (!batch.valid || batch.materialBridge.materials.empty())
	{
		return stats;
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || actor.lightRecords.empty() || actor.materialCount == 0)
		{
			continue;
		}
		if (!actor.inWorldTlasThisFrame)
		{
			stats.skippedActors++;
			stats.skippedRecords += (uint32_t)actor.lightRecords.size();
			continue;
		}

		sceneLights.AppendSurfaceRecords(actor.lightRecords, actor.materialOffset);
		stats.appendedActors++;
		stats.appendedRecords += (uint32_t)actor.lightRecords.size();
	}
	if (voxelStatsEnabled && (stats.appendedActors != 0 || stats.skippedActors != 0))
	{
		Printf("PERF pt voxel light NRI: frame=%u appended_actors=%u skipped_not_tlas=%u appended_records=%u skipped_records=%u actors=%u active=%u\n",
			frameIndex,
			stats.appendedActors,
			stats.skippedActors,
			stats.appendedRecords,
			stats.skippedRecords,
			(uint32_t)batch.actors.size(),
			batch.activeActorCount);
	}
	return stats;
}

NRIPersistentVoxelMemoryUsage NRIPersistentVoxelResidency::GetMemoryUsage() const
{
	NRIPersistentVoxelMemoryUsage usage = {};
	auto accumulateBuffer = [](const NRIBufferResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};
	auto accumulateAs = [](const NRIAccelerationStructureResource& resource, uint64_t& total)
	{
		total += resource.memorySize;
	};

	accumulateBuffer(vertexBuffer, usage.sceneBufferBytes);
	accumulateBuffer(indexBuffer, usage.sceneBufferBytes);
	accumulateBuffer(primitiveBuffer, usage.sceneBufferBytes);
	accumulateBuffer(materialBuffer, usage.sceneBufferBytes);
	for (const auto& pair : meshVariantResources)
	{
		accumulateBuffer(pair.second.vertexBuffer, usage.sceneBufferBytes);
		accumulateBuffer(pair.second.indexBuffer, usage.sceneBufferBytes);
		accumulateAs(pair.second.accelerationStructure, usage.accelerationStructureBytes);
	}
	return usage;
}

NRIPersistentVoxelStatusSnapshot NRIPersistentVoxelResidency::BuildStatusSnapshot() const
{
	NRIPersistentVoxelStatusSnapshot snapshot = {};
	FillResourceStatusSnapshot(snapshot);
	FillBatchStatusSnapshot(snapshot);
	return snapshot;
}

void NRIPersistentVoxelResidency::FillResourceStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const
{
	snapshot.meshVariantResourceCount = (uint32_t)meshVariantResources.size();
	snapshot.materialVariantResourceCount = (uint32_t)materialVariantResources.size();
	snapshot.batchActorCount = (uint32_t)batch.actors.size();
	snapshot.instanceRecordCount = (uint32_t)instances.size();
	snapshot.admissionQueueCount = (uint32_t)admissionQueue.size();

	for (const auto& meshPair : meshVariantResources)
	{
		const PersistentVoxelMeshVariantResource& resource = meshPair.second;
		snapshot.residentResourceBytes += resource.residentBytes;
		if (resource.activeActorReferences == 0)
		{
			snapshot.zeroRefMeshResourceCount++;
			snapshot.zeroRefResourceBytes += resource.residentBytes;
		}
	}

	for (const auto& materialPair : materialVariantResources)
	{
		const PersistentVoxelMaterialVariantResource& resource = materialPair.second;
		snapshot.residentResourceBytes += resource.residentBytes;
		if (resource.activeActorReferences == 0)
		{
			snapshot.zeroRefMaterialResourceCount++;
			snapshot.zeroRefResourceBytes += resource.residentBytes;
		}
	}
}

void NRIPersistentVoxelResidency::FillBatchStatusSnapshot(NRIPersistentVoxelStatusSnapshot& snapshot) const
{
	for (const auto& instancePair : instances)
	{
		if (instancePair.second.pending)
		{
			snapshot.pendingInstanceCount++;
		}
	}

	snapshot.instanceMinPrimitiveCount = UINT32_MAX;
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active || actor.primitiveCount == 0)
		{
			continue;
		}
		snapshot.activeInstanceCount++;
		snapshot.instancePrimitiveCount += actor.primitiveCount;
		snapshot.instanceMaterialCount += actor.materialCount;
		snapshot.instanceMinPrimitiveCount = std::min(snapshot.instanceMinPrimitiveCount, actor.primitiveCount);
		snapshot.instanceMaxPrimitiveCount = std::max(snapshot.instanceMaxPrimitiveCount, actor.primitiveCount);
	}

	if (snapshot.activeInstanceCount == 0)
	{
		snapshot.instanceMinPrimitiveCount = 0;
	}
}

void NRIPersistentVoxelResidency::ApplyPressurePolicy(
	const char* phase,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	uint64_t totalTrackedBytes,
	uint64_t adapterLocalBudget,
	bool traceEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	if (meshVariantResources.empty() && materialVariantResources.empty())
	{
		return;
	}

	std::unordered_map<uint64_t, uint32_t> activeMeshReferences;
	std::unordered_map<uint64_t, uint32_t> activeMaterialReferences;
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active)
		{
			continue;
		}
		if (actor.meshResourceKey != 0)
		{
			activeMeshReferences[actor.meshResourceKey]++;
		}
		if (actor.materialKeyHash != 0)
		{
			activeMaterialReferences[actor.materialKeyHash]++;
		}
	}

	std::unordered_set<uint64_t> admissionMeshes;
	std::unordered_set<uint64_t> admissionMaterials;
	uint64_t queuedBytes = 0;
	for (const auto& pair : admissionQueue)
	{
		const PersistentVoxelAdmissionEntry& entry = pair.second;
		if (entry.mapGeneration != residencyMapGeneration ||
			entry.state == PersistentVoxelAdmissionState::Failed)
		{
			continue;
		}
		admissionMeshes.insert(entry.variant.meshKeyHash);
		admissionMaterials.insert(entry.variant.materialKeyHash);
		queuedBytes += entry.estimatedBytes;
	}

	uint64_t voxelResidentBytes = 0;
	uint64_t coldBytes = 0;
	uint32_t coldMeshCount = 0;
	uint32_t coldMaterialCount = 0;
	for (auto& pair : meshVariantResources)
	{
		PersistentVoxelMeshVariantResource& resource = pair.second;
		resource.activeActorReferences = 0;
		auto activeIt = activeMeshReferences.find(pair.first);
		if (activeIt != activeMeshReferences.end())
		{
			resource.activeActorReferences = activeIt->second;
			resource.lastUsedFrame = frameIndex;
			resource.lastUsedMapGeneration = residencyMapGeneration;
			resource.cold = false;
		}
		resource.residentBytes =
			resource.vertexBuffer.memorySize +
			resource.indexBuffer.memorySize +
			resource.accelerationStructure.memorySize;
		voxelResidentBytes += resource.residentBytes;
		if (resource.cold)
		{
			coldMeshCount++;
			coldBytes += resource.residentBytes;
		}
	}
	for (auto& pair : materialVariantResources)
	{
		PersistentVoxelMaterialVariantResource& resource = pair.second;
		resource.activeActorReferences = 0;
		auto activeIt = activeMaterialReferences.find(pair.first);
		if (activeIt != activeMaterialReferences.end())
		{
			resource.activeActorReferences = activeIt->second;
			resource.lastUsedFrame = frameIndex;
			resource.lastUsedMapGeneration = residencyMapGeneration;
			resource.cold = false;
		}
		resource.residentBytes = (uint64_t)resource.materialBridge.materials.size() * (uint64_t)sizeof(nri_scene::MaterialData);
		if (resource.cold)
		{
			coldMaterialCount++;
		}
	}

	const uint64_t maxResidentBytes = settings.residentMaxBytes;
	const uint64_t minHeadroomBytes = settings.residentMinHeadroomBytes;
	const uint32_t maxColdMaps = settings.residentMaxColdMaps;
	uint64_t pressureBytes = 0;
	if (maxResidentBytes != 0 && voxelResidentBytes > maxResidentBytes)
	{
		pressureBytes = std::max<uint64_t>(pressureBytes, voxelResidentBytes - maxResidentBytes);
	}
	if (maxResidentBytes == 0 && adapterLocalBudget > minHeadroomBytes && totalTrackedBytes + minHeadroomBytes > adapterLocalBudget)
	{
		pressureBytes = std::max<uint64_t>(pressureBytes, totalTrackedBytes + minHeadroomBytes - adapterLocalBudget);
	}

	struct MeshEvictionCandidate
	{
		uint64_t key = 0;
		uint64_t bytes = 0;
		uint32_t primitiveCount = 0;
		uint32_t lastMap = 0;
		uint32_t lastFrame = 0;
		uint32_t sourceBits = 0;
		int32_t priority = 0;
		bool force = false;
		bool prefer = false;
		bool coldAge = false;
	};
	std::vector<MeshEvictionCandidate> meshCandidates;
	meshCandidates.reserve(meshVariantResources.size());
	for (const auto& pair : meshVariantResources)
	{
		const PersistentVoxelMeshVariantResource& resource = pair.second;
		if (!resource.cold || resource.activeActorReferences != 0 || admissionMeshes.find(pair.first) != admissionMeshes.end())
		{
			continue;
		}
		const uint32_t ageMaps = residencyMapGeneration >= resource.lastDesiredMapGeneration ?
			residencyMapGeneration - resource.lastDesiredMapGeneration : 0u;
		const bool oldEnough = maxColdMaps != UINT32_MAX && ageMaps > maxColdMaps;
		if (pressureBytes == 0 && !oldEnough)
		{
			continue;
		}
		meshCandidates.push_back({ pair.first, resource.residentBytes, resource.primitiveCount, resource.lastDesiredMapGeneration,
			resource.lastUsedFrame, resource.sourceBits, resource.priority, resource.gpuForce, resource.gpuPrefer, oldEnough });
	}
	std::sort(meshCandidates.begin(), meshCandidates.end(), [](const MeshEvictionCandidate& left, const MeshEvictionCandidate& right)
	{
		if (left.force != right.force)
		{
			return !left.force;
		}
		if (left.prefer != right.prefer)
		{
			return !left.prefer;
		}
		if (left.priority != right.priority)
		{
			return left.priority > right.priority;
		}
		if (left.lastMap != right.lastMap)
		{
			return left.lastMap < right.lastMap;
		}
		if (left.lastFrame != right.lastFrame)
		{
			return left.lastFrame < right.lastFrame;
		}
		return left.bytes > right.bytes;
	});

	uint64_t evictedBytes = 0;
	uint32_t evictedMeshes = 0;
	for (const MeshEvictionCandidate& candidate : meshCandidates)
	{
		if (pressureBytes != 0 && evictedBytes >= pressureBytes && !candidate.coldAge)
		{
			continue;
		}
		auto it = meshVariantResources.find(candidate.key);
		if (it == meshVariantResources.end())
		{
			continue;
		}
		PersistentVoxelMeshVariantResource& resource = it->second;
		const char* reason = candidate.coldAge ? "cold-age" : "pressure";
		if (traceEnabled)
		{
			Printf("NRI PT voxel residency evict: reason=%s phase=%s tex=-1 voxel=-1 mesh_variant=0x%llx bytes=%llu prims=%u last_map=%u last_frame=%u source_bits=0x%x priority=%d force=%u prefer=%u active_refs=%u\n",
				reason,
				phase != nullptr ? phase : "unknown",
				(unsigned long long)candidate.key,
				(unsigned long long)candidate.bytes,
				resource.primitiveCount,
				resource.lastDesiredMapGeneration,
				resource.lastUsedFrame,
				resource.sourceBits,
				resource.priority,
				resource.gpuForce ? 1u : 0u,
				resource.gpuPrefer ? 1u : 0u,
				resource.activeActorReferences);
		}
		services.RetireBuffer(resource.vertexBuffer);
		services.RetireBuffer(resource.indexBuffer);
		services.RetireAccelerationStructure(resource.accelerationStructure);
		for (auto instIt = instances.begin(); instIt != instances.end(); )
		{
			if (instIt->second.meshResourceKey == candidate.key)
			{
				instIt = instances.erase(instIt);
			}
			else
			{
				++instIt;
			}
		}
		evictedBytes += candidate.bytes;
		evictedMeshes++;
		meshVariantResources.erase(it);
		publishedMeshKeys.erase(candidate.key);
	}

	uint32_t evictedMaterials = 0;
	for (auto it = materialVariantResources.begin(); it != materialVariantResources.end(); )
	{
		PersistentVoxelMaterialVariantResource& resource = it->second;
		if (!resource.cold || resource.activeActorReferences != 0 || admissionMaterials.find(it->first) != admissionMaterials.end())
		{
			++it;
			continue;
		}
		const uint32_t ageMaps = residencyMapGeneration >= resource.lastDesiredMapGeneration ?
			residencyMapGeneration - resource.lastDesiredMapGeneration : 0u;
		const bool oldEnough = maxColdMaps != UINT32_MAX && ageMaps > maxColdMaps;
		if (pressureBytes == 0 && !oldEnough)
		{
			++it;
			continue;
		}
		if (traceEnabled)
		{
			Printf("NRI PT voxel residency evict: reason=%s phase=%s tex=-1 voxel=-1 mesh_variant=0x0 mat_variant=0x%llx bytes=%llu last_map=%u last_frame=%u source_bits=0x%x priority=%d force=%u prefer=%u active_refs=%u\n",
				oldEnough ? "cold-age" : "pressure",
				phase != nullptr ? phase : "unknown",
				(unsigned long long)it->first,
				(unsigned long long)resource.residentBytes,
				resource.lastDesiredMapGeneration,
				resource.lastUsedFrame,
				resource.sourceBits,
				resource.priority,
				resource.gpuForce ? 1u : 0u,
				resource.gpuPrefer ? 1u : 0u,
				resource.activeActorReferences);
		}
		publishedMaterialKeys.erase(it->first);
		it = materialVariantResources.erase(it);
		evictedMaterials++;
	}

	if (traceEnabled || evictedMeshes != 0 || evictedMaterials != 0)
	{
		Printf("NRI PT voxel residency pressure: phase=%s tracked=%llu adapter_budget=%llu headroom=%llu voxel_bytes=%llu max_bytes=%llu queued_bytes=%llu cold_mesh=%u cold_material=%u cold_bytes=%llu pressure_bytes=%llu evicted_mesh=%u evicted_material=%u evicted_bytes=%llu action=%s\n",
			phase != nullptr ? phase : "unknown",
			(unsigned long long)totalTrackedBytes,
			(unsigned long long)adapterLocalBudget,
			(unsigned long long)minHeadroomBytes,
			(unsigned long long)voxelResidentBytes,
			(unsigned long long)maxResidentBytes,
			(unsigned long long)queuedBytes,
			coldMeshCount,
			coldMaterialCount,
			(unsigned long long)coldBytes,
			(unsigned long long)pressureBytes,
			evictedMeshes,
			evictedMaterials,
			(unsigned long long)evictedBytes,
			(evictedMeshes != 0 || evictedMaterials != 0) ? "evict" : "none");
	}
}

bool NRIPersistentVoxelResidency::PumpAdmissionQueue(
	const char* phase,
	uint64_t buildSerial,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	uint64_t totalTrackedBytes,
	uint64_t adapterLocalBudget,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelAdmissionServices& admissionServices)
{
	const bool loadingPhase = phase != nullptr && std::strcmp(phase, "loading") == 0;
	const bool traceLevel1 = loadingTraceLevel >= 1 || voxelStatsEnabled;
	const bool traceLevel2 = loadingTraceLevel >= 2 || voxelStatsEnabled;
	SyncMapGeneration(
		buildSerial,
		"pump-map-generation",
		traceLevel1,
		resetServices);
	ApplyPressurePolicy(
		phase,
		frameIndex,
		settings,
		totalTrackedBytes,
		adapterLocalBudget,
		traceLevel1,
		resetServices);
	const uint32_t variantBudget = loadingPhase ?
		settings.admissionLoadVariants :
		settings.admissionRuntimeVariants;
	const uint64_t legacyByteBudget = loadingPhase ?
		settings.admissionLoadBytes :
		settings.admissionRuntimeBytes;
	const uint64_t chunkByteBudget = loadingPhase ?
		settings.admitMaxBytesLoading :
		settings.admitMaxBytesRuntime;
	auto combineNonZeroBudget = [](uint64_t left, uint64_t right) -> uint64_t
	{
		if (left == 0)
		{
			return right;
		}
		if (right == 0)
		{
			return left;
		}
		return std::min(left, right);
	};
	const uint64_t byteBudget = combineNonZeroBudget(legacyByteBudget, chunkByteBudget);
	const int configuredMsBudget = loadingPhase ?
		(int)settings.admitMaxMsLoading :
		(int)settings.admitMaxMsRuntime;
	const double msBudget = loadingPhase ?
		(configuredMsBudget > 0 ? (double)configuredMsBudget : 250.0) :
		(double)configuredMsBudget;
	const uint32_t blasBudgetLimit = loadingPhase ?
		settings.admitMaxBlasLoading :
		settings.admitMaxBlasRuntime;
	uint32_t blasBudgetRemaining = blasBudgetLimit;
	const auto pumpStart = std::chrono::steady_clock::now();
	auto elapsedMs = [&]() -> double
	{
		return PersistentVoxelDurationMs(pumpStart, std::chrono::steady_clock::now());
	};
	auto isUploadState = [](PersistentVoxelAdmissionState state) -> bool
	{
		return state == PersistentVoxelAdmissionState::UploadingVertices ||
			state == PersistentVoxelAdmissionState::UploadingIndices ||
			state == PersistentVoxelAdmissionState::UploadingPrimitives ||
			state == PersistentVoxelAdmissionState::BuildingBlas;
	};

	uint32_t requiredPendingAtStart = 0;
	uint32_t requiredReadyAtStart = 0;
	uint32_t optionalPendingAtStart = 0;
	uint32_t failedAtStart = 0;
	CountAdmissionWork(requiredPendingAtStart, requiredReadyAtStart, optionalPendingAtStart, failedAtStart);
	const bool requiredOnlyPump = loadingPhase && requiredPendingAtStart != 0;

	PersistentVoxelAdmissionStats stats = {};
	std::vector<PersistentVoxelAdmissionEntry*> candidates;
	candidates.reserve(admissionQueue.size());
	for (auto& pair : admissionQueue)
	{
		PersistentVoxelAdmissionEntry& entry = pair.second;
		if (entry.mapGeneration != residencyMapGeneration)
		{
			continue;
		}
		const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(entry.variant.meshKeyHash, entry.variant.materialKeyHash);
		const bool resourcesReady = readiness.ready;
		if (resourcesReady)
		{
			if (entry.state != PersistentVoxelAdmissionState::Ready && entry.runtimeRequested)
			{
				TraceReadiness("skip-ready", phase, &entry, entry.variant.meshKeyHash, entry.variant.materialKeyHash, readiness, traceLevel2);
			}
			if (entry.uploadPrepared)
			{
				DiscardAdmissionEntry(entry, resetServices);
			}
			entry.state = PersistentVoxelAdmissionState::Ready;
			entry.lastReason = "resident";
			stats.ready++;
			continue;
		}
		if (entry.state == PersistentVoxelAdmissionState::Ready)
		{
			TraceReadiness("stale-ready", phase, &entry, entry.variant.meshKeyHash, entry.variant.materialKeyHash, readiness, traceLevel2);
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "stale-ready";
		}
		if (entry.state == PersistentVoxelAdmissionState::Failed)
		{
			stats.failed++;
			continue;
		}
		if (entry.state == PersistentVoxelAdmissionState::Deferred)
		{
			stats.deferred++;
		}
		stats.queued++;
		stats.bytesPending += entry.estimatedBytes;
		if (entry.gpuForce)
		{
			stats.force++;
		}
		if (entry.gpuPrefer)
		{
			stats.prefer++;
		}
		if (entry.runtimeRequested)
		{
			stats.runtime++;
		}
		candidates.push_back(&entry);
	}

	std::sort(candidates.begin(), candidates.end(), [&](const PersistentVoxelAdmissionEntry* left, const PersistentVoxelAdmissionEntry* right)
	{
		const bool leftUploading = isUploadState(left->state);
		const bool rightUploading = isUploadState(right->state);
		if (leftUploading != rightUploading)
		{
			return leftUploading;
		}
		if (!loadingPhase && left->runtimeRequested != right->runtimeRequested)
		{
			return left->runtimeRequested;
		}
		if (left->admissionRank != right->admissionRank)
		{
			return left->admissionRank < right->admissionRank;
		}
		if (left->priority != right->priority)
		{
			return left->priority < right->priority;
		}
		if (left->gpuForce != right->gpuForce)
		{
			return left->gpuForce;
		}
		if (left->gpuPrefer != right->gpuPrefer)
		{
			return left->gpuPrefer;
		}
		if (left->runtimeRequested != right->runtimeRequested)
		{
			return left->runtimeRequested;
		}
		if (left->variant.primitiveCount != right->variant.primitiveCount)
		{
			return left->variant.primitiveCount > right->variant.primitiveCount;
		}
		return left->pairKey < right->pairKey;
	});

	uint32_t admitted = 0;
	uint32_t blasUsed = 0;
	const char* stopReason = "queue-drained";
	for (PersistentVoxelAdmissionEntry* entry : candidates)
	{
		const PersistentVoxelReadinessStatus currentReadiness = GetSharedVariantReadiness(entry->variant.meshKeyHash, entry->variant.materialKeyHash);
		if (currentReadiness.ready)
		{
			if (entry->state != PersistentVoxelAdmissionState::Ready || entry->runtimeRequested)
			{
				TraceReadiness("skip-ready", phase, entry, entry->variant.meshKeyHash, entry->variant.materialKeyHash, currentReadiness, traceLevel2);
			}
			if (entry->uploadPrepared)
			{
				DiscardAdmissionEntry(*entry, resetServices);
			}
			entry->state = PersistentVoxelAdmissionState::Ready;
			entry->lastReason = "resident";
			stats.ready++;
			continue;
		}
		if (entry->state == PersistentVoxelAdmissionState::Ready)
		{
			TraceReadiness("stale-ready", phase, entry, entry->variant.meshKeyHash, entry->variant.materialKeyHash, currentReadiness, traceLevel2);
			entry->state = PersistentVoxelAdmissionState::Pending;
			entry->lastReason = "stale-ready";
		}
		if (requiredOnlyPump && !IsRequiredAdmission(*entry))
		{
			continue;
		}
		if (msBudget > 0.0 && elapsedMs() >= msBudget)
		{
			stopReason = "ms-budget";
			break;
		}
		const bool uploadState = isUploadState(entry->state);
		if (admitted >= variantBudget && !uploadState)
		{
			stats.skippedBudget++;
			entry->state = PersistentVoxelAdmissionState::Deferred;
			entry->lastReason = "variant-budget";
			continue;
		}
		if (byteBudget != 0 && stats.bytesUploaded >= byteBudget)
		{
			stats.skippedBudget++;
			if (!uploadState)
			{
				entry->state = PersistentVoxelAdmissionState::Deferred;
			}
			entry->lastReason = "byte-budget";
			stopReason = "byte-budget";
			break;
		}

		uint64_t uploadBytes = 0;
		bool reusedMesh = false;
		bool reusedMaterial = false;
		bool inProgress = false;
		const char* failureReason = "none";
		const uint64_t remainingByteBudget = byteBudget == 0 ? 0ull : byteBudget - stats.bytesUploaded;
		const uint32_t blasBefore = blasBudgetRemaining;
		const int isolateBlasPrimitiveThreshold = settings.admitIsolateBlasPrimitives;
		const bool isolateBlasBuild =
			loadingPhase &&
			isolateBlasPrimitiveThreshold > 0 &&
			entry->variant.primitiveCount >= (uint32_t)isolateBlasPrimitiveThreshold;
		if (!admissionServices.AdmitVariantResource(*entry, remainingByteBudget, blasBudgetRemaining, uploadBytes, reusedMesh, reusedMaterial, inProgress, isolateBlasBuild, failureReason))
		{
			entry->state = PersistentVoxelAdmissionState::Failed;
			entry->retryCount++;
			entry->lastReason = failureReason != nullptr ? failureReason : "admit-failed";
			stats.failedThisPump++;
			if (traceLevel1)
			{
				Printf("NRI PT voxel admission queue: event=failed source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu reason=%s\n",
					entry->sourceBits,
					entry->priority,
					entry->admissionRank,
					entry->gpuForce ? 1u : 0u,
					entry->gpuPrefer ? 1u : 0u,
					entry->runtimeRequested ? 1u : 0u,
					entry->variant.sourcePicnum,
					entry->variant.resolvedVoxelIndex,
					(unsigned long long)entry->variant.meshKeyHash,
					(unsigned long long)entry->variant.materialKeyHash,
					entry->variant.primitiveCount,
					(unsigned long long)entry->estimatedBytes,
					entry->lastReason);
			}
			break;
		}
		const uint32_t blasBuiltThisEntry = blasBefore - blasBudgetRemaining;
		blasUsed += blasBuiltThisEntry;
		entry->bytesUploaded += uploadBytes;
		stats.bytesUploaded += uploadBytes;
		if (inProgress)
		{
			entry->lastReason = entry->state == PersistentVoxelAdmissionState::BuildingBlas && blasBudgetRemaining == 0 ? "blas-budget" : "uploading";
			stopReason = entry->lastReason;
			if (traceLevel2)
			{
				Printf("NRI PT voxel admission queue: event=in-progress phase=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu upload_bytes=%llu state=%u reason=%s\n",
					phase != nullptr ? phase : "unknown",
					entry->sourceBits,
					entry->priority,
					entry->admissionRank,
					entry->gpuForce ? 1u : 0u,
					entry->gpuPrefer ? 1u : 0u,
					entry->runtimeRequested ? 1u : 0u,
					entry->variant.sourcePicnum,
					entry->variant.resolvedVoxelIndex,
					(unsigned long long)entry->variant.meshKeyHash,
					(unsigned long long)entry->variant.materialKeyHash,
					entry->variant.primitiveCount,
					(unsigned long long)entry->estimatedBytes,
					(unsigned long long)uploadBytes,
					(uint32_t)entry->state,
					entry->lastReason);
			}
			break;
		}

		entry->state = PersistentVoxelAdmissionState::Ready;
		entry->lastReason = reusedMesh && reusedMaterial ? "already-resident" : "admitted";
		stats.uploaded++;
		admitted++;
		if (traceLevel2)
		{
			Printf("NRI PT voxel admission queue: event=ready phase=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu upload_bytes=%llu reused_mesh=%u reused_material=%u\n",
				phase != nullptr ? phase : "unknown",
				entry->sourceBits,
				entry->priority,
				entry->admissionRank,
				entry->gpuForce ? 1u : 0u,
				entry->gpuPrefer ? 1u : 0u,
				entry->runtimeRequested ? 1u : 0u,
				entry->variant.sourcePicnum,
				entry->variant.resolvedVoxelIndex,
				(unsigned long long)entry->variant.meshKeyHash,
				(unsigned long long)entry->variant.materialKeyHash,
				entry->variant.primitiveCount,
				(unsigned long long)entry->estimatedBytes,
				(unsigned long long)uploadBytes,
				reusedMesh ? 1u : 0u,
				reusedMaterial ? 1u : 0u);
		}
		if (loadingPhase && blasBuiltThisEntry != 0)
		{
			if (!admissionServices.SubmitWaitAndRestart("voxel-loading-blas"))
			{
				entry->state = PersistentVoxelAdmissionState::Failed;
				entry->retryCount++;
				entry->lastReason = "blas-submit-wait-failed";
				stats.failedThisPump++;
				stopReason = entry->lastReason;
				break;
			}
			stopReason = "blas-submit-wait";
			break;
		}
	}

	const bool hasQueueActivity = !candidates.empty() || stats.uploaded != 0 || stats.skippedBudget != 0 || stats.failedThisPump != 0;
	if (((loadingTraceLevel >= 1) && (loadingPhase || hasQueueActivity)) ||
		(voxelStatsEnabled && hasQueueActivity))
	{
		uint32_t requiredPending = 0;
		uint32_t requiredReady = 0;
		uint32_t optionalPending = 0;
		uint32_t failed = 0;
		CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
		Printf("NRI PT voxel admission summary: phase=%s queued=%u ready=%u deferred=%u failed=%u uploaded=%u skipped_budget=%u bytes_pending=%llu bytes_uploaded=%llu force=%u prefer=%u runtime=%u required_pending=%u required_ready=%u optional_pending=%u variants_budget=%u bytes_budget=%llu ms_budget=%.3f ms_used=%.3f blas_budget=%u blas_used=%u stop=%s\n",
			phase != nullptr ? phase : "unknown",
			stats.queued,
			stats.ready,
			stats.deferred,
			stats.failed + stats.failedThisPump,
			stats.uploaded,
			stats.skippedBudget,
			(unsigned long long)stats.bytesPending,
			(unsigned long long)stats.bytesUploaded,
			stats.force,
			stats.prefer,
			stats.runtime,
			requiredPending,
			requiredReady,
			optionalPending,
			variantBudget,
			(unsigned long long)byteBudget,
			msBudget,
			elapsedMs(),
			blasBudgetLimit,
			blasUsed,
			stopReason);
	}
	return stats.failedThisPump == 0;
}

bool NRIPersistentVoxelResidency::BuildAccelerationStructures(
	uint32_t frameIndex,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelAccelerationServices& accelerationServices,
	NRIPersistentVoxelAccelerationBuildStats& outStats)
{
	outStats.calls++;
	if (!batch.valid || batch.actors.empty())
	{
		Reset("persistent-voxel-empty-instance-batch", false, voxelStatsEnabled, resetServices);
		return true;
	}

	std::unordered_set<uint64_t> builtMeshKeys;
	builtMeshKeys.reserve(batch.actors.size());
	auto countActiveActorsUsingMeshResource = [&](uint64_t meshResourceKey) -> uint32_t
	{
		uint32_t count = 0;
		for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
		{
			if (actor.active && actor.meshResourceKey == meshResourceKey)
			{
				count++;
			}
		}
		return count;
	};
	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (actor.active)
		{
			outStats.instances++;
		}
	}

	for (const PersistentVoxelBatch::ActorEntry& actor : batch.actors)
	{
		if (!actor.active)
		{
			continue;
		}

		auto meshResourceIt = meshVariantResources.find(actor.meshResourceKey);
		if (meshResourceIt == meshVariantResources.end())
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=skip reason=missing-mesh actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=0 prims=%u vertices=0 indices=%u blas=0 tlas_ready=0 tlas_published=0 ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					actor.primitiveCount,
					actor.indexCount);
			}
			continue;
		}
		PersistentVoxelMeshVariantResource& meshResource = meshResourceIt->second;
		const bool needsBuild =
			meshResource.accelerationStructure.accelerationStructure == nullptr ||
			meshResource.vertexBuffer.buffer == nullptr ||
			meshResource.indexBuffer.buffer == nullptr;
		if (!needsBuild)
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=reuse reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=1 tlas_ready=%u tlas_published=%u ready=1\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					countActiveActorsUsingMeshResource(actor.meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u);
			}
			continue;
		}

		outStats.builds++;
		if (actor.meshKeyHash != 0)
		{
			builtMeshKeys.insert(actor.meshKeyHash);
		}
		if (!accelerationServices.BuildBottomLevel(
			meshResource.vertexBuffer,
			meshResource.indexBuffer,
			meshResource.vertexCount,
			0u,
			meshResource.indexCount,
			meshResource.primitiveCount,
			meshResource.accelerationStructure))
		{
			if (voxelStatsEnabled)
			{
				Printf("PERF pt voxel blas NRI: frame=%u action=failed reason=build actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=0 tlas_ready=%u tlas_published=%u ready=0\n",
					frameIndex,
					(unsigned long long)actor.identityKey,
					(unsigned long long)actor.meshResourceKey,
					(unsigned long long)actor.meshKeyHash,
					countActiveActorsUsingMeshResource(actor.meshResourceKey),
					meshResource.primitiveCount,
					meshResource.vertexCount,
					meshResource.indexCount,
					meshResource.tlasReadyFrame,
					meshResource.tlasPublished ? 1u : 0u);
			}
			return false;
		}

		if (!accelerationServices.BarrierBuildInputs(meshResource.vertexBuffer, meshResource.indexBuffer))
		{
			return false;
		}

		if (!meshResource.tlasPublished && meshResource.tlasReadyFrame == 0)
		{
			meshResource.tlasReadyFrame = frameIndex + 1u;
		}
		if (voxelStatsEnabled)
		{
			Printf("PERF pt voxel blas NRI: frame=%u action=build reason=none actor_key=0x%llx mesh_resource=0x%llx mesh_key=0x%llx ref_count=%u prims=%u vertices=%u indices=%u blas=1 tlas_ready=%u tlas_published=%u ready=1\n",
				frameIndex,
				(unsigned long long)actor.identityKey,
				(unsigned long long)actor.meshResourceKey,
				(unsigned long long)actor.meshKeyHash,
				countActiveActorsUsingMeshResource(actor.meshResourceKey),
				meshResource.primitiveCount,
				meshResource.vertexCount,
				meshResource.indexCount,
				meshResource.tlasReadyFrame,
				meshResource.tlasPublished ? 1u : 0u);
		}
	}

	outStats.uniqueMeshBuilds += (uint32_t)builtMeshKeys.size();
	return true;
}

void NRIPersistentVoxelResidency::Reset(
	const char* reason,
	bool clearSharedResources,
	bool traceReset,
	const NRIPersistentVoxelResetServices& services)
{
	if (traceReset &&
		(!batch.actors.empty() ||
			!instances.empty() ||
			!meshVariantResources.empty() ||
			!materialVariantResources.empty() ||
			vertexBuffer.buffer != nullptr ||
			indexBuffer.buffer != nullptr ||
			primitiveBuffer.buffer != nullptr))
	{
		Printf("NRI PT voxel reset: action=%s reason=%s actors=%u instances=%u mesh_resources=%u material_resources=%u arena_vertex=%u arena_index=%u arena_primitive=%u published_mesh=%u published_material=%u\n",
			clearSharedResources ? "clear-shared" : "clear-instances",
			reason != nullptr ? reason : "unknown",
			(uint32_t)batch.actors.size(),
			(uint32_t)instances.size(),
			(uint32_t)meshVariantResources.size(),
			(uint32_t)materialVariantResources.size(),
			vertexBuffer.buffer != nullptr ? 1u : 0u,
			indexBuffer.buffer != nullptr ? 1u : 0u,
			primitiveBuffer.buffer != nullptr ? 1u : 0u,
			(uint32_t)publishedMeshKeys.size(),
			(uint32_t)publishedMaterialKeys.size());
	}

	batch = {};
	instances.clear();
	actorRejectedSignatures.clear();
	services.InvalidateSceneDataDescriptors();
	if (!clearSharedResources)
	{
		return;
	}

	services.RetireBuffer(vertexBuffer);
	services.RetireBuffer(indexBuffer);
	services.RetireBuffer(primitiveBuffer);
	services.RetireBuffer(materialBuffer);
	arenaVertexCursor = 0;
	arenaIndexCursor = 0;
	arenaPrimitiveCursor = 0;
	arenaMaterialCursor = 0;
	for (auto& pair : meshVariantResources)
	{
		services.RetireBuffer(pair.second.vertexBuffer);
		services.RetireBuffer(pair.second.indexBuffer);
		services.RetireAccelerationStructure(pair.second.accelerationStructure);
	}
	meshVariantResources.clear();
	materialVariantResources.clear();
	publishedMeshKeys.clear();
	publishedMaterialKeys.clear();
}

bool NRIPersistentVoxelResidency::SyncMapGeneration(
	uint64_t buildSerial,
	const char* reason,
	bool traceEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	if (residencyLastBuildSerial == buildSerial)
	{
		return false;
	}

	residencyLastBuildSerial = buildSerial;
	residencyMapGeneration++;

	if (!admissionQueue.empty())
	{
		if (traceEnabled)
		{
			Printf("NRI PT voxel admission queue: event=clear-stale reason=%s generation=%u entries=%u\n",
				reason != nullptr ? reason : "map-generation",
				residencyMapGeneration,
				(uint32_t)admissionQueue.size());
		}
		for (auto& pair : admissionQueue)
		{
			DiscardAdmissionEntry(pair.second, services);
		}
		admissionQueue.clear();
	}

	return true;
}

void NRIPersistentVoxelResidency::ReconcileResidency(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
	uint64_t buildSerial,
	const char* levelName,
	uint32_t frameIndex,
	int loadingTraceLevel,
	const NRIPersistentVoxelResetServices& services)
{
	SyncMapGeneration(
		buildSerial,
		"reconcile-map-generation",
		loadingTraceLevel >= 1,
		services);
	const uint32_t generation = residencyMapGeneration;

	struct DesiredVoxelResidency
	{
		uint64_t pairKey = 0;
		uint64_t meshKey = 0;
		uint64_t materialKey = 0;
		uint32_t sourceBits = 0;
		int32_t priority = 0;
		int32_t sourcePicnum = -1;
		int32_t resolvedVoxelIndex = -1;
		uint32_t primitiveCount = 0;
		uint64_t uploadBytes = 0;
		bool fromPreload = false;
		bool fromActor = false;
		bool gpuForce = false;
		bool gpuPrefer = false;
		bool cpuReady = false;
	};

	std::unordered_map<uint64_t, DesiredVoxelResidency> desired;
	desired.reserve(variants.size() + cacheEntries.size());
	std::unordered_set<uint64_t> desiredMeshes;
	std::unordered_set<uint64_t> desiredMaterials;
	desiredMeshes.reserve(variants.size() + cacheEntries.size());
	desiredMaterials.reserve(variants.size() + cacheEntries.size());

	auto estimateUploadBytes = [](const nri_scene::SurfaceRef* surface, uint32_t primitiveCount) -> uint64_t
	{
		if (surface == nullptr)
		{
			return 0;
		}
		return (uint64_t)surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
			(uint64_t)surface->indices.size() * (uint64_t)sizeof(uint32_t) +
			(uint64_t)primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	};

	auto addDesired = [&](uint64_t meshKey, uint64_t materialKey, uint32_t sourceBits, int32_t priority, int32_t sourcePicnum, int32_t resolvedVoxelIndex, uint32_t primitiveCount, uint64_t uploadBytes, bool fromPreload, bool fromActor, bool gpuForce, bool gpuPrefer, bool cpuReady)
	{
		if (meshKey == 0 || materialKey == 0)
		{
			return;
		}
		const uint64_t pairKey = nri_scene::HashCombine64(meshKey, materialKey);
		DesiredVoxelResidency& entry = desired[pairKey];
		if (entry.pairKey == 0)
		{
			entry.pairKey = pairKey;
			entry.meshKey = meshKey;
			entry.materialKey = materialKey;
			entry.sourcePicnum = sourcePicnum;
			entry.resolvedVoxelIndex = resolvedVoxelIndex;
			entry.primitiveCount = primitiveCount;
			entry.uploadBytes = uploadBytes;
			entry.priority = priority;
		}
		entry.sourceBits |= sourceBits;
		entry.fromPreload = entry.fromPreload || fromPreload;
		entry.fromActor = entry.fromActor || fromActor;
		entry.gpuForce = entry.gpuForce || gpuForce;
		entry.gpuPrefer = entry.gpuPrefer || gpuPrefer;
		entry.cpuReady = entry.cpuReady || cpuReady;
		if (entry.primitiveCount == 0 && primitiveCount != 0)
		{
			entry.primitiveCount = primitiveCount;
		}
		if (entry.uploadBytes == 0 && uploadBytes != 0)
		{
			entry.uploadBytes = uploadBytes;
		}
		desiredMeshes.insert(meshKey);
		desiredMaterials.insert(materialKey);
	};

	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		addDesired(
			variant.meshKeyHash,
			variant.materialKeyHash,
			variant.sourceBits,
			variant.priority,
			variant.sourcePicnum,
			variant.resolvedVoxelIndex,
			variant.primitiveCount,
			estimateUploadBytes(variant.surface, variant.primitiveCount),
			true,
			false,
			variant.gpuForce,
			variant.gpuPrefer,
			variant.surface != nullptr && variant.primitiveCount != 0);
	}

	for (const nri_scene::PersistentVoxelCacheEntryView& cacheEntry : cacheEntries)
	{
		addDesired(
			cacheEntry.meshKeyHash,
			cacheEntry.materialKeyHash,
			0,
			0,
			cacheEntry.sourcePicnum,
			cacheEntry.resolvedVoxelIndex,
			cacheEntry.primitiveCount,
			estimateUploadBytes(cacheEntry.surface, cacheEntry.primitiveCount),
			false,
			true,
			false,
			false,
			cacheEntry.surface != nullptr && cacheEntry.primitiveCount != 0);
	}

	for (auto it = admissionQueue.begin(); it != admissionQueue.end(); )
	{
		if (it->second.mapGeneration != generation && desired.find(it->first) == desired.end())
		{
			DiscardAdmissionEntry(it->second, services);
			it = admissionQueue.erase(it);
			continue;
		}
		++it;
	}

	uint32_t desiredPreload = 0;
	uint32_t desiredActors = 0;
	uint32_t cpuReady = 0;
	uint32_t gpuReady = 0;
	uint32_t queued = 0;
	uint32_t retained = 0;
	uint32_t forceCount = 0;
	uint32_t preferCount = 0;
	uint32_t meshReadyCount = 0;
	uint32_t materialReadyCount = 0;
	uint32_t blasReadyCount = 0;
	uint32_t materialOnlyCount = 0;
	uint32_t blasOnlyCount = 0;
	uint32_t meshMissingCount = 0;
	uint64_t queuedUploadBytes = 0;

	auto meshReady = [&](uint64_t meshKey, const PersistentVoxelMeshVariantResource** outResource = nullptr) -> bool
	{
		auto it = meshVariantResources.find(meshKey);
		if (it == meshVariantResources.end())
		{
			return false;
		}
		const PersistentVoxelMeshVariantResource& resource = it->second;
		if (outResource != nullptr)
		{
			*outResource = &resource;
		}
		return resource.resourceKey == meshKey &&
			resource.vertexCount != 0 &&
			resource.indexCount != 0 &&
			resource.primitiveCount != 0 &&
			resource.vertexBuffer.buffer != nullptr &&
			resource.indexBuffer.buffer != nullptr &&
			vertexBuffer.buffer != nullptr &&
			indexBuffer.buffer != nullptr &&
			primitiveBuffer.buffer != nullptr;
	};

	auto materialReady = [&](uint64_t materialKey) -> bool
	{
		auto it = materialVariantResources.find(materialKey);
		return it != materialVariantResources.end() &&
			it->second.materialKeyHash == materialKey &&
			it->second.materialCount != 0 &&
			!it->second.materialBridge.materials.empty();
	};

	auto blasReady = [&](uint64_t meshKey) -> bool
	{
		auto it = meshVariantResources.find(meshKey);
		return it != meshVariantResources.end() &&
			it->second.accelerationStructure.accelerationStructure != nullptr;
	};

	for (const auto& desiredPair : desired)
	{
		const DesiredVoxelResidency& entry = desiredPair.second;
		if (entry.fromPreload)
		{
			desiredPreload++;
		}
		if (entry.fromActor)
		{
			desiredActors++;
		}
		if (entry.gpuForce)
		{
			forceCount++;
		}
		if (entry.gpuPrefer)
		{
			preferCount++;
		}
		if (entry.cpuReady)
		{
			cpuReady++;
		}

		const bool hasMesh = meshReady(entry.meshKey);
		const bool hasMaterial = materialReady(entry.materialKey);
		const bool hasBlas = blasReady(entry.meshKey);
		if (hasMesh)
		{
			meshReadyCount++;
		}
		if (hasMaterial)
		{
			materialReadyCount++;
		}
		if (hasBlas)
		{
			blasReadyCount++;
		}

		auto meshIt = meshVariantResources.find(entry.meshKey);
		if (meshIt != meshVariantResources.end())
		{
			meshIt->second.lastDesiredMapGeneration = generation;
			meshIt->second.lastUsedMapGeneration = generation;
			meshIt->second.lastUsedFrame = frameIndex;
			meshIt->second.sourceBits |= entry.sourceBits;
			meshIt->second.priority = entry.priority;
			meshIt->second.gpuForce = meshIt->second.gpuForce || entry.gpuForce;
			meshIt->second.gpuPrefer = meshIt->second.gpuPrefer || entry.gpuPrefer;
			meshIt->second.cold = false;
		}
		auto materialIt = materialVariantResources.find(entry.materialKey);
		if (materialIt != materialVariantResources.end())
		{
			materialIt->second.lastDesiredMapGeneration = generation;
			materialIt->second.lastUsedMapGeneration = generation;
			materialIt->second.lastUsedFrame = frameIndex;
			materialIt->second.sourceBits |= entry.sourceBits;
			materialIt->second.priority = entry.priority;
			materialIt->second.gpuForce = materialIt->second.gpuForce || entry.gpuForce;
			materialIt->second.gpuPrefer = materialIt->second.gpuPrefer || entry.gpuPrefer;
			materialIt->second.cold = false;
		}

		const bool ready = hasMesh && hasMaterial && hasBlas;
		if (ready)
		{
			gpuReady++;
			retained++;
		}
		else if (entry.cpuReady)
		{
			queued++;
			queuedUploadBytes += entry.uploadBytes;
			if (!hasMesh)
			{
				meshMissingCount++;
			}
			else if (!hasMaterial)
			{
				materialOnlyCount++;
			}
			else if (!hasBlas)
			{
				blasOnlyCount++;
			}
		}

		if (loadingTraceLevel >= 2)
		{
			const char* source =
				entry.fromPreload && entry.fromActor ? "preload|actor" :
				(entry.fromPreload ? "preload" : "actor");
			const char* action = ready ? "ready" : (entry.cpuReady ? "queue" : "missing-cpu");
			const char* reason =
				ready ? "resident" :
				(!entry.cpuReady ? "cpu-missing" :
				(!hasMesh ? "mesh-missing" :
				(!hasMaterial ? "material-missing" :
				(!hasBlas ? "blas-missing" : "unknown"))));
			Printf("NRI PT voxel residency entry: action=%s reason=%s source=%s source_bits=0x%x priority=%d force=%u prefer=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu mesh_ready=%u material_ready=%u blas_ready=%u generation=%u\n",
				action,
				reason,
				source,
				entry.sourceBits,
				entry.priority,
				entry.gpuForce ? 1u : 0u,
				entry.gpuPrefer ? 1u : 0u,
				entry.sourcePicnum,
				entry.resolvedVoxelIndex,
				(unsigned long long)entry.meshKey,
				(unsigned long long)entry.materialKey,
				entry.primitiveCount,
				(unsigned long long)entry.uploadBytes,
				hasMesh ? 1u : 0u,
				hasMaterial ? 1u : 0u,
				hasBlas ? 1u : 0u,
				generation);
		}
	}

	uint32_t coldMeshes = 0;
	uint32_t coldMaterials = 0;
	uint64_t coldPrimitiveCount = 0;
	for (auto& pair : meshVariantResources)
	{
		PersistentVoxelMeshVariantResource& resource = pair.second;
		if (resource.resourceKey == 0 || desiredMeshes.find(pair.first) != desiredMeshes.end())
		{
			continue;
		}
		resource.cold = true;
		coldMeshes++;
		coldPrimitiveCount += resource.primitiveCount;
		if (loadingTraceLevel >= 2)
		{
			Printf("NRI PT voxel residency entry: action=cold reason=map-not-desired source=mesh source_bits=0x0 priority=0 force=0 prefer=0 tex=-1 voxel=-1 mesh_variant=0x%llx mat_variant=0x0 prims=%u bytes=0 mesh_ready=%u material_ready=0 blas_ready=%u generation=%u last_desired=%u\n",
				(unsigned long long)pair.first,
				resource.primitiveCount,
				meshReady(pair.first) ? 1u : 0u,
				blasReady(pair.first) ? 1u : 0u,
				generation,
				resource.lastDesiredMapGeneration);
		}
	}
	for (auto& pair : materialVariantResources)
	{
		PersistentVoxelMaterialVariantResource& resource = pair.second;
		if (resource.materialKeyHash == 0 || desiredMaterials.find(pair.first) != desiredMaterials.end())
		{
			continue;
		}
		resource.cold = true;
		coldMaterials++;
		if (loadingTraceLevel >= 2)
		{
			Printf("NRI PT voxel residency entry: action=cold reason=map-not-desired source=material source_bits=0x0 priority=0 force=0 prefer=0 tex=-1 voxel=-1 mesh_variant=0x0 mat_variant=0x%llx prims=0 bytes=0 mesh_ready=0 material_ready=%u blas_ready=0 generation=%u last_desired=%u\n",
				(unsigned long long)pair.first,
				materialReady(pair.first) ? 1u : 0u,
				generation,
				resource.lastDesiredMapGeneration);
		}
	}

	if (loadingTraceLevel >= 1)
	{
		Printf("NRI PT voxel residency reconcile: level=%s build_serial=%llu generation=%u desired=%u desired_preload=%u desired_actor=%u cpu_ready=%u gpu_ready=%u retained=%u queued=%u queue_bytes=%llu mesh_ready=%u material_ready=%u blas_ready=%u mesh_missing=%u material_only=%u blas_only=%u cold_mesh=%u cold_material=%u cold_prims=%llu evicted=0 forced=%u preferred=%u mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
			levelName != nullptr ? levelName : "(none)",
			(unsigned long long)buildSerial,
			generation,
			(uint32_t)desired.size(),
			desiredPreload,
			desiredActors,
			cpuReady,
			gpuReady,
			retained,
			queued,
			(unsigned long long)queuedUploadBytes,
			meshReadyCount,
			materialReadyCount,
			blasReadyCount,
			meshMissingCount,
			materialOnlyCount,
			blasOnlyCount,
			coldMeshes,
			coldMaterials,
			(unsigned long long)coldPrimitiveCount,
			forceCount,
			preferCount,
			(uint32_t)meshVariantResources.size(),
			(uint32_t)materialVariantResources.size(),
			(uint32_t)batch.actors.size(),
			batch.activeActorCount,
			batch.primitiveCount);
	}
}

void NRIPersistentVoxelResidency::DiscardAdmissionEntry(PersistentVoxelAdmissionEntry& entry, const NRIPersistentVoxelResetServices& services)
{
	services.RetireBuffer(entry.uploadMeshResource.vertexBuffer);
	services.RetireBuffer(entry.uploadMeshResource.indexBuffer);
	services.RetireAccelerationStructure(entry.uploadMeshResource.accelerationStructure);
	entry.uploadMeshResource = {};
	entry.uploadMaterialResource = {};
	entry.uploadPrepared = false;
	entry.shaderVertexOffset = 0;
	entry.shaderIndexOffset = 0;
	entry.shaderPrimitiveOffset = 0;
	entry.savedVertexCursor = 0;
	entry.savedIndexCursor = 0;
	entry.savedPrimitiveCursor = 0;
	entry.savedMaterialCursor = 0;
	entry.vertexBytesUploaded = 0;
	entry.vertexArenaBytesUploaded = 0;
	entry.indexBytesUploaded = 0;
	entry.indexArenaBytesUploaded = 0;
	entry.primitiveBytesUploaded = 0;
	entry.bytesUploaded = 0;
	entry.uploadGeometry = {};
	entry.uploadGpuIndices.clear();
	entry.uploadGpuPrimitives.clear();
}

bool NRIPersistentVoxelResidency::EnqueueAdmission(
	const nri_scene::PrecachedVoxelVariantView& variant,
	bool runtimeRequested,
	const char* sourceLabel,
	uint64_t buildSerial,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& services)
{
	if (variant.surface == nullptr || variant.meshKeyHash == 0 || variant.materialKeyHash == 0)
	{
		return false;
	}

	SyncMapGeneration(
		buildSerial,
		"admission-map-generation",
		loadingTraceLevel >= 1 || voxelStatsEnabled,
		services);

	const uint64_t pairKey = nri_scene::HashCombine64(variant.meshKeyHash, variant.materialKeyHash);
	const uint64_t estimatedBytes =
		(uint64_t)variant.surface->vertices.size() * (uint64_t)sizeof(nri_scene::SceneVertex) +
		(uint64_t)variant.surface->indices.size() * (uint64_t)sizeof(uint32_t) +
		(uint64_t)variant.primitiveCount * (uint64_t)sizeof(nri_scene::PrimitiveData);
	const int32_t variantAdmissionRank =
		variant.admissionRank != 0 || variant.priority <= 0 ? variant.admissionRank : variant.priority * 10000 + 9900;
	const uint32_t maxBlasPrimitives = settings.admitMaxBlasPrimitives;
	auto traceAdmissionSkip = [&](const nri_scene::PrecachedVoxelVariantView& skippedVariant, uint64_t skippedBytes, const char* reason)
	{
		if (loadingTraceLevel >= 1 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission queue: event=skip source=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u max_prims=%u bytes=%llu reason=%s generation=%u\n",
				sourceLabel != nullptr ? sourceLabel : "unknown",
				skippedVariant.sourceBits,
				skippedVariant.priority,
				variantAdmissionRank,
				skippedVariant.gpuForce ? 1u : 0u,
				skippedVariant.gpuPrefer ? 1u : 0u,
				runtimeRequested ? 1u : 0u,
				skippedVariant.sourcePicnum,
				skippedVariant.resolvedVoxelIndex,
				(unsigned long long)skippedVariant.meshKeyHash,
				(unsigned long long)skippedVariant.materialKeyHash,
				skippedVariant.primitiveCount,
				maxBlasPrimitives,
				(unsigned long long)skippedBytes,
				reason != nullptr ? reason : "unknown",
				residencyMapGeneration);
		}
	};

	auto found = admissionQueue.find(pairKey);
	if (found != admissionQueue.end() && found->second.mapGeneration != residencyMapGeneration)
	{
		DiscardAdmissionEntry(found->second, services);
		admissionQueue.erase(found);
		found = admissionQueue.end();
	}
	if (found != admissionQueue.end())
	{
		PersistentVoxelAdmissionEntry& entry = found->second;
		const int32_t oldPriority = entry.priority;
		const bool oldForce = entry.gpuForce;
		const bool wasReady = entry.state == PersistentVoxelAdmissionState::Ready;
		const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(entry.variant.meshKeyHash, entry.variant.materialKeyHash);
		const bool resourcesReady = readiness.ready;
		if (!resourcesReady && entry.variant.primitiveCount > maxBlasPrimitives)
		{
			traceAdmissionSkip(entry.variant, entry.estimatedBytes, "blas-primitive-budget");
			DiscardAdmissionEntry(entry, services);
			admissionQueue.erase(found);
			return false;
		}
		entry.sourceBits |= variant.sourceBits;
		entry.priority = std::min(entry.priority, variant.priority);
		entry.admissionRank = std::min(entry.admissionRank, variantAdmissionRank);
		entry.gpuForce = entry.gpuForce || variant.gpuForce;
		entry.gpuPrefer = entry.gpuPrefer || variant.gpuPrefer;
		entry.runtimeRequested = entry.runtimeRequested || runtimeRequested;
		entry.variant.sourceBits = entry.sourceBits;
		entry.variant.priority = entry.priority;
		entry.variant.admissionRank = entry.admissionRank;
		entry.variant.gpuForce = entry.gpuForce;
		entry.variant.gpuPrefer = entry.gpuPrefer;
		if (entry.state == PersistentVoxelAdmissionState::Deferred &&
			(entry.priority != oldPriority || (!oldForce && entry.gpuForce)))
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "reprioritized";
		}
		else if (entry.state == PersistentVoxelAdmissionState::Failed &&
			(entry.priority != oldPriority || (!oldForce && entry.gpuForce)))
		{
			entry.state = PersistentVoxelAdmissionState::Pending;
			entry.lastReason = "retry-priority";
		}
		if (loadingTraceLevel >= 2 || voxelStatsEnabled)
		{
			Printf("NRI PT voxel admission queue: event=%s source=%s source_bits=0x%x priority=%d old_priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu generation=%u\n",
				resourcesReady && runtimeRequested ? "dedupe-ready" : (wasReady && !resourcesReady ? "stale-ready" : (entry.priority != oldPriority ? "promote" : "dedupe")),
				sourceLabel != nullptr ? sourceLabel : "unknown",
				entry.sourceBits,
				entry.priority,
				oldPriority,
				entry.admissionRank,
				entry.gpuForce ? 1u : 0u,
				entry.gpuPrefer ? 1u : 0u,
				entry.runtimeRequested ? 1u : 0u,
				entry.variant.sourcePicnum,
				entry.variant.resolvedVoxelIndex,
				(unsigned long long)entry.variant.meshKeyHash,
				(unsigned long long)entry.variant.materialKeyHash,
				entry.variant.primitiveCount,
				(unsigned long long)entry.estimatedBytes,
				entry.mapGeneration);
		}
		return true;
	}

	const PersistentVoxelReadinessStatus readiness = GetSharedVariantReadiness(variant.meshKeyHash, variant.materialKeyHash);
	const bool resourcesReady = readiness.ready;
	if (!resourcesReady && variant.primitiveCount > maxBlasPrimitives)
	{
		traceAdmissionSkip(variant, estimatedBytes, "blas-primitive-budget");
		return false;
	}

	PersistentVoxelAdmissionEntry entry = {};
	entry.pairKey = pairKey;
	entry.variant = variant;
	entry.state = resourcesReady ?
		PersistentVoxelAdmissionState::Ready :
		PersistentVoxelAdmissionState::Pending;
	entry.sourceBits = variant.sourceBits;
	entry.priority = variant.priority;
	entry.admissionRank = variantAdmissionRank;
	entry.gpuForce = variant.gpuForce;
	entry.gpuPrefer = variant.gpuPrefer;
	entry.runtimeRequested = runtimeRequested;
	entry.mapGeneration = residencyMapGeneration;
	entry.estimatedBytes = estimatedBytes;
	entry.lastReason = entry.state == PersistentVoxelAdmissionState::Ready ? "resident" : "queued";
	admissionQueue[pairKey] = entry;
	if (resourcesReady && runtimeRequested)
	{
		TraceReadiness(
			"dedupe-ready",
			sourceLabel,
			&admissionQueue[pairKey],
			variant.meshKeyHash,
			variant.materialKeyHash,
			readiness,
			loadingTraceLevel >= 2 || voxelStatsEnabled);
	}

	if (loadingTraceLevel >= 2 || voxelStatsEnabled)
	{
		Printf("NRI PT voxel admission queue: event=%s source=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx prims=%u bytes=%llu generation=%u\n",
			entry.state == PersistentVoxelAdmissionState::Ready && runtimeRequested ? "dedupe-ready" : (entry.state == PersistentVoxelAdmissionState::Ready ? "ready" : "enqueue"),
			sourceLabel != nullptr ? sourceLabel : "unknown",
			entry.sourceBits,
			entry.priority,
			entry.admissionRank,
			entry.gpuForce ? 1u : 0u,
			entry.gpuPrefer ? 1u : 0u,
			entry.runtimeRequested ? 1u : 0u,
			entry.variant.sourcePicnum,
			entry.variant.resolvedVoxelIndex,
			(unsigned long long)entry.variant.meshKeyHash,
			(unsigned long long)entry.variant.materialKeyHash,
			entry.variant.primitiveCount,
			(unsigned long long)entry.estimatedBytes,
			entry.mapGeneration);
	}
	return true;
}

bool NRIPersistentVoxelResidency::PreloadVariantResources(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	uint64_t buildSerial,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelPreloadServices& preloadServices)
{
	preloadPending = false;
	if (variants.empty())
	{
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=variant-skip reason=no-shared-variants variants=0 mesh_resources=%u material_resources=%u prims=0\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size());
		}
		return true;
	}

	for (const nri_scene::PrecachedVoxelVariantView& variant : variants)
	{
		EnqueueAdmission(
			variant,
			false,
			"preload",
			buildSerial,
			settings,
			loadingTraceLevel,
			voxelStatsEnabled,
			resetServices);
	}

	auto hasRequiredUploadInProgress = [&]() -> bool
	{
		for (const auto& pair : admissionQueue)
		{
			const PersistentVoxelAdmissionEntry& entry = pair.second;
			if (!IsRequiredAdmission(entry))
			{
				continue;
			}
			if (entry.state == PersistentVoxelAdmissionState::UploadingVertices ||
				entry.state == PersistentVoxelAdmissionState::UploadingIndices ||
				entry.state == PersistentVoxelAdmissionState::UploadingPrimitives ||
				entry.state == PersistentVoxelAdmissionState::BuildingBlas)
			{
				return true;
			}
		}
		return false;
	};

	bool ok = true;
	const auto preloadAdmissionStart = std::chrono::steady_clock::now();
	const int configuredLoadingMsBudget = (int)settings.admitMaxMsLoading;
	const double preloadTickBudgetMs = configuredLoadingMsBudget > 0 ? (double)configuredLoadingMsBudget : 250.0;
	const uint32_t maxPumps = std::max<uint32_t>(1024u, (uint32_t)admissionQueue.size() * 64u + 64u);
	for (uint32_t pump = 0; pump < maxPumps; ++pump)
	{
		uint32_t requiredPendingBefore = 0;
		uint32_t requiredReadyBefore = 0;
		uint32_t optionalPendingBefore = 0;
		uint32_t failedBefore = 0;
		CountAdmissionWork(requiredPendingBefore, requiredReadyBefore, optionalPendingBefore, failedBefore);
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT voxel admission pump: phase=loading pass=%u required_pending=%u required_ready=%u optional_pending=%u failed=%u stop=%s\n",
				pump,
				requiredPendingBefore,
				requiredReadyBefore,
				optionalPendingBefore,
				failedBefore,
				requiredPendingBefore == 0 ? "required-drained" : "none");
		}
		if (requiredPendingBefore == 0)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=ready reason=required-drained required_pending=0 required_ready=%u optional_pending=%u failed=%u\n",
					requiredReadyBefore,
					optionalPendingBefore,
					failedBefore);
			}
			break;
		}

		ok = preloadServices.PumpAdmissionQueue("loading") && ok;

		uint32_t requiredPendingAfter = 0;
		uint32_t requiredReadyAfter = 0;
		uint32_t optionalPendingAfter = 0;
		uint32_t failedAfter = 0;
		CountAdmissionWork(requiredPendingAfter, requiredReadyAfter, optionalPendingAfter, failedAfter);
		if (!ok)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=continue reason=failure required_pending=%u required_ready=%u optional_pending=%u failed=%u\n",
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			break;
		}
		if (requiredPendingAfter == 0)
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=ready reason=required-drained required_pending=0 required_ready=%u optional_pending=%u failed=%u\n",
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			break;
		}
		const double preloadTickMs = PersistentVoxelDurationMs(preloadAdmissionStart, std::chrono::steady_clock::now());
		if (preloadTickBudgetMs > 0.0 && preloadTickMs >= preloadTickBudgetMs)
		{
			preloadPending = true;
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=wait reason=tick-budget pass=%u required_pending=%u required_ready=%u optional_pending=%u failed=%u ms_budget=%.3f ms_used=%.3f\n",
					pump,
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter,
					preloadTickBudgetMs,
					preloadTickMs);
			}
			return false;
		}
		if (requiredPendingAfter >= requiredPendingBefore && requiredReadyAfter <= requiredReadyBefore && !hasRequiredUploadInProgress())
		{
			if (loadingTraceLevel >= 1)
			{
				Printf("NRI PT loading gate: event=voxel-admission result=continue reason=no-progress required_pending=%u required_ready=%u optional_pending=%u failed=%u\n",
					requiredPendingAfter,
					requiredReadyAfter,
					optionalPendingAfter,
					failedAfter);
			}
			break;
		}
		preloadPending = true;
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading gate: event=voxel-admission result=wait reason=pump-budget required_pending=%u optional_pending=%u required_ready=%u failed=%u\n",
				requiredPendingAfter,
				optionalPendingAfter,
				requiredReadyAfter,
				failedAfter);
		}
		return false;
	}
	return ok;
}

bool NRIPersistentVoxelResidency::PreloadResources(
	const std::vector<nri_scene::PrecachedVoxelVariantView>& variants,
	const std::vector<nri_scene::PersistentVoxelCacheEntryView>& cacheEntries,
	bool hasCacheEntries,
	bool gpuLoadingEnabled,
	uint64_t buildSerial,
	const char* levelName,
	uint32_t frameIndex,
	const NRIPersistentVoxelSettings& settings,
	int loadingTraceLevel,
	bool voxelStatsEnabled,
	const NRIPersistentVoxelResetServices& resetServices,
	const NRIPersistentVoxelPreloadServices& preloadServices)
{
	if (!gpuLoadingEnabled)
	{
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=gpu-disabled mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount);
		}
		return true;
	}

	ReconcileResidency(
		variants,
		cacheEntries,
		buildSerial,
		levelName,
		frameIndex,
		loadingTraceLevel,
		resetServices);

	if (!PreloadVariantResources(
		variants,
		buildSerial,
		settings,
		loadingTraceLevel,
		voxelStatsEnabled,
		resetServices,
		preloadServices))
	{
		if (preloadPending)
		{
			if (loadingTraceLevel >= 1)
			{
				uint32_t requiredPending = 0;
				uint32_t requiredReady = 0;
				uint32_t optionalPending = 0;
				uint32_t failed = 0;
				CountAdmissionWork(requiredPending, requiredReady, optionalPending, failed);
				Printf("NRI PT loading voxel resources: event=wait reason=variant-admission-pending required_pending=%u required_ready=%u optional_pending=%u failed=%u mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
					requiredPending,
					requiredReady,
					optionalPending,
					failed,
					(uint32_t)meshVariantResources.size(),
					(uint32_t)materialVariantResources.size(),
					(uint32_t)batch.actors.size(),
					batch.activeActorCount,
					batch.primitiveCount);
			}
			return false;
		}
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=variant-preload-disabled mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount);
		}
		return true;
	}

	if (!hasCacheEntries)
	{
		if (loadingTraceLevel >= 1)
		{
			Printf("NRI PT loading voxel resources: event=skip reason=no-durable-entries entries=0 mesh_resources=%u material_resources=%u actors=%u active=%u prims=%u\n",
				(uint32_t)meshVariantResources.size(),
				(uint32_t)materialVariantResources.size(),
				(uint32_t)batch.actors.size(),
				batch.activeActorCount,
				batch.primitiveCount);
		}
		return true;
	}

	const uint32_t meshResourcesBefore = (uint32_t)meshVariantResources.size();
	const uint32_t materialResourcesBefore = (uint32_t)materialVariantResources.size();
	const uint32_t actorsBefore = (uint32_t)batch.actors.size();
	const uint32_t activeActorsBefore = batch.activeActorCount;
	const uint32_t primitivesBefore = batch.primitiveCount;
	const auto start = std::chrono::steady_clock::now();

	struct LoadingWarmupScope
	{
		bool& active;
		explicit LoadingWarmupScope(bool& value) : active(value) { active = true; }
		~LoadingWarmupScope() { active = false; }
	} loadingWarmupScope(loadingWarmupActive);

	const bool ready = preloadServices.EnsureBatch();
	const auto end = std::chrono::steady_clock::now();

	if (loadingTraceLevel >= 1)
	{
		Printf("NRI PT loading voxel resources: event=%s entries=%u mesh_resources=%u mesh_delta=%d material_resources=%u material_delta=%d actors=%u actor_delta=%d active=%u active_delta=%d prims=%u prim_delta=%d ms=%.3f\n",
			ready ? "admit" : "defer",
			(uint32_t)cacheEntries.size(),
			(uint32_t)meshVariantResources.size(),
			(int32_t)meshVariantResources.size() - (int32_t)meshResourcesBefore,
			(uint32_t)materialVariantResources.size(),
			(int32_t)materialVariantResources.size() - (int32_t)materialResourcesBefore,
			(uint32_t)batch.actors.size(),
			(int32_t)batch.actors.size() - (int32_t)actorsBefore,
			batch.activeActorCount,
			(int32_t)batch.activeActorCount - (int32_t)activeActorsBefore,
			batch.primitiveCount,
			(int32_t)batch.primitiveCount - (int32_t)primitivesBefore,
			PersistentVoxelDurationMs(start, end));
	}
	return true;
}

PersistentVoxelReadinessStatus NRIPersistentVoxelResidency::GetSharedVariantReadiness(uint64_t meshResourceKey, uint64_t materialKeyHash) const
{
	PersistentVoxelReadinessStatus status = {};
	status.meshPublished = publishedMeshKeys.find(meshResourceKey) != publishedMeshKeys.end();
	status.materialPublished = publishedMaterialKeys.find(materialKeyHash) != publishedMaterialKeys.end();

	auto meshIt = meshVariantResources.find(meshResourceKey);
	if (meshIt == meshVariantResources.end())
	{
		status.reason = "mesh-missing";
		return status;
	}
	const PersistentVoxelMeshVariantResource& meshResource = meshIt->second;
	status.meshPresent = true;
	status.meshResourceKey = meshResource.resourceKey;
	status.meshKeyMatches = meshResource.resourceKey == meshResourceKey;
	status.meshVertexCount = meshResource.vertexCount;
	status.meshIndexCount = meshResource.indexCount;
	status.meshPrimitiveCount = meshResource.primitiveCount;
	status.meshCountsValid =
		meshResource.vertexCount != 0 &&
		meshResource.indexCount != 0 &&
		meshResource.primitiveCount != 0;
	status.meshPrivateBuffersReady =
		meshResource.vertexBuffer.buffer != nullptr &&
		meshResource.indexBuffer.buffer != nullptr;
	status.meshArenaBuffersReady =
		vertexBuffer.buffer != nullptr &&
		indexBuffer.buffer != nullptr &&
		primitiveBuffer.buffer != nullptr;
	status.blasReady = meshResource.accelerationStructure.accelerationStructure != nullptr;
	if (!status.meshKeyMatches || !status.meshCountsValid || !status.meshPrivateBuffersReady)
	{
		status.reason = "mesh-invalid";
		return status;
	}
	if (!status.meshArenaBuffersReady)
	{
		status.reason = "arena-missing";
		return status;
	}
	if (!status.blasReady)
	{
		status.reason = "blas-missing";
		return status;
	}

	auto materialIt = materialVariantResources.find(materialKeyHash);
	if (materialIt == materialVariantResources.end())
	{
		status.reason = "material-missing";
		return status;
	}
	const PersistentVoxelMaterialVariantResource& materialResource = materialIt->second;
	status.materialPresent = true;
	status.materialKeyMatches = materialResource.materialKeyHash == materialKeyHash;
	status.materialCount = materialResource.materialCount;
	status.materialBridgeCount = (uint32_t)materialResource.materialBridge.materials.size();
	status.materialCountValid = materialResource.materialCount != 0;
	status.materialBridgeReady = !materialResource.materialBridge.materials.empty();
	if (!status.materialKeyMatches || !status.materialCountValid || !status.materialBridgeReady)
	{
		status.reason = "material-invalid";
		return status;
	}

	status.reason = "ready";
	status.ready = true;
	return status;
}

bool NRIPersistentVoxelResidency::IsSharedVariantReady(uint64_t meshResourceKey, uint64_t materialKeyHash) const
{
	return GetSharedVariantReadiness(meshResourceKey, materialKeyHash).ready;
}

bool NRIPersistentVoxelResidency::IsRequiredAdmission(const PersistentVoxelAdmissionEntry& entry) const
{
	return
		entry.mapGeneration == residencyMapGeneration &&
		!entry.runtimeRequested &&
		entry.priority <= 0 &&
		(entry.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedVoxelPreload) != 0 &&
		(entry.gpuForce ||
			(entry.gpuPrefer && (entry.sourceBits & nri_scene::PrecachedVoxelSourceBit_MountedPreloadMap) != 0));
}

void NRIPersistentVoxelResidency::CountAdmissionWork(uint32_t& requiredPending, uint32_t& requiredReady, uint32_t& optionalPending, uint32_t& failed) const
{
	requiredPending = 0;
	requiredReady = 0;
	optionalPending = 0;
	failed = 0;

	for (const auto& pair : admissionQueue)
	{
		const PersistentVoxelAdmissionEntry& entry = pair.second;
		if (entry.mapGeneration != residencyMapGeneration)
		{
			continue;
		}

		const bool required = IsRequiredAdmission(entry);
		const bool ready = IsSharedVariantReady(entry.variant.meshKeyHash, entry.variant.materialKeyHash);
		if (ready)
		{
			if (required)
			{
				requiredReady++;
			}
			continue;
		}
		if (entry.state == PersistentVoxelAdmissionState::Failed)
		{
			failed++;
			continue;
		}
		if (required)
		{
			requiredPending++;
		}
		else
		{
			optionalPending++;
		}
	}
}

void NRIPersistentVoxelResidency::TraceReadiness(
	const char* event,
	const char* phase,
	const PersistentVoxelAdmissionEntry* entry,
	uint64_t meshResourceKey,
	uint64_t materialKeyHash,
	const PersistentVoxelReadinessStatus& status,
	bool traceEnabled) const
{
	if (!traceEnabled)
	{
		return;
	}
	auto stateName = [](PersistentVoxelAdmissionState state) -> const char*
	{
		switch (state)
		{
		case PersistentVoxelAdmissionState::Pending: return "pending";
		case PersistentVoxelAdmissionState::UploadingVertices: return "uploading-vertices";
		case PersistentVoxelAdmissionState::UploadingIndices: return "uploading-indices";
		case PersistentVoxelAdmissionState::UploadingPrimitives: return "uploading-primitives";
		case PersistentVoxelAdmissionState::BuildingBlas: return "building-blas";
		case PersistentVoxelAdmissionState::Ready: return "ready";
		case PersistentVoxelAdmissionState::Deferred: return "deferred";
		case PersistentVoxelAdmissionState::Failed: return "failed";
		default: return "unknown";
		}
	};

	Printf("NRI PT voxel readiness: event=%s phase=%s reason=%s source_bits=0x%x priority=%d rank=%d force=%u prefer=%u runtime=%u tex=%d voxel=%d mesh_variant=0x%llx mat_variant=0x%llx queue_state=%s published_mesh=%u published_material=%u generation=%u mesh_present=%u mesh_resource=0x%llx mesh_key_match=%u mesh_counts=%u mesh_private=%u arena=%u blas=%u material_present=%u material_key_match=%u material_count=%u material_bridge=%u vertices=%u indices=%u prims=%u material_count_value=%u material_bridge_count=%u\n",
		event != nullptr ? event : "unknown",
		phase != nullptr ? phase : "unknown",
		status.reason != nullptr ? status.reason : "unknown",
		entry != nullptr ? entry->sourceBits : 0u,
		entry != nullptr ? entry->priority : 0,
		entry != nullptr ? entry->admissionRank : 0,
		entry != nullptr && entry->gpuForce ? 1u : 0u,
		entry != nullptr && entry->gpuPrefer ? 1u : 0u,
		entry != nullptr && entry->runtimeRequested ? 1u : 0u,
		entry != nullptr ? entry->variant.sourcePicnum : -1,
		entry != nullptr ? entry->variant.resolvedVoxelIndex : -1,
		(unsigned long long)meshResourceKey,
		(unsigned long long)materialKeyHash,
		entry != nullptr ? stateName(entry->state) : "none",
		status.meshPublished ? 1u : 0u,
		status.materialPublished ? 1u : 0u,
		residencyMapGeneration,
		status.meshPresent ? 1u : 0u,
		(unsigned long long)status.meshResourceKey,
		status.meshKeyMatches ? 1u : 0u,
		status.meshCountsValid ? 1u : 0u,
		status.meshPrivateBuffersReady ? 1u : 0u,
		status.meshArenaBuffersReady ? 1u : 0u,
		status.blasReady ? 1u : 0u,
		status.materialPresent ? 1u : 0u,
		status.materialKeyMatches ? 1u : 0u,
		status.materialCountValid ? 1u : 0u,
		status.materialBridgeReady ? 1u : 0u,
		status.meshVertexCount,
		status.meshIndexCount,
		status.meshPrimitiveCount,
		status.materialCount,
		status.materialBridgeCount);
}
