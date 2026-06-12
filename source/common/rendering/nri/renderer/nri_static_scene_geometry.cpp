#include "nri_static_scene_geometry.h"

#include "../scene/nri_hash.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nri_static_scene_geometry
{
namespace
{
	uint64_t Combine(uint64_t hash, uint64_t value)
	{
		return nri_scene::HashCombine64(hash, value);
	}

	uint32_t FloatBits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	uint64_t HashResidentGeometryVertexPayload(uint64_t hash, const nri_scene::SceneVertex& vertex)
	{
		for (float component : vertex.position)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : vertex.prevPosition)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : vertex.uv)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		return hash;
	}

	uint64_t HashResidentGeometryProvenancePayload(
		uint64_t hash,
		const nri_scene::PTMapWorld& mapWorld,
		const nri_scene::GeometryData& geometry,
		const nri_scene::PrimitiveData& primitive,
		uint32_t primitiveIndex)
	{
		const bool hasProvenance = primitiveIndex < geometry.primitiveProvenance.size();
		const uint32_t visibilityChunk =
			hasProvenance ?
			(uint32_t)ResolveVisibilityChunkIndexForProvenance(mapWorld, geometry.primitiveProvenance[primitiveIndex]) :
			primitive.reserved0;
		hash = Combine(hash, (uint64_t)visibilityChunk);
		if (hasProvenance)
		{
			const auto& provenance = geometry.primitiveProvenance[primitiveIndex];
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sectorIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.wallIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sectionIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.mapChunkIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.nextSectorIndex);
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.actorIndex);
			hash = Combine(hash, (uint64_t)provenance.drawListType);
			hash = Combine(hash, (uint64_t)provenance.cstat);
			hash = Combine(hash, (uint64_t)provenance.materialFlags);
		}
		else
		{
			hash = Combine(hash, UINT64_MAX);
		}
		return hash;
	}
}

int32_t FindMapChunkIndexForSector(const nri_scene::PTMapWorld& mapWorld, int32_t sectorIndex)
{
	if (!mapWorld.valid || sectorIndex < 0)
	{
		return -1;
	}
	if ((size_t)sectorIndex < mapWorld.sectorChunkLookup.size())
	{
		const uint32_t chunkIndex = mapWorld.sectorChunkLookup[(size_t)sectorIndex];
		if (chunkIndex != UINT32_MAX)
		{
			return (int32_t)chunkIndex;
		}
	}

	for (const auto& chunk : mapWorld.chunks)
	{
		if (chunk.kind == nri_scene::PTMapChunkKind::Sector && chunk.sectorIndex == sectorIndex)
		{
			return (int32_t)chunk.chunkIndex;
		}
	}

	return -1;
}

int32_t ResolveVisibilityChunkIndexForProvenance(const nri_scene::PTMapWorld& mapWorld, const nri_scene::SurfaceProvenance& provenance)
{
	if (provenance.mapChunkIndex >= 0)
	{
		return provenance.mapChunkIndex;
	}
	return FindMapChunkIndexForSector(mapWorld, provenance.sectorIndex);
}

uint32_t NormalizeResidentAtlasIndex(uint32_t value, uint32_t base)
{
	return value >= base ? value - base : UINT32_MAX;
}

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
	uint32_t materialCount)
{
	if (vertexOffset + vertexCount > geometry.vertices.size() ||
		indexOffset + indexCount > geometry.indices.size() ||
		primitiveOffset + primitiveCount > geometry.primitives.size())
	{
		return 0;
	}

	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)vertexCount);
	hash = Combine(hash, (uint64_t)indexCount);
	hash = Combine(hash, (uint64_t)primitiveCount);
	hash = Combine(hash, (uint64_t)materialCount);
	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		const auto& vertex = geometry.vertices[vertexOffset + i];
		hash = HashResidentGeometryVertexPayload(hash, vertex);
	}

	for (uint32_t i = 0; i < indexCount; ++i)
	{
		hash = Combine(
			hash,
			(uint64_t)NormalizeResidentAtlasIndex(geometry.indices[indexOffset + i], vertexOffset));
	}

	for (uint32_t i = 0; i < primitiveCount; ++i)
	{
		const uint32_t primitiveIndex = primitiveOffset + i;
		const auto& primitive = geometry.primitives[primitiveIndex];
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[0], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[1], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.indices[2], vertexOffset));
		hash = Combine(hash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
		for (float component : primitive.uv0)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.uv1)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.uv2)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		for (float component : primitive.normal)
		{
			hash = Combine(hash, (uint64_t)FloatBits(component));
		}
		hash = Combine(hash, (uint64_t)primitive.flags);
		hash = Combine(hash, (uint64_t)primitive.portalIndex);
		hash = HashResidentGeometryProvenancePayload(hash, mapWorld, geometry, primitive, primitiveIndex);
	}

	return hash != 0 ? hash : 1;
}

