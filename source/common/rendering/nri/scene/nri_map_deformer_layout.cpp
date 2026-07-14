#include "nri_map_deformer_layout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	using namespace nri_scene;

	struct HorizontalCornerKey
	{
		uint32_t x = 0;
		uint32_t z = 0;

		bool operator==(const HorizontalCornerKey& other) const
		{
			return x == other.x && z == other.z;
		}

		bool operator<(const HorizontalCornerKey& other) const
		{
			return x < other.x || (x == other.x && z < other.z);
		}
	};

	struct HorizontalUvCornerKey
	{
		HorizontalCornerKey horizontal;
		uint32_t u = 0;
		uint32_t v = 0;

		bool operator==(const HorizontalUvCornerKey& other) const
		{
			return horizontal == other.horizontal && u == other.u && v == other.v;
		}

		bool operator<(const HorizontalUvCornerKey& other) const
		{
			if (horizontal < other.horizontal)
				return true;
			if (other.horizontal < horizontal)
				return false;
			return u < other.u || (u == other.u && v < other.v);
		}
	};

	struct TriangleKey
	{
		std::array<HorizontalCornerKey, 3> corners = {};
	};

	struct TriangleUvKey
	{
		std::array<HorizontalUvCornerKey, 3> corners = {};
	};

	uint32_t FloatKey(float value)
	{
		if (value == 0.0f)
			return 0;
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	bool SameFloat(float a, float b)
	{
		return FloatKey(a) == FloatKey(b);
	}

	bool ProvenanceEquals(const SurfaceProvenance& a, const SurfaceProvenance& b)
	{
		if (a.sourceType != b.sourceType ||
			a.sectorIndex != b.sectorIndex ||
			a.wallIndex != b.wallIndex ||
			a.sectionIndex != b.sectionIndex ||
			a.mapChunkIndex != b.mapChunkIndex ||
			a.nextSectorIndex != b.nextSectorIndex ||
			a.actorIndex != b.actorIndex ||
			a.drawListType != b.drawListType ||
			a.cstat != b.cstat ||
			a.materialFlags != b.materialFlags ||
			a.actorOverlayRuleCount != b.actorOverlayRuleCount)
		{
			return false;
		}
		for (uint32_t i = 0; i < MaxActorOverlayRuleIdsPerSurface; ++i)
		{
			if (a.actorOverlayRuleIds[i] != b.actorOverlayRuleIds[i])
				return false;
		}
		return true;
	}

	bool TriangleKeyEquals(const TriangleKey& a, const TriangleKey& b)
	{
		return a.corners == b.corners;
	}

	bool TriangleUvKeyEquals(const TriangleUvKey& a, const TriangleUvKey& b)
	{
		return a.corners == b.corners;
	}

	bool IsFiniteVertexPayload(const SceneVertex& vertex)
	{
		for (float value : vertex.position)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : vertex.prevPosition)
		{
			if (!std::isfinite(value))
				return false;
		}
		return std::isfinite(vertex.uv[0]) && std::isfinite(vertex.uv[1]);
	}

	bool IsFinitePrimitivePayload(const PrimitiveData& primitive)
	{
		for (float value : primitive.uv0)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.uv1)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.uv2)
		{
			if (!std::isfinite(value))
				return false;
		}
		for (float value : primitive.normal)
		{
			if (!std::isfinite(value))
				return false;
		}
		return true;
	}

	bool RangeFits(uint32_t offset, uint32_t count, size_t size)
	{
		return (uint64_t)offset + (uint64_t)count <= (uint64_t)size;
	}

	const float* PrimitiveUv(const PrimitiveData& primitive, uint32_t corner)
	{
		switch (corner)
		{
		case 0: return primitive.uv0;
		case 1: return primitive.uv1;
		default: return primitive.uv2;
		}
	}

	float* PrimitiveUv(PrimitiveData& primitive, uint32_t corner)
	{
		switch (corner)
		{
		case 0: return primitive.uv0;
		case 1: return primitive.uv1;
		default: return primitive.uv2;
		}
	}

	bool BuildTriangleKey(
		const GeometryData& geometry,
		const PrimitiveData& primitive,
		TriangleKey& outKey,
		TriangleUvKey& outUvKey)
	{
		for (uint32_t corner = 0; corner < 3; ++corner)
		{
			const uint32_t vertexIndex = primitive.indices[corner];
			if (vertexIndex >= geometry.vertices.size())
				return false;
			const SceneVertex& vertex = geometry.vertices[vertexIndex];
			if (!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[2]))
				return false;
			const float* primitiveUv = PrimitiveUv(primitive, corner);
			if (!std::isfinite(primitiveUv[0]) || !std::isfinite(primitiveUv[1]))
				return false;
			outKey.corners[corner] = { FloatKey(vertex.position[0]), FloatKey(vertex.position[2]) };
			outUvKey.corners[corner] =
			{
				outKey.corners[corner],
				FloatKey(primitiveUv[0]),
				FloatKey(primitiveUv[1])
			};
		}
		std::sort(outKey.corners.begin(), outKey.corners.end());
		std::sort(outUvKey.corners.begin(), outUvKey.corners.end());
		return true;
	}

	bool ValidatePrimitiveIndexLayout(
		const GeometryData& geometry,
		uint32_t vertexOffset,
		uint32_t vertexCount,
		uint32_t indexOffset,
		uint32_t primitiveOffset,
		uint32_t primitiveCount)
	{
		const uint64_t vertexEnd = (uint64_t)vertexOffset + vertexCount;
		for (uint32_t i = 0; i < primitiveCount; ++i)
		{
			const PrimitiveData& primitive = geometry.primitives[primitiveOffset + i];
			for (uint32_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t index = geometry.indices[indexOffset + i * 3u + corner];
				if (primitive.indices[corner] != index ||
					index < vertexOffset || (uint64_t)index >= vertexEnd)
				{
					return false;
				}
			}
		}
		return true;
	}

	GeometryData BuildNormalizedRetainedGeometry(
		const GeometryData& retainedGeometry,
		const MapDeformerGeometrySlice& slice)
	{
		GeometryData result;
		result.vertices.assign(
			retainedGeometry.vertices.begin() + slice.vertexOffset,
			retainedGeometry.vertices.begin() + slice.vertexOffset + slice.vertexCount);
		result.indices.reserve(slice.indexCount);
		for (uint32_t i = 0; i < slice.indexCount; ++i)
		{
			result.indices.push_back(retainedGeometry.indices[slice.indexOffset + i] - slice.vertexOffset);
		}
		result.primitives.reserve(slice.primitiveCount);
		result.primitiveProvenance.reserve(slice.primitiveCount);
		for (uint32_t i = 0; i < slice.primitiveCount; ++i)
		{
			PrimitiveData primitive = retainedGeometry.primitives[slice.primitiveOffset + i];
			for (uint32_t corner = 0; corner < 3; ++corner)
				primitive.indices[corner] -= slice.vertexOffset;
			primitive.materialIndex -= slice.materialOffset;
			result.primitives.push_back(primitive);
			result.primitiveProvenance.push_back(retainedGeometry.primitiveProvenance[slice.primitiveOffset + i]);
		}
		return result;
	}

	template<typename T>
	std::vector<MapDeformerChangedSpan> BuildChangedSpans(
		const std::vector<T>& retained,
		const std::vector<T>& current,
		uint32_t destinationElementOffset,
		uint64_t& outBytes)
	{
		std::vector<MapDeformerChangedSpan> spans;
		outBytes = 0;
		size_t begin = current.size();
		for (size_t i = 0; i <= current.size(); ++i)
		{
			const bool changed =
				i < current.size() && std::memcmp(&retained[i], &current[i], sizeof(T)) != 0;
			if (changed && begin == current.size())
			{
				begin = i;
			}
			else if (!changed && begin != current.size())
			{
				const uint32_t sourceOffset = (uint32_t)begin;
				const uint32_t count = (uint32_t)(i - begin);
				MapDeformerChangedSpan span;
				span.sourceElementOffset = sourceOffset;
				span.destinationElementOffset = destinationElementOffset + sourceOffset;
				span.elementCount = count;
				span.destinationByteOffset = (uint64_t)span.destinationElementOffset * sizeof(T);
				span.byteCount = (uint64_t)count * sizeof(T);
				outBytes += span.byteCount;
				spans.push_back(span);
				begin = current.size();
			}
		}
		return spans;
	}

	void Reject(MapDeformerLayoutMapping& mapping, uint32_t reason)
	{
		mapping.compatible = false;
		mapping.rejectMask |= reason;
	}
}

