#define NRI_SMOKE_EVALUATE_GRID_LIBRARY 1
#include "SmokeEvaluateGrid.cs.hlsl"

[numthreads(64, 1, 1)]
void main(uint dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId >= gSmokeViewWorkControl[0].CompactCount)
		return;
	const uint froxelLinearIndex = gSmokeViewCompactIndices[dispatchThreadId];
	const uint plane = gSmokeConstants.FroxelWidth * gSmokeConstants.FroxelHeight;
	const uint z = froxelLinearIndex / plane;
	const uint remainder = froxelLinearIndex - z * plane;
	const uint y = remainder / gSmokeConstants.FroxelWidth;
	const uint x = remainder - y * gSmokeConstants.FroxelWidth;
	SmokeEvaluateGridFroxel(uint3(x, y, z));
}
