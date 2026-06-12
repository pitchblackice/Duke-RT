#pragma once

#include "nri_static_scene.h"

#include <cstdint>

namespace nri_static_scene_geometry
{
int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex);
int32_t ResolveVisibilityChunkIndexForProvenance(const nri_scene::PTMapWorld& mapWorld, const nri_scene::SurfaceProvenance& provenance);
uint32_t NormalizeResidentAtlasIndex(uint32_t value, uint32_t base);

uint64_t HashResidentGeometryPayload(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount);

uint64_t HashResidentGeometryPayloadOrderIndependent(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount);

uint64_t ComputeGeometryTopologySignature(const nri_scene::GeometryData& geometry);
uint64_t ComputePrimitiveLayoutSignature(const nri_scene::GeometryData& geometry);
}
