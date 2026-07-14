#include "Include/SmokeGridLightingResources.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint cellIndex = dispatchThreadId.x;
	uint cellCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(cellCapacity, ignoredStride);
	if (cellIndex >= cellCapacity)
		return;
	if ((gSmokeConstants.Flags & 1u) != 0u)
	{
		SmokeGridLightRecord empty = (SmokeGridLightRecord)0;
		gSmokeGridLightCurrent[cellIndex] = empty;
		gSmokeGridLightHistory[cellIndex] = empty;
		gSmokeGridLightLinks[cellIndex] = 0u;
	}
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	uint brickCount;
	gSmokeRenderGridBricks.GetDimensions(brickCount, ignoredStride);
	if (brickIndex >= brickCount)
		return;
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT ||
		(brick.Flags & (NRI_SMOKE_GRID_BRICK_CONTENT | NRI_SMOKE_GRID_BRICK_HALO)) == 0u)
		return;
	uint destination;
	InterlockedAdd(gSmokeGridLightControl[0].ActiveCount, 1u, destination);
	if (destination >= cellCapacity)
	{
		InterlockedAdd(gSmokeGridLightControl[0].OverflowRejects, 1u);
		return;
	}
	gSmokeGridLightActive[destination] = cellIndex;
	InterlockedAdd(gSmokeGridLightControl[0].SupportCount, 1u);
	const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
	const float4 scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
	if (scalar.z > 1e-6)
		InterlockedAdd(gSmokeGridLightControl[0].SourceCount, 1u);
}
