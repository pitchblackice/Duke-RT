#pragma once

#include <cstdint>

struct NRIPersistentVoxelGeometryArenaPlan
{
	uint64_t plannedGeometryBytes = 0;
	uint64_t plannedRuntimeTailBytes = 0;
	uint64_t lateAliasReserveBytes = 0;
	uint64_t totalReserveBytes = 0;
	uint64_t targetGeometryBytes = 0;
	bool overflow = false;
};

NRIPersistentVoxelGeometryArenaPlan BuildNRIPersistentVoxelGeometryArenaPlan(
	uint64_t plannedGeometryBytes,
	uint64_t plannedRuntimeTailBytes,
	uint64_t largestKnownGeometryBytes);
