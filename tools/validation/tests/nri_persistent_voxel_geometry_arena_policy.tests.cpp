#include "nri_persistent_voxel_geometry_arena_policy.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace
{
	constexpr uint64_t MiB = 1024ull * 1024ull;

	bool LargestKnownGeometryProvidesLateAliasReserve()
	{
		const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(64ull * MiB, 0, 11ull * MiB);
		return !plan.overflow &&
			plan.lateAliasReserveBytes == 11ull * MiB &&
			plan.totalReserveBytes == 11ull * MiB &&
			plan.targetGeometryBytes == 75ull * MiB;
	}

	bool PlannedRuntimeTailIsAdditive()
	{
		const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(
			512ull * MiB, 37ull * MiB, 8ull * MiB);
		return !plan.overflow &&
			plan.lateAliasReserveBytes == 8ull * MiB &&
			plan.totalReserveBytes == 45ull * MiB &&
			plan.targetGeometryBytes == 557ull * MiB;
	}

	bool NoManifestGeometryAddsNoHeuristicReserve()
	{
		const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(16ull * MiB, 0, 0);
		return !plan.overflow && plan.totalReserveBytes == 0 &&
			plan.targetGeometryBytes == 16ull * MiB;
	}

	bool OverflowIsRejected()
	{
		const auto plan = BuildNRIPersistentVoxelGeometryArenaPlan(
			std::numeric_limits<uint64_t>::max() - MiB,
			2ull * MiB,
			MiB);
		return plan.overflow;
	}

	bool Run(const char* name, bool (*test)())
	{
		if (test()) return true;
		std::cerr << "FAILED: " << name << '\n';
		return false;
	}
}

int main()
{
	bool passed = true;
	passed &= Run("largest known late alias", LargestKnownGeometryProvidesLateAliasReserve);
	passed &= Run("planned runtime tail", PlannedRuntimeTailIsAdditive);
	passed &= Run("no heuristic reserve", NoManifestGeometryAddsNoHeuristicReserve);
	passed &= Run("overflow rejection", OverflowIsRejected);
	return passed ? 0 : 1;
}
