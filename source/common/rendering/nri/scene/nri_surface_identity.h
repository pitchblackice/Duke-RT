#pragma once

#include <cstdint>

namespace nri_scene
{
enum class RayTracingIdentityDomain : uint32_t
{
	Unknown = 0,
	MapChunk,
	DynamicActor,
	VoxelVariant,
	DebugPrimitive,
};

struct UpdatePartitionIdentity
{
	RayTracingIdentityDomain domain = RayTracingIdentityDomain::Unknown;
	uint32_t partitionIndex = UINT32_MAX;
	int32_t sectorIndex = -1;
};

struct RayTracingGeometryIdentity
{
	RayTracingIdentityDomain domain = RayTracingIdentityDomain::Unknown;
	uint64_t stableKey = 0;
	uint32_t partitionIndex = UINT32_MAX;
};

struct RayTracingInstanceIdentity
{
	RayTracingIdentityDomain domain = RayTracingIdentityDomain::Unknown;
	uint64_t stableKey = 0;
	uint32_t partitionIndex = UINT32_MAX;
};

inline UpdatePartitionIdentity MakeMapChunkUpdatePartitionIdentity(uint32_t chunkIndex, int32_t sectorIndex)
{
	UpdatePartitionIdentity identity = {};
	identity.domain = RayTracingIdentityDomain::MapChunk;
	identity.partitionIndex = chunkIndex;
	identity.sectorIndex = sectorIndex;
	return identity;
}

inline RayTracingGeometryIdentity MakeMapChunkGeometryIdentity(uint32_t chunkIndex)
{
	RayTracingGeometryIdentity identity = {};
	identity.domain = RayTracingIdentityDomain::MapChunk;
	identity.stableKey = chunkIndex;
	identity.partitionIndex = chunkIndex;
	return identity;
}

inline RayTracingInstanceIdentity MakeMapChunkInstanceIdentity(uint32_t chunkIndex)
{
	RayTracingInstanceIdentity identity = {};
	identity.domain = RayTracingIdentityDomain::MapChunk;
	identity.stableKey = chunkIndex;
	identity.partitionIndex = chunkIndex;
	return identity;
}
}
