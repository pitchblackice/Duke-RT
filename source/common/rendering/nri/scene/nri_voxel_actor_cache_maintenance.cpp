#include "nri_voxel_actor_cache_maintenance.h"

#include <algorithm>

NRIVoxelActorMaintenanceDecision NRIVoxelActorMaintenanceGate::Evaluate(
	const NRIVoxelActorMaintenanceInput& input)
{
	NRIVoxelActorMaintenanceDecision decision = {};
	decision.legacyEnumeration = !input.lifecycleModeEnabled;
	if (decision.legacyEnumeration)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_LegacyMode;
	}
	if (!mInitialized)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_Cold;
	}
	else if (mLastLifecycleModeEnabled != input.lifecycleModeEnabled)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_ModeChanged;
	}
	if (input.simulationGeneration == 0)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_MissingGeneration;
	}
	else if (!mInitialized || mLastSimulationGeneration != input.simulationGeneration)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_SimulationChanged;
	}
	if (mInitialized && mLastVoxelsEnabled != input.voxelsEnabled)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_VoxelsChanged;
	}
	if (input.forceReconcile)
	{
		decision.reasonMask |= NRIVoxelActorMaintenanceReason_Forced;
	}

	decision.reconcileCacheEntries = decision.reasonMask != NRIVoxelActorMaintenanceReason_None;
	mLastSimulationGeneration = input.simulationGeneration;
	mLastLifecycleModeEnabled = input.lifecycleModeEnabled;
	mLastVoxelsEnabled = input.voxelsEnabled;
	mInitialized = true;
	return decision;
}

void NRIVoxelActorMaintenanceGate::Reset()
{
	mLastSimulationGeneration = 0;
	mLastLifecycleModeEnabled = true;
	mLastVoxelsEnabled = true;
	mInitialized = false;
}

bool ShouldCollectNRIVoxelActorDuplicationAudit(const NRIVoxelActorDuplicationAuditInput& input)
{
	if (input.voxelStatsEnabled)
	{
		return true;
	}
	if (!input.slowdownTraceEnabled || input.slowdownTop <= 0)
	{
		return false;
	}

	const uint64_t interval = (uint64_t)(std::max)(1u, input.slowdownInterval);
	return input.presentationGeneration == 1 ||
		(input.presentationGeneration != 0 && input.presentationGeneration % interval == 0);
}
