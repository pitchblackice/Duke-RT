#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class NRIPersistentVoxelOnboardingStage : uint8_t
{
	MaterialPlan = 0,
	TextureDependencies,
	MaterialSlotPatch,
	ArenaReserve,
	ComputeRequest,
	ComputePending,
	BlasAllocateRecord,
	BlasPending,
	Publish,
	TlasPending,
	Ready,
	Cancelled,
	Failed,
	Count,
};

struct NRIPersistentVoxelOnboardingProgress
{
	NRIPersistentVoxelOnboardingStage stage = NRIPersistentVoxelOnboardingStage::MaterialPlan;
	uint32_t requestFrame = UINT32_MAX;
	uint32_t lastAdvanceFrame = UINT32_MAX;
	uint32_t tlasFrame = UINT32_MAX;
	bool materialCheckpointPassed = false;
	bool arenaCheckpointPassed = false;
};

struct NRIPersistentVoxelOnboardingPumpStats
{
	double budgetMs = 0.0;
	double usedMs = 0.0;
	double maxStageMs = 0.0;
	uint32_t attemptedStages = 0;
	uint32_t completedStages = 0;
	uint32_t deferredStages = 0;
	uint32_t deadlinePromotions = 0;
	uint32_t estimateOverruns = 0;
	NRIPersistentVoxelOnboardingStage maxStage = NRIPersistentVoxelOnboardingStage::MaterialPlan;
};

class NRIPersistentVoxelOnboardingBudget
{
public:
	void BeginPump(bool loading, double budgetMs, uint32_t deadlineFrames);
	bool CanBegin(
		NRIPersistentVoxelOnboardingStage stage,
		double elapsedPumpMs,
		bool visibleRequired,
		uint32_t ageFrames);
	void Complete(NRIPersistentVoxelOnboardingStage stage, double actualMs);

	double EstimateMs(NRIPersistentVoxelOnboardingStage stage) const;
	const NRIPersistentVoxelOnboardingPumpStats& PumpStats() const { return pumpStats; }

private:
	static constexpr size_t StageCount = (size_t)NRIPersistentVoxelOnboardingStage::Count;
	std::array<double, StageCount> estimatedMs = {};
	std::array<uint64_t, StageCount> samples = {};
	bool loading = false;
	uint32_t deadlineFrames = 0;
	NRIPersistentVoxelOnboardingPumpStats pumpStats = {};
};

NRIPersistentVoxelOnboardingStage InferNRIPersistentVoxelOnboardingStage(
	const NRIPersistentVoxelOnboardingProgress& progress,
	uint8_t admissionState);

const char* NRIPersistentVoxelOnboardingStageName(NRIPersistentVoxelOnboardingStage stage);
