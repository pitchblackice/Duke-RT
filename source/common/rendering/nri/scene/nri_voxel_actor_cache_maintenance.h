#pragma once

#include <cstdint>

enum NRIVoxelActorMaintenanceReason : uint32_t
{
	NRIVoxelActorMaintenanceReason_None = 0,
	NRIVoxelActorMaintenanceReason_LegacyMode = 1u << 0,
	NRIVoxelActorMaintenanceReason_Cold = 1u << 1,
	NRIVoxelActorMaintenanceReason_ModeChanged = 1u << 2,
	NRIVoxelActorMaintenanceReason_SimulationChanged = 1u << 3,
	NRIVoxelActorMaintenanceReason_VoxelsChanged = 1u << 4,
	NRIVoxelActorMaintenanceReason_Forced = 1u << 5,
	NRIVoxelActorMaintenanceReason_MissingGeneration = 1u << 6,
};

struct NRIVoxelActorMaintenanceInput
{
	uint64_t simulationGeneration = 0;
	bool lifecycleModeEnabled = true;
	bool voxelsEnabled = true;
	bool forceReconcile = false;
};

struct NRIVoxelActorMaintenanceDecision
{
	bool reconcileCacheEntries = true;
	bool legacyEnumeration = false;
	uint32_t reasonMask = NRIVoxelActorMaintenanceReason_None;
};

enum class NRIVoxelActorPendingRemovalAction : uint8_t
{
	None,
	RetainCurrentFrame,
	Erase,
};

class NRIVoxelActorMaintenanceGate
{
public:
	NRIVoxelActorMaintenanceDecision Evaluate(const NRIVoxelActorMaintenanceInput& input);
	void Reset();

private:
	uint64_t mLastSimulationGeneration = 0;
	bool mLastLifecycleModeEnabled = true;
	bool mLastVoxelsEnabled = true;
	bool mInitialized = false;
};

struct NRIVoxelActorDuplicationAuditInput
{
	uint64_t presentationGeneration = 0;
	uint32_t slowdownInterval = 1;
	int32_t slowdownTop = 0;
	bool voxelStatsEnabled = false;
	bool slowdownTraceEnabled = false;
};

struct NRIVoxelActorLifecycleJournalInput
{
	bool lifecycleModeEnabled = true;
	bool overflowed = false;
	bool resetSeen = false;
};

struct NRIVoxelActorLifecycleJournalDecision
{
	bool applyEvents = true;
	bool advanceCursor = true;
	bool forceLegacyReconcile = false;
};

bool ShouldCollectNRIVoxelActorDuplicationAudit(const NRIVoxelActorDuplicationAuditInput& input);
NRIVoxelActorPendingRemovalAction ResolveNRIVoxelActorPendingRemoval(
	bool pendingRemoval,
	uint64_t lastSeenFrame,
	uint64_t currentFrame);
NRIVoxelActorLifecycleJournalDecision ResolveNRIVoxelActorLifecycleJournal(
	const NRIVoxelActorLifecycleJournalInput& input);
