#pragma once

#include <array>
#include <cstdint>

constexpr uint32_t NRI_VOXEL_COMPUTE_COMPLETION_SLOT_COUNT = 4;

enum class NRIVoxelComputeCompletionSlotPhase : uint8_t
{
	Free,
	Recording,
	Submitted,
};

enum class NRIVoxelComputeCompletionOutcome : uint8_t
{
	Completed,
	Stale,
	Abandoned,
	Failed,
};

struct NRIVoxelComputeCompletionSlotToken
{
	NRIVoxelComputeCompletionSlotPhase phase = NRIVoxelComputeCompletionSlotPhase::Free;
	uint64_t submissionId = 0;
	uint64_t levelGeneration = 0;
	uint64_t fenceValue = 0;
};

struct NRIVoxelComputeCompletionRingState
{
	std::array<NRIVoxelComputeCompletionSlotToken, NRI_VOXEL_COMPUTE_COMPLETION_SLOT_COUNT> slots = {};
	uint64_t nextSubmissionId = 1;
	uint64_t backpressureCount = 0;
	uint64_t completedCount = 0;
	uint64_t staleCount = 0;
	uint64_t abandonedCount = 0;
	uint64_t failedCount = 0;
	uint32_t cursor = 0;
	uint32_t occupancy = 0;
	uint32_t highWater = 0;

	int32_t Acquire(uint64_t levelGeneration)
	{
		for (uint32_t offset = 0; offset < slots.size(); ++offset)
		{
			const uint32_t index = (cursor + offset) % (uint32_t)slots.size();
			NRIVoxelComputeCompletionSlotToken& slot = slots[index];
			if (slot.phase != NRIVoxelComputeCompletionSlotPhase::Free)
			{
				continue;
			}

			slot.phase = NRIVoxelComputeCompletionSlotPhase::Recording;
			slot.submissionId = nextSubmissionId++;
			slot.levelGeneration = levelGeneration;
			slot.fenceValue = 0;
			cursor = (index + 1u) % (uint32_t)slots.size();
			occupancy++;
			if (occupancy > highWater)
			{
				highWater = occupancy;
			}
			return (int32_t)index;
		}

		backpressureCount++;
		return -1;
	}

	bool Submit(uint32_t index, uint64_t fenceValue)
	{
		if (index >= slots.size() || fenceValue == 0 || slots[index].phase != NRIVoxelComputeCompletionSlotPhase::Recording)
		{
			return false;
		}
		slots[index].fenceValue = fenceValue;
		slots[index].phase = NRIVoxelComputeCompletionSlotPhase::Submitted;
		return true;
	}

	bool IsReady(uint32_t index, uint64_t completedFenceValue) const
	{
		return index < slots.size() &&
			slots[index].phase == NRIVoxelComputeCompletionSlotPhase::Submitted &&
			slots[index].fenceValue != 0 &&
			slots[index].fenceValue <= completedFenceValue;
	}

	void Release(uint32_t index, NRIVoxelComputeCompletionOutcome outcome)
	{
		if (index >= slots.size() || slots[index].phase == NRIVoxelComputeCompletionSlotPhase::Free)
		{
			return;
		}
		slots[index] = {};
		if (occupancy != 0)
		{
			occupancy--;
		}
		switch (outcome)
		{
		case NRIVoxelComputeCompletionOutcome::Completed: completedCount++; break;
		case NRIVoxelComputeCompletionOutcome::Stale: staleCount++; break;
		case NRIVoxelComputeCompletionOutcome::Abandoned: abandonedCount++; break;
		case NRIVoxelComputeCompletionOutcome::Failed: failedCount++; break;
		}
	}
};
