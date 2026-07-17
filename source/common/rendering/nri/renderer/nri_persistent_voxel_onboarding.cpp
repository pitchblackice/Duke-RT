#include "nri_persistent_voxel_onboarding.h"

#include <algorithm>

namespace
{
	double DefaultStageEstimate(NRIPersistentVoxelOnboardingStage stage)
	{
		switch (stage)
		{
		case NRIPersistentVoxelOnboardingStage::MaterialPlan: return 0.50;
		case NRIPersistentVoxelOnboardingStage::TextureDependencies: return 0.20;
		case NRIPersistentVoxelOnboardingStage::MaterialSlotPatch: return 0.25;
		case NRIPersistentVoxelOnboardingStage::ArenaReserve: return 0.75;
		case NRIPersistentVoxelOnboardingStage::ComputeRequest: return 0.20;
		case NRIPersistentVoxelOnboardingStage::ComputePending: return 0.03;
		case NRIPersistentVoxelOnboardingStage::BlasAllocateRecord: return 1.50;
		case NRIPersistentVoxelOnboardingStage::BlasPending: return 0.03;
		case NRIPersistentVoxelOnboardingStage::Publish: return 0.20;
		case NRIPersistentVoxelOnboardingStage::TlasPending: return 0.05;
		default: return 0.01;
		}
	}
}

void NRIPersistentVoxelOnboardingBudget::BeginPump(bool loadingPhase, double budget, uint32_t deadline)
{
	loading = loadingPhase;
	deadlineFrames = deadline;
	pumpStats = {};
	pumpStats.budgetMs = budget;
	for (size_t i = 0; i < StageCount; ++i)
	{
		if (estimatedMs[i] == 0.0)
			estimatedMs[i] = DefaultStageEstimate((NRIPersistentVoxelOnboardingStage)i);
	}
}

bool NRIPersistentVoxelOnboardingBudget::CanBegin(
	NRIPersistentVoxelOnboardingStage stage,
	double elapsedPumpMs,
	bool visibleRequired,
	uint32_t ageFrames)
{
	pumpStats.attemptedStages++;
	if (loading || pumpStats.budgetMs <= 0.0 || elapsedPumpMs + EstimateMs(stage) <= pumpStats.budgetMs)
		return true;
	if (visibleRequired && deadlineFrames != 0 && ageFrames >= deadlineFrames)
	{
		pumpStats.deadlinePromotions++;
		return true;
	}
	pumpStats.deferredStages++;
	return false;
}

void NRIPersistentVoxelOnboardingBudget::Complete(NRIPersistentVoxelOnboardingStage stage, double actualMs)
{
	const size_t index = (size_t)stage;
	if (index >= StageCount)
		return;
	pumpStats.completedStages++;
	pumpStats.usedMs += actualMs;
	if (actualMs > pumpStats.maxStageMs)
	{
		pumpStats.maxStageMs = actualMs;
		pumpStats.maxStage = stage;
	}
	if (actualMs > estimatedMs[index])
		pumpStats.estimateOverruns++;
	const double alpha = samples[index] < 4 ? 0.50 : 0.20;
	estimatedMs[index] = std::max(0.01, estimatedMs[index] * (1.0 - alpha) + actualMs * alpha);
	samples[index]++;
}

double NRIPersistentVoxelOnboardingBudget::EstimateMs(NRIPersistentVoxelOnboardingStage stage) const
{
	const size_t index = (size_t)stage;
	return index < StageCount && estimatedMs[index] != 0.0 ? estimatedMs[index] : DefaultStageEstimate(stage);
}

NRIPersistentVoxelOnboardingStage InferNRIPersistentVoxelOnboardingStage(
	const NRIPersistentVoxelOnboardingProgress& progress,
	uint8_t admissionState)
{
	switch (admissionState)
	{
	case 1: return NRIPersistentVoxelOnboardingStage::BlasAllocateRecord;
	case 2: return NRIPersistentVoxelOnboardingStage::Publish;
	case 7: return NRIPersistentVoxelOnboardingStage::TlasPending;
	case 9: return NRIPersistentVoxelOnboardingStage::Failed;
	default:
		if (!progress.materialCheckpointPassed)
			return progress.stage == NRIPersistentVoxelOnboardingStage::TextureDependencies ?
				NRIPersistentVoxelOnboardingStage::TextureDependencies : NRIPersistentVoxelOnboardingStage::MaterialPlan;
		if (!progress.arenaCheckpointPassed)
			return NRIPersistentVoxelOnboardingStage::ArenaReserve;
		return NRIPersistentVoxelOnboardingStage::ComputeRequest;
	}
}

const char* NRIPersistentVoxelOnboardingStageName(NRIPersistentVoxelOnboardingStage stage)
{
	switch (stage)
	{
	case NRIPersistentVoxelOnboardingStage::MaterialPlan: return "material-plan";
	case NRIPersistentVoxelOnboardingStage::TextureDependencies: return "texture-dependencies";
	case NRIPersistentVoxelOnboardingStage::MaterialSlotPatch: return "material-slot-patch";
	case NRIPersistentVoxelOnboardingStage::ArenaReserve: return "arena-reserve";
	case NRIPersistentVoxelOnboardingStage::ComputeRequest: return "compute-request";
	case NRIPersistentVoxelOnboardingStage::ComputePending: return "compute-pending";
	case NRIPersistentVoxelOnboardingStage::BlasAllocateRecord: return "blas-allocate-record";
	case NRIPersistentVoxelOnboardingStage::BlasPending: return "blas-pending";
	case NRIPersistentVoxelOnboardingStage::Publish: return "publish";
	case NRIPersistentVoxelOnboardingStage::TlasPending: return "tlas-pending";
	case NRIPersistentVoxelOnboardingStage::Ready: return "ready";
	case NRIPersistentVoxelOnboardingStage::Cancelled: return "cancelled";
	case NRIPersistentVoxelOnboardingStage::Failed: return "failed";
	default: return "unknown";
	}
}
