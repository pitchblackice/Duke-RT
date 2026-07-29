#include "nri_persistent_voxel_geometry_arena_policy.h"

#include <limits>

namespace
{
	bool TryAdd(uint64_t left, uint64_t right, uint64_t& result)
	{
		if (right > std::numeric_limits<uint64_t>::max() - left)
		{
			result = std::numeric_limits<uint64_t>::max();
			return false;
		}
		result = left + right;
		return true;
	}
}

NRIPersistentVoxelGeometryArenaPlan BuildNRIPersistentVoxelGeometryArenaPlan(
	uint64_t plannedGeometryBytes,
	uint64_t plannedRuntimeTailBytes,
	uint64_t largestKnownGeometryBytes)
{
	NRIPersistentVoxelGeometryArenaPlan plan = {};
	plan.plannedGeometryBytes = plannedGeometryBytes;
	plan.plannedRuntimeTailBytes = plannedRuntimeTailBytes;
	if (plannedGeometryBytes == 0)
	{
		return plan;
	}

	// Strict preload sees the raw manifest before actor-scoped aliases appear.
	// Reserve one measured maximum manifest geometry unit so the first late
	// alias does not cross an exact-fit multi-gigabyte arena boundary.
	plan.lateAliasReserveBytes = largestKnownGeometryBytes;
	if (!TryAdd(
			plan.plannedRuntimeTailBytes,
			plan.lateAliasReserveBytes,
			plan.totalReserveBytes) ||
		!TryAdd(
			plan.plannedGeometryBytes,
			plan.totalReserveBytes,
			plan.targetGeometryBytes))
	{
		plan.overflow = true;
	}
	return plan;
}
