#include "nri_scene_frame_geometry.h"

#include <algorithm>
#include <chrono>

namespace
{
	class ScopedSceneFrameGeometryTimer
	{
	public:
		explicit ScopedSceneFrameGeometryTimer(double* targetMs)
			: mTargetMs(targetMs)
		{
			if (mTargetMs != nullptr)
			{
				mStart = std::chrono::steady_clock::now();
			}
		}

		~ScopedSceneFrameGeometryTimer()
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

	void ReplaceGeometryOverlayTail(
		const nri_scene::GeometryData& source,
		uint32_t materialIndexOffset,
		uint32_t staticVertexCount,
		uint32_t staticIndexCount,
		uint32_t staticPrimitiveCount,
		uint32_t staticPrimitiveProvenanceCount,
		nri_scene::GeometryData& destination)
	{
		const uint32_t vertexBase = staticVertexCount;

		destination.vertices.resize((size_t)staticVertexCount + source.vertices.size());
		std::copy(source.vertices.begin(), source.vertices.end(), destination.vertices.begin() + staticVertexCount);

		destination.indices.resize((size_t)staticIndexCount + source.indices.size());
		auto indexOut = destination.indices.begin() + staticIndexCount;
		for (uint32_t index : source.indices)
		{
			*indexOut++ = vertexBase + index;
		}

		destination.primitives.resize((size_t)staticPrimitiveCount + source.primitives.size());
		auto primitiveOut = destination.primitives.begin() + staticPrimitiveCount;
		for (const auto& primitive : source.primitives)
		{
			nri_scene::PrimitiveData copy = primitive;
			copy.indices[0] += vertexBase;
			copy.indices[1] += vertexBase;
			copy.indices[2] += vertexBase;
			copy.materialIndex += materialIndexOffset;
			*primitiveOut++ = copy;
		}

		destination.primitiveProvenance.resize((size_t)staticPrimitiveProvenanceCount + source.primitiveProvenance.size());
		std::copy(
			source.primitiveProvenance.begin(),
			source.primitiveProvenance.end(),
			destination.primitiveProvenance.begin() + staticPrimitiveProvenanceCount);
	}
}

NRISceneFrameGeometrySelection NRISceneFrameGeometry::SelectActiveGeometry(const NRISceneFrameGeometrySelectionInputs& inputs)
{
	ScopedSceneFrameGeometryTimer geometryStateTimer(inputs.totalMs);
	NRISceneFrameGeometrySelection selection = {};
	const bool hasOverlay = inputs.overlayGeometry != nullptr && !inputs.overlayGeometry->primitives.empty();
	if (hasOverlay)
	{
		CombinedGeometryCache& cache = mCombinedGeometryCache;
		const uint32_t staticVertexCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->vertices.size() : 0u;
		const uint32_t staticIndexCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->indices.size() : 0u;
		const uint32_t staticPrimitiveCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->primitives.size() : 0u;
		const uint32_t staticPrimitiveProvenanceCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->primitiveProvenance.size() : 0u;
		const uint32_t staticMaterialCount = inputs.staticMaterialBridge != nullptr ? (uint32_t)inputs.staticMaterialBridge->materials.size() : 0u;
		const bool refreshStaticPrefix =
			!cache.staticPrefixValid ||
			cache.staticBuildSerial != inputs.staticBuildSerial ||
			cache.staticVertexCount != staticVertexCount ||
			cache.staticIndexCount != staticIndexCount ||
			cache.staticPrimitiveCount != staticPrimitiveCount ||
			cache.staticPrimitiveProvenanceCount != staticPrimitiveProvenanceCount ||
			cache.staticMaterialCount != staticMaterialCount;
		selection.usedCombinedGeometry = true;
		if (refreshStaticPrefix)
		{
			ScopedSceneFrameGeometryTimer copyTimer(inputs.staticCopyMs);
			cache.geometry = inputs.staticGeometry != nullptr ? *inputs.staticGeometry : nri_scene::GeometryData{};
			cache.staticPrefixValid = true;
			cache.staticBuildSerial = inputs.staticBuildSerial;
			cache.staticVertexCount = staticVertexCount;
			cache.staticIndexCount = staticIndexCount;
			cache.staticPrimitiveCount = staticPrimitiveCount;
			cache.staticPrimitiveProvenanceCount = staticPrimitiveProvenanceCount;
			cache.staticMaterialCount = staticMaterialCount;
		}
		{
			ScopedSceneFrameGeometryTimer appendTimer(inputs.overlayAppendMs);
			ReplaceGeometryOverlayTail(
				*inputs.overlayGeometry,
				inputs.overlayMaterialOffset,
				cache.staticVertexCount,
				cache.staticIndexCount,
				cache.staticPrimitiveCount,
				cache.staticPrimitiveProvenanceCount,
				cache.geometry);
		}
		{
			ScopedSceneFrameGeometryTimer selectTimer(inputs.selectMs);
			selection.staticProbePrimitiveCount = cache.staticPrimitiveCount;
			selection.geometry = &cache.geometry;
			selection.gpuMaterials = inputs.combinedGpuMaterials;
			selection.materialBridge = inputs.combinedMaterialBridge;
			selection.combinedPrimitiveCount = (uint32_t)cache.geometry.primitives.size();
			selection.combinedMaterialCount = inputs.combinedGpuMaterials != nullptr ? (uint32_t)inputs.combinedGpuMaterials->size() : 0u;
		}
	}
	else
	{
		selection.usedStaticOnlyGeometry = true;
		ScopedSceneFrameGeometryTimer selectTimer(inputs.selectMs);
		selection.geometry = inputs.staticGeometry;
		selection.gpuMaterials = inputs.staticGpuMaterials;
		selection.materialBridge = inputs.staticMaterialBridge;
		selection.staticProbePrimitiveCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->primitives.size() : 0u;
		selection.combinedPrimitiveCount = inputs.staticGeometry != nullptr ? (uint32_t)inputs.staticGeometry->primitives.size() : 0u;
		selection.combinedMaterialCount = inputs.staticGpuMaterials != nullptr ? (uint32_t)inputs.staticGpuMaterials->size() : 0u;
	}
	return selection;
}

void NRISceneFrameGeometry::RefreshStaticPrefixForResidentUpdate(const NRISceneFrameGeometryStaticPrefixRefresh& refresh)
{
	CombinedGeometryCache& cache = mCombinedGeometryCache;
	if (!cache.staticPrefixValid)
	{
		return;
	}
	if (cache.staticBuildSerial != refresh.staticBuildSerial)
	{
		cache.staticPrefixValid = false;
		return;
	}
	if (refresh.staticGeometry == nullptr || refresh.changedChunks == nullptr)
	{
		cache.staticPrefixValid = false;
		return;
	}

	nri_scene::GeometryData& destination = cache.geometry;
	const nri_scene::GeometryData& source = *refresh.staticGeometry;
	if (destination.vertices.size() < cache.staticVertexCount ||
		destination.indices.size() < cache.staticIndexCount ||
		destination.primitives.size() < cache.staticPrimitiveCount ||
		destination.primitiveProvenance.size() < cache.staticPrimitiveProvenanceCount)
	{
		cache.staticPrefixValid = false;
		return;
	}

	const uint32_t staticVertexCount = (uint32_t)source.vertices.size();
	const uint32_t staticIndexCount = (uint32_t)source.indices.size();
	const uint32_t staticPrimitiveCount = (uint32_t)source.primitives.size();
	const uint32_t staticPrimitiveProvenanceCount = (uint32_t)source.primitiveProvenance.size();
	const bool prefixSizeChanged =
		cache.staticVertexCount != staticVertexCount ||
		cache.staticIndexCount != staticIndexCount ||
		cache.staticPrimitiveCount != staticPrimitiveCount ||
		cache.staticPrimitiveProvenanceCount != staticPrimitiveProvenanceCount;
	if (prefixSizeChanged)
	{
		destination.vertices.resize(cache.staticVertexCount);
		destination.indices.resize(cache.staticIndexCount);
		destination.primitives.resize(cache.staticPrimitiveCount);
		destination.primitiveProvenance.resize(cache.staticPrimitiveProvenanceCount);
		destination.vertices.resize(staticVertexCount);
		destination.indices.resize(staticIndexCount);
		destination.primitives.resize(staticPrimitiveCount);
		destination.primitiveProvenance.resize(staticPrimitiveProvenanceCount);
		cache.staticVertexCount = staticVertexCount;
		cache.staticIndexCount = staticIndexCount;
		cache.staticPrimitiveCount = staticPrimitiveCount;
		cache.staticPrimitiveProvenanceCount = staticPrimitiveProvenanceCount;
	}
	cache.staticMaterialCount = refresh.staticMaterialCount;

	for (const NRISceneFrameGeometryStaticChunkSlice& chunk : *refresh.changedChunks)
	{
		if (!chunk.active)
		{
			continue;
		}
		if (chunk.vertexOffset > source.vertices.size() ||
			chunk.vertexCount > source.vertices.size() - chunk.vertexOffset ||
			chunk.vertexOffset > destination.vertices.size() ||
			chunk.vertexCount > destination.vertices.size() - chunk.vertexOffset ||
			chunk.indexOffset > source.indices.size() ||
			chunk.indexCount > source.indices.size() - chunk.indexOffset ||
			chunk.indexOffset > destination.indices.size() ||
			chunk.indexCount > destination.indices.size() - chunk.indexOffset ||
			chunk.primitiveOffset > source.primitives.size() ||
			chunk.primitiveCount > source.primitives.size() - chunk.primitiveOffset ||
			chunk.primitiveOffset > destination.primitives.size() ||
			chunk.primitiveCount > destination.primitives.size() - chunk.primitiveOffset)
		{
			cache.staticPrefixValid = false;
			return;
		}

		if (chunk.vertexCount != 0)
		{
			std::copy_n(
				source.vertices.data() + chunk.vertexOffset,
				chunk.vertexCount,
				destination.vertices.data() + chunk.vertexOffset);
		}
		if (chunk.indexCount != 0)
		{
			std::copy_n(
				source.indices.data() + chunk.indexOffset,
				chunk.indexCount,
				destination.indices.data() + chunk.indexOffset);
		}
		if (chunk.primitiveCount != 0)
		{
			std::copy_n(
				source.primitives.data() + chunk.primitiveOffset,
				chunk.primitiveCount,
				destination.primitives.data() + chunk.primitiveOffset);
		}
		if (chunk.primitiveOffset <= source.primitiveProvenance.size() &&
			chunk.primitiveCount <= source.primitiveProvenance.size() - chunk.primitiveOffset &&
			chunk.primitiveOffset <= destination.primitiveProvenance.size() &&
			chunk.primitiveCount <= destination.primitiveProvenance.size() - chunk.primitiveOffset &&
			chunk.primitiveCount != 0)
		{
			std::copy_n(
				source.primitiveProvenance.data() + chunk.primitiveOffset,
				chunk.primitiveCount,
				destination.primitiveProvenance.data() + chunk.primitiveOffset);
		}
	}
}

void NRISceneFrameGeometry::Reset()
{
	mCombinedGeometryCache = {};
}
