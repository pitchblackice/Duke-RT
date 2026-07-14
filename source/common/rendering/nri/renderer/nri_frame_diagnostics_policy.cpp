#include "nri_frame_diagnostics_policy.h"

NRIFrameDiagnosticPolicy EvaluateNRIFrameDiagnosticPolicy(
	const NRIFrameDiagnosticPolicyInput& input)
{
	NRIFrameDiagnosticPolicy policy = {};
	policy.collectBasicSuccessStats =
		input.perfLoopTraceActive ||
		input.selfTestEnabled ||
		input.slowdownTraceEnabled;
	policy.collectInstanceComposition =
		input.selfTestEnabled ||
		(input.perfLoopTraceActive && input.sceneStatsEnabled);
	policy.collectPersistentVoxelStatus =
		input.slowdownTraceEnabled ||
		(input.perfLoopTraceActive && input.voxelStatsEnabled);
	policy.collectAsSummary = input.perfLoopTraceActive;
	policy.collectDeepSceneAudit =
		input.perfLoopTraceActive &&
		input.sceneStatsEnabled;
	return policy;
}

double CalculateNRIFrameUnattributedMs(
	double totalMs,
	double coreStagesMs,
	double postFrameDiagnosticsMs)
{
	const double unattributedMs = totalMs - coreStagesMs - postFrameDiagnosticsMs;
	return unattributedMs > 0.0 ? unattributedMs : 0.0;
}
