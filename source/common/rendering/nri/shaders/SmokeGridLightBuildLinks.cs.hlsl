#include "Include/SmokeGridLightingResources.hlsli"
#include "Include/SmokeFroxel.hlsli"
#include "Include/SmokeLighting.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint activeCapacity, ignoredStride;
	gSmokeGridLightActive.GetDimensions(activeCapacity, ignoredStride);
	if (dispatchThreadId.x >= min(gSmokeGridLightControl[0].ActiveCount, activeCapacity))
		return;
	const uint cellIndex = gSmokeGridLightActive[dispatchThreadId.x];
	const uint brickIndex = cellIndex / NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint localIndex = cellIndex % NRI_SMOKE_GRID_CELLS_PER_BRICK;
	const uint3 local = uint3(localIndex & 7u, (localIndex >> 3u) & 7u, (localIndex >> 6u) & 7u);
	const SmokeGridBrick brick = gSmokeRenderGridBricks[brickIndex];
	const int3 cell = SmokeGridCellCoordinate(brick.Coordinate, local);
	const float cellSize = max(asfloat(gSmokeRenderGridControl[0].CellSizeBits), 0.0001);
	const float3 receiver = SmokeGridLightCellCenter(cell, cellSize);
	uint openMask = 0u;
	[unroll]
	for (uint face = 0u; face < 6u; ++face)
	{
		const int3 neighborCell = cell + NRI_SMOKE_GRID_LIGHT_LOBE_AXES[face];
		uint neighborIndex, neighborGeneration;
		if (!SmokeGridLightCellAddress(neighborCell, neighborIndex, neighborGeneration))
			continue;
		// Transport topology is independent of the selected direct-light quality.
		// Without a scene TLAS the current-frame link fails closed.
		bool open = false;
		if (SmokeShadowTracingReady())
		{
			const float3 direction = (float3)NRI_SMOKE_GRID_LIGHT_LOBE_AXES[face];
			open = SmokeFilteredVisibilityResourcesReady() ?
				SmokePointLightVisibleFiltered(receiver, direction, cellSize, false) :
				SmokePointLightVisible(receiver, direction, cellSize, false);
		}
		else
			InterlockedAdd(gSmokeGridLightControl[0].TopologyMissingTlas, 1u);
		if (open)
		{
			openMask |= 1u << face;
			InterlockedAdd(gSmokeGridLightControl[0].LinksOpen, 1u);
		}
		else
			InterlockedAdd(gSmokeGridLightControl[0].LinksBlocked, 1u);
	}
	gSmokeGridLightLinks[cellIndex] = uint4(openMask, brick.Generation, gSmokeConstants.SimulationEpoch, gSmokeConstants.FrameIndex);
}
