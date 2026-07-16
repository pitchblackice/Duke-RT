#include "nri_voxel_actor_cache_maintenance.h"
#include "../renderer/nri_diagnostic_cadence.h"

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

	return ShouldSampleNRIPeriodicDiagnostic(
		input.presentationGeneration,
		input.slowdownInterval);
}

NRIVoxelActorPendingRemovalAction ResolveNRIVoxelActorPendingRemoval(
	bool pendingRemoval,
	uint64_t lastSeenFrame,
	uint64_t currentFrame)
{
	if (!pendingRemoval)
	{
		return NRIVoxelActorPendingRemovalAction::None;
	}
	return lastSeenFrame == currentFrame ?
		NRIVoxelActorPendingRemovalAction::RetainCurrentFrame :
		NRIVoxelActorPendingRemovalAction::Erase;
}

NRIVoxelActorLifecycleJournalDecision ResolveNRIVoxelActorLifecycleJournal(
	const NRIVoxelActorLifecycleJournalInput& input)
{
	NRIVoxelActorLifecycleJournalDecision decision = {};
	decision.applyEvents = input.lifecycleModeEnabled && !input.overflowed;
	decision.advanceCursor = true;
	decision.forceLegacyReconcile = input.lifecycleModeEnabled &&
		(input.overflowed || input.resetSeen);
	return decision;
}
