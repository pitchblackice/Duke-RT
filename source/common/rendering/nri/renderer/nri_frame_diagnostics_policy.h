#pragma once

struct NRIFrameDiagnosticPolicyInput
{
	bool perfLoopTraceActive = false;
	bool selfTestEnabled = false;
	bool slowdownTraceEnabled = false;
	bool sceneStatsEnabled = false;
	bool voxelStatsEnabled = false;
};

struct NRIFrameDiagnosticPolicy
{
	bool collectBasicSuccessStats = false;
	bool collectInstanceComposition = false;
	bool collectPersistentVoxelStatus = false;
	bool collectAsSummary = false;
	bool collectDeepSceneAudit = false;
};

NRIFrameDiagnosticPolicy EvaluateNRIFrameDiagnosticPolicy(
	const NRIFrameDiagnosticPolicyInput& input);

double CalculateNRIFrameUnattributedMs(
	double totalMs,
	double coreStagesMs,
	double postFrameDiagnosticsMs);
