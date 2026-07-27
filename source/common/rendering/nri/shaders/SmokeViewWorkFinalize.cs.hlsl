#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(1, 1, 1)]
void main()
{
	const uint froxelCount = SmokeViewColumnCount() * gViewConstants.FroxelDepth;
	gViewWorkControl[0].EvaluationRoute = gViewConstants.ExecutionRoute;
	gViewWorkControl[0].EvaluationDispatched = froxelCount;
	gViewWorkControl[0].EvaluationSelected = gViewConstants.ExecutionRoute != 0u ?
		gViewWorkControl[0].UniqueFroxels : froxelCount;
	gViewWorkControl[0].EvaluationSkipped = froxelCount - gViewWorkControl[0].EvaluationSelected;
	// The first argument is reserved for exact compact materialization; the
	// second is reserved for the dense-reference comparator. Until integration
	// supplies compaction, publishing zero prevents accidental indirect use.
	gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
	gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
}
