#pragma once

#include "nri_geometry_bridge.h"

#include <cstdint>
#include <vector>

namespace nri_scene
{
struct MapDeformerGeometrySlice
{
	uint32_t vertexOffset = 0;
	uint32_t vertexCount = 0;
	uint32_t indexOffset = 0;
	uint32_t indexCount = 0;
	uint32_t primitiveOffset = 0;
	uint32_t primitiveCount = 0;
	uint32_t materialOffset = 0;
	uint32_t materialCount = 0;
};

enum MapDeformerLayoutReject : uint32_t
{
	MapDeformerLayoutReject_None = 0,
	MapDeformerLayoutReject_InvalidSlice = 1u << 0,
	MapDeformerLayoutReject_CountMismatch = 1u << 1,
	MapDeformerLayoutReject_IndexLayout = 1u << 2,
	MapDeformerLayoutReject_Provenance = 1u << 3,
	MapDeformerLayoutReject_MaterialSlot = 1u << 4,
	MapDeformerLayoutReject_Flags = 1u << 5,
	MapDeformerLayoutReject_Portal = 1u << 6,
	MapDeformerLayoutReject_Reserved = 1u << 7,
	MapDeformerLayoutReject_SmoothNormals = 1u << 8,
	MapDeformerLayoutReject_NonFiniteKey = 1u << 9,
	MapDeformerLayoutReject_NonFinitePayload = 1u << 10,
	MapDeformerLayoutReject_AmbiguousKey = 1u << 11,
	MapDeformerLayoutReject_UnmatchedTriangle = 1u << 12,
	MapDeformerLayoutReject_DuplicateTriangle = 1u << 13,
	// Live callers may impose a tighter bounded-work limit than the general
	// mapper. This is a fail-closed exact-path result, not a layout mismatch.
	MapDeformerLayoutReject_WorkBudget = 1u << 14,
};

struct MapDeformerChangedSpan
{
	uint32_t sourceElementOffset = 0;
	uint32_t destinationElementOffset = 0;
	uint32_t elementCount = 0;
	uint64_t destinationByteOffset = 0;
	uint64_t byteCount = 0;
};

struct MapDeformerLayoutMapping
{
	bool compatible = false;
	uint32_t rejectMask = MapDeformerLayoutReject_None;
	GeometryData canonicalCurrent;
	std::vector<MapDeformerChangedSpan> changedVertexSpans;
	std::vector<MapDeformerChangedSpan> changedPrimitiveSpans;
	uint64_t changedVertexBytes = 0;
	uint64_t changedPrimitiveBytes = 0;
	uint64_t changedBytes = 0;
};

// Produces chunk-local geometry in the retained primitive, corner, and vertex
// layout. Indices and material indices are normalized from the retained atlas
// slice; changed spans name both the chunk-local source and atlas destination.
MapDeformerLayoutMapping MapCurrentGeometryToRetainedDeformerLayout(
	const GeometryData& retainedGeometry,
	const MapDeformerGeometrySlice& retainedSlice,
	const GeometryData& exactCurrentGeometry);
}