uint64_t HashResidentGeometryPayloadOrderIndependent(
	const nri_scene::PTMapWorld& mapWorld,
	const nri_scene::GeometryData& geometry,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t primitiveOffset,
	uint32_t primitiveCount,
	uint32_t materialOffset,
	uint32_t materialCount)
{
	if (vertexOffset + vertexCount > geometry.vertices.size() ||
		primitiveOffset + primitiveCount > geometry.primitives.size())
	{
		return 0;
	}

	std::vector<uint64_t> primitiveHashes;
	primitiveHashes.reserve(primitiveCount);
	for (uint32_t i = 0; i < primitiveCount; ++i)
	{
		const uint32_t primitiveIndex = primitiveOffset + i;
		const auto& primitive = geometry.primitives[primitiveIndex];
		uint64_t primitiveHash = 1469598103934665603ull;
		primitiveHash = Combine(primitiveHash, (uint64_t)NormalizeResidentAtlasIndex(primitive.materialIndex, materialOffset));
		for (float component : primitive.normal)
		{
			primitiveHash = Combine(primitiveHash, (uint64_t)FloatBits(component));
		}
		primitiveHash = Combine(primitiveHash, (uint64_t)primitive.flags);
		primitiveHash = Combine(primitiveHash, (uint64_t)primitive.portalIndex);
		primitiveHash = HashResidentGeometryProvenancePayload(primitiveHash, mapWorld, geometry, primitive, primitiveIndex);

		const float* primitiveUvs[3] = { primitive.uv0, primitive.uv1, primitive.uv2 };
		for (uint32_t corner = 0; corner < 3; ++corner)
		{
			const uint32_t vertexIndex = primitive.indices[corner];
			if (vertexIndex < vertexOffset || vertexIndex >= vertexOffset + vertexCount)
			{
				return 0;
			}
			primitiveHash = HashResidentGeometryVertexPayload(primitiveHash, geometry.vertices[vertexIndex]);
			for (uint32_t component = 0; component < 2; ++component)
			{
				primitiveHash = Combine(primitiveHash, (uint64_t)FloatBits(primitiveUvs[corner][component]));
			}
		}
		primitiveHashes.push_back(primitiveHash);
	}

	std::sort(primitiveHashes.begin(), primitiveHashes.end());
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)vertexCount);
	hash = Combine(hash, (uint64_t)primitiveCount);
	hash = Combine(hash, (uint64_t)materialCount);
	for (uint64_t primitiveHash : primitiveHashes)
	{
		hash = Combine(hash, primitiveHash);
	}
	return hash != 0 ? hash : 1;
}

uint64_t ComputeGeometryTopologySignature(const nri_scene::GeometryData& geometry)
{
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)geometry.vertices.size());
	hash = Combine(hash, (uint64_t)geometry.indices.size());
	hash = Combine(hash, (uint64_t)geometry.primitives.size());
	for (uint32_t index : geometry.indices)
	{
		hash = Combine(hash, (uint64_t)index);
	}
	return hash;
}

uint64_t ComputePrimitiveLayoutSignature(const nri_scene::GeometryData& geometry)
{
	uint64_t hash = 1469598103934665603ull;
	hash = Combine(hash, (uint64_t)geometry.primitives.size());
	hash = Combine(hash, (uint64_t)geometry.primitiveProvenance.size());
	for (size_t i = 0; i < geometry.primitives.size(); ++i)
	{
		const nri_scene::PrimitiveData& primitive = geometry.primitives[i];
		hash = Combine(hash, (uint64_t)primitive.indices[0]);
		hash = Combine(hash, (uint64_t)primitive.indices[1]);
		hash = Combine(hash, (uint64_t)primitive.indices[2]);
		hash = Combine(hash, (uint64_t)primitive.materialIndex);
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv0[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv0[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv1[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv1[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv2[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.uv2[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[0]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[1]));
		hash = Combine(hash, (uint64_t)FloatBits(primitive.normal[2]));
		hash = Combine(hash, (uint64_t)primitive.flags);
		hash = Combine(hash, (uint64_t)primitive.portalIndex);
		if (i < geometry.primitiveProvenance.size())
		{
			const nri_scene::SurfaceProvenance& provenance = geometry.primitiveProvenance[i];
			hash = Combine(hash, (uint64_t)(uint32_t)provenance.sourceType);
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.sectorIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.wallIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.sectionIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.mapChunkIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.nextSectorIndex + 1));
			hash = Combine(hash, (uint64_t)(uint32_t)(provenance.actorIndex + 1));
			hash = Combine(hash, (uint64_t)provenance.drawListType);
			hash = Combine(hash, (uint64_t)provenance.cstat);
			hash = Combine(hash, (uint64_t)provenance.materialFlags);
		}
	}
	return hash;
}
}
