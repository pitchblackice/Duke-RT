#include "nri_persistent_voxels.h"

#include "../scene/nri_hash.h"
#include "printf.h"

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

void NRIPersistentVoxelResetServices::ClearBoundCounts() const
{
	if (clearBoundCounts != nullptr)
	{
		clearBoundCounts(user);
	}
}

void NRIPersistentVoxelResetServices::InvalidateSceneDataDescriptors() const
{
	if (invalidateSceneDataDescriptors != nullptr)
	{
		invalidateSceneDataDescriptors(user);
	}
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
	services.ClearBoundCounts();
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
