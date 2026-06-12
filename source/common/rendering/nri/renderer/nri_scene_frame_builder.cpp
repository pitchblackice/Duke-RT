#include "nri_scene_frame_builder.h"

#include "nri_render_geometry_helpers.h"

#include <chrono>
#include <cstring>

namespace
{
	class ScopedSceneFrameTimer
	{
	public:
		explicit ScopedSceneFrameTimer(double* targetMs)
			: mTargetMs(targetMs)
		{
			if (mTargetMs != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSceneFrameTimer()
		{
			if (mTargetMs != nullptr)
			{
				const auto end = std::chrono::steady_clock::now();
				*mTargetMs += std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - mStart).count();
			}
		}

	private:
		double* mTargetMs = nullptr;
		std::chrono::steady_clock::time_point mStart = {};
	};

	uint64_t SceneFrameHashCombine64(uint64_t hash, uint64_t value)
	{
		return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
	}

	uint32_t SceneFrameFloatBits(float value)
	{
		uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashVector3(uint64_t hash, const float* values)
	{
		if (values == nullptr)
		{
			return SceneFrameHashCombine64(hash, 0);
		}

		hash = SceneFrameHashCombine64(hash, (uint64_t)SceneFrameFloatBits(values[0]));
		hash = SceneFrameHashCombine64(hash, (uint64_t)SceneFrameFloatBits(values[1]));
		hash = SceneFrameHashCombine64(hash, (uint64_t)SceneFrameFloatBits(values[2]));
		return hash;
	}

	uint64_t BuildUploadSpanStamp(
		uint64_t sourceStamp,
		NRIRenderer::SceneBufferUploadDomain uploadDomain,
		uint64_t offset,
		uint64_t count,
		uint64_t byteSize)
	{
		if (sourceStamp == 0 || count == 0)
		{
			return 0;
		}

		uint64_t hash = 1469598103934665603ull;
		hash = SceneFrameHashCombine64(hash, sourceStamp);
		hash = SceneFrameHashCombine64(hash, (uint64_t)uploadDomain);
		hash = SceneFrameHashCombine64(hash, offset);
		hash = SceneFrameHashCombine64(hash, count);
		hash = SceneFrameHashCombine64(hash, byteSize);
		return hash != 0 ? hash : 1;
	}
}

NRISceneFrameGenerationResult BuildNRISceneFrameGenerationResult(
	const NRISceneFrameGenerationInputs& inputs,
	const NRIRenderer::StateCommitDomainGenerations& previous,
	bool hasPrevious)
{
	NRISceneFrameGenerationResult result = {};
	result.current.staticMap = inputs.staticMapBuildSerial;
	result.current.runtimeMutation = inputs.runtimeMutationGeneration;
	result.current.dynamicActors = inputs.activeDynamicSceneView != nullptr ?
		SceneFrameHashCombine64(
			SceneFrameHashCombine64(
				SceneFrameHashCombine64(inputs.frameIndex, (uint64_t)inputs.activeDynamicSceneView->opaqueSprites.size()),
				inputs.activeDynamicGeometry != nullptr ? (uint64_t)inputs.activeDynamicGeometry->primitives.size() : 0ull),
			inputs.activeDynamicMaterials != nullptr ? (uint64_t)inputs.activeDynamicMaterials->materials.size() : 0ull) :
		0ull;
	result.current.mirrorPlayer = inputs.hasMirrorPlayerScene ?
		SceneFrameHashCombine64(
			SceneFrameHashCombine64(
				inputs.frameIndex,
				inputs.mirrorPlayerGeometry != nullptr ? (uint64_t)inputs.mirrorPlayerGeometry->primitives.size() : 0ull),
			inputs.mirrorPlayerMaterials != nullptr ? (uint64_t)inputs.mirrorPlayerMaterials->materials.size() : 0ull) :
		0ull;
	result.current.persistentVoxels = inputs.persistentVoxelGeneration;
	result.current.materialBridge = inputs.activeMaterialBridge != nullptr ?
		SceneFrameHashCombine64(
			SceneFrameHashCombine64(
				SceneFrameHashCombine64(1469598103934665603ull, (uint64_t)inputs.activeMaterialBridge->materials.size()),
				(uint64_t)inputs.activeMaterialBridge->lightMetadata.size()),
			inputs.activeGpuMaterials != nullptr ? (uint64_t)inputs.activeGpuMaterials->size() : 0ull) :
		0ull;
	result.current.textures = inputs.activeMaterialBridge != nullptr ?
		SceneFrameHashCombine64(
			SceneFrameHashCombine64(1469598103934665603ull, (uint64_t)inputs.activeMaterialBridge->textures.size()),
			(uint64_t)inputs.sceneTextureCacheCount) :
		0ull;
	result.current.tlasInstances = SceneFrameHashCombine64(
		SceneFrameHashCombine64(
			SceneFrameHashCombine64(
				SceneFrameHashCombine64(
					SceneFrameHashCombine64(
						inputs.selectedSceneHasDynamicOverlay ? inputs.frameIndex : inputs.staticAccelerationBuildSerial,
						(uint64_t)inputs.selectedTlasInstanceCount),
					(uint64_t)inputs.selectedSceneInstanceCount),
				(uint64_t)inputs.selectedStaticSceneInstanceCount),
			(uint64_t)inputs.selectedDynamicSceneInstanceCount),
		(uint64_t)inputs.selectedPersistentVoxelSceneInstanceCount);

	uint64_t sceneConstantsHash = 1469598103934665603ull;
	sceneConstantsHash = SceneFrameHashCombine64(sceneConstantsHash, (uint64_t)inputs.renderWidth);
	sceneConstantsHash = SceneFrameHashCombine64(sceneConstantsHash, (uint64_t)inputs.renderHeight);
	sceneConstantsHash = HashVector3(sceneConstantsHash, inputs.currentCameraPos);
	sceneConstantsHash = HashVector3(sceneConstantsHash, inputs.currentCameraForward);
	sceneConstantsHash = HashVector3(sceneConstantsHash, inputs.currentCameraRight);
	sceneConstantsHash = HashVector3(sceneConstantsHash, inputs.currentCameraUp);
	sceneConstantsHash = SceneFrameHashCombine64(sceneConstantsHash, (uint64_t)SceneFrameFloatBits(inputs.currentTanHalfFovX));
	sceneConstantsHash = SceneFrameHashCombine64(sceneConstantsHash, (uint64_t)SceneFrameFloatBits(inputs.currentTanHalfFovY));
	result.current.sceneConstants = sceneConstantsHash;

	const auto domainChanged = [&](uint64_t current, uint64_t previousValue) -> uint32_t
	{
		return (!hasPrevious || current != previousValue) ? 1u : 0u;
	};
	result.changedStaticMap = domainChanged(result.current.staticMap, previous.staticMap);
	result.changedRuntimeMutation = domainChanged(result.current.runtimeMutation, previous.runtimeMutation);
	result.changedDynamicActors = domainChanged(result.current.dynamicActors, previous.dynamicActors);
	result.changedMirrorPlayer = domainChanged(result.current.mirrorPlayer, previous.mirrorPlayer);
	result.changedPersistentVoxels = domainChanged(result.current.persistentVoxels, previous.persistentVoxels);
	result.changedMaterialBridge = domainChanged(result.current.materialBridge, previous.materialBridge);
	result.changedTextures = domainChanged(result.current.textures, previous.textures);
	result.changedTlasInstances = domainChanged(result.current.tlasInstances, previous.tlasInstances);
	result.changedSceneConstants = domainChanged(result.current.sceneConstants, previous.sceneConstants);
	result.changedDomainCount =
		result.changedStaticMap +
		result.changedRuntimeMutation +
		result.changedDynamicActors +
		result.changedMirrorPlayer +
		result.changedPersistentVoxels +
		result.changedMaterialBridge +
		result.changedTextures +
		result.changedTlasInstances +
		result.changedSceneConstants;
	return result;
}

void WriteNRISceneFrameGenerationTraceStats(
	const NRISceneFrameGenerationResult& result,
	NRIRenderer::PerfShellTraceStats& stats)
{
	stats.sceneSelectStateCommitGenStaticMap = result.current.staticMap;
	stats.sceneSelectStateCommitGenRuntimeMutation = result.current.runtimeMutation;
	stats.sceneSelectStateCommitGenDynamicActors = result.current.dynamicActors;
	stats.sceneSelectStateCommitGenMirrorPlayer = result.current.mirrorPlayer;
	stats.sceneSelectStateCommitGenPersistentVoxels = result.current.persistentVoxels;
	stats.sceneSelectStateCommitGenMaterialBridge = result.current.materialBridge;
	stats.sceneSelectStateCommitGenTextures = result.current.textures;
	stats.sceneSelectStateCommitGenTlasInstances = result.current.tlasInstances;
	stats.sceneSelectStateCommitGenSceneConstants = result.current.sceneConstants;
	stats.sceneSelectStateCommitChangedStaticMap = result.changedStaticMap;
	stats.sceneSelectStateCommitChangedRuntimeMutation = result.changedRuntimeMutation;
	stats.sceneSelectStateCommitChangedDynamicActors = result.changedDynamicActors;
	stats.sceneSelectStateCommitChangedMirrorPlayer = result.changedMirrorPlayer;
	stats.sceneSelectStateCommitChangedPersistentVoxels = result.changedPersistentVoxels;
	stats.sceneSelectStateCommitChangedMaterialBridge = result.changedMaterialBridge;
	stats.sceneSelectStateCommitChangedTextures = result.changedTextures;
	stats.sceneSelectStateCommitChangedTlasInstances = result.changedTlasInstances;
	stats.sceneSelectStateCommitChangedSceneConstants = result.changedSceneConstants;
	stats.sceneSelectStateCommitChangedDomainCount = result.changedDomainCount;
}

void AccumulateNRISceneContributionReserve(const NRISceneContribution& contribution, NRISceneContributionReserve& reserve)
{
	if (contribution.geometry != nullptr)
	{
		reserve.vertices += contribution.geometry->vertices.size();
		reserve.indices += contribution.geometry->indices.size();
		reserve.primitives += contribution.geometry->primitives.size();
		reserve.primitiveProvenance += contribution.geometry->primitiveProvenance.size();
	}
	if (contribution.materials != nullptr)
	{
		reserve.materials += contribution.materials->materials.size();
		reserve.lightMetadata += contribution.materials->lightMetadata.size();
		reserve.textures += contribution.materials->textures.size();
		reserve.paletteLookup += contribution.materials->paletteLookup.size();
	}
}

void ReserveNRISceneContributionCapacity(
	const NRISceneContributionReserve& reserve,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge)
{
	overlayGeometry.vertices.reserve(reserve.vertices);
	overlayGeometry.indices.reserve(reserve.indices);
	overlayGeometry.primitives.reserve(reserve.primitives);
	overlayGeometry.primitiveProvenance.reserve(reserve.primitiveProvenance);
	overlayMaterialBridge.materials.reserve(reserve.materials);
	overlayMaterialBridge.lightMetadata.reserve(reserve.lightMetadata);
	overlayMaterialBridge.textures.reserve(reserve.textures);
	overlayMaterialBridge.paletteLookup.reserve(reserve.paletteLookup);
}

void AppendNRISceneContribution(
	const NRISceneContribution& contribution,
	NRISceneContributionAppendStats stats,
	nri_scene::GeometryData& overlayGeometry,
	nri_scene::MaterialBridgeData& overlayMaterialBridge,
	std::vector<NRIRenderer::SceneBufferUploadDomainSpan>& uploadSpans)
{
	ScopedSceneFrameTimer sourceTimer(stats.totalMs);
	const nri_scene::GeometryData* geometry = contribution.geometry;
	const nri_scene::MaterialBridgeData* materials = contribution.materials;
	if (materials == nullptr)
	{
		return;
	}

	const uint32_t primitiveCount = geometry != nullptr ? (uint32_t)geometry->primitives.size() : 0u;
	const uint32_t materialCount = (uint32_t)materials->materials.size();
	if (stats.primitiveCount != nullptr)
	{
		*stats.primitiveCount = primitiveCount;
	}
	if (stats.materialCount != nullptr)
	{
		*stats.materialCount = materialCount;
	}

	NRIRenderer::PerfShellTraceStats::OverlayAppendSourceTraceEntry localTrace = {};
	NRIRenderer::PerfShellTraceStats::OverlayAppendSourceTraceEntry& sourceTrace =
		stats.sourceTrace != nullptr ? *stats.sourceTrace : localTrace;
	sourceTrace = {};
	sourceTrace.primitiveCount = primitiveCount;
	sourceTrace.materialCount = materialCount;
	sourceTrace.vertexBytes = geometry != nullptr ? (uint64_t)geometry->vertices.size() * sizeof(nri_scene::SceneVertex) : 0;
	sourceTrace.indexBytes = geometry != nullptr ? (uint64_t)geometry->indices.size() * sizeof(uint32_t) : 0;
	sourceTrace.primitiveBytes = geometry != nullptr ?
		(uint64_t)geometry->primitives.size() * sizeof(nri_scene::PrimitiveData) +
		(uint64_t)geometry->primitiveProvenance.size() * sizeof(nri_scene::SurfaceProvenance) :
		0;
	sourceTrace.materialBytes = nri_scene::EstimateMaterialBridgeBytes(*materials);
	sourceTrace.byteCount =
		sourceTrace.vertexBytes +
		sourceTrace.indexBytes +
		sourceTrace.primitiveBytes +
		sourceTrace.materialBytes;

	NRIRenderer::SceneBufferUploadDomainSpan uploadSpan = {};
	uploadSpan.domain = contribution.uploadDomain;
	uploadSpan.vertexOffset = (uint32_t)overlayGeometry.vertices.size();
	uploadSpan.indexOffset = (uint32_t)overlayGeometry.indices.size();
	uploadSpan.primitiveOffset = (uint32_t)overlayGeometry.primitives.size();
	uploadSpan.materialOffset = (uint32_t)overlayMaterialBridge.materials.size();
	if (geometry != nullptr)
	{
		uploadSpan.vertexCount = (uint32_t)geometry->vertices.size();
		uploadSpan.indexCount = (uint32_t)geometry->indices.size();
		uploadSpan.primitiveCount = (uint32_t)geometry->primitives.size();
		sourceTrace.vertexCount = uploadSpan.vertexCount;
		sourceTrace.indexCount = uploadSpan.indexCount;
	}
	uploadSpan.materialCount = materialCount;
	if (contribution.producerStamp != nullptr)
	{
		uploadSpan.stamp.vertexPayloadStamp = BuildUploadSpanStamp(
			contribution.producerStamp->vertexPayloadStamp,
			contribution.uploadDomain,
			uploadSpan.vertexOffset,
			uploadSpan.vertexCount,
			(uint64_t)uploadSpan.vertexCount * sizeof(nri_scene::SceneVertex));
		uploadSpan.stamp.indexPayloadStamp = BuildUploadSpanStamp(
			contribution.producerStamp->indexPayloadStamp,
			contribution.uploadDomain,
			uploadSpan.indexOffset,
			uploadSpan.indexCount,
			(uint64_t)uploadSpan.indexCount * sizeof(uint32_t));
		uploadSpan.stamp.primitivePayloadStamp = BuildUploadSpanStamp(
			contribution.producerStamp->primitivePayloadStamp,
			contribution.uploadDomain,
			((uint64_t)uploadSpan.primitiveOffset << 32) ^ (uint64_t)uploadSpan.materialOffset,
			uploadSpan.primitiveCount,
			(uint64_t)uploadSpan.primitiveCount * sizeof(nri_scene::PrimitiveData));
		uploadSpan.stamp.primitiveProvenanceStamp = BuildUploadSpanStamp(
			contribution.producerStamp->primitiveProvenanceStamp,
			contribution.uploadDomain,
			uploadSpan.primitiveOffset,
			uploadSpan.primitiveCount,
			(uint64_t)uploadSpan.primitiveCount);
		uploadSpan.stamp.materialPayloadStamp = BuildUploadSpanStamp(
			contribution.producerStamp->materialPayloadStamp,
			contribution.uploadDomain,
			uploadSpan.materialOffset,
			uploadSpan.materialCount,
			(uint64_t)uploadSpan.materialCount * sizeof(nri_scene::MaterialData));
	}

	if (geometry != nullptr && !geometry->primitives.empty())
	{
		ScopedSceneFrameTimer geometryTimer(stats.geometryMs);
		sourceTrace.geometryGrowthEvents =
			(overlayGeometry.vertices.size() + geometry->vertices.size() > overlayGeometry.vertices.capacity() ? 1u : 0u) +
			(overlayGeometry.indices.size() + geometry->indices.size() > overlayGeometry.indices.capacity() ? 1u : 0u) +
			(overlayGeometry.primitives.size() + geometry->primitives.size() > overlayGeometry.primitives.capacity() ? 1u : 0u) +
			(overlayGeometry.primitiveProvenance.size() + geometry->primitiveProvenance.size() > overlayGeometry.primitiveProvenance.capacity() ? 1u : 0u);
		AppendGeometry(*geometry, (uint32_t)overlayMaterialBridge.materials.size(), overlayGeometry);
	}
	{
		ScopedSceneFrameTimer materialTimer(stats.materialMs);
		sourceTrace.materialGrowthEvents =
			(overlayMaterialBridge.materials.size() + materials->materials.size() > overlayMaterialBridge.materials.capacity() ? 1u : 0u) +
			(overlayMaterialBridge.lightMetadata.size() + materials->lightMetadata.size() > overlayMaterialBridge.lightMetadata.capacity() ? 1u : 0u) +
			(overlayMaterialBridge.textures.size() + materials->textures.size() > overlayMaterialBridge.textures.capacity() ? 1u : 0u) +
			(overlayMaterialBridge.paletteLookup.size() + materials->paletteLookup.size() > overlayMaterialBridge.paletteLookup.capacity() ? 1u : 0u);
		nri_scene::AppendMaterialBridge(*materials, overlayMaterialBridge);
	}
	if (uploadSpan.vertexCount != 0 ||
		uploadSpan.indexCount != 0 ||
		uploadSpan.primitiveCount != 0 ||
		uploadSpan.materialCount != 0)
	{
		uploadSpans.push_back(uploadSpan);
	}
}