namespace nri_scene
{
MapDeformerLayoutMapping MapCurrentGeometryToRetainedDeformerLayout(
	const GeometryData& retainedGeometry,
	const MapDeformerGeometrySlice& retainedSlice,
	const GeometryData& exactCurrentGeometry)
{
	MapDeformerLayoutMapping result;
	const bool validRetainedRanges =
		retainedSlice.vertexCount != 0 &&
		retainedSlice.primitiveCount != 0 &&
		retainedSlice.materialCount != 0 &&
		retainedSlice.indexCount == (uint64_t)retainedSlice.primitiveCount * 3u &&
		RangeFits(retainedSlice.vertexOffset, retainedSlice.vertexCount, retainedGeometry.vertices.size()) &&
		RangeFits(retainedSlice.indexOffset, retainedSlice.indexCount, retainedGeometry.indices.size()) &&
		RangeFits(retainedSlice.primitiveOffset, retainedSlice.primitiveCount, retainedGeometry.primitives.size()) &&
		RangeFits(retainedSlice.primitiveOffset, retainedSlice.primitiveCount, retainedGeometry.primitiveProvenance.size());
	if (!validRetainedRanges)
	{
		Reject(result, MapDeformerLayoutReject_InvalidSlice);
		return result;
	}

	if (exactCurrentGeometry.vertices.size() != retainedSlice.vertexCount ||
		exactCurrentGeometry.indices.size() != retainedSlice.indexCount ||
		exactCurrentGeometry.primitives.size() != retainedSlice.primitiveCount ||
		exactCurrentGeometry.primitiveProvenance.size() != retainedSlice.primitiveCount)
	{
		Reject(result, MapDeformerLayoutReject_CountMismatch);
		return result;
	}

	if (!ValidatePrimitiveIndexLayout(
			retainedGeometry,
			retainedSlice.vertexOffset,
			retainedSlice.vertexCount,
			retainedSlice.indexOffset,
			retainedSlice.primitiveOffset,
			retainedSlice.primitiveCount) ||
		!ValidatePrimitiveIndexLayout(
			exactCurrentGeometry,
			0,
			(uint32_t)exactCurrentGeometry.vertices.size(),
			0,
			0,
			(uint32_t)exactCurrentGeometry.primitives.size()))
	{
		Reject(result, MapDeformerLayoutReject_IndexLayout);
		return result;
	}

	GeometryData retained = BuildNormalizedRetainedGeometry(retainedGeometry, retainedSlice);
	std::vector<TriangleKey> retainedKeys(retainedSlice.primitiveCount);
	std::vector<TriangleUvKey> retainedUvKeys(retainedSlice.primitiveCount);
	std::vector<TriangleKey> currentKeys(retainedSlice.primitiveCount);
	std::vector<TriangleUvKey> currentUvKeys(retainedSlice.primitiveCount);

	for (uint32_t i = 0; i < retainedSlice.primitiveCount; ++i)
	{
		if (!BuildTriangleKey(retained, retained.primitives[i], retainedKeys[i], retainedUvKeys[i]) ||
			!BuildTriangleKey(exactCurrentGeometry, exactCurrentGeometry.primitives[i], currentKeys[i], currentUvKeys[i]))
		{
			Reject(result, MapDeformerLayoutReject_NonFiniteKey);
			return result;
		}
	}

	for (uint32_t i = 0; i < retainedSlice.primitiveCount; ++i)
	{
		for (uint32_t j = i + 1; j < retainedSlice.primitiveCount; ++j)
		{
			const bool duplicateRetained =
				ProvenanceEquals(retained.primitiveProvenance[i], retained.primitiveProvenance[j]) &&
				TriangleKeyEquals(retainedKeys[i], retainedKeys[j]) &&
				TriangleUvKeyEquals(retainedUvKeys[i], retainedUvKeys[j]);
			const bool duplicateCurrent =
				ProvenanceEquals(exactCurrentGeometry.primitiveProvenance[i], exactCurrentGeometry.primitiveProvenance[j]) &&
				TriangleKeyEquals(currentKeys[i], currentKeys[j]) &&
				TriangleUvKeyEquals(currentUvKeys[i], currentUvKeys[j]);
			if (duplicateRetained || duplicateCurrent)
			{
				Reject(result, MapDeformerLayoutReject_DuplicateTriangle);
				return result;
			}
		}
	}

	GeometryData canonical = retained;
	std::vector<uint8_t> currentPrimitiveUsed(retainedSlice.primitiveCount, 0);
	std::vector<uint32_t> retainedToCurrentVertex(retainedSlice.vertexCount, UINT32_MAX);
	std::vector<uint32_t> currentToRetainedVertex(retainedSlice.vertexCount, UINT32_MAX);

	for (uint32_t retainedPrimitiveIndex = 0;
		retainedPrimitiveIndex < retainedSlice.primitiveCount;
		++retainedPrimitiveIndex)
	{
		std::vector<uint32_t> horizontalCandidates;
		std::vector<uint32_t> provenanceCandidates;
		for (uint32_t currentPrimitiveIndex = 0;
			currentPrimitiveIndex < retainedSlice.primitiveCount;
			++currentPrimitiveIndex)
		{
			if (currentPrimitiveUsed[currentPrimitiveIndex] != 0 ||
				!TriangleKeyEquals(retainedKeys[retainedPrimitiveIndex], currentKeys[currentPrimitiveIndex]))
			{
				continue;
			}
			horizontalCandidates.push_back(currentPrimitiveIndex);
			if (ProvenanceEquals(
					retained.primitiveProvenance[retainedPrimitiveIndex],
					exactCurrentGeometry.primitiveProvenance[currentPrimitiveIndex]))
			{
				provenanceCandidates.push_back(currentPrimitiveIndex);
			}
		}

		if (provenanceCandidates.empty())
		{
			Reject(result,
				horizontalCandidates.empty() ?
					MapDeformerLayoutReject_UnmatchedTriangle :
					MapDeformerLayoutReject_Provenance);
			return result;
		}

		uint32_t currentPrimitiveIndex = provenanceCandidates[0];
		if (provenanceCandidates.size() > 1)
		{
			uint32_t uvMatch = UINT32_MAX;
			for (uint32_t candidate : provenanceCandidates)
			{
				if (!TriangleUvKeyEquals(retainedUvKeys[retainedPrimitiveIndex], currentUvKeys[candidate]))
					continue;
				if (uvMatch != UINT32_MAX)
				{
					Reject(result, MapDeformerLayoutReject_AmbiguousKey);
					return result;
				}
				uvMatch = candidate;
			}
			if (uvMatch == UINT32_MAX)
			{
				Reject(result, MapDeformerLayoutReject_AmbiguousKey);
				return result;
			}
			currentPrimitiveIndex = uvMatch;
		}

		const PrimitiveData& retainedPrimitive = retained.primitives[retainedPrimitiveIndex];
		const PrimitiveData& currentPrimitive = exactCurrentGeometry.primitives[currentPrimitiveIndex];
		if (retainedPrimitive.materialIndex >= retainedSlice.materialCount ||
			currentPrimitive.materialIndex >= retainedSlice.materialCount ||
			retainedPrimitive.materialIndex != currentPrimitive.materialIndex)
		{
			Reject(result, MapDeformerLayoutReject_MaterialSlot);
			return result;
		}
		if (retainedPrimitive.flags != currentPrimitive.flags)
		{
			Reject(result, MapDeformerLayoutReject_Flags);
			return result;
		}
		if (retainedPrimitive.portalIndex != currentPrimitive.portalIndex)
		{
			Reject(result, MapDeformerLayoutReject_Portal);
			return result;
		}
		// Exact BuildGeometry payloads carry the pre-atlas sentinel. Resident
		// atlas primitives may already contain their resolved visibility chunk.
		if (currentPrimitive.reserved0 != UINT32_MAX &&
			retainedPrimitive.reserved0 != currentPrimitive.reserved0)
		{
			Reject(result, MapDeformerLayoutReject_Reserved);
			return result;
		}
		if (retainedPrimitive.smoothNormals[0] != 0 || retainedPrimitive.smoothNormals[1] != 0 ||
			currentPrimitive.smoothNormals[0] != 0 || currentPrimitive.smoothNormals[1] != 0)
		{
			Reject(result, MapDeformerLayoutReject_SmoothNormals);
			return result;
		}
		if (!IsFinitePrimitivePayload(currentPrimitive))
		{
			Reject(result, MapDeformerLayoutReject_NonFinitePayload);
			return result;
		}

		std::array<uint32_t, 3> currentCornerForRetained = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
		std::array<uint8_t, 3> currentCornerUsed = {};
		for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
		{
			const SceneVertex& retainedVertex = retained.vertices[retainedPrimitive.indices[retainedCorner]];
			std::vector<uint32_t> cornerCandidates;
			for (uint32_t currentCorner = 0; currentCorner < 3; ++currentCorner)
			{
				if (currentCornerUsed[currentCorner] != 0)
					continue;
				const SceneVertex& currentVertex = exactCurrentGeometry.vertices[currentPrimitive.indices[currentCorner]];
				if (SameFloat(retainedVertex.position[0], currentVertex.position[0]) &&
					SameFloat(retainedVertex.position[2], currentVertex.position[2]))
				{
					cornerCandidates.push_back(currentCorner);
				}
			}

			uint32_t selectedCorner = UINT32_MAX;
			if (cornerCandidates.size() == 1)
			{
				selectedCorner = cornerCandidates[0];
			}
			else if (cornerCandidates.size() > 1)
			{
				const float* retainedUv = PrimitiveUv(retainedPrimitive, retainedCorner);
				for (uint32_t candidate : cornerCandidates)
				{
					const float* currentUv = PrimitiveUv(currentPrimitive, candidate);
					if (!SameFloat(retainedUv[0], currentUv[0]) || !SameFloat(retainedUv[1], currentUv[1]))
						continue;
					if (selectedCorner != UINT32_MAX)
					{
						Reject(result, MapDeformerLayoutReject_AmbiguousKey);
						return result;
					}
					selectedCorner = candidate;
				}
			}

			if (selectedCorner == UINT32_MAX)
			{
				Reject(result,
					cornerCandidates.empty() ?
						MapDeformerLayoutReject_IndexLayout :
						MapDeformerLayoutReject_AmbiguousKey);
				return result;
			}
			currentCornerForRetained[retainedCorner] = selectedCorner;
			currentCornerUsed[selectedCorner] = 1;
		}

		PrimitiveData& canonicalPrimitive = canonical.primitives[retainedPrimitiveIndex];
		for (uint32_t retainedCorner = 0; retainedCorner < 3; ++retainedCorner)
		{
			const uint32_t retainedVertexIndex = retainedPrimitive.indices[retainedCorner];
			const uint32_t currentCorner = currentCornerForRetained[retainedCorner];
			const uint32_t currentVertexIndex = currentPrimitive.indices[currentCorner];
			const SceneVertex& retainedVertex = retained.vertices[retainedVertexIndex];
			const SceneVertex& currentVertex = exactCurrentGeometry.vertices[currentVertexIndex];
			if (!IsFiniteVertexPayload(currentVertex))
			{
				Reject(result, MapDeformerLayoutReject_NonFinitePayload);
				return result;
			}
			if (!SameFloat(retainedVertex.prevPosition[0], currentVertex.prevPosition[0]) ||
				!SameFloat(retainedVertex.prevPosition[2], currentVertex.prevPosition[2]))
			{
				Reject(result, MapDeformerLayoutReject_IndexLayout);
				return result;
			}
			if ((retainedToCurrentVertex[retainedVertexIndex] != UINT32_MAX &&
					retainedToCurrentVertex[retainedVertexIndex] != currentVertexIndex) ||
				(currentToRetainedVertex[currentVertexIndex] != UINT32_MAX &&
					currentToRetainedVertex[currentVertexIndex] != retainedVertexIndex))
			{
				Reject(result, MapDeformerLayoutReject_IndexLayout);
				return result;
			}
			retainedToCurrentVertex[retainedVertexIndex] = currentVertexIndex;
			currentToRetainedVertex[currentVertexIndex] = retainedVertexIndex;

			SceneVertex& canonicalVertex = canonical.vertices[retainedVertexIndex];
			canonicalVertex.position[1] = currentVertex.position[1];
			canonicalVertex.prevPosition[1] = currentVertex.prevPosition[1];
			canonicalVertex.uv[0] = currentVertex.uv[0];
			canonicalVertex.uv[1] = currentVertex.uv[1];

			const float* currentUv = PrimitiveUv(currentPrimitive, currentCorner);
			float* canonicalUv = PrimitiveUv(canonicalPrimitive, retainedCorner);
			canonicalUv[0] = currentUv[0];
			canonicalUv[1] = currentUv[1];
		}
		std::copy(std::begin(currentPrimitive.normal), std::end(currentPrimitive.normal), canonicalPrimitive.normal);
		currentPrimitiveUsed[currentPrimitiveIndex] = 1;
	}

	if (std::find(currentPrimitiveUsed.begin(), currentPrimitiveUsed.end(), 0) != currentPrimitiveUsed.end() ||
		std::find(retainedToCurrentVertex.begin(), retainedToCurrentVertex.end(), UINT32_MAX) != retainedToCurrentVertex.end() ||
		std::find(currentToRetainedVertex.begin(), currentToRetainedVertex.end(), UINT32_MAX) != currentToRetainedVertex.end())
	{
		Reject(result, MapDeformerLayoutReject_IndexLayout);
		return result;
	}

	result.changedVertexSpans = BuildChangedSpans(
		retained.vertices,
		canonical.vertices,
		retainedSlice.vertexOffset,
		result.changedVertexBytes);
	result.changedPrimitiveSpans = BuildChangedSpans(
		retained.primitives,
		canonical.primitives,
		retainedSlice.primitiveOffset,
		result.changedPrimitiveBytes);
	result.changedBytes = result.changedVertexBytes + result.changedPrimitiveBytes;
	result.canonicalCurrent = std::move(canonical);
	result.compatible = true;
	return result;
}
}
