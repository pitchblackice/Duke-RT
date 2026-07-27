#include "Include/SmokeGridLightingResources.hlsli"

bool SmokeGridLightOpticalSource(uint cellIndex)
{
	const bool fieldB = gSmokeRenderGridControl[0].FieldPing != 0u;
	const float4 scalar = fieldB ? gSmokeRenderGridScalarB[cellIndex] : gSmokeRenderGridScalarA[cellIndex];
	const float4 optical = fieldB ? gSmokeRenderGridOpticalB[cellIndex] : gSmokeRenderGridOpticalA[cellIndex];
	return scalar.z > 1e-6 && any(optical.rgb > 0.0);
}

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
		if (SmokeSelfShadowEnabled(gSmokeConstants.DebugMode))
		{
			gSmokeGridLightSelfShadowCurrent[cellIndex] = empty;
			gSmokeGridLightSelfShadowHistory[cellIndex] = empty;
		}
		gSmokeGridLightLinks[cellIndex] = 0u;
	}
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	uint brickCount;
	gSmokeRenderGridBricks.GetDimensions(brickCount, ignoredStride);
	if (brickIndex >= brickCount)
		return;
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	if (brick.State != NRI_SMOKE_GRID_RESIDENT ||
		(brick.Flags & (NRI_SMOKE_GRID_BRICK_CONTENT | NRI_SMOKE_GRID_BRICK_HALO)) == 0u ||
		!SmokeGridLightOpticalSource(cellIndex))
		return;

	const uint localIndex = cellIndex % NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint3 local = uint3(localIndex & 7u, (localIndex >> 3u) & 7u, (localIndex >> 6u) & 7u);
	const int3 sourceCell = SmokeGridCellCoordinate(brick.Coordinate, local);
	for (int z = -1; z <= 0; ++z)
	for (int y = -1; y <= 0; ++y)
	for (int x = -1; x <= 0; ++x)
	{
		const int3 supportCell = sourceCell + int3(x, y, z);
		uint supportIndex, supportGeneration;
		if (!SmokeGridLightCellAddress(supportCell, supportIndex, supportGeneration))
			continue;
		uint previousFrameStamp;
		InterlockedCompareExchange(gSmokeGridLightSupportStamps[supportIndex].FrameStamp,
			0u, gSmokeConstants.FrameIndex + 1u, previousFrameStamp);
		if (previousFrameStamp != 0u)
		{
			InterlockedAdd(gSmokeGridLightControl[0].DuplicateCount, 1u);
			continue;
		}
		gSmokeGridLightSupportStamps[supportIndex].BrickGeneration = supportGeneration;
		uint destination;
		InterlockedAdd(gSmokeGridLightControl[0].ActiveCount, 1u, destination);
		if (destination >= cellCapacity)
		{
			InterlockedAdd(gSmokeGridLightControl[0].SupportOverflowCount, 1u);
			InterlockedAdd(gSmokeGridLightControl[0].OverflowRejects, 1u);
			continue;
		}
		gSmokeGridLightActive[destination] = supportIndex;
		InterlockedAdd(gSmokeGridLightControl[0].SupportCount, 1u);
		if (SmokeGridLightOpticalSource(supportIndex))
			InterlockedAdd(gSmokeGridLightControl[0].SourceCount, 1u);
		else
			InterlockedAdd(gSmokeGridLightControl[0].SupportOnlyCount, 1u);
	}
	// With zero overflow, SourceCount + SupportOnlyCount == ActiveCount.
}
