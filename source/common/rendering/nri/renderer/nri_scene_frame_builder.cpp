#include "nri_scene_frame_builder.h"

#include "nri_render_geometry_helpers.h"

#include <chrono>

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
