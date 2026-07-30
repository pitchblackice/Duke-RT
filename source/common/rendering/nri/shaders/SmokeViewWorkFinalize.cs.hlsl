#include "Include/SmokeViewWorkResources.hlsli"

[numthreads(1, 1, 1)]
void main()
{
	const uint froxelCount = SmokeViewColumnCount() * gViewConstants.FroxelDepth;
	gViewWorkControl[0].EvaluationRoute = gViewConstants.ExecutionRoute;
	if (gViewConstants.ExecutionRoute == 2u)
	{
		const bool compactFailed = gViewWorkControl[0].Overflow != 0u ||
			gViewWorkControl[0].CompactCount > gViewConstants.FroxelCapacity;
		if (compactFailed)
		{
			gViewWorkControl[0].CompactCount = 0u;
			gViewIndirectArgs[0] = uint3(0u, 1u, 1u);
			gViewIndirectArgs[1] = uint3(0u, 1u, 1u);
		}
	}
	gViewWorkControl[0].EvaluationDispatched = gViewConstants.ExecutionRoute == 2u ?
		gViewWorkControl[0].CompactCount : froxelCount;
	gViewWorkControl[0].EvaluationSelected = gViewConstants.ExecutionRoute != 0u ?
		gViewWorkControl[0].UniqueFroxels : froxelCount;
	gViewWorkControl[0].EvaluationSkipped = froxelCount - gViewWorkControl[0].EvaluationSelected;
}
