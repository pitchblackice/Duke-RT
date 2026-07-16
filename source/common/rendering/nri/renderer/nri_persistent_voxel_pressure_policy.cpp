#include "nri_persistent_voxel_pressure_policy.h"

NRIPersistentVoxelPressureEvaluationDecision DecideNRIPersistentVoxelPressureEvaluation(
	const NRIPersistentVoxelPressureEvaluationInput& input)
{
	NRIPersistentVoxelPressureEvaluationDecision decision = {};
	if (input.traceEnabled)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Trace;
	}
	if (!input.evaluationValid)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Invalid;
	}
	if (input.evaluationGeneration != input.maintenanceGeneration)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Mutation;
	}
	if (input.evaluationSettingsSignature != input.settingsSignature)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_Settings;
	}
	if (input.evaluationAdapterBudget != input.adapterBudget)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_AdapterBudget;
	}
	if (input.externalPressure)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_ExternalPressure;
	}
	if (input.evaluationValid && input.safetyAuditFrames != 0u &&
		input.frameIndex - input.evaluationFrame >= input.safetyAuditFrames)
	{
		decision.reasonMask |= NRIPersistentVoxelPressureEvaluationReason_SafetyAudit;
	}
	decision.evaluate = decision.reasonMask != NRIPersistentVoxelPressureEvaluationReason_None;
	return decision;
}

bool ShouldInvalidateNRIPersistentVoxelPressureForMembershipChange(bool protectionBlocked)
{
	return protectionBlocked;
}
